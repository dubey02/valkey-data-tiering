/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "storage.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* robj is defined in server.h; forward-declare it here to avoid pulling all of server.h */
typedef struct serverObject robj;

/* Extern declarations for server functions needed by the IO thread */
extern void decrRefCount(robj *o);

/* In-memory hash table simulating FlashCache with async IO */
#define FC_BUCKETS 4096
#define FC_COMP_RING 4096

typedef struct fcEntry {
    void *key; size_t klen;
    void *value; size_t vlen;
    int64_t expire_ms;
    uint32_t db_id;
    struct fcEntry *next;
} fcEntry;

typedef struct fcRequest {
    int op;
    uint32_t db_id;
    void *key;    /* robj* for key */
    void *value;  /* robj* for value (PUT only) */
    int64_t expire_ms;
    void *request_ctx;
    struct fcRequest *next;
} fcRequest;

/* One record captured at the snapshot cut. The bytes are COPIED at start
 * time: that copy is what makes the cut a point in time. A later PUT or DEL
 * mutates the live hashtable and cannot disturb an already captured record,
 * which is how the mock discharges the same obligation the real backend meets
 * with its expedite path. */
typedef struct fcSnapRec {
    uint32_t db_id;
    char *key; size_t klen;
    char *value; size_t vlen;
} fcSnapRec;

typedef struct fcCtx {
    fcEntry *buckets[FC_BUCKETS];
    pthread_mutex_t ht_lock;

    /* Request queue */
    fcRequest *req_head, *req_tail;
    pthread_mutex_t req_lock;
    pthread_cond_t req_cond;

    /* Completion ring */
    storageCompletion comp_ring[FC_COMP_RING];
    volatile int comp_head, comp_tail;
    pthread_mutex_t comp_lock;

    pthread_t worker;
    volatile int shutdown;
    /* Snapshot support: park the worker (outside locks) around fork(). */
    volatile int snap_hold;
    volatile int snap_held;
    /* Streaming snapshot state. Owned by the worker once snap_stream_active
     * is set; the starting thread must not touch the array after that. */
    storageSnapshotSink snap_sink;
    fcSnapRec *snap_recs;
    size_t snap_count;
    size_t snap_pos;
    volatile int snap_stream_active;
    volatile int snap_stream_abort;
    storageCompletionFn completion_fn;
    void *completion_privdata;
} fcCtx;

static uint32_t fc_hash(uint32_t db_id, const void *key, size_t klen) {
    uint32_t h = db_id * 31;
    const unsigned char *p = key;
    for (size_t i = 0; i < klen; i++) h = h * 131 + p[i];
    return h % FC_BUCKETS;
}

static void fc_enqueue_completion(fcCtx *ctx, storageCompletion *c) {
    pthread_mutex_lock(&ctx->comp_lock);
    int next = (ctx->comp_head + 1) % FC_COMP_RING;
    if (next != ctx->comp_tail) {
        ctx->comp_ring[ctx->comp_head] = *c;
        ctx->comp_head = next;
    }
    pthread_mutex_unlock(&ctx->comp_lock);
}

/* External serialize/deserialize functions (defined in ext_storage.c) */
extern int extStorageSerializeKey(void *key, char **serialized_key);
extern int extStorageSerializeValue(void *value, char **serialized_value);
extern void *extStorageDeserializeValue(char *value, int length);
extern void extStorageFreeSerializedValue(void *value);
extern void extStorageInflightAddRam(size_t bytes);
extern void extStorageOnSpillSerialize(size_t bytes);
extern size_t objectComputeSize(void *key, void *value, int samples, int dbid);

/* Defined below, next to the rest of the streaming snapshot code. */
static int fc_snap_stream_step(fcCtx *ctx, int budget);

static void *fc_worker(void *arg) {
    fcCtx *ctx = arg;
    while (1) {
        /* Snapshot support: park with NO locks held so a fork() in the main
         * thread inherits free mutexes (a mutex held at fork deadlocks any
         * child that later locks it). */
        if (ctx->snap_hold) {
            ctx->snap_held = 1;
            while (ctx->snap_hold && !ctx->shutdown) usleep(50);
            ctx->snap_held = 0;
        }
        /* Advance an in flight snapshot before serving requests, in bounded
         * batches so neither side starves the other. */
        if (ctx->snap_stream_active) fc_snap_stream_step(ctx, 64);

        pthread_mutex_lock(&ctx->req_lock);
        while (!ctx->req_head && !ctx->shutdown && !ctx->snap_hold &&
               !ctx->snap_stream_active)
            pthread_cond_wait(&ctx->req_cond, &ctx->req_lock);
        if (ctx->snap_hold) {
            pthread_mutex_unlock(&ctx->req_lock);
            continue; /* go park at the top */
        }
        if (ctx->shutdown && !ctx->req_head) {
            pthread_mutex_unlock(&ctx->req_lock);
            break;
        }
        if (!ctx->req_head) {
            /* Runnable only because a snapshot is streaming: no request to
             * serve this turn. Yield briefly and loop to emit more. */
            pthread_mutex_unlock(&ctx->req_lock);
            usleep(10);
            continue;
        }
        fcRequest *req = ctx->req_head;
        ctx->req_head = req->next;
        if (!ctx->req_head) ctx->req_tail = NULL;
        pthread_mutex_unlock(&ctx->req_lock);

        usleep(10); /* Simulate NVMe latency */

        storageCompletion comp = {0};
        comp.request_ctx = req->request_ctx;
        comp.op_type = req->op;
        comp.db_id = req->db_id;

        switch (req->op) {
        case STORAGE_OP_PUT: {
            /* Serialize key and value (same as real FlashCache IO thread) */
            char *key_bytes = NULL;
            int key_len = extStorageSerializeKey(req->key, &key_bytes);
            char *val_bytes = NULL;
            int val_len = extStorageSerializeValue(req->value, &val_bytes);

            if (key_len > 0 && val_len > 0) {
                uint32_t bucket = fc_hash(req->db_id, key_bytes, key_len);
                pthread_mutex_lock(&ctx->ht_lock);
                fcEntry *e = ctx->buckets[bucket];
                while (e) {
                    if (e->db_id == req->db_id && e->klen == (size_t)key_len &&
                        memcmp(e->key, key_bytes, key_len) == 0) break;
                    e = e->next;
                }
                if (e) {
                    storage_free(e->value);
                    e->value = storage_malloc(val_len);
                    memcpy(e->value, val_bytes, val_len);
                    e->vlen = val_len;
                    e->expire_ms = req->expire_ms;
                } else {
                    e = storage_malloc(sizeof(fcEntry));
                    e->key = storage_malloc(key_len); memcpy(e->key, key_bytes, key_len);
                    e->klen = key_len;
                    e->value = storage_malloc(val_len); memcpy(e->value, val_bytes, val_len);
                    e->vlen = val_len;
                    e->expire_ms = req->expire_ms;
                    e->db_id = req->db_id;
                    e->next = ctx->buckets[bucket];
                    ctx->buckets[bucket] = e;
                }
                pthread_mutex_unlock(&ctx->ht_lock);
                comp.status = STORAGE_OK;
            } else {
                comp.status = STORAGE_ERR_IO;
            }
            extStorageFreeSerializedValue(val_bytes);

            /* Mirror real backend: carry robj pointers + compute RAM bytes */
            comp.key = req->key;
            comp.klen = 1;
            comp.value = req->value;
            comp.ram_bytes = objectComputeSize(NULL, req->value, 5, (int)req->db_id);
            extStorageInflightAddRam(comp.ram_bytes);
            extStorageOnSpillSerialize(comp.ram_bytes);
            req->key = NULL;
            req->value = NULL;
            break;
        }
        case STORAGE_OP_GET: {
            char *key_bytes = NULL;
            int key_len = extStorageSerializeKey(req->key, &key_bytes);
            uint32_t bucket = fc_hash(req->db_id, key_bytes, key_len);

            pthread_mutex_lock(&ctx->ht_lock);
            fcEntry *e = ctx->buckets[bucket];
            while (e) {
                if (e->db_id == req->db_id && e->klen == (size_t)key_len &&
                    memcmp(e->key, key_bytes, key_len) == 0) break;
                e = e->next;
            }
            if (e) {
                /* Deserialize value bytes back to robj* */
                comp.value = extStorageDeserializeValue(e->value, (int)e->vlen);
                comp.vlen = e->vlen;
                comp.expire_ms = e->expire_ms;
                comp.status = STORAGE_OK;
            } else {
                comp.status = STORAGE_NOT_FOUND;
            }
            pthread_mutex_unlock(&ctx->ht_lock);

            /* Free the key robj (bridge allocated it for the GET) */
            decrRefCount((robj*)req->key);
            req->key = NULL;
            break;
        }
        case STORAGE_OP_DEL: {
            char *key_bytes = NULL;
            int key_len = extStorageSerializeKey(req->key, &key_bytes);
            uint32_t bucket = fc_hash(req->db_id, key_bytes, key_len);

            pthread_mutex_lock(&ctx->ht_lock);
            fcEntry **pp = &ctx->buckets[bucket];
            while (*pp) {
                fcEntry *e = *pp;
                if (e->db_id == req->db_id && e->klen == (size_t)key_len &&
                    memcmp(e->key, key_bytes, key_len) == 0) {
                    *pp = e->next;
                    storage_free(e->key); storage_free(e->value); storage_free(e);
                    comp.status = STORAGE_OK;
                    goto del_done;
                }
                pp = &e->next;
            }
            comp.status = STORAGE_NOT_FOUND;
            del_done:
            pthread_mutex_unlock(&ctx->ht_lock);

            decrRefCount((robj*)req->key);
            req->key = NULL;
            break;
        }
        }

        fc_enqueue_completion(ctx, &comp);
        storage_free(req);
    }
    return NULL;
}

static void *fc_open(storageConfig *cfg) {
    fcCtx *ctx = storage_calloc(1, sizeof(fcCtx));
    pthread_mutex_init(&ctx->ht_lock, NULL);
    pthread_mutex_init(&ctx->req_lock, NULL);
    pthread_cond_init(&ctx->req_cond, NULL);
    pthread_mutex_init(&ctx->comp_lock, NULL);
    ctx->completion_fn = cfg->completion_fn;
    ctx->completion_privdata = cfg->completion_privdata;
    pthread_create(&ctx->worker, NULL, fc_worker, ctx);
    return ctx;
}

static void fc_close(void *opaque) {
    fcCtx *ctx = opaque;
    pthread_mutex_lock(&ctx->req_lock);
    ctx->shutdown = 1;
    pthread_cond_signal(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    pthread_join(ctx->worker, NULL);
    /* Free hash table */
    for (int i = 0; i < FC_BUCKETS; i++) {
        fcEntry *e = ctx->buckets[i];
        while (e) { fcEntry *n = e->next; storage_free(e->key); storage_free(e->value); storage_free(e); e = n; }
    }
    pthread_mutex_destroy(&ctx->ht_lock);
    pthread_mutex_destroy(&ctx->req_lock);
    pthread_cond_destroy(&ctx->req_cond);
    pthread_mutex_destroy(&ctx->comp_lock);
    storage_free(ctx);
}

static storageStatus fc_put_async(void *opaque, uint32_t db_id,
                                   const void *key, size_t klen,
                                   const void *value, size_t vlen,
                                   int64_t expire_ms, void *request_ctx) {
    (void)klen; (void)vlen; /* key/value are robj*, not raw bytes */
    fcCtx *ctx = opaque;
    fcRequest *req = storage_malloc(sizeof(fcRequest));
    req->op = STORAGE_OP_PUT; req->db_id = db_id;
    req->key = (void*)key;
    req->value = (void*)value;
    req->expire_ms = expire_ms; req->request_ctx = request_ctx; req->next = NULL;

    pthread_mutex_lock(&ctx->req_lock);
    if (ctx->req_tail) ctx->req_tail->next = req; else ctx->req_head = req;
    ctx->req_tail = req;
    pthread_cond_signal(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_get_async(void *opaque, uint32_t db_id,
                                   const void *key, size_t klen,
                                   void *request_ctx) {
    (void)klen; /* key is robj*, not raw bytes */
    fcCtx *ctx = opaque;
    fcRequest *req = storage_malloc(sizeof(fcRequest));
    req->op = STORAGE_OP_GET; req->db_id = db_id;
    req->key = (void*)key;
    req->value = NULL;
    req->expire_ms = 0;
    req->request_ctx = request_ctx; req->next = NULL;

    pthread_mutex_lock(&ctx->req_lock);
    if (ctx->req_tail) ctx->req_tail->next = req; else ctx->req_head = req;
    ctx->req_tail = req;
    pthread_cond_signal(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    return STORAGE_WOULDBLOCK;
}

static storageStatus fc_del_async(void *opaque, uint32_t db_id,
                                   const void *key, size_t klen,
                                   void *request_ctx) {
    (void)klen; /* key is robj*, not raw bytes */
    fcCtx *ctx = opaque;
    fcRequest *req = storage_malloc(sizeof(fcRequest));
    req->op = STORAGE_OP_DEL; req->db_id = db_id;
    req->key = (void*)key;
    req->value = NULL;
    req->expire_ms = 0;
    req->request_ctx = request_ctx; req->next = NULL;

    pthread_mutex_lock(&ctx->req_lock);
    if (ctx->req_tail) ctx->req_tail->next = req; else ctx->req_head = req;
    ctx->req_tail = req;
    pthread_cond_signal(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    return STORAGE_WOULDBLOCK;
}

static int fc_poll_completions(void *opaque, int max) {
    fcCtx *ctx = opaque;
    int count = 0;
    pthread_mutex_lock(&ctx->comp_lock);
    while (count < max && ctx->comp_tail != ctx->comp_head) {
        storageCompletion *c = &ctx->comp_ring[ctx->comp_tail];
        if (ctx->completion_fn) ctx->completion_fn(c, ctx->completion_privdata);
        ctx->comp_tail = (ctx->comp_tail + 1) % FC_COMP_RING;
        count++;
    }
    pthread_mutex_unlock(&ctx->comp_lock);
    return count;
}

/* ---------------------------------------------------------------------------
 * Snapshot support (see storage.h)
 * ---------------------------------------------------------------------------*/
static void fc_snapshot_hold(void *vctx) {
    fcCtx *ctx = vctx;
    ctx->snap_hold = 1;
    /* Wake the worker if it is blocked on the request cond var. */
    pthread_mutex_lock(&ctx->req_lock);
    pthread_cond_broadcast(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    while (!ctx->snap_held) usleep(50);
}

static void fc_snapshot_release(void *vctx) {
    fcCtx *ctx = vctx;
    ctx->snap_hold = 0;
}

static void fc_gc_pause(void *vctx, int paused) {
    /* The mock has no GC: nothing relocates stored bytes. */
    (void)vctx; (void)paused;
}

/* Fork-child-safe read: plain hashtable lookup. Safe because the worker is
 * guaranteed parked (holding no locks) at fork time, so the (duplicated)
 * mutex in the child is free. Uses malloc, not zmalloc. */
static storageStatus fc_fork_read(void *vctx, uint32_t db_id,
                                  const void *key, size_t klen,
                                  void **value, size_t *vlen) {
    fcCtx *ctx = vctx;
    uint32_t bucket = fc_hash(db_id, key, klen);
    pthread_mutex_lock(&ctx->ht_lock);
    fcEntry *e = ctx->buckets[bucket];
    while (e) {
        if (e->db_id == db_id && e->klen == klen &&
            memcmp(e->key, key, klen) == 0) break;
        e = e->next;
    }
    if (!e || !e->value || e->vlen == 0) {
        pthread_mutex_unlock(&ctx->ht_lock);
        return STORAGE_NOT_FOUND;
    }
    char *copy = storage_malloc(e->vlen);
    if (!copy) {
        pthread_mutex_unlock(&ctx->ht_lock);
        return STORAGE_NOT_FOUND;
    }
    memcpy(copy, e->value, e->vlen);
    *value = copy;
    *vlen = e->vlen;
    pthread_mutex_unlock(&ctx->ht_lock);
    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * Streaming snapshot (mock): copy on snapshot.
 * ---------------------------------------------------------------------------*/

/* Release the captured array. Worker context, or start-path error unwind. */
static void fc_snap_stream_free(fcCtx *ctx) {
    if (!ctx->snap_recs) return;
    for (size_t i = 0; i < ctx->snap_count; i++) {
        storage_free(ctx->snap_recs[i].key);
        storage_free(ctx->snap_recs[i].value);
    }
    storage_free(ctx->snap_recs);
    ctx->snap_recs = NULL;
    ctx->snap_count = ctx->snap_pos = 0;
}

static storageStatus fc_snapshot_stream_start(void *vctx, storageSnapshotSink *sink) {
    fcCtx *ctx = vctx;
    if (ctx->snap_stream_active) return STORAGE_ERR_REJECTED; /* one at a time */

    /* Establish the cut: copy every live record under the hashtable lock.
     * Holding ht_lock for the whole walk is what makes this atomic with
     * respect to the worker's PUT/DEL handling. */
    pthread_mutex_lock(&ctx->ht_lock);

    size_t n = 0;
    for (int b = 0; b < FC_BUCKETS; b++)
        for (fcEntry *e = ctx->buckets[b]; e; e = e->next)
            if (e->value && e->vlen) n++;

    fcSnapRec *recs = n ? storage_malloc(n * sizeof(fcSnapRec)) : NULL;
    if (n && !recs) {
        pthread_mutex_unlock(&ctx->ht_lock);
        return STORAGE_ERR_REJECTED;
    }

    size_t i = 0;
    size_t total_bytes = 0;
    for (int b = 0; b < FC_BUCKETS && i < n; b++) {
        for (fcEntry *e = ctx->buckets[b]; e && i < n; e = e->next) {
            if (!e->value || !e->vlen) continue;
            recs[i].db_id = e->db_id;
            recs[i].klen = e->klen;
            recs[i].vlen = e->vlen;
            recs[i].key = storage_malloc(e->klen);
            recs[i].value = storage_malloc(e->vlen);
            if (!recs[i].key || !recs[i].value) {
                /* Unwind what we captured so far, then fail the start. */
                storage_free(recs[i].key);
                storage_free(recs[i].value);
                ctx->snap_recs = recs;
                ctx->snap_count = i;
                fc_snap_stream_free(ctx);
                pthread_mutex_unlock(&ctx->ht_lock);
                return STORAGE_ERR_REJECTED;
            }
            memcpy(recs[i].key, e->key, e->klen);
            memcpy(recs[i].value, e->value, e->vlen);
            total_bytes += e->klen + e->vlen;
            i++;
        }
    }
    pthread_mutex_unlock(&ctx->ht_lock);

    ctx->snap_sink = *sink;
    ctx->snap_recs = recs;
    ctx->snap_count = i;
    ctx->snap_pos = 0;
    ctx->snap_stream_abort = 0;
    if (sink->set_size_hint) sink->set_size_hint(sink->privdata, total_bytes);

    /* Publish last, then wake the worker: it owns the array from here. */
    ctx->snap_stream_active = 1;
    pthread_mutex_lock(&ctx->req_lock);
    pthread_cond_broadcast(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
    return STORAGE_OK;
}

static void fc_snapshot_stream_abort(void *vctx) {
    fcCtx *ctx = vctx;
    if (!ctx->snap_stream_active) return; /* idempotent */
    ctx->snap_stream_abort = 1;
    pthread_mutex_lock(&ctx->req_lock);
    pthread_cond_broadcast(&ctx->req_cond);
    pthread_mutex_unlock(&ctx->req_lock);
}

/* Emit up to `budget` records. Worker context only. Returns 1 while the
 * stream is still running, 0 once it has terminated (complete() fired).
 *
 * The budget makes the drain incremental so a large snapshot does not starve
 * request traffic, matching how the real backend's snapshot advances on FC's
 * cron alongside normal IO. */
static int fc_snap_stream_step(fcCtx *ctx, int budget) {
    if (!ctx->snap_stream_active) return 0;

    if (ctx->snap_stream_abort || ctx->shutdown) {
        ctx->snap_sink.complete(ctx->snap_sink.privdata, 0);
        fc_snap_stream_free(ctx);
        ctx->snap_stream_active = 0;
        return 0;
    }

    while (budget-- > 0 && ctx->snap_pos < ctx->snap_count) {
        if (!ctx->snap_sink.writable(ctx->snap_sink.privdata))
            return 1; /* backpressure: retry on a later worker turn */
        fcSnapRec *r = &ctx->snap_recs[ctx->snap_pos];
        ctx->snap_sink.on_record(ctx->snap_sink.privdata, r->db_id,
                                 r->key, r->klen, r->value, r->vlen);
        ctx->snap_pos++;
    }

    if (ctx->snap_pos >= ctx->snap_count) {
        ctx->snap_sink.complete(ctx->snap_sink.privdata, 1);
        fc_snap_stream_free(ctx);
        ctx->snap_stream_active = 0;
        return 0;
    }
    return 1;
}

static storageType flashcache_type = {
    .name = "flashcache",
    .version = VALKEY_STORAGE_VERSION,
    .open = fc_open,
    .close = fc_close,
    .put = NULL, .get = NULL, .del = NULL,
    .put_async = fc_put_async,
    .get_async = fc_get_async,
    .del_async = fc_del_async,
    .poll_completions = fc_poll_completions,
    .cron = NULL,
    .get_stats = NULL,
    .snapshot_hold = fc_snapshot_hold,
    .snapshot_release = fc_snapshot_release,
    .gc_pause = fc_gc_pause,
    .fork_read = fc_fork_read,
    .snapshot_stream_start = fc_snapshot_stream_start,
    .snapshot_stream_abort = fc_snapshot_stream_abort,
};

storageType *storageGetFlashCacheType(void) { return &flashcache_type; }

/* Alias: "rocksdb" config value uses the same in-memory mock for testing.
 * This avoids duplicating 300 lines of identical hashtable code. */
storageType *storageGetRocksDBAsyncType(void) { return &flashcache_type; }
