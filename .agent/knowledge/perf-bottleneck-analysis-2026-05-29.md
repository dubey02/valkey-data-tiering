# Performance Bottleneck Analysis & Fixes (2026-05-29/30)

## Bottlenecks Identified & Fixed

### 1. Spill Loop Stalling Main Thread
**Symptom:** Event loop duration 3.5 SECONDS, server stuck at 18 TPS
**Root cause:** `continue` on non-spillable keys in spill loop caused infinite sampling
**Fix:** Bounded retry (EVPOOL_SIZE attempts) + `break` on non-spillable (matching key-spilling workspace)

### 2. Eviction Pool Pollution
**Symptom:** 88% of eviction pool samples hit non-spillable keys (COPYING_TO_FLASH state)
**Root cause:** `evictionPoolPopulate` didn't filter by tiering state
**Fix:** Added `ext_storage_spill_pool_active` flag; when set, `evictionPoolPopulate` skips non-ONLY_MEMORY keys
**Result:** Memory stabilized at 1.10GB (was 1.40GB), 86% spill hit rate (was 11%)

### 3. Throttle Permanently at Maximum
**Symptom:** TPS capped at 50K regardless of available CPU/disk
**Root cause:** `spillover_limit = maxmemory * 1.1` — with tiering overhead, memory always exceeds this
**Fix:** Raised `THROTTLE_MIN_CMD_MAX_TPS` from 50K to 150K; spill threshold = maxmemory + 10%

### 4. hasSpillableItems Doing Expensive Sampling
**Symptom:** Main thread at 100% CPU with only 65K TPS (should be 130K+)
**Root cause:** `extStoragePerformEvictions` called `findBestEvictionCandidate` on EVERY write command
**Fix:** Replaced with O(1) counter check: `total_keys > num_items_on_flash`

### 5. Tiering State Hashtable Overhead
**Symptom:** 5 mallocs + 3 key copies per spill; ~68B per tiered key
**Root cause:** Separate `keys_tiering_state` hashtable with `tieringStateEntry` per key
**Fix:** Packed tiering state into 3-bit robj bitfield (refcount: 29→26 bits). Eliminated entire hashtable.

### 6. Uninitialized tiering_state Bitfield (CRITICAL BUG)
**Symptom:** Clients permanently blocked on keys with state=4 (PENDING_EVICT) that were never spilled
**Root cause:** New `tiering_state` field in robj not initialized to 0; jemalloc doesn't zero memory
**Fix:** Added `o->tiering_state = 0` in both `createObject()` and `createEmbeddedStringObject()`

### 7. Bridge Key Copy Elimination
**Symptom:** Extra sdsdup per spill in bridge submitPut
**Fix:** Borrow key sds from robj (stable while in COPYING_TO_FLASH); added `key_owned` flag to bridgeRequestCtx

## Current Performance (ezbench r6gd.2xlarge)
- **81K TPS** in RUN phase (80/20 GET/SET, 200 clients, 10M keys, 1GB maxmem)
- Main thread: 100% CPU (bottleneck)
- IO thread: 50% CPU
- NVMe: 28K read IOPS, 0.08ms latency, 43% util (not bottleneck)
- Memory: 2.28GB (2.28x maxmem) — churn from always-promote
- Zero blocked clients, zero crashes

## Remaining Bottlenecks (Next Steps)
1. **Blind-write for SET** — eliminate fetch before overwrite of flash keys (~22% CPU savings)
2. **kvstoreHashtableFind overhead** — extStorageGetState does full HT lookup for 3 bits; pass entry pointer where available
3. **Spill/fetch churn** — always-promote causes 28M fetches that get re-spilled; lazy promotion would halve IO
4. **PENDING_EVICT state** — still exists but rarely triggers; consider removing entirely

## Key Learnings
- `break` vs `continue` in spill loop: `break` is correct (try next tick), `continue` causes stalls
- Eviction pool entries are removed on return by `findBestEvictionCandidate` — pool self-cleans
- robj bitfields MUST be initialized — jemalloc doesn't zero memory
- `kvstoreHashtableFind(db->keys, slot, key)` works with sds keys (dictSdsKeyCompare)
- dt-poc uses dedicated LRU linked list (O(1) pop) — we use random sampling (more expensive but no extra memory)
- Spill threshold should be maxmemory+10% (not maxmemory) to avoid over-spilling from in-flight copies

## Benchmark Framework — ezbench Integration (2026-05-31)

### Usage
```bash
./benchmark.sh --ezbench --config flashcache --tag <name> <SCENARIO>
```

### How it works
- Generates ezbench package from scenario config at /tmp/ezbench-${TAG}/
- Provisions r6gd.2xlarge (NVMe) + 10 client machines via CloudFormation
- Deploys binaries + scenario scripts
- Runs warmup (populate) then benchmark workload
- Pushes metrics to CloudWatch namespace `valkey-bench-${TAG}`
- Uploads live HTML reports to S3 every 30s
- Tears down on completion (keep_stack=no)

### Key details
- Uses `--no-auth` flag (kinit broken on dev desktop, mwinit works)
- ext-storage-path overridden to `/dev/nvme1n1` (raw NVMe block device)
- ext-storage-capacity-mb set to 406937 (full NVMe capacity)
- ezbench can't parse `--csv` output — removed from client scripts
- CloudWatch namespace per test: `valkey-bench-${TAG}`
- S3 reports: `s3://ezbench-833348497722/reports/<hostname>-<timestamp>/`

### Results achieved
- W3 (Mixed R/W, 32MB, 500K keys): 85K TPS
- W1 (Cold-Start, 32MB): 81K TPS
- Full cluster-sim (1GB, 10M keys): 85.8K TPS
- All scenarios: zero stuck clients, stable operation
