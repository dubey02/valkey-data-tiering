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
#include "serialization.h"
#include "flashcache.h"
#include "flashcache_common.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
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

    /* Snapshot support: snap_hold parks the IO worker at the top of its loop
     * (outside all FlashCache code, holding nothing); snap_held acks the
     * park. gc_paused is synced into FlashCache by the worker each loop, so
     * the GC stays off across the whole snapshot-child lifetime while normal
     * reads/writes keep flowing. */
    _Atomic int snap_hold;
    _Atomic int snap_held;
    _Atomic int gc_paused;
    int eviction_enabled;      /* 0 = noeviction policy: GC runs but doesn't delete keys from engine */
    storageCompletionFn completion_fn;
    void *completion_privdata;

    /* Completion overflow list. The completion ring is fixed size, and a
     * dropped completion strands its key in a COPYING_* state forever, which
     * permanently inflates the in-flight tallies the snapshot barrier reads.
     * Parking overflow here instead makes the push lossless. Guarded by
     * comp_ovf_lock (worker appends, main thread drains); comp_ovf_len is
     * atomic so the producer can take the fast ring path without locking. */
    /* Requests refused because the request ring was full. Previously these
     * were dropped silently: the caller had already counted the operation as
     * in flight, so the tally never came back down. */
    size_t req_ring_rejects;

    struct fcRealOverflowNode *comp_ovf_head, *comp_ovf_tail;
    pthread_mutex_t comp_ovf_lock;
    _Atomic long comp_ovf_len;
} fcRealCtx;

typedef struct fcRealOverflowNode {
    storageCompletion c;
    struct fcRealOverflowNode *next;
} fcRealOverflowNode;

/* Times a completion had to be parked because the ring was full. Exposed so a
 * sustained non-zero value is visible rather than silently absorbed. */
long long storage_completion_overflows = 0;

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
/* Lossless push: ring fast path, overflow list when the ring is full or a
 * backlog already exists (order must be preserved, so once anything is parked
 * everything goes to the list until it drains). Never drops.
 *
 * The previous version returned -1 on a full ring and every call site either
 * ignored it or handled it with an empty block, so completions were silently
 * discarded. A discarded PUT completion leaves its key in
 * TIERING_STATE_COPYING_TO_FLASH forever and never decrements
 * total_items_spilling_to_ext_storage, so one burst of spills permanently
 * inflates that tally. The snapshot barrier waits for it to reach zero, which
 * means a single burst disabled snapshotting for the life of the process.
 * Worker-side only. */
static void fc_comp_push(fcRealCtx *ctx, storageCompletion *comp) {
    if (atomic_load_explicit(&ctx->comp_ovf_len, memory_order_acquire) == 0) {
        int head = atomic_load_explicit(&ctx->comp_head, memory_order_relaxed);
        int next = (head + 1) % FC_COMP_RING;
        if (next != atomic_load_explicit(&ctx->comp_tail, memory_order_acquire)) {
            ctx->comp_ring[head] = *comp;
            atomic_store_explicit(&ctx->comp_head, next, memory_order_release);
            return;
        }
    }
    fcRealOverflowNode *n = storage_malloc(sizeof(*n));
    n->c = *comp;
    n->next = NULL;
    pthread_mutex_lock(&ctx->comp_ovf_lock);
    if (ctx->comp_ovf_tail) ctx->comp_ovf_tail->next = n;
    else ctx->comp_ovf_head = n;
    ctx->comp_ovf_tail = n;
    atomic_fetch_add_explicit(&ctx->comp_ovf_len, 1, memory_order_release);
    pthread_mutex_unlock(&ctx->comp_ovf_lock);
    storage_completion_overflows++;
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
    fc_comp_push(g_fc_ctx, &comp);
}

/* ---------------------------------------------------------------------------
 * IO thread — processes requests, calls FlashCache, runs cron
 * ---------------------------------------------------------------------------*/
/* Defined below, next to the rest of the streaming snapshot code. Runs on this
 * thread because every FlashCache call must come from the IO thread. */
static void fc_snap_stream_begin_on_io_thread(void);

static void *fc_io_worker(void *arg) {
    fcRealCtx *ctx = arg;
    pthread_setname_np(pthread_self(), "fc_io_worker");

    while (!atomic_load_explicit(&ctx->shutdown, memory_order_acquire)) {
        /* Snapshot support: park here (a safe point -- no FC call in
         * progress, no partial request) while the main thread forks. */
        if (atomic_load_explicit(&ctx->snap_hold, memory_order_acquire)) {
            atomic_store_explicit(&ctx->snap_held, 1, memory_order_release);
            while (atomic_load_explicit(&ctx->snap_hold, memory_order_acquire) &&
                   !atomic_load_explicit(&ctx->shutdown, memory_order_acquire)) {
                struct timespec hts = {0, 50000};
                nanosleep(&hts, NULL);
            }
            atomic_store_explicit(&ctx->snap_held, 0, memory_order_release);
        }
        /* Sync the GC pause flag into FlashCache (worker owns all FC calls). */
        int gp = atomic_load_explicit(&ctx->gc_paused, memory_order_acquire);
        if (gp != flashcacheGetGcPaused()) flashcacheSetGcPaused(gp);

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
            case STORAGE_OP_SNAPSHOT_START: {
                /* Must run here: flashcacheStartStreamBasedSave quiesces
                 * pending IO and GC internally, which is only safe on the
                 * thread that owns every other FlashCache call. */
                fc_snap_stream_begin_on_io_thread();
                continue;
            }
            case STORAGE_OP_SNAPSHOT_ABORT: {
                flashcacheCancelSave();
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
/* Forward FlashCache log output into the server log.
 *
 * This was a no-op stub, which made every FlashCache assert undiagnosable:
 * flashcacheAssert writes its state dump through this callback and then
 * deliberately crashes (*(char *)-1 = 'x'), so with a no-op logger the crash
 * arrives with no message at all. FC levels 0-3 (DEBUG, VERBOSE, NOTICE,
 * WARNING) match Valkey's LL_* values one to one, so the level passes through.
 * serverLog is a macro over _serverLog in server.h; this file avoids pulling
 * in server.h, so declare the underlying function. Runs on the FC IO thread;
 * _serverLog is already used from bio and module threads, so that is fine. */
extern void _serverLog(int level, const char *fmt, ...);

static void fc_log(int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    _serverLog(level, "%s", buf);
}

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
    atomic_init(&ctx->comp_ovf_len, 0);
    ctx->comp_ovf_head = ctx->comp_ovf_tail = NULL;
    pthread_mutex_init(&ctx->comp_ovf_lock, NULL);
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
    if (fc_req_push(ctx, &req) != 0) {
        ctx->req_ring_rejects++;
        return STORAGE_ERR_REJECTED;
    }
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_real_get_async(void *opaque, uint32_t db_id,
                                        const void *key, size_t klen,
                                        void *request_ctx) {
    (void)klen;
    fcRealCtx *ctx = opaque;
    fcRequest req = {.op = STORAGE_OP_GET, .db_id = db_id, .key_robj = (void*)key,
                     .value_robj = NULL, .expire_ms = 0, .request_ctx = request_ctx};
    if (fc_req_push(ctx, &req) != 0) {
        ctx->req_ring_rejects++;
        return STORAGE_ERR_REJECTED;
    }
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_real_del_async(void *opaque, uint32_t db_id,
                                        const void *key, size_t klen,
                                        void *request_ctx) {
    (void)klen;
    fcRealCtx *ctx = opaque;
    fcRequest req = {.op = STORAGE_OP_DEL, .db_id = db_id, .key_robj = (void*)key,
                     .value_robj = NULL, .expire_ms = 0, .request_ctx = request_ctx};
    if (fc_req_push(ctx, &req) != 0) {
        ctx->req_ring_rejects++;
        return STORAGE_ERR_REJECTED;
    }
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
    /* Drain parked completions. These are strictly newer than the ring's
     * contents, because the push path keeps appending here until the list is
     * empty again, so ring-then-list preserves order. */
    while (count < max && atomic_load_explicit(&ctx->comp_ovf_len, memory_order_acquire) > 0) {
        pthread_mutex_lock(&ctx->comp_ovf_lock);
        fcRealOverflowNode *n = ctx->comp_ovf_head;
        if (n) {
            ctx->comp_ovf_head = n->next;
            if (!ctx->comp_ovf_head) ctx->comp_ovf_tail = NULL;
            atomic_fetch_sub_explicit(&ctx->comp_ovf_len, 1, memory_order_release);
        }
        pthread_mutex_unlock(&ctx->comp_ovf_lock);
        if (!n) break;
        if (ctx->completion_fn) ctx->completion_fn(&n->c, ctx->completion_privdata);
        storage_free(n);
        count++;
    }
    return count;
}

static int fc_real_cron(void *opaque) {
    (void)opaque;
    return 0; /* cron runs on IO thread, not here */
}

/* ---------------------------------------------------------------------------
 * Snapshot support (see storage.h)
 * ---------------------------------------------------------------------------*/
static void fc_real_snapshot_hold(void *vctx) {
    fcRealCtx *ctx = vctx;
    atomic_store_explicit(&ctx->snap_hold, 1, memory_order_release);
    while (!atomic_load_explicit(&ctx->snap_held, memory_order_acquire)) {
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
}

static void fc_real_snapshot_release(void *vctx) {
    fcRealCtx *ctx = vctx;
    atomic_store_explicit(&ctx->snap_hold, 0, memory_order_release);
}

static void fc_real_gc_pause(void *vctx, int paused) {
    fcRealCtx *ctx = vctx;
    atomic_store_explicit(&ctx->gc_paused, paused, memory_order_release);
}

/* Synchronous fork-child-safe read. Reads the full serialized item via
 * FlashCache's pread path (no async ring), then extracts the value bytes.
 * The returned buffer uses storage_malloc (zmalloc in-tree; safe in the fork
 * child -- its memory accounting is CoW-private, same as rdbSaveObject). */
static storageStatus fc_real_fork_read(void *vctx, uint32_t db_id,
                                       const void *key, size_t klen,
                                       void **value, size_t *vlen) {
    (void)vctx;
    char *item = NULL;
    size_t item_len = 0;
    if (flashcacheForkChildReadItem(db_id, (char const *)key, klen,
                                    &item, &item_len) != FC_OK)
        return STORAGE_NOT_FOUND;
    char *val = NULL;
    size_t val_len = 0;
    extractValueFromSerializedItem(item, &val, &val_len);
    if (val == NULL || val_len == 0) {
        free(item);
        return STORAGE_NOT_FOUND;
    }
    char *copy = storage_malloc(val_len);
    if (copy == NULL) {
        free(item); /* from flashcacheForkChildReadItem (plain malloc) */
        return STORAGE_NOT_FOUND;
    }
    memcpy(copy, val, val_len);
    free(item); /* from flashcacheForkChildReadItem (plain malloc) */
    *value = copy;
    *vlen = val_len;
    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * Streaming snapshot (real FlashCache).
 *
 * FlashCache already produces a point in time stream via
 * flashcacheStartStreamBasedSave. Two details drive this implementation:
 *
 *  1. SAVE TYPE MUST BE BGSAVE, NOT THREADSAVE. FlashCache's expedite path is
 *     what preserves the pre-cut bytes of a record that gets overwritten,
 *     deleted, or GC relocated before the snapshot cursor reaches it. In
 *     threadsave mode (writer != NULL AND save type == THREADSAVE) expedite
 *     degrades to a counter and DROPS the bytes, because replication carries
 *     the mutation to the replica instead. Passing FC_SAVE_TYPE_BGSAVE with a
 *     writer gives us stream transport with strict point in time semantics.
 *
 *  2. THE STREAM IS BYTES, THE SINK IS RECORDS. FlashCache hands us page
 *     aligned chunks of a logical snapshot file: a metadata block followed by
 *     serialized items (40 byte itemHeader, key, value). Chunk boundaries fall
 *     wherever they like, so items split across chunks and we reassemble.
 * ---------------------------------------------------------------------------*/

/* Byte offset of data_section_start_offset within FlashCache's snapshot
 * metadata block. The struct is private to snapshot_version_two.c, but its
 * prefix is stable and asserted there: uint32_t version, uint32_t
 * num_databases, size_t data_section_start_offset. We read just that field and
 * sanity check it rather than mirroring the whole struct. */
#define FC_SNAP_META_MIN_BYTES   16
#define FC_SNAP_META_DATA_START  8

typedef struct fcSnapStream {
    storageSnapshotSink sink;
    /* FlashCache stores the writer BY POINTER for the life of the snapshot,
     * so it must not live on a stack frame. */
    flashcacheSnapshotWriter writer;
    flashcacheSnapshotSecret secret;

    _Atomic int active;
    _Atomic int abort_requested;
    int seen_meta;
    int saw_eof;
    int failed;

    size_t data_start;      /* first byte of the item section */
    size_t next_offset;     /* offset we expect the next chunk to start at */

    char *buf;              /* reassembly buffer, holds one partial item */
    size_t len, cap;

    size_t records;         /* records handed to the sink */
    size_t skipped;         /* non key/value markers skipped */
} fcSnapStream;

/* One snapshot at a time, matching FlashCache's own single snapshot state. */
static fcSnapStream g_snap;

static int fc_snap_buf_append(fcSnapStream *s, const char *src, size_t n) {
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : 8192;
        while (cap < s->len + n) cap *= 2;
        char *nb = storage_realloc(s->buf, cap);
        if (!nb) return 0;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    return 1;
}

/* Drop the first n bytes of the reassembly buffer. */
static void fc_snap_buf_consume(fcSnapStream *s, size_t n) {
    if (n >= s->len) { s->len = 0; return; }
    memmove(s->buf, s->buf + n, s->len - n);
    s->len -= n;
}

/* Parse as many whole items out of the buffer as possible, emitting records.
 * Leaves any trailing partial item in place for the next chunk. */
static void fc_snap_parse(fcSnapStream *s) {
    /* Metadata first: learn where the item section begins, then skip it. */
    if (!s->seen_meta) {
        if (s->len < FC_SNAP_META_MIN_BYTES) return;
        size_t data_start = 0;
        memcpy(&data_start, s->buf + FC_SNAP_META_DATA_START, sizeof(data_start));
        /* The metadata block is page aligned and asserted to fit one page, so
         * anything outside these bounds means the layout moved under us. Fail
         * loudly rather than misparse the item stream. */
        if (data_start == 0 || (data_start % FC_PAGESIZE) != 0 ||
            data_start > (size_t)FC_PAGESIZE * 8) {
            s->failed = 1;
            return;
        }
        s->data_start = data_start;
        s->seen_meta = 1;
    }

    /* Skip whatever remains of the metadata block. */
    if (s->data_start > 0) {
        size_t drop = s->len < s->data_start ? s->len : s->data_start;
        fc_snap_buf_consume(s, drop);
        s->data_start -= drop;
        if (s->data_start > 0) return; /* still inside metadata */
    }

    while (!s->saw_eof && !s->failed) {
        if (s->len < FC_ITEM_HEADER_LEN) return;

        uint32_t flag = 0, dbid = 0;
        size_t key_len = 0, value_len = 0;
        memcpy(&flag,      s->buf + 4,  sizeof(flag));
        memcpy(&key_len,   s->buf + 8,  sizeof(key_len));
        memcpy(&value_len, s->buf + 16, sizeof(value_len));
        memcpy(&dbid,      s->buf + 24, sizeof(dbid));

        /* Items in the FC log (and therefore in the snapshot stream) are
         * padded to FC_ITEM_ALIGNMENT_BYTES (8): serializeKeyValuePair
         * allocates getAlignedSizeBytes(header+key+value), and
         * extractTotalLenFromSerializedItem returns the ALIGNED size. Consume
         * the aligned size or the parser desyncs on the padding after the
         * first record whose payload is not a multiple of 8. */
        size_t total = FC_ITEM_HEADER_LEN + key_len + value_len;
        total = (total + 7) & ~(size_t)7;
        /* A wildly out of range length means we lost stream alignment. Stop
         * instead of walking off into the buffer. */
        if (total < FC_ITEM_HEADER_LEN || key_len > (size_t)1 << 30 ||
            value_len > (size_t)1 << 40) {
            s->failed = 1;
            return;
        }
        if (s->len < total) return; /* wait for the rest of this item */

        if (flag & FC_EOF_INDICATOR) {
            s->saw_eof = 1;
            /* The snapshot file is page aligned, so padding can trail the EOF
             * marker. Nothing after EOF is data: drop the whole buffer, and
             * the write callback ignores any further chunks. */
            s->len = 0;
            return;
        }
        if (flag & (FC_SKIP_SEGMENT | FC_REPL_CMD_DELETE)) {
            /* Skip markers pad the log to a page boundary; delete commands are
             * a threadsave replication construct and should not appear under
             * FC_SAVE_TYPE_BGSAVE. Neither is a record. */
            s->skipped++;
            fc_snap_buf_consume(s, total);
            continue;
        }
        if (key_len == 0 || value_len == 0) { /* nothing to hand up */
            s->skipped++;
            fc_snap_buf_consume(s, total);
            continue;
        }

        /* No writable() gate here on purpose. Leaving an assembled item
         * buffered depends on FlashCache coming back to poll again, and it does
         * not: it aborts the snapshot the first time the sink reports full.
         * Backpressure lives entirely in the sink's bounded overflow now. */
        s->sink.on_record(s->sink.privdata, dbid,
                          s->buf + FC_ITEM_HEADER_LEN, key_len,
                          s->buf + FC_ITEM_HEADER_LEN + key_len, value_len);
        s->records++;
        fc_snap_buf_consume(s, total);
    }
}

/* --- FlashCache writer trampolines. All run on the FC IO thread. --- */

static void fc_snap_w_set_size(void *cctx, size_t size) {
    fcSnapStream *s = cctx;
    if (s->sink.set_size_hint) s->sink.set_size_hint(s->sink.privdata, size);
}

static int fc_snap_w_is_writable(void *cctx) {
    fcSnapStream *s = cctx;
    if (atomic_load_explicit(&s->abort_requested, memory_order_acquire) || s->failed)
        return 0;
    /* Report unwritable while an item is still buffered for backpressure, so
     * FlashCache does not pile more on top of a sink that already said no. */
    return s->sink.writable(s->sink.privdata);
}

static void fc_snap_w_write(void *cctx, size_t offset, char *buf, size_t buf_len) {
    fcSnapStream *s = cctx;
    if (s->failed) return;
    /* Everything after the EOF marker is page padding, not data. Track the
     * expected offset so the in-order check below stays satisfied. */
    if (s->saw_eof) {
        if (offset == s->next_offset) s->next_offset = offset + buf_len;
        return;
    }
    /* The logical snapshot file is written strictly in order. If that ever
     * stops holding, reassembly by append is wrong, so check it. */
    if (offset != s->next_offset) {
        s->failed = 1;
        return;
    }
    s->next_offset = offset + buf_len;
    if (!fc_snap_buf_append(s, buf, buf_len)) {
        s->failed = 1;
        return;
    }
    fc_snap_parse(s);
}

static void fc_snap_w_keep_alive(void *cctx) {
    (void)cctx; /* replication link keepalive; unused for a local save */
}

static void fc_snap_w_complete(void *cctx, int completed) {
    fcSnapStream *s = cctx;
    /* Anything still buffered at completion is a partial item, which means the
     * stream ended mid record. Treat as failure. */
    int ok = completed && !s->failed &&
             !atomic_load_explicit(&s->abort_requested, memory_order_acquire) &&
             s->len == 0;
    s->sink.complete(s->sink.privdata, ok);
    storage_free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
    atomic_store_explicit(&s->active, 0, memory_order_release);
}

/* Called on the IO thread in response to STORAGE_OP_SNAPSHOT_START. */
static void fc_snap_stream_begin_on_io_thread(void) {
    fcSnapStream *s = &g_snap;
    s->writer.callback_context = s;
    s->writer.set_snapshot_size = fc_snap_w_set_size;
    s->writer.write = fc_snap_w_write;
    s->writer.is_writable = fc_snap_w_is_writable;
    s->writer.keep_alive = fc_snap_w_keep_alive;
    s->writer.complete = fc_snap_w_complete;
    /* FlashCache asserts (size > 0 && size <= FC_SNAPSHOT_MAX_SECRET_SIZE)
     * unconditionally at snapshotV2StartSave -- an empty secret crashes the
     * process. The secret exists for RDB/FDB pair correlation in the two file
     * design; our stream lands in a single artifact, so any non empty value
     * satisfies the contract. Use a random value rather than a constant so a
     * future consumer cannot accidentally rely on it matching across saves. */
    s->secret.size = 16;
    for (size_t i = 0; i < s->secret.size; i++)
        s->secret.secret[i] = (unsigned char)(rand() & 0xff);
    flashcacheStartStreamBasedSave(&s->secret, &s->writer,
                                   FC_SNAPSHOT_VERSION_TWO,
                                   FC_SAVE_TYPE_BGSAVE, NULL);
}

static storageStatus fc_real_snapshot_stream_start(void *vctx, storageSnapshotSink *sink) {
    fcRealCtx *ctx = vctx;
    if (!ctx) return STORAGE_ERR_REJECTED;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_snap.active, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
        return STORAGE_ERR_REJECTED; /* one snapshot at a time */

    /* Reset parse state before the IO thread can observe it. */
    g_snap.sink = *sink;
    atomic_store_explicit(&g_snap.abort_requested, 0, memory_order_relaxed);
    g_snap.seen_meta = g_snap.saw_eof = g_snap.failed = 0;
    g_snap.data_start = 0;
    g_snap.next_offset = 0;
    g_snap.len = 0;
    g_snap.records = g_snap.skipped = 0;

    fcRequest req = {.op = STORAGE_OP_SNAPSHOT_START};
    fc_req_push(ctx, &req);
    return STORAGE_OK;
}

static void fc_real_snapshot_stream_abort(void *vctx) {
    fcRealCtx *ctx = vctx;
    if (!ctx) return;
    if (!atomic_load_explicit(&g_snap.active, memory_order_acquire)) return;
    atomic_store_explicit(&g_snap.abort_requested, 1, memory_order_release);
    fcRequest req = {.op = STORAGE_OP_SNAPSHOT_ABORT};
    fc_req_push(ctx, &req);
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
    .snapshot_hold = fc_real_snapshot_hold,
    .snapshot_release = fc_real_snapshot_release,
    .gc_pause = fc_real_gc_pause,
    .fork_read = fc_real_fork_read,
    .snapshot_stream_start = fc_real_snapshot_stream_start,
    .snapshot_stream_abort = fc_real_snapshot_stream_abort,
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

/* Named metric accessors.
 *
 * fc_get_metric takes a raw int, so callers outside this file had to pass
 * integer literals with a comment claiming which metric they meant. Every one
 * of those literals had drifted from the enum as FlashCache grew it, with no
 * compiler check to catch it. These accessors keep the enum symbol on the side
 * of the boundary that can see it, so a future insertion into the enum is a
 * recompile rather than a silently mislabelled INFO field. */
#define FC_DEFINE_METRIC_ACCESSOR(fn, sym)                       \
    size_t fn(void) {                                            \
        if (!g_fc_ctx) return 0;                                 \
        return flashcacheGetCountBasedMetric(sym);               \
    }

FC_DEFINE_METRIC_ACCESSOR(fc_metric_items_evicted,        FC_NUM_ITEMS_EVICTED)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_evicted_bytes,        FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_disk_write_bytes,     FC_TOTAL_DISK_WRITE_BYTES)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_disk_read_bytes,      FC_TOTAL_DISK_READ_BYTES)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_num_disk_writes,      FC_NUM_DISK_WRITE)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_num_disk_reads,       FC_NUM_DISK_READ)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_reads_in_flight,      FC_NUM_READ_IN_FLIGHT)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_active_memory_bytes,  FC_ACTIVE_MEMORY_SIZE)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_retryable_disk_errs,  FC_NUM_RETRYABLE_DISK_ERROR)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_evicting_under_max,   FC_IS_EVICTING_UNDER_MAX_LOGSIZE)
FC_DEFINE_METRIC_ACCESSOR(fc_metric_evicted_under_max,    FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE)

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
