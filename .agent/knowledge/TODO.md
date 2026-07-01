# TODOs

## Completed ✅

- [x] Lock-free MPSC queue for native IO thread (C11 atomics, pipe wakeup, SPSC completion ring)
- [x] Module completion hang fix (removed max_in_flight_reads throttle, batched IO worker)
- [x] Module load ordering fix (extStorage_init after moduleLoadFromQueue)
- [x] --ext-storage-backend config option (string config in config.c)
- [x] Both paths independently functional and stress-tested (10M ops each, 0 crashes)
- [x] DEBUG SPILL command working on both paths

## Active TODOs

### 1. Auto-create flash file in fc_real_open
**Problem**: FlashCache asserts if /tmp/valkey-flash.db doesn't exist. User must manually `truncate -s 1G`.
**Fix**: Call truncate/fallocate in fc_real_open() if file doesn't exist.
**Priority**: High (usability)

### 2. Fix PENDING_EVICT design
**Problem**: Key being fetched can get evicted immediately after fetch completes — wasteful I/O.
**Fix**: Don't let eviction sampler pick keys in COPYING_TO_MEMORY state.
**Priority**: Medium (correctness is fine, just wasteful)

### 3. Reduce data tiering hooks scattered across core
**Problem**: Tiering checks sprinkled ad-hoc throughout codebase (server.c, networking.c, rdb.c, defrag.c, object.c, beforeSleep).
**Goal**: Consolidate into fewer integration points. Core only needs preCommandExec + beforeSleep + objectIsTiered() guards.
**Priority**: Medium (code quality)

### 4. Zero-copy spill
**Problem**: spillItemAsync() does full memcpy of value before submitting to ASIO.
**Fix**: Pass sds pointer directly (key is blocked, safe). Embedded values still need copy.
**Savings**: Eliminates zmalloc + memcpy per spill at 500B-1000B+ values.
**Priority**: Medium (performance)

### 5. Smarter eviction tracking to reduce read misses
**Problem**: Under extreme pressure, FlashCache GC evicts keys before engine can fetch them (19K misses native, 11K module in 200K test).
**Fix**: Track in-flight fetch keys and skip them during GC, or add a short grace period.
**Priority**: Low (expected behavior under extreme pressure, not a correctness bug)

### 6. Snapshot/RDB design for tiered keys
**Problem**: Currently tiered keys are skipped during RDB save.
**Design needed**: How to include tiered values in snapshots without blocking.
**Priority**: High (production requirement)

### 7. Flush/drain mechanism
**Problem**: No way to bring all tiered keys back to memory (e.g., before shutdown or migration).
**Priority**: High (production requirement)

### 8. Real RocksDB backend
**Problem**: Only FlashCache backend is real. RocksDB is a mock.
**Priority**: Medium (needed for comparison/flexibility)
<<<<<<< HEAD

### 9. Measure eviction sampling cost — evaluate true LRU vs approximation
**Problem**: Current spill candidate selection uses LRU pool sampling (evictionPoolPopulate). Unknown cost per sample under tiering workloads where many sampled keys may be in COPYING_TO_FLASH or already tiered (wasted samples). Need to measure: (a) CPU time spent in sampling per spill cycle, (b) hit rate of sampling (% of samples that yield a spillable candidate), (c) whether true LRU (O(1) eviction via linked list) or clock/FIFO would be cheaper for tiering's access pattern.
**Approach**: Instrument evictionPoolPopulate with cycle counter, track wasted samples, compare with a simple FIFO/clock prototype.
**Priority**: Medium (admission policy design input)

### 10. Optimize tiering state memory overhead
**Problem**: Each spilled key costs ~68B extra: a separate `tieringStateEntry` (16B struct + 28B sds key copy + 8B hashtable bucket) plus a placeholder sds (16B). With millions of tiered keys, this adds hundreds of MB. dt-poc avoids this by packing tiering state into spare bits of the robj's `lru` field — no separate hashtable, no key duplication, reducing per-key overhead from ~130B to ~73B.
**Approach**: Pack TieringState (2 bits) + inflight_op (2 bits) into unused bits of robj (e.g., top bits of `lru` field or a new bitfield). Eliminate `keys_tiering_state` hashtable entirely. Replace placeholder sds with NULL value pointer (use encoding=TIERED as the signal).
**Priority**: Medium-High (directly reduces memory overshoot, improves dataset:DRAM ratio)

### 11. Blind-write for SET (highest priority perf optimization)
**Problem**: Every SET to a flash key triggers a fetch (read from NVMe) before the write can proceed. This blocks the client for ~0.1ms and wastes 26% of disk IOPS on unnecessary reads. With 67% of keys on flash, this is the #1 TPS limiter.
**Approach**: For SET commands, skip the fetch. Mark key as ONLY_MEMORY, submit async DELETE to flash (fire-and-forget), proceed with SET immediately. No client blocking needed.
**Expected impact**: ~22% main-thread CPU savings, TPS from 81K → ~100K
**Priority**: HIGH (biggest single optimization remaining)

### 12. Pass entry pointer to extStorageGetState
**Problem**: `extStorageGetState(db, key)` does `kvstoreHashtableFind` (full HT lookup) just to read 3 bits from the robj. Many call sites already have the entry pointer from a previous lookup.
**Approach**: Add `extStorageGetStateFromEntry(robj *entry)` that reads `entry->tiering_state` directly. Refactor call sites that already have the entry.
**Priority**: Medium (saves ~16% of main-thread CPU)
=======
>>>>>>> bf0756fee (feat: pluggable storage with lock-free native + module paths)
