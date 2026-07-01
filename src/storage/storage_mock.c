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

static void *fc_worker(void *arg) {
    fcCtx *ctx = arg;
    while (1) {
        pthread_mutex_lock(&ctx->req_lock);
        while (!ctx->req_head && !ctx->shutdown)
            pthread_cond_wait(&ctx->req_cond, &ctx->req_lock);
        if (ctx->shutdown && !ctx->req_head) {
            pthread_mutex_unlock(&ctx->req_lock);
            break;
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
};

storageType *storageGetFlashCacheType(void) { return &flashcache_type; }

/* Alias: "rocksdb" config value uses the same in-memory mock for testing.
 * This avoids duplicating 300 lines of identical hashtable code. */
storageType *storageGetRocksDBAsyncType(void) { return &flashcache_type; }
