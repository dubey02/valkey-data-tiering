# Replication Strategy for Data Tiering (NKS)

## Status
- **Design:** Complete (2026-07-22)
- **Implementation:** Not started
- **Dependencies:** FlashCache `flashcacheStartStreamBasedSave()`, `fc_real_drain()`

---

## 1. Problem Statement

In the Non-Key-Spilling (NKS) architecture, keys always remain in the DB hashtable, but cold values are spilled to flash storage with `robj->ptr = NULL` and `tiering_state = TIERED`. The existing replication and persistence paths cannot handle this:

| Path | Current Behavior | Impact |
|------|-----------------|--------|
| RDB save / full sync | Silently skips tiered entries (`rdb.c:1191`) | Data loss — replicas missing all flash data |
| AOF rewrite | `serverAssert(false)` crash (`aof.c:2441`) | Server crash |
| Slot migration snapshot | Same assert via `rewriteSlotToAppendOnlyFileRio` | Server crash |
| Incremental replication | Commands propagate logically via `propagateNow()` | ✅ Already works — tiering state invisible |

**Design goal:** A replication strategy that is fast, reliable, compatible with DT↔non-DT migration, slot migration, and all concurrent engine operations.

---

## 2. Key Design Decisions

1. **Single unified RDB** — flash data and memory data in the same stream. One format, any node can load it.
2. **Zero deserialization** — `extStorageSerializeValue()` uses `createDumpPayload()`, so on-flash bytes are already RDB-compatible. Emit directly.
3. **RDB_OPCODE_TIERED hint** — new opcode marks entries that were on flash at snapshot time. Replica uses this to decide RAM vs flash placement.
4. **DT↔non-DT compatible** — non-DT nodes ignore the hint and load to RAM. DT nodes spill directly to flash during load.
5. **Two implementation phases** — fork-based (V1, simpler, correct) then forkless epoch-scan (V2, eliminates memory doubling).

---

## 3. Steady-State Replication (No Changes Needed)

Write commands are propagated to replicas at the **command level** via `propagateNow()` → `replicationFeedSlaves()`. The replication stream contains the original command with full value bytes, captured from command arguments **before** any spill occurs.

The tiering state of a key is invisible to the replication stream. This means:
- Partial sync / backlog: ✅ Works today, no changes
- Command propagation: ✅ Works today, no changes
- Replica applies commands normally and spills independently under its own memory pressure

---

## 4. Full Sync — Fork-Based (V1)

### 4.1 Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│ PRIMARY (main thread)                                           │
├─────────────────────────────────────────────────────────────────┤
│ 1. fc_real_drain(-1)                                            │
│    └─ spin-wait: all in-flight spill/fetch IO completes         │
│ 2. processCompletedStorageRequests()                            │
│    └─ apply all pending completions to DB                       │
│    └─ INVARIANT: every value is EITHER in-memory OR on flash    │
│ 3. flashcacheStartStreamBasedSave()                             │
│    └─ freeze a point-in-time iterator over flash entries        │
│ 4. fork()                                                       │
│    └─ CoW freezes in-memory DB state                            │
│ 5. Parent resumes normal operations                             │
│    └─ new spills/fetches/GC hit post-snapshot state             │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ CHILD (fork)                                                    │
├─────────────────────────────────────────────────────────────────┤
│ 6. Pass 1 — In-memory keys:                                    │
│    └─ iterate DB, rdbSaveKeyValuePair() normally                │
│    └─ skip entries where objectIsTiered(val) == true            │
│ 7. Pass 2 — Flash keys:                                        │
│    └─ iterate FC stream (flashcacheStartStreamBasedSave iter)   │
│    └─ for each: emit key + RDB_OPCODE_TIERED + DUMP payload    │
│    └─ DUMP payload is byte-for-byte createDumpPayload() output  │
│ 8. Write RDB EOF marker                                        │
│ 9. Exit                                                         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ PARENT (after child exit)                                       │
├─────────────────────────────────────────────────────────────────┤
│ 10. flashcacheEndStreamBasedSave()                              │
│     └─ release the FC snapshot, GC can reclaim again            │
│ 11. Ship RDB to replica (file or diskless pipe)                 │
│ 12. Switch replica to live replication stream                   │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Fork Safety Considerations

| Concern | Mitigation |
|---------|-----------|
| FC file descriptors in child | Child uses read-only FC stream iterator (no writes, no io_uring submissions) |
| io_uring ring in child | Not used — stream iterator reads sequentially via `pread()` |
| CoW memory amplification | Accepted tradeoff for V1. Mitigated by dual-channel replication (backlog streams concurrently, reducing overall sync duration) |
| GC relocating entries during child read | FC snapshot freezes the index — GC blocked until `flashcacheEndStreamBasedSave()` |
| In-flight IO at fork time | Eliminated by step 1-2 (drain + process completions) |

### 4.3 Disjointness Guarantee

The drain→snapshot→fork ordering ensures **no duplicate and no missing keys**:
- After drain: no value is in-flight (all are committed to either DB or flash)
- FC snapshot captures exactly the set of flash entries at that instant
- CoW captures exactly the set of in-memory entries at that instant
- These two sets are disjoint by construction (a value is either `ptr != NULL` in DB or `ptr == NULL` on flash, never both)

---

## 5. Full Sync — Forkless Epoch-Scan (V2)

### 5.1 Why Forkless is Better for Data Tiering

| Problem with Fork | Forkless Solution |
|-------------------|-------------------|
| 2× memory during sync (CoW pages) | Zero CoW — no fork at all |
| Child can't use async IO (sequential flash reads) | Main thread's ASIO available — full parallelism |
| FC/io_uring not fork-safe (needs workaround) | No fork-safety issues |
| Longer sync = more CoW dirtying | Incremental, non-blocking |

### 5.2 Sequence

```
┌─────────────────────────────────────────────────────────────────┐
│ Phase 1: Start Snapshot Epoch                                   │
├─────────────────────────────────────────────────────────────────┤
│ 1. Bump global snapshot_epoch counter                           │
│ 2. fc_real_drain(-1) + processCompletedStorageRequests()        │
│ 3. flashcacheStartStreamBasedSave()                             │
│ 4. Record current repl_backlog position as changelog_start      │
│ 5. Begin buffering: mutations to already-scanned keys → changelog│
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Phase 2: Incremental Scan (main thread, N keys/event-loop cycle)│
├─────────────────────────────────────────────────────────────────┤
│ 6. Each cycle: advance dictScan cursor by batch_size buckets    │
│ 7. In-memory entries: rdbSaveKeyValuePair() → output buffer     │
│ 8. Tiered entries: async FC read → on completion →              │
│    RDB_OPCODE_TIERED + DUMP payload → output buffer             │
│ 9. Mutations tracking:                                          │
│    - Key in already-scanned bucket → append to changelog        │
│    - Key in not-yet-scanned bucket → no-op (scan sees new value)│
│    This is "relaxed point-in-time" consistency                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Phase 3: Finalize                                               │
├─────────────────────────────────────────────────────────────────┤
│ 10. Scan complete — all buckets visited                         │
│ 11. Ship changelog (mutations to already-scanned keys)          │
│ 12. flashcacheEndStreamBasedSave()                              │
│ 13. Switch replica to live stream (propagateNow path)           │
└─────────────────────────────────────────────────────────────────┘
```

### 5.3 Cursor Tracking: "Already Scanned?" Check

Valkey's `dictScan()` visits buckets in a deterministic reversed-bit-increment order that is stable across rehashing. To determine if a key's bucket has been scanned:

```c
// bucket_index = hash(key) & ht->sizemask
// already_scanned = (reverse_bits(bucket_index) < reverse_bits(scan_cursor))
int isAlreadyScanned(dict *d, sds key, unsigned long scan_cursor) {
    uint64_t hash = dictHashKey(d, key);
    uint64_t bucket = hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    return (dictReverseBits(bucket) < dictReverseBits(scan_cursor));
}
```

This is O(1) per mutation and adds no memory overhead.

### 5.4 Relaxed Point-in-Time Semantics

The forkless RDB does not represent a single instant — it represents a consistent window:
- Every key appears **exactly once**
- Its value existed at **some point** during the scan window
- The changelog ensures no mutation to a scanned key is lost
- The replica converges to the exact live state after applying changelog + live stream

This is acceptable for replication. For user-facing BGSAVE (must be exact PIT), use fork-based (V1) or briefly pause writes during changelog drain.

---

## 6. Replica-Side Load

### 6.1 DT Replica (tiering enabled)

```
RDB entry with RDB_OPCODE_TIERED:
  → Parse key + TTL + DUMP payload
  → Create dbEntry in hashtable (key metadata in RAM)
  → Spill payload directly to flash (extStorageSpillDuringLoad)
  → Set robj->ptr = NULL, tiering_state = TIERED
  → NO RAM spike — value never inflates into memory
```

**Benefit:** Replica inherits the primary's hot/cold distribution. Post-failover warmth is instant — no minutes of re-warming latency.

### 6.2 Non-DT Replica (DT → non-DT migration)

```
RDB entry with RDB_OPCODE_TIERED:
  → Parse key + TTL + DUMP payload
  → RESTORE payload to robj (createObjectFromDump)
  → Load into RAM normally (ignore tiered hint)
  → If maxmemory exceeded: OOM failure (graceful, documented contract)
```

### 6.3 Non-DT → DT Migration

Free — DT replica loads a standard RDB from a non-DT primary normally. As memory pressure builds, the replica's own spill policy ejects cold values to flash. No special handling needed.

---

## 7. Slot Migration

### 7.1 Snapshot Phase (SLOT_EXPORT_SNAPSHOTTING)

The slot migration snapshot child calls `rewriteSlotToAppendOnlyFileRio()`. Fix the assert:

```c
// Before (crashes):
if (objectIsTiered(o)) serverAssert(false);

// After:
if (objectIsTiered(o)) {
    // Emit as RESTORE command — works on any target (DT or non-DT)
    sds dump_payload = extStorageGetSerializedValue(entry);  // read from FC
    rioWriteBulkString(aof, "RESTORE", 7);
    rioWriteBulkString(aof, key, sdslen(key));
    rioWriteBulkLongLong(aof, ttl_ms);
    rioWriteBulkString(aof, dump_payload, sdslen(dump_payload));
    rioWriteBulkString(aof, "REPLACE", 7);
    sdsfree(dump_payload);
    continue;
}
```

### 7.2 Streaming Phase (SLOT_EXPORT_STREAMING)

During streaming, mutations are propagated as normal commands (the existing write-handler guard from PR #4104 ensures no IO-thread races). Since command propagation doesn't involve tiering state, this phase needs **no changes**.

### 7.3 Target Node Behavior

- **DT target:** RESTORE creates the key in RAM. Under memory pressure, the target's own spill policy will tier it.
- **Non-DT target:** RESTORE creates the key in RAM. Standard behavior.
- **Bidirectional:** DT→non-DT, non-DT→DT, DT→DT all work with the same RESTORE path.

---

## 8. Concurrent Operations During Replication

### 8.1 Ongoing Client Traffic (Reads + Writes)

| Operation | Fork-Based (V1) | Forkless (V2) |
|-----------|-----------------|---------------|
| Read (in-memory key) | Parent serves normally; child sees CoW snapshot | Served normally; if already scanned, no issue |
| Read (tiered key, triggers fetch) | Parent fetches; child doesn't see it (CoW isolation) | Fetch completes, value in RAM; if not-yet-scanned, scan sees it in RAM |
| Write (existing key) | Parent writes; child sees pre-fork value (CoW) | If already-scanned → changelog; if not-yet-scanned → scan sees new value |
| Write (new key) | Parent adds; child doesn't see it (not in CoW snapshot) | If cursor already passed the bucket → changelog; otherwise scan picks it up |

**Key invariant:** The replication stream (backlog) captures all mutations that happen after the snapshot point. The replica applies the RDB then replays the backlog — converging to the current state.

### 8.2 Spilling (Memory Pressure → Value Moves to Flash)

| Scenario | Fork-Based (V1) | Forkless (V2) |
|----------|-----------------|---------------|
| Spill starts during pre-fork drain | Impossible — drain waits for all in-flight IO | Impossible — drain waits for all in-flight IO |
| Spill after fork | Parent spills normally; child sees pre-fork state (CoW) | If key not-yet-scanned: scan sees it as tiered (reads from FC stream). If already-scanned: the write that triggered spill was already propagated to backlog |
| Spill during forkless scan of the same bucket | N/A | Scan holds a reference to the bucket; spill completion sets ptr=NULL but the scan has already captured the in-memory value OR the entry is atomically tiered before scan reaches it |

**Safety:** Spilling is a background operation triggered by memory pressure. It doesn't generate replication stream entries (it's an internal state change, not a client command). The **command** that caused the write was already propagated.

### 8.3 Fetching (Read Triggers Value Recall from Flash)

| Scenario | Impact on Replication |
|----------|----------------------|
| Fetch during fork child execution | Parent fetches (ptr set to value); child still sees ptr=NULL from CoW — correctly reads from FC stream |
| Fetch during forkless scan | If not-yet-scanned: scan sees value in-memory (skips FC stream for this key). If already-scanned: the GET command was already in backlog |

**Safety:** Fetch is triggered by a read command (GET, etc.), which is **not propagated** to replicas (reads don't replicate). No changelog entry needed.

### 8.4 Eviction (maxmemory Policy)

| Scenario | Impact |
|----------|--------|
| Key evicted during fork child | Parent evicts (key deleted); child still sees it in CoW snapshot — correctly included in RDB. Replica will get the DEL from backlog after loading RDB. |
| Key evicted during forkless scan (already scanned) | Key was already serialized. DEL command propagated to backlog → replica applies DEL after loading. Correct. |
| Key evicted during forkless scan (not yet scanned) | Key is gone when scan reaches its bucket — not serialized. The SET that created it and the DEL that evicted it are both in backlog (or neither, if created+evicted within the scan window). Correct. |

**Safety:** Eviction generates a DEL propagation to the replication stream. Standard Valkey behavior — no tiering-specific handling needed.

### 8.5 Deletion (Client DEL/UNLINK Command)

Identical to eviction from the replication perspective. The DEL command is propagated to replicas via the backlog. The snapshot may or may not include the key depending on timing, but the backlog DEL ensures convergence.

**Tiering-specific note:** DEL on a tiered key transitions to `PENDING_DELETION` state until the flash delete IO completes. This is invisible to replication — the DEL command propagation happens immediately regardless of the async flash deletion.

### 8.6 FlashCache Garbage Collection (GC)

| Scenario | Impact |
|----------|--------|
| GC evicts a flash entry (FC capacity pressure) | Engine discovers on next access (GET returns NOT_FOUND) → engine deletes the stale DB entry. This generates a synthetic DEL for propagation. |
| GC during fork child | FC snapshot is frozen (GC blocked) → child reads consistent data. GC resumes after `flashcacheEndStreamBasedSave()`. |
| GC during forkless scan | FC snapshot is frozen for the duration of the scan. GC blocked. This is the main cost of forkless — GC cannot reclaim space during sync. For large datasets, the scan should be fast enough that GC pause is tolerable (< 60s for typical NVMe throughput). |

**Design choice:** GC is blocked during snapshot lifetime for both fork and forkless. This is simpler and safer than trying to reconcile GC relocations with the scan. The FC snapshot API (`flashcacheStartStreamBasedSave`) already provides this guarantee.

### 8.7 Active Defragmentation

| Scenario | Impact |
|----------|--------|
| Defrag moves a value in memory | Pointer changes but data is identical. Fork child sees pre-defrag pointer (CoW). Forkless scan sees whichever pointer is current — same data. |
| Defrag moves a key/entry in hashtable | dictScan is stable across rehashing (reversed-bit-increment). Entry movement doesn't affect scan correctness. |
| Defrag on a tiered entry | No-op — tiered entries have ptr=NULL, nothing to defrag in memory. Flash layout is managed by FC's own compaction. |

**Safety:** Defragmentation is invisible to replication. No tiering-specific handling needed.

### 8.8 Key Expiry (Active + Lazy)

| Scenario | Impact |
|----------|--------|
| Key expires during fork child | Parent expires (generates DEL propagation); child sees unexpired key in CoW. Replica loads the key from RDB then applies the DEL from backlog. Correct. |
| Key expires during forkless scan (already scanned) | Key was serialized with its TTL. Replica loads it and will expire it locally (TTL is replicated). Also, the expiry DEL is in backlog. Replica converges. |
| Key expires during forkless scan (not yet scanned) | Key is gone when scan reaches it. Not serialized. Correct — key is expired on primary and won't exist on replica. |
| Tiered key with TTL | TTL is stored in the DB entry metadata (expires dict), not in flash. Active expiry can fire on a tiered key → async flash delete → `PENDING_DELETION`. DEL propagated regardless of tiering state. |

**Safety:** Standard Valkey expiry propagation handles all cases. Tiering adds no complexity here.

### 8.9 New Keys Created During Sync

| Scenario | Fork-Based (V1) | Forkless (V2) |
|----------|-----------------|---------------|
| New key after fork | Not in child's CoW snapshot. SET command in backlog → replica creates it after loading RDB. | If bucket not-yet-scanned: scan picks it up. If already-scanned: SET in changelog. Either way, replica gets it. |

**Safety:** Covered by the fundamental replication contract: backlog captures everything after snapshot point.

### 8.10 SWAPDB

**Current status:** SWAPDB is **blocked** when data tiering is enabled (returns error). Flash-resident values are addressed by database ID — swapping databases would create dangling references.

**Replication impact:** N/A (command rejected, never propagated).

### 8.11 FLUSHDB / FLUSHALL

| Scenario | Fork-Based (V1) | Forkless (V2) |
|----------|-----------------|---------------|
| FLUSH during fork child | Parent flushes (deletes all keys + flash entries). Child sees pre-flush state (CoW) — generates full RDB. Replica loads full RDB then applies FLUSHALL from backlog → empty. Correct but wasteful. | N/A — same for forkless |
| FLUSH during forkless scan | Scan is **aborted** (all data being serialized is now deleted). Restart the full sync from scratch. FLUSHALL propagated to backlog → replica starts fresh. |
| FLUSH of non-active DB during scan | Only the flushed DB is affected. If scanning a different DB, no impact. If scanning the flushed DB, abort scan for that DB. |

**Tiering-specific note:** FLUSHALL with tiering triggers `waitWhileFlushInProgress()` which drains all flash deletes synchronously. This can block the main thread (see known Bug #2 — FlashCache deadlock during flush + index growth). Replication design assumes this bug is fixed.

### 8.12 BGSAVE / BGREWRITEAOF (User-Initiated Persistence)

| Scenario | Impact |
|----------|--------|
| BGSAVE during active replication sync | Standard Valkey behavior: only one background save at a time. If replication needs BGSAVE and one is already running, it waits. No tiering-specific change. |
| BGSAVE standalone (not replication) | Same fork-based flow as full sync (drain→snapshot→fork→two-pass). RDB file contains all data (memory + flash). |
| BGREWRITEAOF | Same materializer: tiered entries emitted as RESTORE commands in the AOF. On reload, RESTORE recreates the key in RAM; tiering resumes under memory pressure. |

---

## 9. AOF Rewrite

Replace the `serverAssert(false)` with the same materializer used by slot migration:

```c
// In rewriteAppendOnlyFileRio():
if (objectIsTiered(o)) {
    sds dump_payload = extStorageGetSerializedValue(entry);
    // Emit: RESTORE key ttl dump_payload REPLACE
    if (rioWriteAofRestore(aof, key, ttl_ms, dump_payload) == 0) goto werr;
    sdsfree(dump_payload);
    continue;
}
```

On AOF reload:
- DT node: RESTORE creates the key, then spill policy ejects cold values
- Non-DT node: RESTORE creates the key in RAM

This makes AOF files portable across DT and non-DT nodes.

---

## 10. REPLCONF Capability Negotiation (V2 Fast Path)

For DT↔DT replication where both nodes use the same storage backend:

```
Replica → Primary:  REPLCONF ext-storage flashcache
Primary → Replica:  +OK (or -ERR if incompatible)
```

If negotiated:
- **In-memory data:** Standard RDB (engine-owned)
- **Flash data:** Physical FlashCache segment transfer (module-owned)
- Replica lands flash data **directly on its flash** — zero RAM, zero re-serialization

This is the "fast path" from PingXie's proposal (issue #83). It requires:
- Both nodes running compatible FC versions
- FC exposing a segment-export/import API (not yet built)
- Falls back to universal RDB if negotiation fails

**Phase:** V3 (after V1 fork + V2 forkless are stable).

---

## 11. Implementation Touchpoints

### Must-Modify Functions (V1)

| File:Line | Function | Change |
|-----------|----------|--------|
| `src/rdb.c:1191` | `rdbSaveKeyValuePair()` | Remove `if (objectIsTiered(val)) return 0;` — replace with pass-2 handling |
| `src/rdb.c:~1150` | `rdbSaveRio()` (main save loop) | Add pass-2: iterate FC stream after pass-1 |
| `src/rdb.c:3219` | RDB load opcode dispatch | Add `case RDB_OPCODE_TIERED:` handler |
| `src/aof.c:2441` | `rewriteAppendOnlyFileRio()` | Replace assert with RESTORE emit |
| `src/aof.c:2492` | `rewriteSlotToAppendOnlyFileRio()` | Same — RESTORE emit for tiered entries |
| `src/replication.c:~890` | `startBgsaveForReplication()` | Add pre-fork drain + FC snapshot start |
| `src/replication.c:3441` | Replica RDB load completion | Add spill-on-load path for `RDB_OPCODE_TIERED` entries |
| `src/ext_storage.c` (new) | `extStoragePreForkDrain()` | Wire `fc_real_drain(-1)` + `processCompletedStorageRequests()` |
| `src/ext_storage.c` (new) | `extStorageGetSerializedValue()` | Read DUMP payload from FC for a tiered entry |

### New Functions/APIs Needed

| Function | Purpose |
|----------|---------|
| `extStoragePreForkDrain()` | Quiesce all in-flight IO before fork/snapshot |
| `extStorageGetSerializedValue(dbEntry*)` | Synchronous read of DUMP payload from FC |
| `extStorageSpillDuringLoad(sds key, sds payload, long long ttl)` | Spill directly to flash during RDB load |
| `rioWriteAofRestore(rio*, sds key, long long ttl, sds payload)` | Helper to emit RESTORE command to AOF/slot-migration stream |

---

## 12. Phasing

| Phase | Scope | Complexity | Prerequisite |
|-------|-------|-----------|--------------|
| **V1** (correctness) | Fork-based full sync + AOF rewrite + slot migration | ~300 LOC | FC stream-save API (exists), fc_real_drain (exists) |
| **V2** (performance) | Forkless epoch-scan, parallel flash reads, replica direct-to-flash load | ~500 LOC | V1 working, cursor-tracking infra |
| **V3** (fast path) | REPLCONF negotiation, physical FC segment transfer for DT↔DT | ~400 LOC | FC export/import API (new), V2 working |

### V1 Deliverables
- [ ] `extStoragePreForkDrain()` wired into BGSAVE and AOF rewrite fork sites
- [ ] Pass-2 FC stream iteration in RDB save child
- [ ] `RDB_OPCODE_TIERED` emitter + loader
- [ ] RESTORE-based materializer for AOF rewrite and slot migration
- [ ] Replica-side spill-on-load for DT replicas
- [ ] TCL tests: full sync DT→DT, DT→non-DT, non-DT→DT, slot migration with tiered keys

---

## 13. Industry Context

| System | Approach | Fork? | Flash Read on Sync? |
|--------|----------|-------|---------------------|
| Redis Enterprise Auto Tiering | Logical (in-memory replication) | No (proprietary snapshot) | Yes — reads all flash values back |
| Apache Kvrocks | Physical files (RocksDB backup) + WAL streaming | No fork | No — ships SST files directly |
| Dragonfly | Forkless epoch-scan + changelog | No fork | Yes — reads from disk during scan |
| KeyDB FLASH | Fork + RDB (standard Redis) | Yes | Yes — sequential reads in child |
| Aerospike Hybrid | Record-level migration + direct-to-SSD on replica | No fork | Yes — reads from SSD per record |
| **Our V1** | Fork + FC stream + unified RDB | Yes | Yes — but async via FC stream iterator |
| **Our V2** | Forkless epoch-scan + async ASIO reads | No fork | Yes — parallel via main thread's ASIO |

---

## 14. Requirements Traceability

| Requirement | Source | Status |
|-------------|--------|--------|
| R1: RDB contains both memory + flash data | QuChen88, #3326 | ✅ Addressed (unified RDB, two-pass) |
| R2: RDB compatible across DT and non-DT | QuChen88, #3326 | ✅ Addressed (hint opcode, graceful fallback) |
| R3: Slower full sync acceptable | QuChen88, #3326 | ✅ Accepted (V2 optimizes) |
| R4: Parallel disk reads | QuChen88, #3326 | ✅ V1: FC stream, V2: ASIO parallel |
| R5: Delay mutations on keys being read | QuChen88, #3326 | ✅ Fork: CoW isolation. Forkless: relaxed PIT + changelog |
| R6: REPLCONF-negotiated hybrid sync | PingXie, #83 | 🔜 V3 (physical FC transfer) |
| R7: create_snapshot() API | PingXie, #83 | ✅ `flashcacheStartStreamBasedSave()` exists |
| Failover warmth (replica mirrors hot/cold) | OSS discussion | ✅ DT replica spills on load, inherits distribution |
| Spill-as-it-loads (avoid OOM) | OSS discussion | ✅ `extStorageSpillDuringLoad()` |
| NKS preferred for slot migration simplicity | madolson, #83 | ✅ NKS design — keys always in HT |
| No conflict with Raft (#1355) | PingXie, #83 | ✅ Raft operates on command stream, not RDB format |
| Bidirectional DT↔non-DT migration | QuChen88, #3326 | ✅ All three directions work |

---

## 15. Open Items

1. **FC stream iterator performance:** Need to benchmark sequential read throughput from FC stream during child execution. Target: saturate NVMe bandwidth (3+ GB/s on r7gd).
2. **GC pause duration during snapshot:** For large datasets (100M+ keys, 500GB+ flash), the GC pause during FC snapshot could be significant. Monitor and add metrics. If problematic, explore copy-on-write at the FC index level.
3. **Forkless + dictScan stability during rehash:** Valkey's dictScan handles rehashing, but the "already-scanned" check needs testing with concurrent rehash. The reversed-bit-increment property should make this correct, but edge cases need TCL tests.
4. **RDB_OPCODE_TIERED backward compatibility:** Older Valkey versions will fail to load an RDB with unknown opcodes. The opcode should be registered upstream even before the full feature lands, or use the existing aux-field mechanism for graceful degradation.
