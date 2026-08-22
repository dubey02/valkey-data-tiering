/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ext_storage_bridge.c — Bridges the new pluggable storage interface
 * into ext_storage.c's existing state machine and completion processing.
 *
 * This file provides:
 *   1. extStorageBridge_init() — initializes the storage backend
 *   2. extStorageBridge_submitPut() — replaces moduleFireExternalStorageEvent for WRITE
 *   3. extStorageBridge_submitGet() — replaces moduleFireExternalStorageEvent for READ
 *   4. extStorageBridge_submitDel() — replaces moduleFireExternalStorageEvent for DELETE
 *   5. extStorageBridge_pollCompletions() — replaces moduleGetCompletedExternalStorageResponses
 *   6. extStorageBridge_isReady() — replaces moduleHasExternalStorageSubscribers
 *
 * The bridge translates between:
 *   - ext_storage.c's robj/sds world (keys as sds, values as robj)
 *   - storage interface's opaque bytes world (keys and values as void ptr + size_t)
 *
 * Completion delivery: the storage backend calls our callback from its IO thread.
 * We enqueue completions into a ring buffer. ext_storage.c polls from main thread.
 */

#include "server.h"
#include "module.h"
#define STORAGE_USE_ZMALLOC
#include "storage/storage.h"
#include "ext_storage.h"
#include <string.h>
#include <pthread.h>

#define BRIDGE_COMP_RING 4096

/* Request context — passed through the storage layer and returned in completion */
typedef struct bridgeRequestCtx {
    int op_type;        /* STORAGE_OP_PUT / GET / DEL */
    uint32_t db_id;
    sds key;            /* Key name (borrowed or owned depending on key_owned) */
    int key_owned;      /* 1 = we sdsdup'd it (free on completion), 0 = borrowed */
    robj *value_copy;   /* For PUT: the robj copy (to free on completion) */
    robj *key_robj;     /* For PUT: the key robj to decrRefCount on completion */
    int64_t expire_ms;
} bridgeRequestCtx;

/* Completion ring (single-threaded: written by bridge_on_completion during
 * storagePollCompletions, read immediately after in same function call) */
static struct {
    storageCompletion ring[BRIDGE_COMP_RING];
    volatile int head, tail;
} comp_ring;

/* Completion callback — called from backend's IO thread */
static void bridge_on_completion(storageCompletion *c, void *privdata) {
    (void)privdata;
    int next = (comp_ring.head + 1) % BRIDGE_COMP_RING;
    if (next != comp_ring.tail) {
        comp_ring.ring[comp_ring.head] = *c;
        comp_ring.head = next;
    }
}

/* Initialize the storage backend */
int extStorageBridge_init(const char *backend_name, const char *path, size_t capacity) {
    storageType *type = NULL;

    /* Check if a module registered a storage backend */
    if (moduleHasRegisteredStorageBackend()) {
        type = (storageType *)moduleGetRegisteredStorageBackend();
        serverLog(LL_NOTICE, "ext_storage_bridge: using module-registered backend '%s'", type->name);
    } else if (strcmp(backend_name, "flashcache") == 0) {
        type = storageGetFlashCacheRealType();
    } else if (strcmp(backend_name, "flashcache-mock") == 0) {
        type = storageGetFlashCacheType(); /* In-memory mock — for testing only */
    } else if (strcmp(backend_name, "rocksdb") == 0) {
        type = storageGetRocksDBAsyncType();
    }

    if (!type) {
        serverLog(LL_WARNING, "ext_storage_bridge: unknown backend '%s'", backend_name);
        return -1;
    }

    /* comp_ring accessed single-threaded — no mutex needed */
    comp_ring.head = comp_ring.tail = 0;

    storageConfig cfg = {
        .path = path,
        .capacity_bytes = capacity,
        .num_databases = server.dbnum,
        .io_threads = 1,
        .eviction_enabled = (server.maxmemory_policy != MAXMEMORY_NO_EVICTION),
        .completion_fn = bridge_on_completion,
        .completion_privdata = NULL,
        .fast_boot = ext_storage_fast_boot,
        .recovery_item_fn = extStorageRecoveryItem,
        .recovery_item_ctx = NULL,
        .index_only = ext_key_spill_enabled,
        .recovery_counts_fn = extStorageRecoveryCounts,
        .index_size = (size_t)ext_storage_index_size,
        .max_allocated_percent = (uint32_t)ext_storage_max_allocated_percent,
        .max_in_flight_reads = (uint32_t)ext_storage_max_in_flight_reads,
        .min_gc_rate = (uint32_t)ext_storage_min_gc_rate,
        .max_gc_rate = (uint32_t)ext_storage_max_gc_rate,
        .max_buffered_write_size = (size_t)ext_storage_max_buffered_write_size,
        .buffered_write_flush_threshold = (size_t)ext_storage_buffered_write_flush_threshold,
    };

    /* Module's open() is safe to call (just stores completion_fn).
     * Native backends init FlashCache/RocksDB in open(). */
    int rc = storageInit(type, &cfg);
    if (rc != 0) {
        serverLog(LL_WARNING, "ext_storage_bridge: backend '%s' init failed at path '%s'", backend_name, path);
        return -1;
    }

    serverLog(LL_NOTICE, "ext_storage_bridge: initialized backend '%s' at %s", backend_name, path);
    return 0;
}

/* Check if storage backend is ready */
int extStorageBridge_isReady(void) {
    extern storageType *server_storage;
    extern void *server_storage_ctx;
    return server_storage != NULL && server_storage_ctx != NULL;
}

/* Submit a PUT (spill to storage) — passes robj* pointers.
 * The storage backend's IO thread will serialize them. */
int extStorageBridge_submitPut(int db_id, robj *key, robj *value, int64_t expire_ms) {
    bridgeRequestCtx *ctx = zmalloc(sizeof(bridgeRequestCtx));
    ctx->op_type = STORAGE_OP_PUT;
    ctx->db_id = db_id;
    ctx->key = (sds)objectGetVal(key); /* Borrow key sds from the robj (IO thread will serialize it).
                                        * Safe: the db entry's key is stable while in COPYING_TO_FLASH state. */
    ctx->key_owned = 0; /* Don't free on completion — it's borrowed */
    ctx->value_copy = value;
    ctx->key_robj = key;  /* Store for decrRefCount on completion (frees the key_ref from spillItemAsync) */
    ctx->expire_ms = expire_ms;

    storageStatus s = storageSubmitPut(db_id, (void*)key, 0,
                                       (void*)value, 0, expire_ms, ctx);
    if (s == STORAGE_WOULDBLOCK || s == STORAGE_OK) {
        return 0;
    }
    /* Rejected — don't free ctx->key (borrowed) */
    zfree(ctx);
    return -1;
}

/* Submit a GET (fetch from storage) */
int extStorageBridge_submitGet(int db_id, sds key, int get_flags) {
    bridgeRequestCtx *ctx = zmalloc(sizeof(bridgeRequestCtx));
    ctx->op_type = STORAGE_OP_GET;
    ctx->db_id = db_id;
    ctx->key_robj = NULL;
    ctx->key = sdsdup(key);
    ctx->key_owned = 1;
    ctx->value_copy = NULL;
    ctx->expire_ms = 0;

    robj *keyobj = createStringObject(key, sdslen(key));
    storageStatus s = storageSubmitGet(db_id, (void*)keyobj, 0, get_flags, ctx);
    if (s == STORAGE_WOULDBLOCK || s == STORAGE_OK) {
        return 0;
    }
    decrRefCount(keyobj);
    sdsfree(ctx->key);
    zfree(ctx);
    return -1;
}

/* Submit a DELETE (remove from storage) */
int extStorageBridge_submitDel(int db_id, sds key) {
    bridgeRequestCtx *ctx = zmalloc(sizeof(bridgeRequestCtx));
    ctx->op_type = STORAGE_OP_DEL;
    ctx->db_id = db_id;
    ctx->key_robj = NULL;
    ctx->key = sdsdup(key);
    ctx->key_owned = 1;
    ctx->value_copy = NULL;
    ctx->expire_ms = 0;

    robj *keyobj = createStringObject(key, sdslen(key));
    storageStatus s = storageSubmitDel(db_id, (void*)keyobj, 0, ctx);
    if (s == STORAGE_WOULDBLOCK || s == STORAGE_OK) {
        return 0;
    }
    decrRefCount(keyobj);
    sdsfree(ctx->key);
    zfree(ctx);
    return -1;
}

/*
 * Poll completions and translate to ValkeyModuleExternalStorageMsg format
 * for compatibility with existing processCompletedStorageRequests() logic.
 *
 * Returns number of completions fetched. Fills `out` array.
 */
int extStorageBridge_pollCompletions(ValkeyModuleExternalStorageMsg **out, int max) {
    int count = 0;

    /* First: trigger the backend to deliver completions to our ring */
    storagePollCompletions(max);

    /* comp_ring is written by bridge_on_completion (called from storagePollCompletions
     * above, on this same thread) and read here — single-threaded, no lock needed. */
    while (count < max && comp_ring.tail != comp_ring.head) {
        storageCompletion *c = &comp_ring.ring[comp_ring.tail];
        bridgeRequestCtx *req_ctx = (bridgeRequestCtx *)c->request_ctx;

        if (req_ctx == NULL) {
            /* GC eviction from FlashCache — key was evicted internally.
             * Create a DELETE completion so engine removes key from dict. */
            if (c->op_type == STORAGE_OP_DEL && c->value != NULL && c->vlen > 0) {
                ValkeyModuleExternalStorageMsg *msg = zmalloc(sizeof(ValkeyModuleExternalStorageMsg));
                msg->msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE;
                msg->status = VALKEYMODULE_OK;
                msg->db_id = c->db_id;
                msg->ttl = 0;
                msg->key = createStringObject((char*)c->value, c->vlen);
                msg->value = NULL;
                msg->ram_bytes = 0;
                out[count++] = msg;
                storage_free(c->value);
            }
            comp_ring.tail = (comp_ring.tail + 1) % BRIDGE_COMP_RING;
            continue;
        }

        /* Translate to ValkeyModuleExternalStorageMsg for existing processing */
        ValkeyModuleExternalStorageMsg *msg = zmalloc(sizeof(ValkeyModuleExternalStorageMsg));

        switch (req_ctx->op_type) {
        case STORAGE_OP_PUT:
            msg->msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_WRITE;
            break;
        case STORAGE_OP_GET:
            msg->msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_READ;
            break;
        case STORAGE_OP_DEL:
            msg->msg_type = VALKEYMODULE_EXTERNAL_STORAGE_MSG_TYPE_DELETE;
            break;
        }

        msg->db_id = req_ctx->db_id;
        /* For GET, preserve transient backpressure (read queue full) as a
         * distinct retry signal so the core re-issues the fetch instead of
         * treating it as a genuine miss. STORAGE_NOT_FOUND still maps to ERR. */
        if (req_ctx->op_type == STORAGE_OP_GET &&
            (c->status == STORAGE_ERR_REJECTED || c->status == STORAGE_ERR_FULL)) {
            msg->status = VALKEYMODULE_EXTERNAL_STORAGE_READ_RETRY;
        } else {
            msg->status = (c->status == STORAGE_OK) ? VALKEYMODULE_OK : VALKEYMODULE_ERR;
        }
        msg->ttl = c->expire_ms;
        msg->ram_bytes = c->ram_bytes;   /* carried from IO-thread serialize; debited on completion */

        /* Key: wrap in robj for existing code */
        msg->key = createStringObject(req_ctx->key, sdslen(req_ctx->key));

        /* Value: c->value is an robj* (deserialized by backend's IO thread).
        if (req_ctx->op_type == STORAGE_OP_GET) {
        }
         * Pass it through directly — no wrapping needed. */
        if (req_ctx->op_type == STORAGE_OP_GET && c->status == STORAGE_OK && c->value) {
            msg->value = c->value; /* Already an robj* from backend */
        } else {
            msg->value = req_ctx->value_copy; /* For PUT: the original robj copy */
        }

        out[count++] = msg;

        /* Free key_robj returned by IO thread (for PUT completions) */
        if (c->key != NULL && c->klen > 0) {
            decrRefCount((robj*)c->key);
        } else if (req_ctx->key_robj != NULL && req_ctx->op_type == STORAGE_OP_PUT) {
            /* Module path: key_robj not returned via completion, free from ctx */
            decrRefCount(req_ctx->key_robj);
        }

        /* Cleanup request context */
        if (req_ctx->key_owned) sdsfree(req_ctx->key);
        zfree(req_ctx);

        comp_ring.tail = (comp_ring.tail + 1) % BRIDGE_COMP_RING;
    }

    return count;
}

/* ---------------------------------------------------------------------------
 * Snapshot support passthrough (native backends only; a module-registered
 * backend reports unsupported and the engine keeps its fail-loudly gates).
 * ---------------------------------------------------------------------------*/
int extStorageBridge_snapshotSupported(void) {
    return storageSnapshotSupported();
}

void extStorageBridge_snapshotHold(void) {
    storageSnapshotHold();
}

void extStorageBridge_snapshotRelease(void) {
    storageSnapshotRelease();
}

void extStorageBridge_gcPause(int paused) {
    storageGcPause(paused);
}

/* Fork-child-safe read of a tiered key's serialized DUMP payload.
 * Returns 0 + malloc'd *payload (caller free()s) on success, -1 if absent. */
int extStorageBridge_forkRead(int db_id, sds key, char **payload, size_t *plen) {
    void *val = NULL;
    size_t vlen = 0;
    if (storageForkRead((uint32_t)db_id, key, sdslen(key), &val, &vlen) != STORAGE_OK)
        return -1;
    *payload = (char *)val;
    *plen = vlen;
    return 0;
}

void extStorageBridge_shutdown(void) {
    storageShutdown();

}

int extStorageBridge_flushDB(int db_id) {
    extern void processCompletedStorageRequests(void);
    extern long long total_items_spilling_to_ext_storage;
    extern long long total_items_fetching_from_ext_storage;
    extern void fc_real_drain(uint32_t db_id);

    /* Native backend: use the barrier mechanism (drain + flush on IO thread).
     * fc_real_drain is a no-op if module backend is active (g_fc_ctx==NULL). */
    fc_real_drain((uint32_t)db_id);

    /* Module backend: spin-poll completions until all in-flight ops complete.
     * For native this is also correct (barrier already drained, this is instant). */
    int max_iters = 10000;
    while ((total_items_spilling_to_ext_storage > 0 ||
            total_items_fetching_from_ext_storage > 0) && max_iters-- > 0) {
        storagePollCompletions(64);
        processCompletedStorageRequests();
        if (total_items_spilling_to_ext_storage > 0 ||
            total_items_fetching_from_ext_storage > 0) {
            struct timespec ts = {0, 50000};
            nanosleep(&ts, NULL);
        }
    }
    storagePollCompletions(64);
    processCompletedStorageRequests();
    return 0;
}

/* Drain all in-flight backend IO WITHOUT flushing any data (snapshot
 * prepare). For the real FlashCache backend this is a barrier op on the IO
 * thread; for async backends without a barrier (mock) it is a no-op -- the
 * caller's settle loop polls completions until the in-flight counters
 * reach zero. */
void extStorageBridge_drainOnly(void) {
    extern void fc_real_drain(uint32_t db_id);
    fc_real_drain((uint32_t)-2); /* -2 = drain only, no flush */
    storagePollCompletions(4096);
}

int extStorageBridge_flushAll(void) {
    extern void processCompletedStorageRequests(void);
    extern long long total_items_spilling_to_ext_storage;
    extern long long total_items_fetching_from_ext_storage;
    extern void fc_real_drain(uint32_t db_id);

    fc_real_drain((uint32_t)-1);

    int max_iters = 10000;
    while ((total_items_spilling_to_ext_storage > 0 ||
            total_items_fetching_from_ext_storage > 0) && max_iters-- > 0) {
        storagePollCompletions(64);
        processCompletedStorageRequests();
        if (total_items_spilling_to_ext_storage > 0 ||
            total_items_fetching_from_ext_storage > 0) {
            struct timespec ts = {0, 50000};
            nanosleep(&ts, NULL);
        }
    }
    storagePollCompletions(64);
    processCompletedStorageRequests();
    return 0;
}

/* Propagate MODIFIABLE FC tuning configs to the live FlashCache instance.
 * Called from config.c update callback after CONFIG SET. Works for both
 * native and module backends since FlashCache is a process-global singleton. */
void extStorageBridge_applyFcConfigs(void) {
    if (!extStorageBridge_isReady()) return;
    extern void fc_apply_runtime_configs(void);
    fc_apply_runtime_configs();
}
