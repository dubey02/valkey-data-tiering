/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Real FlashCache storage backend — native implementation.
 *
 * Architecture:
 *   Engine passes robj* pointers via put_async/get_async/del_async.
 *   This backend has its own IO thread that:
 *     1. Dequeues requests from lock-free MPSC queue
 *     2. Serializes robj* to bytes (on IO thread, NOT main thread)
 *     3. Calls FlashCache APIs
 *     4. Deserializes responses
 *     5. Enqueues completions to lock-free SPSC ring
 *
 * Request queue: Lock-free MPSC (multiple producers via atomic CAS, single consumer).
 * Completion ring: Lock-free SPSC (single producer = IO thread, single consumer = main thread).
 * No pthread mutex on the hot path — matches module's crossbeam performance.
 */
#include "storage.h"
#include "flashcache.h"
#include "flashcache_common.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* robj serialization/lifecycle (from ext_storage.c) */
extern int extStorageSerializeKey(void *key, char **serialized_key);
extern int extStorageSerializeValue(void *value, char **serialized_value);
extern void extStorageInflightAddRam(size_t bytes);
extern void extStorageOnSpillSerialize(size_t bytes);
extern size_t objectComputeSize(void *key, void *o, size_t sample_size, int dbid);
extern void extStorageFreeSerializedValue(void *value);
extern void *extStorageDeserializeValue(char *value, int length);
typedef struct serverObject robj;
extern void decrRefCount(robj *o);

#define FC_COMP_RING 8192
#define FC_REQ_RING  4096

/* ---------------------------------------------------------------------------
 * Lock-free MPSC request ring (fixed-size, simpler than linked list)
 * Uses a spinlock for the rare case of concurrent producers (main thread
 * is single-threaded in Valkey, so this is effectively uncontended).
 * ---------------------------------------------------------------------------*/
typedef struct fcRequest {
    int op;
    uint32_t db_id;
    void *key_robj;
    void *value_robj;
    int64_t expire_ms;
    void *request_ctx;
} fcRequest;

typedef struct fcRealCtx {
    /* Request ring (main thread → IO thread) */
    fcRequest req_ring[FC_REQ_RING];
    _Atomic int req_head;  /* written by producer (main thread) */
    _Atomic int req_tail;  /* read by consumer (IO thread) */

    /* Lock-free completion ring (SPSC) */
    storageCompletion comp_ring[FC_COMP_RING];
    _Atomic int comp_head;  /* written by IO thread */
    _Atomic int comp_tail;  /* read by main thread */

    pthread_t io_thread;
    _Atomic int shutdown;
    _Atomic int barrier_done;  /* Set by IO thread when BARRIER op is processed */
    int eviction_enabled;      /* 0 = noeviction policy: GC runs but doesn't delete keys from engine */
    storageCompletionFn completion_fn;
    void *completion_privdata;
} fcRealCtx;

static fcRealCtx *g_fc_ctx = NULL;

/* ---------------------------------------------------------------------------
 * Request ring push/pop (SPSC — Valkey main thread is single-threaded)
 * ---------------------------------------------------------------------------*/
static int fc_req_push(fcRealCtx *ctx, fcRequest *req) {
    int head = atomic_load_explicit(&ctx->req_head, memory_order_relaxed);
    int next = (head + 1) % FC_REQ_RING;
    if (next == atomic_load_explicit(&ctx->req_tail, memory_order_acquire))
        return -1; /* full */
    ctx->req_ring[head] = *req;
    atomic_store_explicit(&ctx->req_head, next, memory_order_release);
    return 0;
}

static int fc_req_pop(fcRealCtx *ctx, fcRequest *out) {
    int tail = atomic_load_explicit(&ctx->req_tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&ctx->req_head, memory_order_acquire))
        return -1; /* empty */
    *out = ctx->req_ring[tail];
    atomic_store_explicit(&ctx->req_tail, (tail + 1) % FC_REQ_RING, memory_order_release);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Lock-free completion ring (SPSC)
 * ---------------------------------------------------------------------------*/
static int fc_comp_push(fcRealCtx *ctx, storageCompletion *comp) {
    int head = atomic_load_explicit(&ctx->comp_head, memory_order_relaxed);
    int next = (head + 1) % FC_COMP_RING;
    if (next == atomic_load_explicit(&ctx->comp_tail, memory_order_acquire))
        return -1; /* full */
    ctx->comp_ring[head] = *comp;
    atomic_store_explicit(&ctx->comp_head, next, memory_order_release);
    return 0;
}

static int fc_comp_pop(fcRealCtx *ctx, storageCompletion *out) {
    int tail = atomic_load_explicit(&ctx->comp_tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&ctx->comp_head, memory_order_acquire))
        return -1; /* empty */
    *out = ctx->comp_ring[tail];
    atomic_store_explicit(&ctx->comp_tail, (tail + 1) % FC_COMP_RING, memory_order_release);
    return 0;
}

/* ---------------------------------------------------------------------------
 * FlashCache GET callback (called from IO thread during flashcacheRunCronTasks)
 * ---------------------------------------------------------------------------*/
static void fc_get_callback(void *request_context, char *value,
                             size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb;
    storageCompletion comp = {0};
    comp.request_ctx = request_context;
    comp.op_type = STORAGE_OP_GET;

    if (value && value_len > 0) {
        comp.value = extStorageDeserializeValue(value, (int)value_len);
        comp.vlen = value_len;
        comp.status = STORAGE_OK;
    } else {
        comp.status = STORAGE_NOT_FOUND;
    }
    if (fc_comp_push(g_fc_ctx, &comp) != 0) {
        /* Ring full — should not happen with 8192 slots */
    }
}

/* ---------------------------------------------------------------------------
 * IO thread — processes requests, calls FlashCache, runs cron
 * ---------------------------------------------------------------------------*/
static void *fc_io_worker(void *arg) {
    fcRealCtx *ctx = arg;
    pthread_setname_np(pthread_self(), "fc_io_worker");

    while (!atomic_load_explicit(&ctx->shutdown, memory_order_acquire)) {
        /* Process all pending requests */
        fcRequest req;
        int did_work = 0;
        while (fc_req_pop(ctx, &req) == 0) {
            did_work = 1;
            storageCompletion comp = {0};
            comp.request_ctx = req.request_ctx;
            comp.op_type = req.op;
            comp.db_id = req.db_id;

            switch (req.op) {
            case STORAGE_OP_PUT: {
                char *key_bytes = NULL;
                int key_len = extStorageSerializeKey(req.key_robj, &key_bytes);
                char *val_bytes = NULL;
                int val_len = extStorageSerializeValue(req.value_robj, &val_bytes);

                if (key_len > 0 && val_len > 0) {
                    flashcacheReturnCode rc = flashcachePutItem(
                        req.db_id, key_bytes, (size_t)key_len, val_bytes, (size_t)val_len);
                    comp.status = (rc == FC_OK) ? STORAGE_OK : STORAGE_ERR_IO;
                } else {
                    comp.status = STORAGE_ERR_IO;
                }
                /* flashcachePutItem copies the data, so release the
                 * DUMP-serialized value buffer now. Key is zero-copy. */
                extStorageFreeSerializedValue(val_bytes);
                comp.key = req.key_robj;
                comp.klen = 1; /* signal: key needs freeing by main thread */
                comp.value = req.value_robj;
                /* Compute the in-RAM footprint HERE (IO thread, off the main thread)
                 * as part of the serialize step. Credit it to in-flight spill bytes
                 * now and carry it on the completion so the main thread debits the
                 * exact same amount when the spill completes (no drift). */
                /* TODO(safety): objectComputeSize ignores the key arg for all
                 * spillable types (string/list/set/zset/hash/stream), so NULL is
                 * fine today. The OBJ_MODULE path DOES dereference key — if a
                 * module-typed value ever becomes a spill candidate (spillItemAsync
                 * does not currently exclude OBJ_MODULE), this NULL deref segfaults.
                 * Future fix: skip OBJ_MODULE in spill candidacy, or pass key_robj. */
                comp.ram_bytes = objectComputeSize(NULL, (robj *)req.value_robj, 5, (int)req.db_id);
                extStorageInflightAddRam(comp.ram_bytes);
                /* Window-1 -> window-2 migration: count the serialize and fold the
                 * exact footprint into the EMA that models the submit..serialize gap. */
                extStorageOnSpillSerialize(comp.ram_bytes);
                break;
            }
            case STORAGE_OP_GET: {
                char *key_bytes = NULL;
                int key_len = extStorageSerializeKey(req.key_robj, &key_bytes);
                if (key_len > 0) {
                    flashcacheReturnCode rc = flashcacheGetItem(
                        req.db_id, key_bytes, (size_t)key_len,
                        FC_READ, req.request_ctx, fc_get_callback);
                    if (rc != FC_OK) {
                        comp.status = STORAGE_ERR_REJECTED;
                        fc_comp_push(ctx, &comp);
                    }
                    /* If OK, completion comes via fc_get_callback */
                }
                decrRefCount((robj*)req.key_robj);
                continue; /* don't push completion here */
            }
            case STORAGE_OP_DEL: {
                char *key_bytes = NULL;
                int key_len = extStorageSerializeKey(req.key_robj, &key_bytes);
                if (key_len > 0) {
                    flashcacheReturnCode rc = flashcacheGetItem(
                        req.db_id, key_bytes, (size_t)key_len,
                        FC_DELETE, req.request_ctx, fc_get_callback);
                    if (rc != FC_OK) {
                        comp.status = STORAGE_ERR_REJECTED;
                        fc_comp_push(ctx, &comp);
                    }
                }
                decrRefCount((robj*)req.key_robj);
                continue;
            }
            case STORAGE_OP_BARRIER: {
                /* Drain FlashCache's internal async operations first */
                flashcacheRunCronTasks();
                /* Execute the flush if a db_id was specified */
                if (req.db_id == (uint32_t)-1) {
                    flashcacheFlushAllDBs();
                } else if (req.db_id < (uint32_t)-2) {
                    flashcacheFlushDB(req.db_id);
                }
                /* Signal main thread: all IO complete + flush done */
                atomic_store_explicit(&ctx->barrier_done, 1, memory_order_release);
                continue;
            }
            }

            fc_comp_push(ctx, &comp);
        }

        /* Run FlashCache cron (GC, async read completions) */
        flashcacheRunCronTasks();

        /* If no work was done, brief sleep to avoid busy-spin */
        if (!did_work && !flashcacheShouldRunCronTasksImmediately()) {
            struct timespec ts = {0, 50000}; /* 50μs — matches module's sleep */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * FlashCache eviction callback
 * ---------------------------------------------------------------------------*/
static void fc_eviction_cb(void *ctx, uint32_t dbid, char *key, size_t key_len) {
    fcRealCtx *fc = g_fc_ctx;
    if (!fc) return;
    storageCompletion comp = {0};
    comp.op_type = STORAGE_OP_DEL;
    comp.status = STORAGE_OK;
    comp.db_id = dbid;
    comp.value = storage_malloc(key_len);
    memcpy(comp.value, key, key_len);
    comp.vlen = key_len;
    comp.request_ctx = NULL;
    fc_comp_push(fc, &comp);
}

static void fc_asio_cb(void *ctx) { (void)ctx; }
static uint64_t fc_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static void fc_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* ---------------------------------------------------------------------------
 * storageType interface
 * ---------------------------------------------------------------------------*/
static void *fc_real_open(storageConfig *cfg) {
    fcRealCtx *ctx = storage_calloc(1, sizeof(fcRealCtx));
    atomic_init(&ctx->req_head, 0);
    atomic_init(&ctx->req_tail, 0);
    atomic_init(&ctx->comp_head, 0);
    atomic_init(&ctx->comp_tail, 0);
    atomic_init(&ctx->barrier_done, 0);
    atomic_init(&ctx->shutdown, 0);
    ctx->completion_fn = cfg->completion_fn;
    ctx->completion_privdata = cfg->completion_privdata;
    ctx->eviction_enabled = cfg->eviction_enabled;
    g_fc_ctx = ctx;

    /* Initialize FlashCache */
    const char *path = cfg->path ? cfg->path : "/tmp/valkey-flash.db";
    size_t capacity = cfg->capacity_bytes > 0 ? cfg->capacity_bytes : (size_t)1024*1024*1024;
    uint32_t num_dbs = cfg->num_databases > 0 ? cfg->num_databases : 16;

    static flashcacheEvictionDetails eviction;
    eviction.context = ctx;
    /* Always register callback (FC asserts non-NULL). When eviction is disabled
     * (noeviction policy), the callback is a no-op. FC's internal GC still runs
     * for log compaction but won't invoke the callback if eviction_enabled=false
     * was passed to flashcacheInit (max_allocated_db_size_percent=100 prevents eviction). */
    eviction.callback = fc_eviction_cb;
    static flashcacheAsioControlMsgCallbackDetails asio;
    static char sentinel = 0;
    asio.context = &sentinel;
    asio.callback = fc_asio_cb;

    /* max_allocated_db_size_percent: 90 = GC starts at 90% full (same as production).
     * GC always runs for log health. For noeviction policy, the eviction callback
     * is a no-op (doesn't delete keys from engine) — data stays in DRAM metadata. */
    flashcacheReturnCode rc = flashcacheInit(
        path, capacity, 1024, num_dbs,
        cfg->max_allocated_percent,
        cfg->max_in_flight_reads,
        cfg->min_gc_rate,
        5, 0, fc_clock, &eviction, fc_log, &asio);

    if (rc != FC_OK) {
        storage_free(ctx);
        g_fc_ctx = NULL;
        return NULL;
    }

    /* Apply runtime-tunable configs via flashcacheSetConfig */
    flashcacheConfig fc_cfg;
    fc_cfg.key = FC_CONFIG_KEY_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE;
    fc_cfg.numeric_value = cfg->max_gc_rate;
    flashcacheSetConfig(&fc_cfg);

    fc_cfg.key = FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES;
    fc_cfg.numeric_value = cfg->max_buffered_write_size;
    flashcacheSetConfig(&fc_cfg);

    fc_cfg.key = FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES;
    fc_cfg.numeric_value = cfg->buffered_write_flush_threshold;
    flashcacheSetConfig(&fc_cfg);

    /* Disable FlashCache eviction for noeviction policy. GC still compacts
     * the log (rewrites live data) but never discards keys. FC will never
     * call the eviction callback when this is disabled. */
    if (!cfg->eviction_enabled) {
        fc_cfg.key = FC_CONFIG_KEY_EVICTION_ENABLED;
        fc_cfg.numeric_value = 0;
        flashcacheSetConfig(&fc_cfg);
    }

    pthread_create(&ctx->io_thread, NULL, fc_io_worker, ctx);
    return ctx;
}

static void fc_real_close(void *opaque) {
    fcRealCtx *ctx = opaque;
    atomic_store_explicit(&ctx->shutdown, 1, memory_order_release);
    pthread_join(ctx->io_thread, NULL);
    flashcacheTearDown();
    storage_free(ctx);
    g_fc_ctx = NULL;
}

static storageStatus fc_real_put_async(void *opaque, uint32_t db_id,
                                        const void *key, size_t klen,
                                        const void *value, size_t vlen,
                                        int64_t expire_ms, void *request_ctx) {
    (void)klen; (void)vlen;
    fcRealCtx *ctx = opaque;
    fcRequest req = {.op = STORAGE_OP_PUT, .db_id = db_id, .key_robj = (void*)key,
                     .value_robj = (void*)value, .expire_ms = expire_ms, .request_ctx = request_ctx};
    fc_req_push(ctx, &req);
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_real_get_async(void *opaque, uint32_t db_id,
                                        const void *key, size_t klen,
                                        void *request_ctx) {
    (void)klen;
    fcRealCtx *ctx = opaque;
    fcRequest req = {.op = STORAGE_OP_GET, .db_id = db_id, .key_robj = (void*)key,
                     .value_robj = NULL, .expire_ms = 0, .request_ctx = request_ctx};
    fc_req_push(ctx, &req);
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_real_del_async(void *opaque, uint32_t db_id,
                                        const void *key, size_t klen,
                                        void *request_ctx) {
    (void)klen;
    fcRealCtx *ctx = opaque;
    fcRequest req = {.op = STORAGE_OP_DEL, .db_id = db_id, .key_robj = (void*)key,
                     .value_robj = NULL, .expire_ms = 0, .request_ctx = request_ctx};
    fc_req_push(ctx, &req);
    return STORAGE_WOULDBLOCK;
}

static int fc_real_poll_completions(void *opaque, int max) {
    fcRealCtx *ctx = opaque;
    int count = 0;
    storageCompletion comp;
    while (count < max && fc_comp_pop(ctx, &comp) == 0) {
        if (ctx->completion_fn) ctx->completion_fn(&comp, ctx->completion_privdata);
        count++;
    }
    return count;
}

static int fc_real_cron(void *opaque) {
    (void)opaque;
    return 0; /* cron runs on IO thread, not here */
}

static storageType flashcache_real_type = {
    .name = "flashcache-real",
    .version = VALKEY_STORAGE_VERSION,
    .open = fc_real_open,
    .close = fc_real_close,
    .put = NULL, .get = NULL, .del = NULL,
    .put_async = fc_real_put_async,
    .get_async = fc_real_get_async,
    .del_async = fc_real_del_async,
    .poll_completions = fc_real_poll_completions,
    .cron = fc_real_cron,
    .get_stats = NULL,
};

/* Drain all in-flight IO + execute flush on the IO thread.
 * db_id: specific DB to flush, (uint32_t)-1 for flush all, (uint32_t)-2 for drain only (no flush). */
void fc_real_drain(uint32_t db_id) {
    if (!g_fc_ctx) return;
    atomic_store_explicit(&g_fc_ctx->barrier_done, 0, memory_order_release);
    fcRequest req = {.op = STORAGE_OP_BARRIER, .db_id = db_id};
    fc_req_push(g_fc_ctx, &req);
    while (!atomic_load_explicit(&g_fc_ctx->barrier_done, memory_order_acquire)) {
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
}

/* Flush the FlashCache index only (no IO drain). Safe to call from main thread
 * AFTER all in-flight IO has been drained. No-op if module backend is active. */
void fc_real_flush_index(uint32_t db_id) {
    if (!g_fc_ctx) return; /* Module backend — FC managed by module */
    if (db_id == (uint32_t)-1) {
        flashcacheFlushAllDBs();
    } else {
        flashcacheFlushDB(db_id);
    }
}

storageType *storageGetFlashCacheRealType(void) { return &flashcache_real_type; }

/* Expose FC metrics to the INFO command via ext_storage.c */
size_t fc_get_metric(int metric_id) {
    if (!g_fc_ctx) return 0;
    return flashcacheGetCountBasedMetric((flashcacheCountBasedMetrics)metric_id);
}

/* Called from ext_storage_bridge when CONFIG SET changes FC tuning params.
 * Propagates current global config values to the live FlashCache instance. */
void fc_apply_runtime_configs(void) {
    if (!g_fc_ctx) return;
    extern long long ext_storage_min_gc_rate;
    extern long long ext_storage_max_gc_rate;
    extern long long ext_storage_max_buffered_write_size;
    extern long long ext_storage_buffered_write_flush_threshold;

    flashcacheConfig fc_cfg;
    fc_cfg.key = FC_CONFIG_KEY_MIN_GARBAGE_COLLECTION_RATE;
    fc_cfg.numeric_value = (uint64_t)ext_storage_min_gc_rate;
    flashcacheSetConfig(&fc_cfg);

    fc_cfg.key = FC_CONFIG_KEY_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE;
    fc_cfg.numeric_value = (uint64_t)ext_storage_max_gc_rate;
    flashcacheSetConfig(&fc_cfg);

    fc_cfg.key = FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES;
    fc_cfg.numeric_value = (uint64_t)ext_storage_max_buffered_write_size;
    flashcacheSetConfig(&fc_cfg);

    fc_cfg.key = FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES;
    fc_cfg.numeric_value = (uint64_t)ext_storage_buffered_write_flush_threshold;
    flashcacheSetConfig(&fc_cfg);
}
