/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "storage.h"
#include <pthread.h>
#include <string.h>

#define MW_MAX_COMPLETIONS 4096

typedef struct mwRequest {
    int op;
    uint32_t db_id;
    void *key;
    size_t klen;
    void *value;
    size_t vlen;
    int64_t expire_ms;
    void *request_ctx;
    struct mwRequest *next;
} mwRequest;

static struct {
    pthread_t *threads;
    int num_threads;
    volatile int shutdown;

    storageType *backend;
    void *backend_ctx;

    /* Request queue */
    mwRequest *req_head, *req_tail;
    pthread_mutex_t req_lock;
    pthread_cond_t req_cond;

    /* Completion ring */
    storageCompletion completions[MW_MAX_COMPLETIONS];
    volatile int comp_head, comp_tail;
    pthread_mutex_t comp_lock;

    storageCompletionFn completion_fn;
    void *completion_privdata;
} mw;

static void mw_enqueue_completion(storageCompletion *c) {
    pthread_mutex_lock(&mw.comp_lock);
    int next = (mw.comp_head + 1) % MW_MAX_COMPLETIONS;
    if (next != mw.comp_tail) {
        mw.completions[mw.comp_head] = *c;
        mw.comp_head = next;
    }
    pthread_mutex_unlock(&mw.comp_lock);
}

static void *mw_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&mw.req_lock);
        while (!mw.req_head && !mw.shutdown)
            pthread_cond_wait(&mw.req_cond, &mw.req_lock);
        if (mw.shutdown && !mw.req_head) {
            pthread_mutex_unlock(&mw.req_lock);
            break;
        }
        mwRequest *req = mw.req_head;
        mw.req_head = req->next;
        if (!mw.req_head) mw.req_tail = NULL;
        pthread_mutex_unlock(&mw.req_lock);

        storageCompletion comp = {0};
        comp.request_ctx = req->request_ctx;
        comp.op_type = req->op;
        comp.db_id = req->db_id;
        comp.key = req->key;
        comp.klen = req->klen;

        switch (req->op) {
        case STORAGE_OP_PUT:
            comp.status = mw.backend->put(mw.backend_ctx, req->db_id,
                                           req->key, req->klen,
                                           req->value, req->vlen, req->expire_ms);
            storage_free(req->value);
            break;
        case STORAGE_OP_GET: {
            void *val = NULL;
            size_t vlen = 0;
            int64_t exp = 0;
            comp.status = mw.backend->get(mw.backend_ctx, req->db_id,
                                           req->key, req->klen, &val, &vlen, &exp);
            comp.value = val;
            comp.vlen = vlen;
            comp.expire_ms = exp;
            break;
        }
        case STORAGE_OP_DEL:
            comp.status = mw.backend->del(mw.backend_ctx, req->db_id,
                                           req->key, req->klen);
            break;
        }

        mw_enqueue_completion(&comp);
        storage_free(req);
    }
    return NULL;
}

int storageMiddlewareInit(storageType *type, void *ctx, int num_threads,
                          storageCompletionFn fn, void *privdata) {
    memset(&mw, 0, sizeof(mw));
    mw.backend = type;
    mw.backend_ctx = ctx;
    mw.num_threads = num_threads;
    mw.completion_fn = fn;
    mw.completion_privdata = privdata;
    pthread_mutex_init(&mw.req_lock, NULL);
    pthread_cond_init(&mw.req_cond, NULL);
    pthread_mutex_init(&mw.comp_lock, NULL);

    mw.threads = storage_malloc(sizeof(pthread_t) * num_threads);
    for (int i = 0; i < num_threads; i++)
        pthread_create(&mw.threads[i], NULL, mw_worker, NULL);
    return 0;
}

void storageMiddlewareShutdown(void) {
    pthread_mutex_lock(&mw.req_lock);
    mw.shutdown = 1;
    pthread_cond_broadcast(&mw.req_cond);
    pthread_mutex_unlock(&mw.req_lock);
    for (int i = 0; i < mw.num_threads; i++)
        pthread_join(mw.threads[i], NULL);
    storage_free(mw.threads);
    pthread_mutex_destroy(&mw.req_lock);
    pthread_cond_destroy(&mw.req_cond);
    pthread_mutex_destroy(&mw.comp_lock);
}

storageStatus storageMiddlewareSubmit(int op, uint32_t db_id,
                                      const void *key, size_t klen,
                                      const void *value, size_t vlen,
                                      int64_t expire_ms, void *request_ctx) {
    mwRequest *req = storage_malloc(sizeof(mwRequest));
    req->op = op;
    req->db_id = db_id;
    req->key = storage_malloc(klen);
    memcpy(req->key, key, klen);
    req->klen = klen;
    if (value && vlen > 0) {
        req->value = storage_malloc(vlen);
        memcpy(req->value, value, vlen);
        req->vlen = vlen;
    } else {
        req->value = NULL;
        req->vlen = 0;
    }
    req->expire_ms = expire_ms;
    req->request_ctx = request_ctx;
    req->next = NULL;

    pthread_mutex_lock(&mw.req_lock);
    if (mw.req_tail) mw.req_tail->next = req;
    else mw.req_head = req;
    mw.req_tail = req;
    pthread_cond_signal(&mw.req_cond);
    pthread_mutex_unlock(&mw.req_lock);
    return STORAGE_WOULDBLOCK;
}

int storageMiddlewarePoll(int max) {
    int count = 0;
    pthread_mutex_lock(&mw.comp_lock);
    while (count < max && mw.comp_tail != mw.comp_head) {
        storageCompletion *c = &mw.completions[mw.comp_tail];
        if (mw.completion_fn) mw.completion_fn(c, mw.completion_privdata);
        mw.comp_tail = (mw.comp_tail + 1) % MW_MAX_COMPLETIONS;
        count++;
    }
    pthread_mutex_unlock(&mw.comp_lock);
    return count;
}
