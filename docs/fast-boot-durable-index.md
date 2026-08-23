# Durable Index Persistence (Fast Boot Phase 3)

Goal: restore the FC index at boot in O(index size + bounded delta) with **no
clean-shutdown requirement**. Crash at any instant must recover to a
consistent keyspace with a bounded, configurable loss window (AOF-everysec
semantics), never a corrupt or resurrected state.

Replaces the Phase 3-lite scheme (single index snapshot written only at clean
shutdown; crash = cold start).

## Core insight: the log is already the WAL

Every index INSERT is derivable from the item log: an item appended at offset
X implies `insert(hash(key), X)`. Full-log replay is exactly what Phase 1/2
scan-recovery does — its only problem is cost, O(all log bytes).

So we do not build a second journal of index mutations. We periodically
**checkpoint** the in-memory index (the Phase 3-lite serializer, reused
verbatim: bucket geometry + 8-byte logEntry chains + allocated-bytes + SipHash
seed) and on boot **replay only the log segment written after the
checkpoint**. The delta segment IS the WAL tail. Recovery cost:

    O(keys x 11B)  checkpoint load (measured: 2M keys = 22MB = ~58ms on r7gd)
  + O(write rate x checkpoint interval)  delta scan

Both terms are bounded and tunable. A write-idle instance recovers in pure
checkpoint-load time regardless of uptime.

## The four gaps that make "checkpoint + delta replay" correct

### 1. Finding the durable head after a crash (head journal)

The delta replay needs to know where the log's valid data ends. Today only
the clean-shutdown superblock records head/tail. The log is circular, so a
forward scan cannot distinguish "next valid item" from stale bytes of a
previous lap without help.

Fix: a tiny append-only **head journal** (`<path>.headj`). One 24-byte record
per staging-buffer flush:

    { head_offset u64, tail_offset u64, epoch u32, crc32c u32 }

written immediately after the flush completes. The log fd is O_DIRECT
(fio.c:63), so a completed flush write has already bypassed the page cache —
the journal record (own fd, written + fdatasync'd, or O_DSYNC) marks that
boundary durable; an fsync on the log fd covers the device volatile cache,
as the superblock writer in recovery.c already does. Boot reads the journal,
takes the last CRC-valid record: that is the durable head. Bytes beyond it
were never acknowledged as durable and are ignored. The journal is truncated
at each checkpoint (records before the checkpoint are dead), so it stays
tiny.

This also fixes full-log-scan fallback after crash (today impossible: no
superblock), independent of checkpoints.

### 2. GC vs checkpoint (space-reuse fence)

GC relocates live items head-ward and frees tail space. A checkpoint entry
pointing at offset X is invalidated if GC's freed space at X is overwritten
by new writes before the *next* checkpoint completes — after a crash, boot
would load the checkpoint and follow X into unrelated bytes.

Fix: **two-generation space-reuse fence**. Space freed by GC after checkpoint
generation G is not handed to the allocator until checkpoint G+1 completes.
Invariant: for the newest complete checkpoint, every entry offset either
still holds its item, or the item was relocated to an offset >= that
checkpoint's head — and the delta replay observes the relocation as a fresh
append (last-write-wins), correcting the entry.

Cost: GC's reclaim latency grows by one checkpoint interval. Capacity
headroom must absorb up to one interval of writes; the checkpointer must run
often enough under write pressure (see policy below).

### 3. Deletes (tombstones in the log)

`deleteItem` is index-only today; any replay (delta or full) resurrects keys
deleted since the checkpoint. This was an accepted POC gap; durable boot
makes it load-bearing.

Fix: append a **delete tombstone** to the log on `deleteItem`. The format
already reserves `FC_REPL_CMD_DELETE` in serialization.h, so this is
format-compatible: a header-only item (key, no value, DELETE flag). Replay
applies it as `index_delete(hash)`. GC drops tombstones once the tail passes
the offset of every older instance of that key (conservative POC: keep
tombstones until the segment cycles; refine later).

### 4. Acknowledgement ordering (the durability contract)

Under key-spilling, a spilled key's ONLY copy is the flash log — once the
dict entry is dropped, an un-fsync'd staging buffer is real data loss on
crash. Today spill completions are acked at staging-buffer write time.

Fix: make the ack point configurable, exactly like AOF. Because the log is
O_DIRECT, "durable" = "flushed out of the staging buffer" (+ device-cache
fsync); the loss window is precisely staging-buffer residency, so the knob is
flush/ack cadence:

> **Superseded as the ack mechanism by the client-ack WAL (section below)**,
> which moves durability to command time with group-committed fdatasync on a
> dedicated fd — decoupling ack latency from staging-flush cadence entirely.
> The flush-cadence knob remains relevant only as a bound on WAL retirement
> lag (how long WAL segments stay live before the main log covers them).

    ext-storage-flush everysec   (default) ack at staging-buffer write; a
                                 time-based flush (io-thread cron) bounds
                                 buffer residency -> bounded <=1s loss
    ext-storage-flush always     completion delivered only after the item's
                                 staging buffer is flushed + journaled; the
                                 drop-at-completion (and hence dict removal)
                                 never precedes durability -> zero loss

Invariant either way: **the index checkpoint and head journal never reference
bytes still resident in the staging buffer** (checkpointer runs on the io
thread after a flush; journal record follows the flush it describes).

## Boot sequence

1. Load newest complete checkpoint generation (tmp+rename, two generations
   retained; partial/corrupt -> older generation).
2. Install the SipHash seed from the checkpoint BEFORE any hashing
   (Phase 3-lite rule, unchanged).
3. Read head journal -> durable head H (and tail T for fallback scan).
4. Replay log [checkpoint.head, H): CRC-validate each item; apply
   insert / relocate (same op) / tombstone, last-write-wins.
   CRC break before H = torn tail inside the fsync window -> truncate there
   (those bytes were never durability-acknowledged under `always`; under
   `everysec` they are inside the declared loss window).
5. Set num_items / allocated-bytes accounting from checkpoint + replay delta.
6. Serve. (Keyspill-only, as today: the index holds hashes, not keys.
   Non-keyspill boots use full-log scan [T, H) — now crash-safe too via the
   head journal.)

Fallback ladder: checkpoint+delta -> full scan via head journal -> cold start.

## Checkpoint policy

Trigger on the io thread: `ext-storage-index-checkpoint-interval` seconds
(default 60) OR N bytes appended since last checkpoint (default: 25% of log
capacity — bounds both delta-replay time and GC fence pressure), whichever
first. Write cost is small and sequential: 11B/key (22MB per 2M keys) + a
rename; skipped entirely if no mutations since the last generation.

## Client-ack WAL (durability contract: +OK implies durable)

Supersedes gap 4's flush-cadence knob as the ack mechanism. Contract: a
client-visible reply to a write (`+OK` from SET, the EXEC reply array, a
script's return) implies that write is durably committed. Scope is fixed:
**admission=flash, promotion=never** — assumed everywhere below; every write
eventually reaches the main log, which is what makes WAL retirement bounded.

The main log cannot provide low-latency durability directly: it is O_DIRECT
and page-aligned, so per-write durability means either waiting on the 4MB
staging flush (high ack latency) or page-padding small flushes (write
amplification + log fragmentation). The WAL decouples the two: big lazy
sequential batches for the permanent log, compact synchronous appends for
durability.

### Write path

1. **Submit at write time**: command execution serializes the dirty write
   (same FC item bytes: header + crc32c + dbid + key + value; deletes are
   tombstone records) and submits to FC via the request ring. This replaces
   the beforeSleep spill controller as the submission point for dirty data
   (and collapses the dirty-transient double-spill race class). The FC
   request ring becomes the durability path for every write: ring-full
   backpressure surfaces as ack latency, by design.
2. **io thread**: pops the ring in FIFO order, appends the record to the WAL
   (buffered write, own fd) AND to the staging buffer, group-commits with one
   `fdatasync` per completion-pump cycle (or timer), then posts the
   completion carrying the durable LSN high-water mark.
3. **LSN-gated reply release** (main thread): the write executes fully —
   dict updated, reply written into the client output buffer — nothing
   parks or re-executes (blocked-client machinery cannot suspend mid-EXEC or
   mid-script). The client is stamped with the LSN of its last write;
   `handleClientsWithPendingWrites` skips clients whose stamp exceeds the
   durable LSN learned from the completion pump. Valkey already defers all
   reply bytes to the post-beforeSleep write phase, so the gate adds only
   the fsync latency, amortized across every write in the iteration.

fsync policy is the AOF-style knob, applied to the WAL fd:
`always` = fdatasync before every completion batch (zero loss for acked
writes); `everysec` = timer-batched (<=1s window, replies released without
waiting).

### Transaction-group framing

Motivation: the WAL is a per-key item log and replay is per-record
last-write-wins, so a crash can leave a **partial transaction** durable
(6 of an EXEC's 10 SETs fsync'd). No acked-durability violation — the EXEC
reply was LSN-gated on the whole group and never released — but recovery
would apply a torn transaction, which vanilla AOF avoids by truncating an
incomplete trailing MULTI at load. Framing restores that atomicity.

Format — three record kinds distinguished by a flags byte in the WAL record
header:

    STANDALONE item/tombstone   applies directly at replay (the common case,
                                zero framing overhead)
    GROUP_BEGIN                 { lsn u64, group_seq u64 }
    IN_GROUP item/tombstone     body identical to STANDALONE
    GROUP_COMMIT                { group_seq u64, record_count u32,
                                  group_crc u32 (crc32c over member CRCs) }

Rules:

- **Group = one atomic execution unit** that emits more than one WAL record:
  an EXEC, a script invocation, or a single command dirtying multiple keys
  (MSET, LMPOP, ...). Units emitting exactly one record stay STANDALONE;
  units emitting zero records emit no framing at all.
- **Contiguity is structural, not enforced**: the main thread executes the
  unit atomically and pushes its records to the ring back-to-back;
  the io thread appends in FIFO ring order — so a group's records are
  contiguous in the WAL and replay buffering is a single pending group, not
  a map.
- **Ack gating**: the client's LSN stamp is the GROUP_COMMIT record's LSN, so
  the reply releases only when the entire group is durable. fsync boundaries
  may land mid-group (timer); harmless — the commit marker, not the fsync,
  is the atomicity token.
- **Replay**: STANDALONE applies immediately; IN_GROUP records buffer until a
  GROUP_COMMIT with matching group_seq, record_count, and group_crc, then
  apply in order; an unterminated or mismatched group truncates the WAL at
  its GROUP_BEGIN (everything from there on was never acked — LSN gating
  guarantees it). Any CRC break likewise truncates at the break.
- **Segment rotation only at group boundaries**: rotation is deferred until
  the open group's commit record is appended, so a group never spans
  segments and per-segment retirement stays trivial.

### Retirement and recovery

A WAL record is dead once its item is durable in the main log — i.e. once
the head journal records a flush covering its log offset. Under
admission=flash every write spills, so retirement is continuous: two
ping-pong segments of ~staging-buffer size, a segment recycled when every
record in it is covered by the durable head. Bytes are written twice
(WAL + log) but IOPS are trivial — buffered sequential + one fdatasync per
group commit.

Boot composes with everything above: checkpoint load -> log delta replay
[checkpoint.head, durable head) -> **WAL tail replay** on top, last-write-
wins, framing rules applied. The 26-72ms boot is preserved; the WAL tail is
at most two staging-buffer-sized segments.

## What this deliberately does not do

- No per-item sequence numbers / log format changes in the MAIN log (head
  journal avoids it; LSNs live only in the WAL).
- No index-mutation WAL — index inserts remain derivable from the main log.
  The client-ack WAL journals *data* for the ack contract; it is not a
  second index journal.
- No on-disk index structure (B-tree/LSM) — the index stays a RAM hash;
  disk holds snapshots + the natural log.
- TTLs still absent from the log (pre-existing gap, orthogonal).

## Implementation order (each step independently testable)

1. **Delete tombstones** — fixes the resurrection gap for ALL recovery modes.
2. **Head journal** — makes full-log scan crash-safe (first crash-tolerant
   boot, before checkpoints even exist).
3. **Periodic checkpoint** — generations, tmp+rename, reuse 3-lite serializer.
4. **GC space-reuse fence** — two-generation rule.
5. **Boot delta-replay path** + fallback ladder.
6. **Client-ack WAL, write side** — submit-at-write-time, io-thread append +
   group-commit fdatasync, LSN-gated reply release, STANDALONE records only
   (single-key writes ack durably end-to-end).
7. **Transaction-group framing** — GROUP_BEGIN/IN_GROUP/GROUP_COMMIT,
   boundary-aligned segment rotation, torn-group truncation at replay.
8. **WAL retirement + boot replay** — segment recycling against the head
   journal's durable head; WAL tail replay in the boot ladder; INFO counters
   (checkpoint age/duration, delta bytes replayed, journal lag, fence-held
   bytes, WAL size / retirement lag / fsyncs-per-sec).
9. **Crash matrix**: kill -9 during {steady write, staging flush, checkpoint
   write, GC relocation, delete burst, mid-EXEC, mid-group-fsync}; assert
   keyspace equivalence against a synchronous oracle — acked writes present,
   unacked transactions all-or-nothing — x {everysec, always} WAL policies.

## Open issue carried on this branch

Tiered fetch path kills client connections at value sizes >=16KB under >=32
connections (timing-sensitive; invisible under strace; <=4KB clean to 200
conns; vanilla clean). Root cause TBD — orthogonal to this design but must be
fixed before any durability claims are benchmarked at those sizes.
