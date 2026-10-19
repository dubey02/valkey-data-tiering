/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FlashCache storage engine.
 *
 * Requests from the main thread go through a request queue to a dedicated IO
 * thread, which executes them and posts the results to a completion queue for
 * the main thread to poll. */

#include "fmacros.h"

#include "storage_flashcache.h"
#include "mutexqueue.h"
#include "serverassert.h"
#include "zmalloc.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* Maximum number of requests in flight. */
#define FC_QUEUE_DEPTH 256

typedef struct fcRequest {
    storageOpType op;
    uint32_t db_id;
    char *key;
    size_t key_len;
    /* The object to store. Only set for a put. */
    const valkeyObject *entry;
    /* The completion result, handed back to the main thread. */
    storageCompletion completed_request;
} fcRequest;

/* Queued ahead of the pending requests to tell the IO thread to exit. */
static fcRequest FC_STOP_REQUEST;

static struct {
    /* Converts objects to and from bytes. */
    storageSerializer serializer;
    /* Thread setup from the valkey server. */
    storageThreadConfig thread_config;
    /* Storage logger. It writes to the valkey server log. */
    storageLogFn logger;
    /* Number of databases. */
    uint32_t num_databases;

    /* Requests waiting for the IO thread. */
    mutexQueue *requests;
    /* Finished requests waiting for the main thread. */
    mutexQueue *completions;
    /* Requests submitted and not yet polled. */
    size_t outstanding;

    /* The IO thread. */
    pthread_t io_thread;
    /* Whether the storage engine is open. */
    int running;
} fc_io;

/* ---------------------------------------------------------------------------
 * IO thread
 * ---------------------------------------------------------------------------*/

/* Runs one request and posts its completion. */
static void fcExecute(fcRequest *r) {
    switch (r->op) {
    case STORAGE_OP_PUT: {
        char *bytes = NULL;
        int len = fc_io.serializer.serialize(r->key, r->key_len, r->entry, &bytes);
        if (len < 0 || bytes == NULL) {
            r->completed_request.status = STORAGE_ERR_REJECTED;
            break;
        }
        fc_io.serializer.free_serialized(bytes);
        r->completed_request.status = STORAGE_OK;
        r->completed_request.stored_bytes = (size_t)len;
        break;
    }
    case STORAGE_OP_GET:
    case STORAGE_OP_DEL: r->completed_request.status = STORAGE_NOT_FOUND; break;
    }

    mutexQueueAdd(fc_io.completions, r);
}

/* The IO thread. Runs requests until it pops the stop request. */
static void *fcIOThreadMain(void *arg) {
    (void)arg;
    if (fc_io.thread_config.on_thread_start) fc_io.thread_config.on_thread_start("fc_io");

    while (true) {
        fcRequest *r = mutexQueuePop(fc_io.requests, true);
        if (r == &FC_STOP_REQUEST) break;
        fcExecute(r);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Main thread
 * ---------------------------------------------------------------------------*/

static void fcFreeRequest(fcRequest *r) {
    zfree(r->key);
    zfree(r);
}

/* Queues a request for the IO thread. */
static storageStatus fcSubmit(storageOpType op, uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    if (!fc_io.running) return STORAGE_ERR_REJECTED;
    if (db_id >= fc_io.num_databases) return STORAGE_ERR_REJECTED;
    if (fc_io.outstanding >= FC_QUEUE_DEPTH) return STORAGE_WOULDBLOCK;

    fcRequest *r = zmalloc(sizeof(*r));
    r->op = op;
    r->db_id = db_id;
    r->key = zmalloc(key_len);
    memcpy(r->key, key, key_len);
    r->key_len = key_len;
    r->entry = entry;
    r->completed_request = (storageCompletion){.request_ctx = request_ctx, .op_type = op, .db_id = db_id};

    fc_io.outstanding++;
    mutexQueueAdd(fc_io.requests, r);
    return STORAGE_OK;
}

static storageStatus fcPutAsync(uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    return fcSubmit(STORAGE_OP_PUT, db_id, key, key_len, entry, request_ctx);
}

static storageStatus fcGetAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return fcSubmit(STORAGE_OP_GET, db_id, key, key_len, NULL, request_ctx);
}

static storageStatus fcDelAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return fcSubmit(STORAGE_OP_DEL, db_id, key, key_len, NULL, request_ctx);
}

/* Collects finished requests. */
static int fcPollCompletions(storageCompletion *out, int max) {
    if (!fc_io.running || max <= 0) return 0;
    fifo *done = mutexQueuePopAll(fc_io.completions, false);
    if (done == NULL) return 0;

    int total = 0;
    fcRequest *r;
    while (total < max && fifoPop(done, (void **)&r)) {
        out[total++] = r->completed_request;
        fcFreeRequest(r);
    }

    /* Anything past the caller's limit goes back ahead of newer completions. The
     * priority lane keeps insertion order, so this preserves completion order. */
    while (fifoPop(done, (void **)&r)) mutexQueuePushPriority(fc_io.completions, r);
    fifoRelease(done);

    fc_io.outstanding -= (size_t)total;
    return total;
}

static void fcGetStats(storageStats *out) {
    memset(out, 0, sizeof(*out));
    if (fc_io.running) out->num_completions_pending = mutexQueueLength(fc_io.completions);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

static void fcClose(void);

/* Starts the IO thread. */
static storageStatus fcOpen(const storageConfig *cfg) {
    if (cfg == NULL || cfg->num_databases == 0) return STORAGE_ERR_REJECTED;
    if (cfg->serializer.serialize == NULL || cfg->serializer.deserialize == NULL ||
        cfg->serializer.free_serialized == NULL || cfg->logger == NULL) {
        return STORAGE_ERR_REJECTED;
    }
    /* No options are recognized yet, so any supplied option is unknown and
     * rejected rather than silently ignored. */
    if (cfg->num_options > 0) {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: unknown option \"%s\"", cfg->options[0].name);
        return STORAGE_ERR_REJECTED;
    }

    fcClose();

    fc_io.serializer = cfg->serializer;
    fc_io.thread_config = cfg->thread_config;
    fc_io.logger = cfg->logger;
    fc_io.num_databases = cfg->num_databases;

    fc_io.requests = mutexQueueCreate();
    fc_io.completions = mutexQueueCreate();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stack_size = 0;
    pthread_attr_getstacksize(&attr, &stack_size);
    if (stack_size < fc_io.thread_config.stack_size) pthread_attr_setstacksize(&attr, fc_io.thread_config.stack_size);
    int err = pthread_create(&fc_io.io_thread, &attr, fcIOThreadMain, NULL);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: can't start the IO thread: %s", strerror(err));
        mutexQueueRelease(fc_io.requests);
        mutexQueueRelease(fc_io.completions);
        memset(&fc_io, 0, sizeof(fc_io));
        return STORAGE_ERR_IO;
    }
    fc_io.running = 1;
    return STORAGE_OK;
}

/* Stops the IO thread, then frees the queued requests and the unpolled
 * completions. The request the IO thread is running finishes first. */
static void fcClose(void) {
    if (!fc_io.running) return;

    /* Ahead of the queued requests, so none of them run. */
    mutexQueuePushPriority(fc_io.requests, &FC_STOP_REQUEST);
    int join_err = pthread_join(fc_io.io_thread, NULL);
    assert(join_err == 0);
    fc_io.running = 0;

    fcRequest *r;
    fifo *left = mutexQueuePopAll(fc_io.requests, false);
    if (left) {
        while (fifoPop(left, (void **)&r)) fcFreeRequest(r);
        fifoRelease(left);
    }
    left = mutexQueuePopAll(fc_io.completions, false);
    if (left) {
        while (fifoPop(left, (void **)&r)) fcFreeRequest(r);
        fifoRelease(left);
    }

    mutexQueueRelease(fc_io.requests);
    mutexQueueRelease(fc_io.completions);
    memset(&fc_io, 0, sizeof(fc_io));
}

/* Cancels the IO thread without any cleanup. */
static void fcKillThreads(void) {
    if (!fc_io.running) return;
    if (pthread_equal(fc_io.io_thread, pthread_self())) return;
    if (pthread_cancel(fc_io.io_thread) != 0) return;
    pthread_join(fc_io.io_thread, NULL);
}

static const storageEngine flashcache_storage_engine = {
    .name = "flashcache",
    .api_version = VALKEY_STORAGE_API_VERSION,
    .open = fcOpen,
    .close = fcClose,
    .kill_threads = fcKillThreads,
    .put_async = fcPutAsync,
    .get_async = fcGetAsync,
    .del_async = fcDelAsync,
    .poll_completions = fcPollCompletions,
    .get_stats = fcGetStats,
};

const storageEngine *storageFlashCacheEngine(void) {
    return &flashcache_storage_engine;
}
