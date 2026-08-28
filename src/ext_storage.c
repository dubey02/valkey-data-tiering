/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Tiering State Machine (v3) — Non-Key-Spilling
 *
 * States: ONLY_MEMORY, COPYING_TO_FLASH, ONLY_FLASH, COPYING_TO_MEMORY, PENDING_EVICT
 * See ext_storage.h for the TieringState enum and tieringStateEntry struct.
 */

#include "ext_storage.h"
#include "ext_storage_bridge.h"
#include "server.h"
#include "module.h"
#include "rdb.h"
#include "rio.h"
#include "ae.h"
#include "ext_storage_throttle.h"
#include <stdbool.h>
#include <unistd.h>
#include <stdatomic.h>

const int COMPLETED_STORAGE_REQUESTS_PROCESSING_BATCH_SIZE = 256;

/* In-flight spilled RAM bytes. Incremented on the IO thread when a value is
 * serialized (its in-RAM footprint is computed there, off the main thread), and
 * decremented on the main thread when the spill completion is processed. The
 * footprint is carried on the completion msg so add and subtract use the same
 * number (no drift). Read on the main thread to form "projected" memory. */
static _Atomic size_t inflight_spill_ram_bytes = 0;   /* window 2 (serialize..completion): EXACT */

/* ---- Two-stage Smith predictor for spill dead-time ----
 * Window 1 = [submit .. serialize]: the value's footprint is NOT yet known (no
 *   serialize walk has run), so it is MODELED as submit_depth * mean_spill_ram,
 *   where submit_depth is an exact integer count.
 * Window 2 = [serialize .. completion]: footprint IS known (computed during the
 *   IO-thread serialize walk), so it is EXACT, held in inflight_spill_ram_bytes.
 * An item migrates window-1 -> window-2 automatically at serialize: serialized_count++
 *   shrinks submit_depth and inflight_spill_ram_bytes += exact grows window 2. Each
 *   counter is single-writer monotonic, so plain relaxed atomics suffice. */
static _Atomic long long spill_submitted_count  = 0;  /* ++ on MAIN thread at submit    */
static _Atomic long long spill_serialized_count = 0;  /* ++ on IO thread at serialize   */
static _Atomic size_t    mean_spill_ram         = 0;  /* EMA of exact footprint (IO writes, main reads) */

void extStorageInflightAddRam(size_t bytes) {
    atomic_fetch_add_explicit(&inflight_spill_ram_bytes, bytes, memory_order_relaxed);
}

/* MAIN thread: a spill was just submitted to the IO thread. Counting it here makes
 * projected drop in real time inside the fill loop, braking the within-loop burst. */
void extStorageOnSpillSubmit(void) {
    atomic_fetch_add_explicit(&spill_submitted_count, 1, memory_order_relaxed);
}

/* IO thread: a spilled value has been serialized and its exact in-RAM footprint is
 * now known. Move it out of window 1 (serialized_count++) and fold the exact size
 * into the EMA that models window 1. The EMA initializes to the first real sample
 * (not seeded) so it is accurate after the first serialize; cold start tolerates a
 * brief burst that self-limits the instant mean_spill_ram becomes non-zero. */
void extStorageOnSpillSerialize(size_t exact_bytes) {
    atomic_fetch_add_explicit(&spill_serialized_count, 1, memory_order_relaxed);
    long long m = (long long)atomic_load_explicit(&mean_spill_ram, memory_order_relaxed);
    if (m == 0) {
        m = (long long)exact_bytes;                  /* init EMA on first real sample */
    } else {
        m += ((long long)exact_bytes - m) >> 4;      /* EMA, alpha = 1/16 */
        if (m < 0) m = 0;
    }
    atomic_store_explicit(&mean_spill_ram, (size_t)m, memory_order_relaxed);
}

/* Projected memory = used_memory minus the spill actuation already committed but not
 * yet freed, across BOTH dead-time windows (two-stage Smith predictor). The spill
 * controller gates on THIS so it self-terminates the moment it has committed enough
 * drain — no fixed concurrency cap. The throttle, by contrast, reads RAW used_memory
 * (see ext_storage_throttle.c): with this loop pinning projected at maxmemory, the
 * gap used_memory - projected IS the in-flight backlog / disk-saturation signal. */
size_t extStorageProjectedMemory(void) {
    size_t used = zmalloc_used_memory();
    size_t inflight = atomic_load_explicit(&inflight_spill_ram_bytes, memory_order_relaxed); /* window 2 exact */
    long long depth = atomic_load_explicit(&spill_submitted_count,  memory_order_relaxed) -
                      atomic_load_explicit(&spill_serialized_count, memory_order_relaxed);
    if (depth < 0) depth = 0;
    size_t mean = atomic_load_explicit(&mean_spill_ram, memory_order_relaxed);
    size_t credit = inflight + (size_t)depth * mean;                                         /* + window 1 modeled */
    return (used > credit) ? (used - credit) : 0;
}

// Timer event ID for periodic completion processing
static long long ext_storage_timer_id = AE_ERR;

// Configuration parameters
int ext_data_enabled = 0;
int items_spillover_batch_size = 10;
char *ext_storage_backend = NULL;
char *ext_storage_path = NULL;
long long ext_storage_capacity_mb = 1024;
long long ext_storage_max_spill_size = 128 * 1024 * 1024; /* 128MB — skip items larger than this */
/* FlashCache tuning */
long long ext_storage_index_size = 1048576;           /* 1M entries per DB */
int ext_storage_max_allocated_percent = 90;           /* GC starts at 90% full */
int ext_storage_max_in_flight_reads = 128;            /* max concurrent reads */
long long ext_storage_min_gc_rate = 4096;             /* 4KB/s min GC */
long long ext_storage_max_gc_rate = 30 * 1024 * 1024; /* 30MB/s max GC */
long long ext_storage_max_buffered_write_size = 4 * 1024 * 1024; /* 4MB staging buffer */
long long ext_storage_buffered_write_flush_threshold = 1024 * 1024; /* 1MB flush threshold */

/* Strategy selectors (A/B benchmark switches; backed by MODIFIABLE enum configs).
 * Defaults preserve the current HEAD behavior (decoupled throttle + projected spill). */
int ext_storage_throttling_strategy = THROTTLING_STRATEGY_V2;
int ext_storage_spilling_strategy   = SPILLING_STRATEGY_V2;
int ext_storage_throttle_band_start = 100; /* 1.0x maxmemory */
int ext_storage_throttle_band_end   = 120; /* 1.2x maxmemory */

/* Dynamic spill concurrency cap — used ONLY by the legacy ITEM_COUNT spill strategy.
 * The PROJECTED (Smith-predictor) strategy is cap-less and ignores this entirely.
 * Under COUPLED throttling, extStorageUpdateSpillConcurrency() ramps the cap with
 * memory pressure; under DECOUPLED throttling it is never updated (stays at BASE). */
#define SPILL_CONCURRENT_BASE  50
#define SPILL_CONCURRENT_LIMIT 200
static int max_num_concurrent_items_spilled = SPILL_CONCURRENT_BASE;

// Spillover margin: memory zone above maxmemory where throttling applies
// dt-poc default: 10% of maxmemory
#define SPILLOVER_MARGIN_PERCENT 10

// Track consecutive spill failures to detect backend saturation
static long long consecutive_spill_failures = 0;
#define SPILL_FAILURE_THRESHOLD 5  /* After 5 consecutive failures, declare unable to spill */

// Metrics
long long total_items_spilled_to_ext_storage = 0;
long long total_items_fetched_from_ext_storage = 0;
long long num_items_on_flash = 0; /* current items residing on flash (not in-flight) */
long long total_items_spilling_to_ext_storage = 0;
long long total_items_fetching_from_ext_storage = 0;
long long total_items_deleted_from_ext_storage = 0;

/* Flag: when set, evictionPoolPopulate skips non-ONLY_MEMORY keys */
int ext_storage_spill_pool_active = 0;

// Memory pressure metrics (non-static: accessed from evict.c)
long long oom_reject_write_count = 0;
long long no_spillable_items_count = 0; /* times eviction found no spillable items (metric only, no action taken) */
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
        ((robj *)entry)->tiering_state = TIERING_STATE_ONLY_MEMORY;
    }
}

/* ---------------------------------------------------------------------------
 * Metrics — keyBlocksClient decision path
 * ---------------------------------------------------------------------------*/
static long long kbc_in_memory_count = 0;
static long long kbc_spilling_block_count = 0;
static long long kbc_fetching_block_count = 0;
static long long kbc_pending_evict_block_count = 0;
static long long kbc_pending_deletion_block_count = 0; /* blocked because a DEL is draining */

/* ---- Synchronous fetch (mid-execution) ---- */
static list *deferred_completions = NULL; /* other keys' completions polled out during a sync fetch */
static long long sync_fetch_count = 0;
static long long sync_fetch_miss_count = 0;
static long long sync_fetch_wait_us_total = 0;
static long long sync_fetch_wait_us_max = 0;
static long long sync_fetch_deferred_count = 0;
static long long kbc_confirmed_absent_count = 0;
static long long kbc_key_may_exist_false_count = 0;
static long long kbc_key_may_exist_true_count = 0;
static long long kbc_total_calls = 0;

/* DRAM-hit metric denominator: count of key accesses (read OR write) ALLOWED by
 * keyBlocksClient with the value resident in DRAM. A fetched request crosses the
 * gate twice — once as a BLOCK (issues the fetch) and once as a resident ALLOW
 * after promotion — so this counts each fetched request exactly once (its re-exec)
 * plus every immediate DRAM hit. True misses are excluded (see kbc_confirmed_absent).
 * DRAM_hit% = (Δdram_value_hits − Δcompletion_read_ok) / Δdram_value_hits. */
static long long dram_value_hits = 0;

/* ---------------------------------------------------------------------------
 * Completion metrics
 * ---------------------------------------------------------------------------*/
static long long completion_batches_processed = 0;
static long long completion_read_ok = 0;
static long long completion_read_miss = 0;
static long long completion_read_retry = 0;  /* transient read rejections re-issued (not misses) */
static long long completion_write_ok = 0;
static long long completion_write_fail = 0;
static long long completion_delete_ok = 0;
static long long completion_pending_evict_count = 0;


/* ---------------------------------------------------------------------------
 * Serialization callbacks (unchanged from original)
 * ---------------------------------------------------------------------------*/

int extStorageSerializeKey(void *key, char **serialized_key) {
    robj *key_obj = (robj *)key;
    sds key_sds = (sds)objectGetVal(key_obj);
    *serialized_key = key_sds;
    return sdslen(key_sds);
}

/* Serialize any object type (string, list, set, hash, zset, stream) into the
 * RDB DUMP format: <type byte><rdb object><2B version><8B CRC64>. This is the
 * same encoding used by DUMP/RESTORE, so every encoding is handled correctly.
 * Returns a heap-allocated sds buffer that must be released with
 * extStorageFreeSerializedValue. */
int extStorageSerializeValue(void *value, char **serialized_value) {
    rio payload;
    createDumpPayload(&payload, (robj *)value, NULL, -1);
    int len = (int)sdslen(payload.io.buffer.ptr);
    /* Check max spill size — abort if serialized value exceeds threshold.
     * This runs on the IO thread (exact size, post-serialization). */
    if (ext_storage_max_spill_size > 0 && (long long)len > ext_storage_max_spill_size) {
        sdsfree(payload.io.buffer.ptr);
        *serialized_value = NULL;
        return -1;
    }
    *serialized_value = payload.io.buffer.ptr;
    return len;
}

void *extStorageDeserializeKey(char *key, int length) {
    return (void *)sdsnewlen(key, length);
}

/* Reconstruct an robj from RDB DUMP-format bytes produced by
 * extStorageSerializeValue. Returns NULL on a malformed payload. */
void *extStorageDeserializeValue(char *value, int length) {
    rio payload;
    sds buf = sdsnewlen(value, length);
    rioInitWithBuffer(&payload, buf);
    int type = rdbLoadType(&payload);
    robj *value_obj = (type == -1)
        ? NULL
        : rdbLoadObject(type, &payload, NULL, -1, NULL, RDBFLAGS_NONE, 0);
    sdsfree(buf);
    return (void *)value_obj;
}

void extStorageFreeSerializedKey(void *key) {
    UNUSED(key);
}

void extStorageFreeSerializedValue(void *value) {
    if (value) sdsfree((sds)value);
}

/* ---------------------------------------------------------------------------
 * Timer callback
 * ---------------------------------------------------------------------------*/

static long long extStorageTimerCallback(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    processCompletedStorageRequestsAndSpillOldItems();
    return 1;
}

/* ---------------------------------------------------------------------------
 * SWAPDB support: db-id indirection
 *
 * Flash records and in-flight IO messages are addressed by a PHYSICAL db id
 * that follows a keyspace across SWAPDB. The server.db[] index used in
 * command context is the LOGICAL id. Identity-mapped at init; SWAPDB swaps
 * the two mapping entries, so:
 *   - new submissions for a logical db reach the flash namespace its
 *     keyspace has always used, and
 *   - completions tagged with a physical id route back to whichever logical
 *     db currently owns that keyspace (correct even for IO in flight across
 *     the swap, because dbSwapDatabases moves the entries - and their
 *     tiering-state bits - together with the keyspace).
 * ---------------------------------------------------------------------------*/
static int *logical_to_physical_db = NULL;
static int *physical_to_logical_db = NULL;

/* True once extStorage_init completed successfully (post config load). */
int extStorageIsInitialized(void) {
    return logical_to_physical_db != NULL;
}

int extStoragePhysicalDbId(int logical_id) {
    if (!logical_to_physical_db) return logical_id;
    return logical_to_physical_db[logical_id];
}

int extStorageLogicalDbId(int physical_id) {
    if (!physical_to_logical_db) return physical_id;
    return physical_to_logical_db[physical_id];
}

void extStorageSwapDbIds(int id1, int id2) {
    if (!logical_to_physical_db) return;
    int p1 = logical_to_physical_db[id1];
    int p2 = logical_to_physical_db[id2];
    logical_to_physical_db[id1] = p2;
    logical_to_physical_db[id2] = p1;
    physical_to_logical_db[p1] = id2;
    physical_to_logical_db[p2] = id1;
}

/* ---------------------------------------------------------------------------
 * Initialization
 * ---------------------------------------------------------------------------*/

void extStorage_init(void) {
    if (!ext_data_enabled) return;

    /* Validate maxmemory-policy: only allkeys-lru, allkeys-lfu, and noeviction
     * are supported with tiering. volatile-* requires engine-driven eviction
     * from flash (sampling from expires dict for ONLY_FLASH keys) — not yet implemented.
     * TODO: Implement volatile-lru/lfu via engine-driven flash eviction (Option B). */
    if (server.maxmemory_policy != MAXMEMORY_ALLKEYS_LRU &&
        server.maxmemory_policy != MAXMEMORY_ALLKEYS_LFU &&
        server.maxmemory_policy != MAXMEMORY_NO_EVICTION) {
        serverLog(LL_WARNING,
            "ext-storage-enabled requires maxmemory-policy allkeys-lru, allkeys-lfu, or noeviction. "
            "volatile-* policies are not yet supported with tiering. Disabling tiering.");
        ext_data_enabled = 0;
        return;
    }

    serverLog(LL_NOTICE, "Initializing the external storage (state machine v3)...");

    /* Identity-map the SWAPDB db-id indirection. */
    logical_to_physical_db = zmalloc(sizeof(int) * server.dbnum);
    physical_to_logical_db = zmalloc(sizeof(int) * server.dbnum);
    for (int i = 0; i < server.dbnum; i++) {
        logical_to_physical_db[i] = i;
        physical_to_logical_db[i] = i;
    }

    /* Initialize the pluggable storage backend */
    const char *backend = "flashcache"; /* Default: real FlashCache */
    if (ext_storage_backend && ext_storage_backend[0]) backend = ext_storage_backend;
    const char *path = (ext_storage_path && ext_storage_path[0]) ? ext_storage_path : "/tmp/valkey-flash.db";
    size_t capacity = (size_t)ext_storage_capacity_mb * 1024 * 1024;
    if (extStorageBridge_init(backend, path, capacity) != 0) {
        serverLog(LL_WARNING, "Failed to initialize storage backend '%s', disabling tiering", backend);
        ext_data_enabled = 0;
        return;
    }

    completed_storage_requests = zcalloc(COMPLETED_STORAGE_REQUESTS_PROCESSING_BATCH_SIZE * sizeof(ValkeyModuleExternalStorageMsg*));
    spillPoolLRU = zcalloc(sizeof(struct evictionPoolEntry) * EVPOOL_SIZE);
    for (int j = 0; j < EVPOOL_SIZE; j++) {
        spillPoolLRU[j].cached = sdsnewlen(NULL, EVPOOL_CACHED_SDS_SIZE);
    }

    extStorageThrottle_init();

    ext_storage_timer_id = aeCreateTimeEvent(server.el, 1, extStorageTimerCallback, NULL, NULL);
    if (ext_storage_timer_id == AE_ERR) {
        serverLog(LL_WARNING, "ext_storage: Failed to create timer event");
    }
}

/* ---------------------------------------------------------------------------
 * Helper: create a storage message
 * ---------------------------------------------------------------------------*/

/* createStorageMessage — no longer needed, bridge handles message creation.
 * Kept commented for reference during transition. */
#if 0
static ValkeyModuleExternalStorageMsg *createStorageMessage(int type, int db_id, robj *key, robj *value, long long expireMs) {
    serverAssert(key);
    ValkeyModuleExternalStorageMsg *msg = (ValkeyModuleExternalStorageMsg*)zmalloc(sizeof(ValkeyModuleExternalStorageMsg));
    msg->msg_type = type;
    msg->status = 0;
    msg->ttl = expireMs;
    msg->db_id = db_id;
    msg->key = key;
    msg->value = value;
    return msg;
}
#endif

/* ---------------------------------------------------------------------------
 * Helper: check if dbEntry is embedded
 * ---------------------------------------------------------------------------*/

static bool isEmbeddedObject(dbEntry *o) {
    return (o->encoding == OBJ_ENCODING_EMBSTR || o->encoding == OBJ_ENCODING_INT || o->hasembval);
}

/* ---------------------------------------------------------------------------
 * keyBlocksClient — State machine aware blocking decision
 *
 * Rules (matching dt-poc / v3 design):
 *   ONLY_MEMORY:        GET=allow, SET/DEL=allow
 *   COPYING_TO_FLASH:   GET=allow (value still in RAM), SET/DEL=BLOCK
 *   ONLY_FLASH:         GET=BLOCK+fetch, SET=BLOCK+fetch, DEL=BLOCK+delete
 *   COPYING_TO_MEMORY:  GET/SET/DEL=BLOCK (wait for in-flight op)
 *   PENDING_EVICT:      GET/SET/DEL=BLOCK (wait for in-flight op)
 *
 * Returns 1 if the key blocks the client, 0 otherwise.
 * Side effect: may issue a READ or DELETE request to the module.
 * ---------------------------------------------------------------------------*/

static int keyBlocksClient(serverDb *db, sds key, bool is_write_cmd, bool is_delete_cmd) {
    kbc_total_calls++;

    TieringState state = extStorageGetState(db, key);

    switch (state) {
    case TIERING_STATE_COPYING_TO_FLASH:
        /* Value is still in RAM during spill. Reads can proceed. Writes must wait. */
        if (is_write_cmd) {
            kbc_spilling_block_count++;
            return 1;
        }
        /* Verify the value is actually still in RAM (not already completed) */
        {
            dbEntry *cf_entry = dbFind(db, key);
            if (cf_entry && objectIsTiered(cf_entry)) {
                /* State says COPYING_TO_FLASH but encoding is TIERED — completion
                 * was processed but state not updated. Fix and block. */
                serverLog(LL_WARNING, "KBC_RACE: key %s state=COPYING_TO_FLASH but encoding=TIERED! Fixing.", key);
                extStorageSetState(db, key, TIERING_STATE_ONLY_FLASH, 0);
                kbc_key_may_exist_true_count++;
                return 1;
            }
        }
        /* GET on COPYING_TO_FLASH: serve from RAM (value still there) */
        kbc_in_memory_count++;
        dram_value_hits++;  /* resident DRAM hit: read served while value still in RAM */
        return 0;

    case TIERING_STATE_COPYING_TO_MEMORY:
        /* Fetch or delete in-flight — block all commands on this key */
        kbc_fetching_block_count++;
        return 1;

    case TIERING_STATE_PENDING_EVICT:
        /* Eviction pending — block all commands */
        kbc_pending_evict_block_count++;
        return 1;

    case TIERING_STATE_PENDING_DELETION: {
        /* Flash copy already deleted on behalf of a client DEL (internal-TS
         * style optimized delete). The entry is retained so the re-executed
         * DEL performs the keyspace removal itself with full command-layer
         * side effects: reply count, signalModifiedKey (WATCH), keyspace
         * "del" notification, and dirty++. DEL/UNLINK pass through; all
         * other commands wait until the DEL drains (the db.c delete hook
         * unblocks them). */
        if (is_delete_cmd) return 0;
        robj pd_keyobj;
        initStaticStringObject(pd_keyobj, key);
        if (!blockedInUseClientWithPendingDeleteExists(&pd_keyobj)) {
            /* Orphaned: the deleting client vanished before re-executing.
             * Finish the deletion inline with the command-layer side effects
             * it would have produced, then let this command proceed against
             * the post-delete keyspace. dbDelete's pending-deletion hook
             * unblocks any other waiters. */
            if (dbDelete(db, &pd_keyobj)) {
                signalModifiedKey(NULL, db, &pd_keyobj);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", &pd_keyobj, db->id);
                server.dirty++;
            }
            return 0;
        }
        kbc_pending_deletion_block_count++;
        return 1;
    }

    case TIERING_STATE_ONLY_FLASH:
        /* Value on disk — need to fetch (for GET/SET) or delete (for DEL/eviction) */
        kbc_key_may_exist_true_count++;
        serverLog(LL_DEBUG, "KBC: key blocked due to ONLY_FLASH state");
        return 1;

    case TIERING_STATE_ONLY_MEMORY:
    default:
        break;
    }

    /* State is ONLY_MEMORY (not in HT). Check if key is actually in dict. */
    dbEntry *entry = dbFind(db, key);
    if (entry != NULL) {
        if (!objectIsTiered(entry)) {
            /* Key in memory with valid value — no blocking */
            kbc_in_memory_count++;
            dram_value_hits++;  /* resident DRAM hit: value in DRAM (read, write, or fetched re-exec) */
            return 0;
        }
        /* Key has TIERED encoding — value is on flash. Must block and fetch.
         * This is a safety net — should have been caught by state check above.
         * Log this as it indicates a state inconsistency. */
        serverLog(LL_WARNING, "KEY_STATE_BUG: key %s has TIERED encoding but state=%d (expected ONLY_FLASH=2)", key, (int)state);
        /* Fix the state to match reality */
        extStorageSetState(db, key, TIERING_STATE_ONLY_FLASH, 0);
        kbc_key_may_exist_true_count++;
        return 1;
    }

    /* Key not in dict — it doesn't exist (TRUE MISS). Non-key-spilling keeps all
     * keys in the dict, so if it's not there, it's not on flash either. This is NOT
     * a DRAM hit: count it as confirmed-absent and EXCLUDE it from dram_value_hits
     * (the DRAM-hit denominator). Previously mis-attributed to kbc_in_memory_count. */
    kbc_confirmed_absent_count++;
    return 0;
}


/* ---------------------------------------------------------------------------
 * preCommandExec — Block client if key state requires it
 *
 * For keys in ONLY_FLASH: issues READ (for GET/SET) or DELETE (for DEL) to module.
 * For keys in COPYING_TO_FLASH/COPYING_TO_MEMORY/PENDING_EVICT: just blocks.
 * ---------------------------------------------------------------------------*/

int preCommandExec(client *c) {
    if (!ext_data_enabled) return CMD_FILTER_ACCEPT;

    /* NOTE: Completions are processed from beforeSleep() and the 1ms timer,
     * NOT here. Processing completions here causes a race: a spill completion
     * can mark a key TIERED, then keyBlocksClient misses it for a concurrent
     * client in the same event loop tick. dt-poc also processes completions
     * only from beforeSleep, not from preCommandExec. */

    /* Determine which keys block this client */
    serverDb *current_db = c->db;
    getKeysResult result;
    initGetKeysResult(&result);

    /* For MULTI/EXEC: check ALL keys from ALL queued commands (matching dt-poc's
     * determineBlockedKeysForMultiExec). A key may have been spilled between
     * MULTI and EXEC. */
    bool is_multi_exec = (c->flag.multi && c->cmd->proc == execCommand);

    if (is_multi_exec) {
        /* Iterate queued commands and check each key */
        int num_keys_to_block = 0;
        robj **blocking_keys = NULL;
        int max_keys = 0;

        /* Count total keys across all queued commands */
        for (int i = 0; i < c->mstate->count; i++) {
            max_keys += c->mstate->commands[i].argc;
        }
        blocking_keys = (robj **)zmalloc(sizeof(robj *) * (max_keys > 0 ? max_keys : 1));

        for (int i = 0; i < c->mstate->count; i++) {
            struct multiCmd *mc = &c->mstate->commands[i];
            getKeysResult mc_result;
            initGetKeysResult(&mc_result);
            int mc_num_keys = getKeysFromCommand(mc->cmd, mc->argv, mc->argc, &mc_result);
            keyReference *mc_keys = mc_result.keys;
            bool mc_is_write = mc->cmd->flags & CMD_WRITE;

            for (int j = 0; j < mc_num_keys; j++) {
                sds key_str = objectGetVal(mc->argv[mc_keys[j].pos]);
                if (keyBlocksClient(current_db, key_str, mc_is_write, false)) {
                    blocking_keys[num_keys_to_block++] = mc->argv[mc_keys[j].pos];

                    TieringState state = extStorageGetState(current_db, key_str);
                    if (state == TIERING_STATE_ONLY_FLASH ||
                        (state == TIERING_STATE_ONLY_MEMORY && dbFind(current_db, key_str) != NULL &&
                         objectIsTiered(dbFind(current_db, key_str)))) {
                        if (state == TIERING_STATE_ONLY_MEMORY) {
                            extStorageSetState(current_db, key_str, TIERING_STATE_ONLY_FLASH, 0);
                        }
                        /* Issue fetch for this key */
                        int rc = extStorageBridge_submitGet(extStoragePhysicalDbId(current_db->id), key_str);
                        if (rc == 0) {
                            extStorageSetState(current_db, key_str, TIERING_STATE_COPYING_TO_MEMORY,
                                VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ);
                            total_items_fetching_from_ext_storage++;
                        } else {
                            num_keys_to_block--;
                        }
                    }
                }
            }
            getKeysFreeResult(&mc_result);
        }

        if (num_keys_to_block > 0) {
            c->flag.pending_command = 1;
            blockClientInUseOnKeys(c, num_keys_to_block, blocking_keys);
        }
        zfree(blocking_keys);
        return num_keys_to_block > 0 ? CMD_FILTER_REJECT : CMD_FILTER_ACCEPT;
    }

    /* Normal (non-MULTI) command path */
    int num_keys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
    if (num_keys == 0) return CMD_FILTER_ACCEPT;

    keyReference *keys = result.keys;
    bool is_write_cmd = (c->cmd)->flags & CMD_WRITE;
    /* Detect DEL/UNLINK commands for optimized delete path */
    bool is_delete_cmd = (c->cmd->proc == delCommand || c->cmd->proc == unlinkCommand);

    int num_keys_to_block = 0;
    robj **blocking_keys = (robj **)zmalloc(sizeof(robj *) * num_keys);

    for (int i = 0; i < num_keys; i++) {
        sds key_str = objectGetVal(c->argv[keys[i].pos]);

        if (!keyBlocksClient(current_db, key_str, is_write_cmd, is_delete_cmd)) {
            continue;
        }

        /* This key will block the client */
        blocking_keys[num_keys_to_block++] = c->argv[keys[i].pos];

        TieringState state = extStorageGetState(current_db, key_str);

        if (state == TIERING_STATE_ONLY_FLASH ||
            (state == TIERING_STATE_ONLY_MEMORY && dbFind(current_db, key_str) != NULL &&
             objectIsTiered(dbFind(current_db, key_str)))) {
            /* Key is on flash — need to issue IO request.
             * This also handles the case where encoding is TIERED but state
             * entry is missing (defensive — set state to match reality). */
            if (state == TIERING_STATE_ONLY_MEMORY) {
                /* Fix inconsistency: encoding says TIERED but no state entry */
                extStorageSetState(current_db, key_str, TIERING_STATE_ONLY_FLASH, 0);
                state = TIERING_STATE_ONLY_FLASH;
            }

            int msg_type;
            if (is_delete_cmd) {
                msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE;
            } else {
                msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ;
            }

            /* Fix: check TTL before fetching from flash. If key is expired,
             * skip the read IO and issue a delete instead — avoids wasted fetch. */
            if (msg_type == VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ) {
                dbEntry *entry = dbFind(current_db, key_str);
                if (entry) {
                    /* Expire lives on the ENTRY robj. objectGetVal(entry) is the
                     * tiered placeholder sds — passing it to objectGetExpire
                     * reads garbage robj bitfields (heap-dependent), which could
                     * fabricate an "expired" verdict and silently convert this
                     * READ into a DELETE (flaky key loss). */
                    long long expire_ms = objectGetExpire(entry);
                    if (expire_ms > 0 && expire_ms < mstime() && !server.loading) {
                        msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE;
                        is_delete_cmd = true;  /* treat as delete for the rest of this iteration */
                    }
                }
            }

            int rc;
            if (is_delete_cmd) {
                rc = extStorageBridge_submitDel(extStoragePhysicalDbId(current_db->id), key_str);
            } else {
                rc = extStorageBridge_submitGet(extStoragePhysicalDbId(current_db->id), key_str);
            }

            if (rc != 0) {
                /* Backend rejected (throttled). Undo block for this key. */
                serverLog(LL_DEBUG, "Storage rejected %s for key %s (throttled)",
                    is_delete_cmd ? "DELETE" : "READ", key_str);
                hashtableAdd(current_db->keys_confirmed_absent, sdsdup(key_str));
                num_keys_to_block--;
            } else {
                /* Transition: ONLY_FLASH → COPYING_TO_MEMORY */
                extStorageSetState(current_db, key_str, TIERING_STATE_COPYING_TO_MEMORY, msg_type);
                if (msg_type == VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ) {
                    total_items_fetching_from_ext_storage++;
                }
            }
        }
        /* For COPYING_TO_FLASH, COPYING_TO_MEMORY, PENDING_EVICT:
         * just block — IO is already in-flight, no new request needed. */
    }

    getKeysFreeResult(&result);
    if (num_keys_to_block > 0) {
        c->flag.pending_command = 1;
        blockClientInUseOnKeys(c, num_keys_to_block, blocking_keys);
    }
    zfree(blocking_keys);
    return num_keys_to_block > 0 ? CMD_FILTER_REJECT : CMD_FILTER_ACCEPT;
}


/* ---------------------------------------------------------------------------
 * processCompletedStorageRequests — State machine completion handler
 *
 * Handles READ, WRITE, and DELETE completions with proper state transitions.
 * ---------------------------------------------------------------------------*/

/* Process a single completion message. Extracted from the batch loop so
 * extStorageSyncFetch can drive an individual key's completion mid-command.
 * Consumes the message (frees msg and its key ref). */
static void processOneCompletion(ValkeyModuleExternalStorageMsg *msg) {
    {
            int db_id = msg->db_id; /* PHYSICAL id (storage namespace) */
            robj *key = (robj*)msg->key;
            sds key_name = (sds)objectGetVal(key);
            /* Route to whichever logical db currently owns this physical
             * flash namespace (identity unless SWAPDB ran; see indirection). */
            serverDb *db = server.db[extStorageLogicalDbId(db_id)];

            switch (msg->msg_type) {

            /* ----- WRITE completion (spill to flash) ----- */
            case VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_WRITE: {
                /* Release the in-flight RAM credit for this spill — the same
                 * footprint the IO thread added at serialize, carried on the msg. */
                atomic_fetch_sub_explicit(&inflight_spill_ram_bytes, msg->ram_bytes, memory_order_relaxed);
                TieringState state = extStorageGetState(db, key_name);
                serverAssert(state == TIERING_STATE_COPYING_TO_FLASH ||
                             state == TIERING_STATE_PENDING_EVICT);
                /* Free the borrowed value robj (zero-copy: sds belongs to dict entry) */
                if (msg->value != NULL) {
                    zfree(msg->value); /* Don't decrRefCount — sds is not owned */
                }

                if (state == TIERING_STATE_PENDING_EVICT) {
                    /* FC eviction arrived while spill was in-flight.
                     * Spill completed but we need to evict the key now. */
                    completion_pending_evict_count++;
                    dbEntry *entry = dbFind(db, key_name);
                    if (entry != NULL) {
                        robj keyobj;
                        initStaticStringObject(keyobj, key_name);
                        dbDelete(db, &keyobj);
                    }
                    extStorageRemoveState(db, key_name);
                    total_items_spilling_to_ext_storage--;
                    break;
                }

                if (msg->status != VALKEYMODULE_OK) {
                    /* Spill FAILED — keep value in RAM, revert to ONLY_MEMORY */
                    completion_write_fail++;
                    consecutive_spill_failures++;
                    serverLog(LL_WARNING, "Spill failed for key %s, keeping in memory", key_name);
                    extStorageRemoveState(db, key_name);
                } else {
                    /* Spill OK — free RAM value, mark as ONLY_FLASH */
                    completion_write_ok++;
                    consecutive_spill_failures = 0; /* Backend is healthy */
                    dbEntry *entry = dbFind(db, key_name);
                    if (entry != NULL && !objectIsTiered(entry)) {
                        /* Free the original in-RAM value by its real type (string
                         * OR compound: list/set/hash/zset/stream), reclaiming the
                         * RAM, then tombstone the entry as ONLY_FLASH. */
                        if (entry->hasembval) {
                            objectUnembedVal(entry);
                        } else {
                            robj *old = createObject(entry->type, objectGetVal(entry));
                            old->encoding = entry->encoding;
                            /* Detach value from entry BEFORE freeing — prevents
                             * dangling pointer if jemalloc reuses the freed region
                             * for the placeholder allocation below. */
                            objectSetVal(entry, NULL);
                            decrRefCount(old);
                        }
                        entry->encoding = OBJ_ENCODING_TIERED;
                        objectSetVal(entry, sdsnewlen("", 0));
                        total_items_spilled_to_ext_storage++;
                        num_items_on_flash++;
                        extStorageSetState(db, key_name, TIERING_STATE_ONLY_FLASH, 0);
                    } else if (entry == NULL) {
                        /* Key was deleted during spill (shouldn't happen since we block DEL,
                         * but handle defensively). Remove state. */
                        serverLog(LL_WARNING, "Key %s deleted during spill — orphaned flash data", key_name);
                        extStorageRemoveState(db, key_name);
                    }
                }
                total_items_spilling_to_ext_storage--;
                break;
            }

            /* ----- READ completion (fetch from flash) ----- */
            case VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ: {
                TieringState state = extStorageGetState(db, key_name);

                if (state == TIERING_STATE_PENDING_EVICT) {
                    /* Eviction was requested while fetch was in-flight.
                     * Discard fetched value and evict the key. */
                    completion_pending_evict_count++;
                    if (msg->value != NULL) {
                        decrRefCount((robj*)msg->value);
                    }
                    /* Delete the key from the dict entirely */
                    dbEntry *entry = dbFind(db, key_name);
                    if (entry != NULL) {
                        robj keyobj;
                        initStaticStringObject(keyobj, key_name);
                        dbDelete(db, &keyobj);
                    }
                    extStorageRemoveState(db, key_name);
                    total_items_fetching_from_ext_storage--;
                    break;
                }

                serverAssert(state == TIERING_STATE_COPYING_TO_MEMORY);

                robj *new_value = (robj*)msg->value;
                if (new_value != NULL) {
                    dbEntry *entry = dbFind(db, key_name);

                    /* Check if key expired while being fetched — don't promote,
                     * just delete. Avoids wasted memory from promoting a dead value. */
                    if (entry != NULL) {
                        /* Entry carries the expire; objectGetVal(entry) is the placeholder
                     * sds — see the KBC-site comment (garbage-expire key loss). */
                    long long expire_ms = objectGetExpire(entry);
                        if (expire_ms > 0 && expire_ms < (long long)mstime() && !server.loading) {
                            decrRefCount(new_value);
                            robj keyobj;
                            initStaticStringObject(keyobj, key_name);
                            deleteExpiredKeyAndPropagate(db, &keyobj);
                            extStorageRemoveState(db, key_name);
                            total_items_fetching_from_ext_storage--;
                            if (num_items_on_flash > 0) num_items_on_flash--;
                            break;
                        }
                    }

                    /* FATAL assertion: read must succeed (matching dt-poc behavior) */
                    completion_read_ok++;
                    if (entry != NULL && objectIsTiered(entry)) {
                        /* Restore value to existing entry. Free the tombstone
                         * placeholder (empty sds) first. */
                        void *placeholder = objectGetVal(entry);
                        if (placeholder != NULL) sdsfree((sds)placeholder);
                        if (new_value->hasembval) {
                            /* EMBSTR: the value bytes live INSIDE new_value's own
                             * allocation (objectGetVal returns a pointer into it).
                             * Aliasing that pointer into the entry and then freeing
                             * new_value would leave entry->val_ptr dangling — a
                             * use-after-free that crashes on the next read once the
                             * freed region is reused under allocation churn. Detach
                             * into a standalone RAW sds the entry owns. Values that
                             * embed (<=~115B here) are restored as RAW; correctness
                             * over re-embedding. */
                            objectSetVal(entry, sdsdup((sds)objectGetVal(new_value)));
                            entry->encoding = OBJ_ENCODING_RAW;
                            entry->type = new_value->type;
                            decrRefCount(new_value);
                        } else {
                            /* RAW/INT string or compound (list/set/hash/zset/stream):
                             * the value is a separate allocation (or an inline int),
                             * so transfer the pointer and free only the robj wrapper. */
                            objectSetVal(entry, objectGetVal(new_value));
                            entry->encoding = new_value->encoding;
                            entry->type = new_value->type;
                            new_value->refcount = 0;
                            zfree(new_value);
                        }
                    } else if (entry == NULL) {
                        /* Key not in dict (bloom filter false positive or race) — add it */
                        dbAdd(db, key, &new_value);
                    } else {
                        /* Key was overwritten (shouldn't happen since we block writes) */
                        decrRefCount(new_value);
                    }
                    if (msg->ttl > 0) {
                        setExpire(NULL, db, key, msg->ttl);
                    }
                    total_items_fetched_from_ext_storage++;
                    if (num_items_on_flash > 0) num_items_on_flash--;
                    extStorageRemoveState(db, key_name); /* → ONLY_MEMORY */
                } else if (msg->status == VALKEYMODULE_EXTERNAL_STORAGE_READ_RETRY) {
                    /* Transient backpressure (FlashCache read queue full), NOT a
                     * miss: the value is still on flash and the index entry is
                     * intact. Re-issue the fetch and stay in COPYING_TO_MEMORY so
                     * blocked clients keep waiting. Do NOT touch tiering state,
                     * num_items_on_flash, or unblock clients — treating this as a
                     * miss would leave the entry TIERED with ONLY_MEMORY state
                     * (KEY_STATE_BUG wedge). The resubmit replaces this in-flight
                     * fetch, so the in-flight count is left unchanged. */
                    completion_read_retry++;
                    extStorageBridge_submitGet(db_id, key_name); /* submitGet sdsdup's the key */
                    decrRefCount(key);
                    zfree(msg);
                    return;
                } else {
                    /* Value not found on disk — the flash copy is gone (FC GC
                     * eviction, or a cross-db inconsistency). Mark confirmed
                     * absent AND delete the placeholder entry: leaving a TIERED
                     * entry with its state cleared wedges KBC into an infinite
                     * resubmit-miss loop (the defensive ONLY_MEMORY+tiered
                     * branch re-marks it ONLY_FLASH forever). Live-reproduced
                     * via SWAPDB; also reachable via real FlashCache GC. */
                    completion_read_miss++;
                    if (num_items_on_flash > 0) num_items_on_flash--;
                    hashtableAdd(db->keys_confirmed_absent, sdsdup(key_name));
                    extStorageRemoveState(db, key_name);
                    dbEntry *miss_entry = dbFind(db, key_name);
                    if (miss_entry != NULL && objectIsTiered(miss_entry)) {
                        robj miss_keyobj;
                        initStaticStringObject(miss_keyobj, key_name);
                        dbDelete(db, &miss_keyobj);
                    }
                }
                total_items_fetching_from_ext_storage--;
                break;
            }

            /* ----- DELETE completion (eviction or DEL from flash) ----- */
            case VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE: {
                TieringState state = extStorageGetState(db, key_name);

                if (state == TIERING_STATE_COPYING_TO_FLASH) {
                    /* GC eviction arrived while spill is in-flight — defer eviction.
                     * When the spill completes, it will see PENDING_EVICT and
                     * delete the key. */
                    extStorageSetState(db, key_name, TIERING_STATE_PENDING_EVICT, 0);
                    completion_pending_evict_count++;
                    break;
                }

                if (state == TIERING_STATE_ONLY_MEMORY) {
                    /* Key was already fetched back to memory — FC eviction is
                     * stale (the flash copy was already consumed). Ignore. */
                    break;
                }

                /* ONLY_FLASH, COPYING_TO_MEMORY, or PENDING_EVICT — flash copy is gone.
                 * COPYING_TO_MEMORY here means our DEL completed (we set it in submitDel). */
                dbEntry *entry = dbFind(db, key_name);
                int entry_kept_pending_deletion = 0;
                if (entry != NULL) {
                    robj keyobj;
                    initStaticStringObject(keyobj, key_name);
                    /* Check if this was an expiry-triggered delete — if key has
                     * an expired TTL, use the proper expiry propagation path. */
                    /* Entry carries the expire; objectGetVal(entry) is the placeholder
                     * sds — see the KBC-site comment (garbage-expire key loss). */
                    long long expire_ms = objectGetExpire(entry);
                    if (expire_ms > 0 && expire_ms < mstime()) {
                        deleteExpiredKeyAndPropagate(db, &keyobj);
                    } else if (state == TIERING_STATE_COPYING_TO_MEMORY) {
                        /* Client-initiated DEL (submitDel set COPYING_TO_MEMORY).
                         * Do NOT remove the entry here — the blocked client's DEL
                         * re-executes after unblock and must find the entry so
                         * the command layer produces the correct reply count,
                         * signalModifiedKey (WATCH), keyspace "del" notification,
                         * and dirty++. Mark PENDING_DELETION: KBC lets DEL/UNLINK
                         * through and blocks everything else until the DEL drains
                         * (mirrors internal TS MSG_TYPE_DELETE_KEY handling). */
                        extStorageSetState(db, key_name, TIERING_STATE_PENDING_DELETION, 0);
                        entry_kept_pending_deletion = 1;
                    } else {
                        /* GC eviction / PENDING_EVICT: no client is waiting on a
                         * reply — remove the entry directly. */
                        dbDelete(db, &keyobj);
                    }
                }
                if (!entry_kept_pending_deletion) extStorageRemoveState(db, key_name);
                completion_delete_ok++;
                total_items_deleted_from_ext_storage++;
                if (num_items_on_flash > 0) num_items_on_flash--;
                break;
            }

            default:
                break;
            }

            /* Unblock all clients waiting on this key */
            unblockClientsInUseOnKey(key);
            decrRefCount(key);
            zfree(msg);
    }
}

/* Debug-only: when set (DEBUG EXT-STORAGE-PAUSE-COMPLETIONS 1), completion
 * processing is skipped, holding submitted flash operations (and the clients
 * blocked on them) in flight. Used by tests to deterministically exercise
 * in-flight windows (e.g. the SWAPDB pending-DEL guard). */
int ext_storage_debug_pause_completions = 0;

void processCompletedStorageRequests(void) {
    int next_batch_size;

    if (ext_storage_debug_pause_completions) return;

    /* Completions deferred by extStorageSyncFetch run FIRST: they were polled
     * out of the bridge during a sync fetch and precede anything still queued
     * (arrival-order preservation; per-key order is safe regardless because
     * each key has at most one in-flight operation). */
    if (deferred_completions) {
        listNode *ln;
        while ((ln = listFirst(deferred_completions)) != NULL) {
            ValkeyModuleExternalStorageMsg *dmsg = listNodeValue(ln);
            listDelNode(deferred_completions, ln);
            processOneCompletion(dmsg);
        }
    }

    while ((next_batch_size = extStorageBridge_pollCompletions(
                completed_storage_requests,
                COMPLETED_STORAGE_REQUESTS_PROCESSING_BATCH_SIZE)) > 0) {
        completion_batches_processed++;
        for (int j = 0; j < next_batch_size; j++) {
            processOneCompletion(completed_storage_requests[j]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * extStorageSyncFetch — mid-execution synchronous fetch
 * Design: .agent/knowledge/sync-fetch-design.md
 *
 * Called from lookupKey() when a non-resident key is accessed in a context
 * that cannot block-and-re-execute (Lua/EXEC-inner commands, SORT BY/GET
 * pattern resolution, module OpenKey). Stalls the main thread until the
 * key's IO resolves, processing ONLY this key's completions; other keys'
 * completions are deferred to preserve keyspace isolation for the running
 * command (a spill completion processed mid-command could free memory the
 * command still references).
 *
 * Deliberately never times out: callers cannot roll back partial execution
 * (a Lua script may already have applied writes).
 * ---------------------------------------------------------------------------*/
void extStorageSyncFetch(serverDb *db, sds key) {
    if (!ext_data_enabled) return;

    monotime start = getMonotonicUs();
    long long next_warn_us = 5000000; /* warn every 5s while stalled */
    int backoff_us = 0;

    while (1) {
        dbEntry *entry = dbFind(db, key);
        if (entry == NULL) goto done_absent; /* deleted / never existed */

        TieringState state = extStorageGetState(db, key);
        if (state == TIERING_STATE_ONLY_MEMORY) {
            if (!objectIsTiered(entry)) goto done_resident;
            /* TIERED encoding with no state entry — normalize and fetch. */
            extStorageSetState(db, key, TIERING_STATE_ONLY_FLASH, 0);
            state = TIERING_STATE_ONLY_FLASH;
        }
        if (state == TIERING_STATE_PENDING_DELETION) goto done_absent; /* logically deleted */

        if (state == TIERING_STATE_ONLY_FLASH) {
            if (extStorageBridge_submitGet(extStoragePhysicalDbId(db->id), key) == 0) {
                extStorageSetState(db, key, TIERING_STATE_COPYING_TO_MEMORY,
                                   VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ);
                total_items_fetching_from_ext_storage++;
            }
            /* submit rejected (throttled): fall through, drain, retry */
        }
        /* COPYING_TO_FLASH / COPYING_TO_MEMORY / PENDING_EVICT: an operation
         * is in flight — wait for ITS completion. For COPYING_TO_FLASH the
         * value is still in RAM, but the caller may mutate it while the IO
         * thread serializes it; waiting for the spill is the race-free
         * choice (then fetch back). */

        /* Selective drain: process our key's completions, defer the rest. */
        int n = extStorageBridge_pollCompletions(
            completed_storage_requests, COMPLETED_STORAGE_REQUESTS_PROCESSING_BATCH_SIZE);
        int progressed = 0;
        int our_read_missed = 0;
        for (int j = 0; j < n; j++) {
            ValkeyModuleExternalStorageMsg *m = completed_storage_requests[j];
            sds mk = (sds)objectGetVal((robj *)m->key);
            if (sdslen(mk) == sdslen(key) && memcmp(mk, key, sdslen(key)) == 0) {
                long long miss_before = completion_read_miss;
                processOneCompletion(m);
                progressed = 1;
                if (completion_read_miss > miss_before) our_read_missed = 1;
            } else {
                if (!deferred_completions) deferred_completions = listCreate();
                listAddNodeTail(deferred_completions, m);
                sync_fetch_deferred_count++;
            }
        }
        if (our_read_missed) goto done_absent; /* value gone from flash (GC) */

        if (progressed) {
            backoff_us = 0; /* state advanced — re-evaluate immediately */
        } else {
            if (backoff_us < 200) backoff_us += 50;
            usleep(backoff_us);
        }

        long long elapsed = (long long)(getMonotonicUs() - start);
        if (elapsed > next_warn_us) {
            serverLog(LL_WARNING,
                "extStorageSyncFetch stalled %llds waiting for key IO (state=%d)"
                " — backend slow or wedged",
                (long long)(elapsed / 1000000), (int)extStorageGetState(db, key));
            next_warn_us += 5000000;
        }
    }

done_absent:
    sync_fetch_miss_count++;
    /* fall through */
done_resident: ;
    long long waited = (long long)(getMonotonicUs() - start);
    sync_fetch_count++;
    sync_fetch_wait_us_total += waited;
    if (waited > sync_fetch_wait_us_max) sync_fetch_wait_us_max = waited;
}


/* ---------------------------------------------------------------------------
 * spillItemAsync — Spill a key's value to external storage
 *
 * Transition: ONLY_MEMORY → COPYING_TO_FLASH
 * ---------------------------------------------------------------------------*/

static int spillItemAsync(sds key, int db_id) {
    serverAssert(key != NULL && db_id >= 0 && server.db[db_id]);
    serverDb *db = server.db[db_id];
    dbEntry *item = dbFind(db, key);

    if (item == NULL) return -1;
    if (isEmbeddedObject(item)) return -1;
    if (objectIsTiered(item)) return -1;
    if (item->refcount != 1) return -1;

    /* Only spill keys in ONLY_MEMORY state */
    TieringState state = extStorageGetState(db, key);
    if (state != TIERING_STATE_ONLY_MEMORY) return -1;

    if (!extStorageBridge_isReady()) return -1;

    /* Zero-copy spill: key is small (copy is fine), but value can be large.
     * For the value, create a lightweight robj that BORROWS the original value
     * (no memcpy), faithfully mirroring its real type/encoding so the IO-thread
     * serializer (createDumpPayload/rdbSaveObject) handles every object type —
     * string, list, set, hash, zset, stream — not just strings.
     * Safe because key is blocked (SET/DEL) during COPYING_TO_FLASH, the object
     * is refcount==1 and non-embedded, and rehash is paused while in-flight.
     * On completion, the borrowed robj is freed with zfree (not decrRefCount)
     * since it doesn't own the underlying value. */
    robj *key_ref = createStringObject(key, sdslen(key));
    long long expireMs = objectGetExpire(item);

    /* Borrowed value robj — points to original value, no copy */
    void *raw_value = objectGetVal(item);
    robj *value_ref = zmalloc(sizeof(robj));
    memset(value_ref, 0, sizeof(robj));
    value_ref->type = item->type;
    value_ref->encoding = item->encoding;
    value_ref->refcount = 1;
    /* hasembval=0, hasembkey=0, hasexpire=0 already from memset */
    objectSetVal(value_ref, raw_value); /* Points to ORIGINAL value — no copy! */

    int rc = extStorageBridge_submitPut(extStoragePhysicalDbId(db_id), key_ref, value_ref, expireMs);

    if (rc != 0) {
        decrRefCount(key_ref);
        zfree(value_ref); /* Borrowed robj — don't free the sds it points to */
        consecutive_spill_failures++;
        return -1;
    }
    /* DO NOT free key_ref/value_ref here — ownership transferred to IO thread.
     * key_ref: IO thread serializes then bridge frees via decrRefCount
     * value_ref: IO thread serializes then completion frees via zfree (borrowed) */

    /* Window-1 (submit..serialize) opens now: count the submit so projected credits
     * it immediately on the main thread (exact size folds in at serialize). This is
     * what brakes the cap-less fill loop within a single invocation. */
    extStorageOnSpillSubmit();

    /* Transition: ONLY_MEMORY → COPYING_TO_FLASH */
    extStorageSetState(db, key, TIERING_STATE_COPYING_TO_FLASH,
        VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_WRITE);
    consecutive_spill_failures = 0; /* Submission accepted — reset failure counter */
    return 0;
}

/* ---------------------------------------------------------------------------
 * extStorageEvictFlashKey — Evict a key that is on flash
 *
 * Sends async DELETE to flash. Does NOT fetch the value first.
 * Transition: ONLY_FLASH → COPYING_TO_MEMORY (delete in-flight)
 *
 * If key is in COPYING_TO_MEMORY (fetch in-flight), transitions to PENDING_EVICT.
 * Returns 0 on success, -1 if eviction cannot proceed.
 * ---------------------------------------------------------------------------*/

int extStorageEvictFlashKey(serverDb *db, sds key) {
    TieringState state = extStorageGetState(db, key);

    switch (state) {
    case TIERING_STATE_ONLY_FLASH: {
        /* Send async DELETE to flash */
        int rc = extStorageBridge_submitDel(extStoragePhysicalDbId(db->id), key);

        if (rc != 0) {
            return -1;
        }
        /* Transition: ONLY_FLASH → COPYING_TO_MEMORY (delete in-flight) */
        extStorageSetState(db, key, TIERING_STATE_COPYING_TO_MEMORY,
            VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE);
        return 0;
    }

    case TIERING_STATE_COPYING_TO_MEMORY:
        /* Fetch already in-flight — mark for eviction on completion */
        extStorageSetState(db, key, TIERING_STATE_PENDING_EVICT, 0);
        return 0;

    case TIERING_STATE_COPYING_TO_FLASH:
    case TIERING_STATE_PENDING_EVICT:
        /* Already in-flight or already pending eviction — skip */
        return -1;

    case TIERING_STATE_ONLY_MEMORY:
    default:
        /* Not on flash — caller should use normal eviction */
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * extStorageUpdateSpillConcurrency was removed. Spill concurrency is no longer
 * throttle-driven or capped: the spill loop fills until projected <= maxmemory
 * (see spillFillToProjected). The throttle and spill controllers are decoupled
 * and communicate only implicitly through memory.
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * extStoragePerformEvictions — Override standard eviction logic
 *
 * Returns 1 if tiered storage handled the eviction decision (caller should
 * NOT proceed with standard eviction). Sets *result to C_OK or C_ERR.
 *
 * Logic (matching dt-poc's isOverMaxmemoryAndNoSpillableItems):
 * - If memory <= maxmemory → C_OK (no eviction needed)
 * - If there are spillable items (in LRU pool or in-flight) → C_OK
 * - If no spillable items exist → C_ERR (truly out of capacity, reject writes)
 *
 * Note: We do NOT check consecutive_spill_failures here. Even if FC is
 * temporarily throttling, as long as there are items that COULD be spilled,
 * we return OK. The throttle layer handles rate limiting. Only when the LRU
 * is empty AND nothing is in-flight do we declare failure.
 * ---------------------------------------------------------------------------*/

int extStoragePerformEvictions(int *result) {
    if (!ext_data_enabled) return 0; /* Not handled — let standard eviction proceed */

    size_t used_memory = zmalloc_used_memory();

    /* Hard memory cap FIRST: if raw used_memory > 1.2x maxmemory, reject writes
     * unconditionally — being this far over the cap means the spill rate is NOT
     * keeping up with the write rate. This check MUST precede the projected
     * (Smith-predictor) short-circuit below: projected = used - in-flight-credit,
     * and if that credit is inflated (e.g. stalled/dropped WRITE completions that
     * never drain inflight_spill_ram_bytes) projected stays pinned <= maxmemory
     * while raw used runs away unbounded. Evaluating the hard cap first guarantees
     * the OOM reject fires regardless of the projected credit. */
    if (used_memory > server.maxmemory + (server.maxmemory / 5)) {
        memory_hard_cap_exceeded_count++;
        oom_reject_write_count++;
        *result = C_ERR;
        return 1;
    }

    /* Under the hard cap: control the over-budget decision on PROJECTED memory
     * (used minus in-flight spills already committed to be freed) — Smith
     * predictor for the spill dead-time. */
    size_t projected = extStorageProjectedMemory();
    if (projected <= server.maxmemory) {
        *result = C_OK;
        return 1;
    }

    /* Over maxmemory (projected) but under 1.2x hard cap.
     * The throttle zone [1.0x, 1.2x] handles back-pressure via the bytes-
     * throttle. We should NEVER reject writes in this zone — the throttle
     * slows incoming traffic and spilling drains memory. Rejecting here
     * would be premature since the hard cap hasn't been breached.
     *
     * Track a metric for observability: if no spillable items exist, the
     * spill mechanism can't help and we're relying purely on the throttle
     * to prevent reaching the hard cap. This is a sign of a degenerate
     * workload (all values already on flash, only metadata in memory). */
    if (total_items_spilling_to_ext_storage == 0) {
        long long total_keys = 0;
        for (int i = 0; i < server.dbnum; i++) {
            if (server.db[i]) total_keys += kvstoreSize(server.db[i]->keys);
        }
        long long spillable_keys = total_keys - num_items_on_flash;
        if (spillable_keys <= 0) {
            /* Metric only — no spillable items exist (all on flash or in-flight).
             * The throttle is the sole back-pressure mechanism in this state. */
            no_spillable_items_count++;
        }
    }

    /* Always OK below hard cap — throttle handles back-pressure */
    *result = C_OK;
    return 1;
}

/* ---------------------------------------------------------------------------
 * processCompletedStorageRequestsAndSpillOldItems
 *
 * Fix #2: Spill at maxmemory (not maxmemory+10%) — matching dt-poc behavior.
 * The spillover margin is the THROTTLE zone, not the spill trigger.
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * spillFillToProjected — THE spill controller (cap-less, projected-gated)
 *
 * Submit spills until PROJECTED memory is back at the setpoint (maxmemory), or
 * until there is nothing left to submit. Queue depth is NOT capped by a constant —
 * it is an emergent output of the projected-memory signal. Each submit credits the
 * two-stage predictor (window-1 immediately via submit_depth*mean_spill_ram,
 * window-2 at serialize via inflight_spill_ram_bytes), so projected falls in real
 * time and the loop self-terminates the instant enough drain is committed.
 *
 * Stop conditions are ONLY: projected <= maxmemory (setpoint reached), no spillable
 * candidate left, or the submit queue rejects (backpressure). There is deliberately
 * NO memory-headroom clamp: halting on memory pressure would starve the disk and
 * RAISE memory (only completions free RAM). Disk saturation is handled implicitly —
 * if the disk can't drain, completions lag, RAW used_memory (not projected) climbs
 * into the throttle band, which is the throttle's job, not this loop's.
 * ---------------------------------------------------------------------------*/
static long long spillFillToProjected(void) {
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) return total_items_spilling_to_ext_storage;
    if (extStorageProjectedMemory() <= server.maxmemory) return total_items_spilling_to_ext_storage;

    ext_storage_spill_pool_active = 1;
    int skipped = 0;
    /* TODO(perf): cold-start / stale-mean under-braking. window-1 credit is
     * depth*mean_spill_ram, but mean_spill_ram==0 until the first serialize, and
     * used_memory does not drop within a single pass (RAM frees on completion).
     * So on the first overshoot the predictor does not brake the loop — it stops
     * only on candidate exhaustion or FC_REQ_RING backpressure (~4096 submits).
     * Same effect when mean lags actual size on heavy-tailed/mixed workloads.
     * Acceptable for now (same-size target workload converges in one pass); fix
     * later by seeding mean from a main-thread objectComputeSize at submit, or a
     * per-pass fallback bound until mean is known. */
    while (extStorageProjectedMemory() > server.maxmemory) {
        int best_dbid;
        int best_slot;
        sds best_key = findBestEvictionCandidate(spillPoolLRU, &best_dbid, &best_slot);
        if (best_key == NULL) { spill_skipped_null++; break; }

        /* Re-check after pool populate: projected may have dropped below maxmemory
         * while we were sampling (completions can land between iterations). */
        if (extStorageProjectedMemory() <= server.maxmemory) break;

        spill_attempts++;
        /* Pool is filtered to ONLY_MEMORY while active, but re-validate defensively
         * (TOCTOU): skip non-spillable and try the next candidate, up to a budget. */
        if (extStorageGetState(server.db[best_dbid], best_key) != TIERING_STATE_ONLY_MEMORY) {
            spill_skipped_non_spillable++;
            if (++skipped < EVPOOL_SIZE) continue;
            break;
        }
        skipped = 0;
        if (spillItemAsync(best_key, best_dbid) == -1) break;  /* submit-queue backpressure */

        spill_submitted++;
        total_items_spilling_to_ext_storage++;
        if (spill_submitted >= 64) break;  /* cold-start brake: yield to event loop */
    }
    ext_storage_spill_pool_active = 0;
    return total_items_spilling_to_ext_storage;
}

/* ---------------------------------------------------------------------------
 * extStorageUpdateSpillConcurrency — legacy dynamic spill-concurrency actuator
 *
 * Scales max_num_concurrent_items_spilled from BASE (gentle, at/below maxmemory)
 * to LIMIT (aggressive, at full throttle) as throttle_rate ramps 0->1. Called by
 * the COUPLED (legacy) throttle. Only the ITEM_COUNT spill strategy reads the cap;
 * the PROJECTED strategy is cap-less and ignores it.
 * ---------------------------------------------------------------------------*/
void extStorageUpdateSpillConcurrency(double throttle_rate) {
    if (throttle_rate <= 0.0) {
        max_num_concurrent_items_spilled = SPILL_CONCURRENT_BASE;
    } else {
        int dynamic = SPILL_CONCURRENT_BASE +
            (int)((SPILL_CONCURRENT_LIMIT - SPILL_CONCURRENT_BASE) * throttle_rate);
        if (dynamic > SPILL_CONCURRENT_LIMIT) dynamic = SPILL_CONCURRENT_LIMIT;
        max_num_concurrent_items_spilled = dynamic;
    }
}

/* ---------------------------------------------------------------------------
 * Legacy ITEM_COUNT spill strategy (selected by ext_storage_spilling_strategy).
 *
 * beforeSleep pump: spill at 1.0x maxmemory, bounded per tick by
 *   items_spillover_batch_size AND the dynamic max_num_concurrent_items_spilled cap.
 * aggressive (per-command) pump: spill at 1.1x maxmemory, bounded only by the cap.
 *
 * Restored verbatim from pre-Smith HEAD (b0f5bb9e^), with one intentional fix:
 * the aggressive path now sets ext_storage_spill_pool_active around candidate
 * selection (the original omitted it — a known pool-pollution bug that skipped
 * non-spillable candidates). This isolates the control-strategy comparison from
 * that unrelated defect; the beforeSleep path already had the guard.
 * ---------------------------------------------------------------------------*/
static long long spillItemCountBeforeSleep(void) {
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) return total_items_spilling_to_ext_storage;

    int num_items_spilling = 0;
    size_t spill_threshold = server.maxmemory;  /* 1.0x */
    if (zmalloc_used_memory() > spill_threshold) {
        ext_storage_spill_pool_active = 1;
        int skipped = 0;
        while (num_items_spilling < items_spillover_batch_size) {
            if (total_items_spilling_to_ext_storage >= max_num_concurrent_items_spilled) break;

            int best_dbid;
            int best_slot;
            sds best_key = findBestEvictionCandidate(spillPoolLRU, &best_dbid, &best_slot);
            if (best_key == NULL) { spill_skipped_null++; break; }

            spill_attempts++;
            if (extStorageGetState(server.db[best_dbid], best_key) != TIERING_STATE_ONLY_MEMORY) {
                spill_skipped_non_spillable++;
                if (++skipped < EVPOOL_SIZE) continue;
                break;
            }

            skipped = 0;
            if (spillItemAsync(best_key, best_dbid) == -1) break;

            spill_submitted++;
            num_items_spilling++;
            total_items_spilling_to_ext_storage++;
        }
        ext_storage_spill_pool_active = 0;
    }
    return total_items_spilling_to_ext_storage;
}

static long long spillItemCountAggressive(void) {
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) return total_items_spilling_to_ext_storage;

    size_t spill_threshold = server.maxmemory + (server.maxmemory / 10);  /* 1.1x */
    if (zmalloc_used_memory() > spill_threshold) {
        ext_storage_spill_pool_active = 1;  /* fix: original omitted this (pool pollution) */
        while (1) {
            if (total_items_spilling_to_ext_storage >= max_num_concurrent_items_spilled) break;

            int best_dbid;
            int best_slot;
            sds best_key = findBestEvictionCandidate(spillPoolLRU, &best_dbid, &best_slot);
            if (best_key == NULL) break;

            if (extStorageGetState(server.db[best_dbid], best_key) != TIERING_STATE_ONLY_MEMORY) break;

            if (spillItemAsync(best_key, best_dbid) == -1) break;

            total_items_spilling_to_ext_storage++;
        }
        ext_storage_spill_pool_active = 0;
    }
    return total_items_spilling_to_ext_storage;
}

/* ---------------------------------------------------------------------------
 * processCompletedStorageRequestsAndSpillOldItems
 *
 * Drain completions, then run the spill controller selected by
 * ext_storage_spilling_strategy (PROJECTED cap-less Smith vs ITEM_COUNT legacy).
 * Called from beforeSleep and the periodic timer.
 * ---------------------------------------------------------------------------*/

int processCompletedStorageRequestsAndSpillOldItems(void) {
    if (!ext_data_enabled) return 0;
    if (server.maxmemory == 0) return 0;

    processCompletedStorageRequests();
    if (ext_storage_spilling_strategy == SPILLING_STRATEGY_V1)
        return spillItemCountBeforeSleep();
    return spillFillToProjected();
}

/* Per-command spill pump (called from evict.c on the command path). Dispatches to
 * the same selected strategy as the beforeSleep pump. */
int processCompletedStorageRequestsAndSpillOldItemsAggressive(void) {
    if (!ext_data_enabled) return 0;
    if (server.maxmemory == 0) return 0;

    processCompletedStorageRequests();
    if (ext_storage_spilling_strategy == SPILLING_STRATEGY_V1)
        return spillItemCountAggressive();
    return spillFillToProjected();
}

/* ---------------------------------------------------------------------------
 * INFO string generation
 * ---------------------------------------------------------------------------*/

sds genExternalStorageInfoString(sds info) {
    if (!ext_data_enabled) return info;
    info = sdscatprintf(info,
        "total_num_items_spilled_to_ext_storage:%lld\r\n"
        "total_num_items_fetched_from_ext_storage:%lld\r\n"
        "total_num_items_deleted_from_ext_storage:%lld\r\n"
        "num_items_spilling_to_ext_storage:%lld\r\n"
        "num_items_fetching_from_ext_storage:%lld\r\n"
        "num_items_on_flash:%lld\r\n"
        "kbc_total_calls:%lld\r\n"
        "kbc_in_memory:%lld\r\n"
        "kbc_spilling_block:%lld\r\n"
        "kbc_fetching_block:%lld\r\n"
        "kbc_pending_evict_block:%lld\r\n"
        "kbc_pending_deletion_block:%lld\r\n"
        "sync_fetch_count:%lld\r\n"
        "sync_fetch_miss_count:%lld\r\n"
        "sync_fetch_wait_us_total:%lld\r\n"
        "sync_fetch_wait_us_max:%lld\r\n"
        "sync_fetch_deferred_completions:%lld\r\n"
        "kbc_confirmed_absent:%lld\r\n"
        "kbc_key_may_exist_false:%lld\r\n"
        "kbc_key_may_exist_true:%lld\r\n"
        "completion_batches:%lld\r\n"
        "completion_read_ok:%lld\r\n"
        "completion_read_miss:%lld\r\n"
        "completion_read_retry:%lld\r\n"
        "completion_write_ok:%lld\r\n"
        "completion_write_fail:%lld\r\n"
        "completion_delete_ok:%lld\r\n"
        "completion_pending_evict:%lld\r\n"
        "oom_reject_write_count:%lld\r\n"
        "no_spillable_items_count:%lld\r\n"
        "oom_reject_read_count:%lld\r\n"
        "memory_hard_cap_exceeded_count:%lld\r\n"
        "spill_attempts:%lld\r\n"
        "spill_submitted:%lld\r\n"
        "spill_skipped_non_spillable:%lld\r\n"
        "spill_skipped_null:%lld\r\n"
        "dram_value_hits:%lld\r\n"
        "throttle_total_throttled:%lld\r\n"
        "throttle_queued_clients:%lld\r\n"
        "throttle_current_rate:%.4f\r\n"
        "throttle_allowed_tps:%.1f\r\n",
        total_items_spilled_to_ext_storage,
        total_items_fetched_from_ext_storage,
        total_items_deleted_from_ext_storage,
        total_items_spilling_to_ext_storage,
        total_items_fetching_from_ext_storage,
        num_items_on_flash,
        kbc_total_calls,
        kbc_in_memory_count,
        kbc_spilling_block_count,
        kbc_fetching_block_count,
        kbc_pending_evict_block_count,
        kbc_pending_deletion_block_count,
        sync_fetch_count,
        sync_fetch_miss_count,
        sync_fetch_wait_us_total,
        sync_fetch_wait_us_max,
        sync_fetch_deferred_count,
        kbc_confirmed_absent_count,
        kbc_key_may_exist_false_count,
        kbc_key_may_exist_true_count,
        completion_batches_processed,
        completion_read_ok,
        completion_read_miss,
        completion_read_retry,
        completion_write_ok,
        completion_write_fail,
        completion_delete_ok,
        completion_pending_evict_count,
        oom_reject_write_count,
        no_spillable_items_count,
        oom_reject_read_count,
        memory_hard_cap_exceeded_count,
        spill_attempts,
        spill_submitted,
        spill_skipped_non_spillable,
        spill_skipped_null,
        dram_value_hits,
        extStorageThrottle_getThrottledCount(),
        extStorageThrottle_getQueuedClients(),
        extStorageThrottle_getCurrentRate(),
        extStorageThrottle_getAllowedTps());

    info = sdscatprintf(info,
        "spill_submitted_count:%lld\r\n"
        "spill_serialized_count:%lld\r\n"
        "mean_spill_ram:%zu\r\n"
        "inflight_spill_ram_bytes:%zu\r\n"
        "projected_memory:%zu\r\n",
        (long long)atomic_load_explicit(&spill_submitted_count, memory_order_relaxed),
        (long long)atomic_load_explicit(&spill_serialized_count, memory_order_relaxed),
        (size_t)atomic_load_explicit(&mean_spill_ram, memory_order_relaxed),
        (size_t)atomic_load_explicit(&inflight_spill_ram_bytes, memory_order_relaxed),
        extStorageProjectedMemory());

    info = sdscatprintf(info,
        "throttling_strategy:%s\r\n"
        "spilling_strategy:%s\r\n",
        ext_storage_throttling_strategy == THROTTLING_STRATEGY_V1 ? "v1" : "v2",
        ext_storage_spilling_strategy == SPILLING_STRATEGY_V1 ? "v1" : "v2");

    /* FlashCache internal metrics (via flashcacheGetCountBasedMetric).
     *
     * These are passed as enum symbols rather than integer literals. The
     * literals that used to be here had ALL drifted from the enum: id 10 was
     * labelled FC_NUM_ITEMS_EVICTED but actually reads
     * FC_GARBAGE_COLLECTION_WRITE_BYTES, and id 11 labelled
     * FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES actually reads
     * FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED. That is why the two fields looked
     * mutually contradictory: a byte count reported as an item count next to an
     * item count reported as bytes. Comments naming the intended metric cannot
     * be checked by the compiler, so the block silently rotted as FlashCache's
     * enum grew. Symbols make a future insertion a compile-time concern. */
    if (extStorageBridge_isReady()) {
        extern size_t fc_metric_items_evicted(void);
        extern size_t fc_metric_evicted_bytes(void);
        extern size_t fc_metric_disk_write_bytes(void);
        extern size_t fc_metric_disk_read_bytes(void);
        extern size_t fc_metric_num_disk_writes(void);
        extern size_t fc_metric_num_disk_reads(void);
        extern size_t fc_metric_reads_in_flight(void);
        extern size_t fc_metric_active_memory_bytes(void);
        extern size_t fc_metric_retryable_disk_errs(void);
        extern size_t fc_metric_evicting_under_max(void);
        extern size_t fc_metric_evicted_under_max(void);
        info = sdscatprintf(info,
            "fc_num_items_evicted:%zu\r\n"
            "fc_total_evicted_bytes:%zu\r\n"
            "fc_total_disk_write_bytes:%zu\r\n"
            "fc_total_disk_read_bytes:%zu\r\n"
            "fc_num_disk_writes:%zu\r\n"
            "fc_num_disk_reads:%zu\r\n"
            "fc_num_read_in_flight:%zu\r\n"
            "fc_active_memory_bytes:%zu\r\n"
            "fc_num_retryable_disk_errors:%zu\r\n"
            "fc_is_evicting_under_max_logsize:%zu\r\n"
            "fc_num_evicted_under_max_logsize:%zu\r\n",
            fc_metric_items_evicted(),
            fc_metric_evicted_bytes(),
            fc_metric_disk_write_bytes(),
            fc_metric_disk_read_bytes(),
            fc_metric_num_disk_writes(),
            fc_metric_num_disk_reads(),
            fc_metric_reads_in_flight(),
            fc_metric_active_memory_bytes(),
            fc_metric_retryable_disk_errs(),
            fc_metric_evicting_under_max(),
            fc_metric_evicted_under_max());
    }

    /* Append module-side metrics */
    sds module_metrics = moduleGetExternalStorageMetrics();
    if (sdslen(module_metrics) > 0) {
        info = sdscatsds(info, module_metrics);
    }
    sdsfree(module_metrics);

    info = genExternalStorageSnapshotInfoString(info);
    return info;
}



/* ---------------------------------------------------------------------------
 * Snapshot support (fork-based RDB save with tiered values)
 * Design: .agent/knowledge/replication-design.md, design-docs/data-tiering/replication.md
 *
 * Protocol (parent, main thread):
 *   1. extStorageSnapshotPrepare():
 *        a. settle loop: drain in-flight IO + apply completions until no key
 *           is in a COPYING_* state (every value is EITHER in the hashtable
 *           OR on flash -- the disjointness invariant);
 *        b. park the backend IO thread at a safe point (so fork() inherits
 *           no torn backend state and no held locks);
 *        c. pause backend GC (on-flash offsets stay valid for the child).
 *   2. fork(). Child inherits: consistent CoW hashtable, consistent CoW
 *      backend index, stable flash file regions.
 *   3. Parent: extStorageSnapshotResume() -- unpark the IO thread. Normal
 *      traffic (spills/fetches/appends) resumes; GC stays paused.
 *   4. Child exits (or foreground save completes):
 *      extStorageSnapshotDone() -- unpause GC.
 *
 * The child (or the main thread, for foreground SAVE) materializes tiered
 * values with extStorageMaterializeTiered(): a synchronous pread-based read
 * that never touches the async IO path.
 * ---------------------------------------------------------------------------*/
static int snapshot_active = 0;           /* prepare done, done pending */
static long long snapshot_saves = 0;      /* snapshots started (INFO) */
static long long snapshot_tiered_saved = 0;   /* tiered values materialized (INFO, parent-visible only for foreground SAVE) */
static long long snapshot_tiered_skipped = 0; /* pending-deletion / GC-evicted keys skipped */

int extStorageSnapshotSupported(void) {
    return ext_data_enabled && extStorageBridge_snapshotSupported();
}

int extStorageSnapshotActive(void) {
    return snapshot_active;
}

int extStorageSnapshotPrepare(void) {
    if (!ext_data_enabled) return C_OK;
    if (!extStorageBridge_snapshotSupported()) return C_ERR;
    serverAssert(!snapshot_active);

    /* Settle: bounded drain until no in-flight IO remains. Completion
     * processing can re-issue transiently rejected reads, so loop. */
    for (int i = 0; i < 200; i++) {
        extStorageBridge_drainOnly();
        processCompletedStorageRequests();
        if (total_items_spilling_to_ext_storage == 0 &&
            total_items_fetching_from_ext_storage == 0) break;
        usleep(500); /* let the IO worker finish queued work */
    }
    if (total_items_spilling_to_ext_storage != 0 ||
        total_items_fetching_from_ext_storage != 0) {
        serverLog(LL_WARNING,
            "Snapshot prepare: in-flight tiering IO did not settle "
            "(spilling=%lld fetching=%lld) -- refusing snapshot",
            total_items_spilling_to_ext_storage,
            total_items_fetching_from_ext_storage);
        return C_ERR;
    }

    /* Park the IO thread OUTSIDE any backend call, then freeze GC. Order
     * matters: the parked worker syncs the GC flag on resume, and while it
     * is parked no GC can run at all. */
    extStorageBridge_snapshotHold();
    extStorageBridge_gcPause(1);
    snapshot_active = 1;
    snapshot_saves++;
    return C_OK;
}

void extStorageSnapshotResume(void) {
    if (!snapshot_active) return;
    extStorageBridge_snapshotRelease();
}

void extStorageSnapshotDone(void) {
    if (!snapshot_active) return;
    extStorageBridge_gcPause(0);
    extStorageBridge_snapshotRelease(); /* idempotent; covers foreground path */
    snapshot_active = 0;
}

/* Materialize a tiered value for RDB serialization.
 * Returns:  1 -- *payload/(plen) set to the DUMP-format bytes WITHOUT the
 *                10-byte (version+CRC) footer: [type byte][rdbSaveObject
 *                bytes], exactly what rdbSaveKeyValuePair needs. Caller
 *                must zfree().
 *           0 -- key must be skipped (logically deleted, or evicted from
 *                flash by GC before the snapshot froze it).
 * Runs in the fork child, or on the main thread during foreground SAVE
 * while the IO thread is held. */
int extStorageMaterializeTiered(int dbid, robj *key, robj *val, char **payload, size_t *plen) {
    serverAssert(objectIsTiered(val));

    /* PENDING_DELETION: flash copy is (being) deleted for a client DEL --
     * the key is logically gone. */
    if (val->tiering_state == TIERING_STATE_PENDING_DELETION) {
        snapshot_tiered_skipped++;
        return 0;
    }

    sds keyname = (sds)objectGetVal(key);
    char *buf = NULL;
    size_t len = 0;
    if (extStorageBridge_forkRead(extStoragePhysicalDbId(dbid), keyname, &buf, &len) != 0) {
        /* Not on flash: GC evicted it before the freeze. Consistent with the
         * engine lazily discovering NOT_FOUND and dropping the key. */
        snapshot_tiered_skipped++;
        return 0;
    }
    /* DUMP payload = [type][object bytes][2B rdbver][8B crc64]. Strip the
     * footer; sanity-check there is at least a type byte under it. */
    if (len <= 10) {
        zfree(buf);
        snapshot_tiered_skipped++;
        return 0;
    }
    *payload = buf;
    *plen = len - 10;
    snapshot_tiered_saved++;
    return 1;
}

sds genExternalStorageSnapshotInfoString(sds info) {
    info = sdscatprintf(info,
        "snapshot_supported:%d\r\n"
        "snapshot_active:%d\r\n"
        "snapshot_saves:%lld\r\n"
        "snapshot_tiered_values_saved:%lld\r\n"
        "snapshot_tiered_values_skipped:%lld\r\n",
        extStorageSnapshotSupported() ? 1 : 0,
        snapshot_active,
        snapshot_saves,
        snapshot_tiered_saved,
        snapshot_tiered_skipped);
    return info;
}
