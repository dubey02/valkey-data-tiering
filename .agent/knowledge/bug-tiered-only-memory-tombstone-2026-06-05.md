# BUG: Tiered fetch transient miss → KEY_STATE_BUG storm + throughput collapse

Status: OPEN. Root cause identified (corrected 2026-06-06 via per-key trace). Fix NOT applied.

> NOTE (history): An earlier 2026-06-05 version of this doc blamed FlashCache
> *capacity eviction* and proposed `dbDelete`-ing the key on miss. That was WRONG —
> the value is NOT lost (the retry recovers it), so dbDelete would destroy live data.
> Corrected below from a 1M-ops per-key state trace.

## Symptom
Server spam-logs `KEY_STATE_BUG: key <k> has TIERED encoding but state=0 (expected
ONLY_FLASH=2)` and throughput collapses (at the extreme, ~0 rps — looks like a hang).
Server stays alive; not a crash; no data loss.

## Impact
- W4 zipfian, 16KB values, 200 clients, maxmemory=128mb (working set ~918MB): 52,593
  spurious misses. At 16KB/400 clients: 161,098 misses, throughput → ~0 (apparent hang).
- One `KEY_STATE_BUG` WARNING per miss (tens–hundreds of thousands of lines).
- Latent correctness hazard: the miss path inserts a PRESENT key into keys_confirmed_absent.

## Reproduce
```
benchmark/scenarios/mixed-rw-zipfian/configs/flashcache-16k-200-128.env
  MAXMEMORY=128mb DATASIZE=16336 CLIENTS=200 KEYSPACE=500000 OPS=1000000
  ext-storage-backend=flashcache, capacity=16192mb (flash NOT capacity-constrained)
./benchmark.sh --remote --config flashcache-16k-200-128 W4
```
Churn-gated: 200K ops → 0 misses; 600K → 0; 1M → ~52K (per-transition trace overhead
raises the threshold; non-trace 1M hits 52K). Does NOT occur when maxmemory ≥ working
set (no spilling) or at low churn (W3 512mb → 0).

## Evidence (per-key state trace, key …000616; states 1=COPYING_TO_FLASH 2=ONLY_FLASH 3=COPYING_TO_MEMORY)
~9 healthy cycles, then one bad cycle:
```
242502 SETSTATE ->1 tiered=0     spill submit
242542 SETSTATE ->2 tiered=1     WRITE-OK  (value PUT to flash, confirmed)
255811 SETSTATE ->3 tiered=1     fetch submit
255830 REMOVESTATE  tiered=1     READ-MISS  ← fetch returned NULL (value still on flash)
256125 KEY_STATE_BUG: TIERED encoding but state=0
256126 SETSTATE ->2 tiered=1     guard "fix" → ONLY_FLASH
256127 SETSTATE ->3 tiered=1     re-fetch
256501 REMOVESTATE  tiered=0     retry SUCCEEDS (value was there all along)
```
Same key, same flash value (PUT at 242542; retry finds it at 256501) → the intervening
fetch missed TRANSIENTLY. Ruled out by counters: FC capacity eviction
(completion_delete_ok=0, eviction callback never fired — GC was compaction-only),
engine eviction (evicted_keys=0), completion-ring drops (submit==complete exactly,
in-flight gauges + blocked_clients drain to 0). Reproduced on a 16GB flash at ~5%
utilization → NOT capacity-driven.

## Root cause (two parts)

### 1. FlashCache GET vs GC-compaction race (true root cause)
FC GET is async: it captures the key's log offset from the index, then reads it. FC log
*compaction* (constant under churn — forward-copies live items; NOT eviction) moves the
item to a new offset and recycles the old one. A GET whose read captures the pre-move
offset gets stale/empty data → spurious NOT_FOUND, even though the key is alive at its
new offset. Scales with compaction activity = re-spill churn.
(FlashCache/src/FlashCache/src/log.c: logIteratorCoreLogicProcessingGarbageCollectionCallback,
compaction branch — invalidateIndexEntry then re-insert at head.)

### 2. Engine treats the transient miss as permanent absence (the amplifier)
processCompletedStorageRequests READ-completion, miss branch (new_value == NULL),
src/ext_storage.c ~731-737:
```c
} else {
    completion_read_miss++;
    if (num_items_on_flash > 0) num_items_on_flash--;
    hashtableAdd(db->keys_confirmed_absent, sdsdup(key_name)); // (!) marks a PRESENT key absent
    extStorageRemoveState(db, key_name);   // → ONLY_MEMORY, but entry encoding stays TIERED
}
```
It clears state to ONLY_MEMORY without restoring the value and without clearing the
TIERED encoding → the TIERED+ONLY_MEMORY desync. encoding=OBJ_ENCODING_TIERED is set in
exactly ONE place — spill WRITE-OK (ext_storage.c:660), always paired with ONLY_FLASH
(:663) — so only this miss path creates the desync.

The guard at ext_storage.c:411 (keyBlocksClient) then detects TIERED+ONLY_MEMORY, logs
KEY_STATE_BUG, flips state→ONLY_FLASH, and the client's re-dispatched command issues a
NEW fetch (which succeeds). So it self-heals — but via a noisy path with three costs per
miss: (a) KEY_STATE_BUG log spam, (b) keys_confirmed_absent pollution with a present key,
(c) a full unblock→reprocess→re-block→re-fetch cycle. At 52K+ misses the (a)+(c) overhead
is a large fraction of the throughput collapse.

## Proposed fix
- Engine (immediate): in the READ-miss branch, if the entry encoding is still
  OBJ_ENCODING_TIERED, treat it as a RETRYABLE TRANSIENT miss — re-submit the fetch
  directly (stay in COPYING_TO_MEMORY, keep the client blocked), with NO KEY_STATE_BUG,
  NO keys_confirmed_absent insert, NO guard reprocess cycle. Only genuinely-absent keys
  (encoding not TIERED) should be marked absent. Do NOT dbDelete — the value is on flash.
- FlashCache (true fix): close the GET-vs-GC offset race — re-validate the index entry
  after the async read, or don't recycle a log offset with an in-flight read.

## Notes
- Review keys_confirmed_absent: inserting a present key is a latent correctness hazard
  (a later GET could short-circuit to nil); also verify it's cleared on successful fetch.
- The KEY_STATE_BUG line is the only emitted symptom; the seed read-miss is otherwise silent.

## Repro configs (uncommitted)
- scenarios/mixed-rw-zipfian/configs/flashcache-16k-200-128.env  (isolates race; 16GB flash)
- scenarios/mixed-rw-zipfian/configs/flashcache-16k-400.env / flashcache-8k-400.env (severity vs value size)
- Trace was captured with debug instrumentation (ext_storage_trace_enabled) in
  extStorageSetState/extStorageRemoveState — revert before committing.
- Results: benchmark/results/{w4-16k-200-128, w4-trace1m, w4-16k-400, w4-8k-400}/


tracing used:

/ Track consecutive spill failures to detect backend saturation
static long long consecutive_spill_failures = 0;
#define SPILL_FAILURE_THRESHOLD 5  /* After 5 consecutive failures, declare unable to spill */

// Metrics
long long total_items_spilled_to_ext_storage = 0;
long long total_items_fetched_from_ext_storage = 0;
long long num_items_on_flash = 0; /* current items residing on flash (not in-flight) */
int ext_storage_trace_enabled = 1; /* DEBUG: per-key state-transition trace (revert after) */long long total_items_spilling_to_ext_storage = 0;
long long total_items_fetching_from_ext_storage = 0;
long long total_items_deleted_from_ext_storage = 0;

/* Flag: when set, evictionPoolPopulate skips non-ONLY_MEMORY keys */
int ext_storage_spill_pool_active = 0;

// Memory pressure metrics (non-static: accessed from evict.c)
long long oom_reject_write_count = 0;
long long spill_attempts = 0;          /* total calls to findBestEvictionCandidate for spilling */
long long spill_skipped_non_spillable = 0; /* candidate was not in ONLY_MEMORY state */
long long spill_skipped_null = 0;      /* findBestEvictionCandidate returned NULL */
long long spill_submitted = 0;         /* successfully submitted to IO thread */
long long oom_reject_read_count = 0;
long long memory_hard_cap_exceeded_count = 0;

// State variables
static struct evictionPoolEntry *spillPoolLRU = NULL;
static ValkeyModuleExternalStorageMsg **completed_storage_requests = NULL;

// Defined in evict.c
int evictionPoolPopulate(serverDb *db, kvstore *samplekvs, struct evictionPoolEntry *pool);
sds findBestEvictionCandidate(struct evictionPoolEntry *pool, int *bestdbid, int *bestslot);

/* ---------------------------------------------------------------------------
 * Tiering State Machine
 *
 * State is stored directly in robj->tiering_state (3 bits in the serverObject).
 * No separate hashtable needed — eliminates ~68B per tiered key overhead.
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Tiering State Machine — Public API (robj bitfield implementation)
 *
 * State is stored directly in the robj's tiering_state bitfield (3 bits).
 * No separate hashtable needed — O(1) read/write from the entry pointer.
 * ---------------------------------------------------------------------------*/

/**
 * Get the tiering state for a key. Looks up the entry in db->keys.
 * Returns ONLY_MEMORY if key not found (shouldn't happen in normal flow).
 */
TieringState extStorageGetState(serverDb *db, sds key) {
    void *entry = NULL;
    if (kvstoreHashtableFind(db->keys, getKVStoreIndexForKey(key), key, &entry)) {
        return (TieringState)((robj *)entry)->tiering_state;
    }
    return TIERING_STATE_ONLY_MEMORY;
}

/**
 * Set the tiering state for a key directly in the robj bitfield.
 */
void extStorageSetState(serverDb *db, sds key, TieringState state, int inflight_op) {
    (void)inflight_op;
    void *entry = NULL;
    if (kvstoreHashtableFind(db->keys, getKVStoreIndexForKey(key), key, &entry)) {
        if (ext_storage_trace_enabled)
            serverLog(LL_WARNING, "TRC SETSTATE %s -> %d tiered=%d", key, (int)state,
                      (((robj *)entry)->encoding == OBJ_ENCODING_TIERED));
        ((robj *)entry)->tiering_state = state;
    } else {
    }
}

/**
 * Remove the tiering state (set to ONLY_MEMORY).
 */
void extStorageRemoveState(serverDb *db, sds key) {
    void *entry = NULL;
    if (kvstoreHashtableFind(db->keys, getKVStoreIndexForKey(key), key, &entry)) {
        if (ext_storage_trace_enabled)
            serverLog(LL_WARNING, "TRC REMOVESTATE %s ->ONLY_MEMORY tiered=%d", key,
                      (((robj *)entry)->encoding == OBJ_ENCODING_TIERED));
        ((robj *)entry)->tiering_state = TIERING_STATE_ONLY_MEMORY;
    }
}
