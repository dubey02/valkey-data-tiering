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

## What this deliberately does not do

- No per-item sequence numbers / log format changes (head journal avoids it).
- No separate index-mutation WAL (redundant with the log).
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
6. **flush policy config** + INFO counters (checkpoint age/duration, delta
   bytes replayed, journal lag, fence-held bytes).
7. **Crash matrix**: kill -9 during {steady write, staging flush, checkpoint
   write, GC relocation, delete burst}; assert keyspace equivalence against a
   synchronous oracle, x {everysec, always} flush policies.

## Open issue carried on this branch

Tiered fetch path kills client connections at value sizes >=16KB under >=32
connections (timing-sensitive; invisible under strace; <=4KB clean to 200
conns; vanilla clean). Root cause TBD — orthogonal to this design but must be
fixed before any durability claims are benchmarked at those sizes.
