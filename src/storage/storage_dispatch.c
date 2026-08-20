/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "storage.h"
#include <stdlib.h>

storageType *server_storage = NULL;
void *server_storage_ctx = NULL;
static storageCompletionFn active_completion_fn = NULL;
static void *active_completion_privdata = NULL;

/* Forward declarations for middleware */
int storageMiddlewareInit(storageType *type, void *ctx, int num_threads,
                          storageCompletionFn fn, void *privdata);
void storageMiddlewareShutdown(void);
storageStatus storageMiddlewareSubmit(int op, uint32_t db_id,
                                      const void *key, size_t klen,
                                      const void *value, size_t vlen,
                                      int64_t expire_ms, void *request_ctx);
int storageMiddlewarePoll(int max);

int storageInit(storageType *type, storageConfig *cfg) {
    server_storage = type;
    server_storage_ctx = type->open(cfg);
    if (!server_storage_ctx) return -1;
    active_completion_fn = cfg->completion_fn;
    active_completion_privdata = cfg->completion_privdata;

    if (!type->put_async) {
        int threads = cfg->io_threads > 0 ? cfg->io_threads : 2;
        storageMiddlewareInit(type, server_storage_ctx, threads,
                              cfg->completion_fn, cfg->completion_privdata);
    }
    return 0;
}

void storageShutdown(void) {
    if (!server_storage) return;
    if (!server_storage->put_async) {
        storageMiddlewareShutdown();
    }
    server_storage->close(server_storage_ctx);
    server_storage = NULL;
    server_storage_ctx = NULL;
}

storageStatus storageSubmitPut(uint32_t db_id, const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms, void *request_ctx) {
    if (server_storage->put_async) {
        return server_storage->put_async(server_storage_ctx, db_id, key, klen,
                                       value, vlen, expire_ms, request_ctx);
    }
    return storageMiddlewareSubmit(STORAGE_OP_PUT, db_id, key, klen,
                                   value, vlen, expire_ms, request_ctx);
}

storageStatus storageSubmitGet(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx) {
    if (server_storage->get_async) {
        return server_storage->get_async(server_storage_ctx, db_id, key, klen, request_ctx);
    }
    return storageMiddlewareSubmit(STORAGE_OP_GET, db_id, key, klen,
                                   NULL, 0, 0, request_ctx);
}

storageStatus storageSubmitDel(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx) {
    if (server_storage->del_async) {
        return server_storage->del_async(server_storage_ctx, db_id, key, klen, request_ctx);
    }
    return storageMiddlewareSubmit(STORAGE_OP_DEL, db_id, key, klen,
                                   NULL, 0, 0, request_ctx);
}

int storagePollCompletions(int max) {
    if (server_storage->poll_completions) {
        return server_storage->poll_completions(server_storage_ctx, max);
    }
    return storageMiddlewarePoll(max);
}

void storageCron(void) {
    if (server_storage && server_storage->cron) {
        server_storage->cron(server_storage_ctx);
    }
}

/* ---------------------------------------------------------------------------
 * Snapshot support dispatch
 * ---------------------------------------------------------------------------*/
int storageSnapshotSupported(void) {
    return server_storage && server_storage->fork_read &&
           server_storage->snapshot_hold && server_storage->snapshot_release;
}

void storageSnapshotHold(void) {
    if (server_storage && server_storage->snapshot_hold)
        server_storage->snapshot_hold(server_storage_ctx);
}

void storageSnapshotRelease(void) {
    if (server_storage && server_storage->snapshot_release)
        server_storage->snapshot_release(server_storage_ctx);
}

void storageGcPause(int paused) {
    if (server_storage && server_storage->gc_pause)
        server_storage->gc_pause(server_storage_ctx, paused);
}

storageStatus storageForkRead(uint32_t db_id, const void *key, size_t klen,
                              void **value, size_t *vlen) {
    if (!server_storage || !server_storage->fork_read) return STORAGE_NOT_FOUND;
    return server_storage->fork_read(server_storage_ctx, db_id, key, klen, value, vlen);
}

/* ---------------------------------------------------------------------------
 * Streaming snapshot dispatch
 * ---------------------------------------------------------------------------*/
int storageSnapshotStreamSupported(void) {
    return server_storage && server_storage->snapshot_stream_start &&
           server_storage->snapshot_stream_abort;
}

storageStatus storageSnapshotStreamStart(storageSnapshotSink *sink) {
    if (!storageSnapshotStreamSupported()) return STORAGE_ERR_REJECTED;
    /* A sink missing any required callback is a programming error, not a
     * runtime condition: fail before the backend freezes a cut it cannot
     * report on. set_size_hint is the only optional member. */
    if (!sink || !sink->on_record || !sink->writable || !sink->complete)
        return STORAGE_ERR_REJECTED;
    return server_storage->snapshot_stream_start(server_storage_ctx, sink);
}

void storageSnapshotStreamAbort(void) {
    if (server_storage && server_storage->snapshot_stream_abort)
        server_storage->snapshot_stream_abort(server_storage_ctx);
}
