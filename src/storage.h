/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Pluggable Storage Interface for Valkey Data Tiering
 *
 * Supports two integration modes via the same struct:
 *   - Sync path: backend implements put/get/del (called from shared middleware IO thread)
 *   - Async path: backend implements put_async/get_async/del_async (backend owns IO)
 *
 * If put_async != NULL, engine calls async path directly (backend manages IO).
 * If put_async == NULL, engine submits to shared middleware which wraps sync calls.
 *
 * Both native backends and module-registered backends use this same interface.
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------------*/
#define VALKEY_STORAGE_VERSION 1

/* ---------------------------------------------------------------------------
 * Status Codes
 * ---------------------------------------------------------------------------*/
typedef enum {
    STORAGE_OK = 0,
    STORAGE_NOT_FOUND = 1,
    STORAGE_WOULDBLOCK = 2,     /* Async op submitted successfully */
    STORAGE_ERR_IO = -1,
    STORAGE_ERR_FULL = -2,
    STORAGE_ERR_CORRUPT = -3,
    STORAGE_ERR_REJECTED = -4,  /* Backend at capacity / throttled */
} storageStatus;

/* ---------------------------------------------------------------------------
 * Operation types
 * ---------------------------------------------------------------------------*/
#define STORAGE_OP_PUT    0
#define STORAGE_OP_GET    1
#define STORAGE_OP_DEL    2

/* ---------------------------------------------------------------------------
 * Completion (delivered from IO thread to main thread)
 * ---------------------------------------------------------------------------*/
typedef struct storageCompletion {
    void *request_ctx;      /* Opaque context from the original request */
    int op_type;            /* STORAGE_OP_PUT/GET/DEL */
    storageStatus status;   /* Result */
    uint32_t db_id;
    void *key;              /* Key (ownership depends on op) */
    size_t klen;
    void *value;            /* GET: deserialized value (caller takes ownership) */
    size_t vlen;            /* GET: value length */
    int64_t expire_ms;      /* GET: TTL if stored, -1 if none */
} storageCompletion;

typedef void (*storageCompletionFn)(storageCompletion *completion);

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/
typedef struct storageConfig {
    const char *path;               /* Storage path / device / URI */
    size_t capacity_bytes;          /* Max storage size */
    uint32_t num_databases;         /* Number of Valkey DBs */
    int io_threads;                 /* Middleware thread count (0 = auto) */
    uint32_t max_inflight_reads;    /* Concurrency limit for reads */
    uint32_t max_inflight_writes;   /* Concurrency limit for writes */
    storageCompletionFn completion_fn; /* Completion delivery function */
    void *backend_opts;             /* Backend-specific opaque config */
} storageConfig;

/* ---------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------------*/
typedef struct storageStats {
    size_t used_bytes;
    size_t capacity_bytes;
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_deletes;
    uint64_t inflight_reads;
    uint64_t inflight_writes;
} storageStats;

/* ---------------------------------------------------------------------------
 * Iterator (for snapshot / replication)
 * ---------------------------------------------------------------------------*/
typedef struct storageIterator storageIterator;

/* ---------------------------------------------------------------------------
 * storageType — The pluggable interface
 * ---------------------------------------------------------------------------*/
typedef struct storageType {
    const char *name;       /* "flashcache", "rocksdb", "cachelib" */
    int version;            /* VALKEY_STORAGE_VERSION */

    /* =======================================================================
     * Lifecycle
     * ======================================================================= */
    void *(*open)(storageConfig *cfg);
    void (*close)(void *ctx);
    int (*drain)(void *ctx, int timeout_ms);    /* Drain in-flight ops. Returns 0=done, -1=timeout */

    /* =======================================================================
     * Sync KV Operations (called from middleware IO thread)
     *
     * Implement these if your backend is a sync library (RocksDB, CacheLib).
     * The shared middleware wraps them in async.
     * If put_async is also set, sync versions are ignored.
     * ======================================================================= */
    storageStatus (*put)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen,
                         const void *value, size_t vlen,
                         int64_t expire_ms);

    storageStatus (*get)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen,
                         void **value, size_t *vlen,
                         int64_t *expire_ms);

    storageStatus (*del)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen);

    /* =======================================================================
     * Async KV Operations (backend owns IO)
     *
     * Implement these if your backend has its own IO mechanism (io_uring, SPDK).
     * Set to NULL to use shared middleware with sync ops above.
     *
     * Returns STORAGE_WOULDBLOCK on successful submission.
     * Completions delivered via poll_completions().
     * ======================================================================= */
    storageStatus (*put_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms,
                               void *request_ctx);

    storageStatus (*get_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               void *request_ctx);

    storageStatus (*del_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               void *request_ctx);

    /* Poll completed async operations. Returns count processed.
     * Required if put_async is set. Called from main thread (beforeSleep). */
    int (*poll_completions)(void *ctx, int max);

    /* =======================================================================
     * Background Maintenance
     *
     * Called periodically. For GC, compaction bookkeeping, etc.
     * NULL if backend handles maintenance internally.
     * Returns 1 = more work pending, 0 = idle.
     * ======================================================================= */
    int (*cron)(void *ctx);

    /* =======================================================================
     * Iterator (snapshot / replication)
     * ======================================================================= */
    storageIterator *(*iterator_create)(void *ctx, uint32_t db_id);
    storageStatus (*iterator_next)(storageIterator *iter,
                                   void **key, size_t *klen,
                                   void **val, size_t *vlen);
    void (*iterator_destroy)(storageIterator *iter);

    /* =======================================================================
     * Bulk Operations (called synchronously from main thread AFTER drain)
     * ======================================================================= */
    storageStatus (*flush_db)(void *ctx, uint32_t db_id);
    storageStatus (*flush_all)(void *ctx);

    /* =======================================================================
     * Snapshot (backend-specific, opaque)
     * ======================================================================= */
    storageStatus (*snapshot_save)(void *ctx, const char *path, void *opts);
    storageStatus (*snapshot_load)(void *ctx, const char *path, void *opts);

    /* =======================================================================
     * Config & Metrics (opaque key-value, called from main thread)
     * ======================================================================= */
    storageStatus (*set_config)(void *ctx, const char *key, const char *value);
    storageStatus (*get_config)(void *ctx, const char *key, char *buf, size_t buflen);
    void (*get_stats)(void *ctx, storageStats *out);
    char *(*get_info)(void *ctx);   /* Returns malloc'd info string, caller frees */

    /* =======================================================================
     * Optional (NULL if not supported)
     * ======================================================================= */
    int (*exists)(void *ctx, uint32_t db_id,
                  const void *key, size_t klen);    /* Fast existence check */
    storageStatus (*sync)(void *ctx);               /* Force flush to media */

} storageType;

/* ---------------------------------------------------------------------------
 * Global storage instance
 * ---------------------------------------------------------------------------*/
extern storageType *server_storage;
extern void *server_storage_ctx;

/* ---------------------------------------------------------------------------
 * Engine dispatch (called by ext_storage.c)
 *
 * These handle the hybrid logic: async path if available, else middleware.
 * ---------------------------------------------------------------------------*/
storageStatus storageSubmitPut(uint32_t db_id, const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms, void *request_ctx);
storageStatus storageSubmitGet(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);
storageStatus storageSubmitDel(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);
int storagePollCompletions(int max);
int storageCron(void);

/* ---------------------------------------------------------------------------
 * Shared Middleware
 * ---------------------------------------------------------------------------*/
int storageMiddlewareInit(int num_threads);
void storageMiddlewareShutdown(void);
void storageMiddlewareSubmit(int op_type, uint32_t db_id,
                             const void *key, size_t klen,
                             const void *value, size_t vlen,
                             int64_t expire_ms, void *request_ctx);
int storageMiddlewarePollCompletions(int max);

/* ---------------------------------------------------------------------------
 * Serialization Utility (shared, thread-safe)
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Backend Registration
 * ---------------------------------------------------------------------------*/
storageType *storageGetFlashCacheType(void);

#endif /* STORAGE_H */
