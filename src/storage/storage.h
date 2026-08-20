/*
 * Pluggable Storage Interface for Valkey Data Tiering
 * Hybrid: sync backends use shared middleware, async backends own their IO.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* When compiled within Valkey, use zmalloc. Standalone tests use stdlib. */
#ifdef STORAGE_USE_ZMALLOC
#include "zmalloc.h"
#define storage_malloc(sz) zmalloc(sz)
#define storage_calloc(n,sz) zcalloc((n)*(sz))
#define storage_realloc(p,sz) zrealloc(p,sz)
#define storage_free(p) zfree(p)
#else
#include <stdlib.h>
#define storage_malloc(sz) malloc(sz)
#define storage_calloc(n,sz) calloc(n,sz)
#define storage_realloc(p,sz) realloc(p,sz)
#define storage_free(p) free(p)
#endif

#define VALKEY_STORAGE_VERSION 1

/* Status codes */
typedef enum {
    STORAGE_OK = 0,
    STORAGE_NOT_FOUND = 1,
    STORAGE_WOULDBLOCK = 2,
    STORAGE_ERR_IO = -1,
    STORAGE_ERR_FULL = -2,
    STORAGE_ERR_REJECTED = -3,
} storageStatus;

/* Operation types */
#define STORAGE_OP_PUT 0
#define STORAGE_OP_GET 1
#define STORAGE_OP_DEL 2
#define STORAGE_OP_BARRIER 3  /* Drain barrier — no IO, just signals completion */
/* Snapshot stream control. These exist as request ops because a backend that
 * owns its own IO thread must issue every backend-library call from that
 * thread; starting a stream inline from the main thread would race the
 * worker inside the library. */
#define STORAGE_OP_SNAPSHOT_START 4
#define STORAGE_OP_SNAPSHOT_ABORT 5

/* Completion delivered from IO to main thread */
typedef struct storageCompletion {
    void *request_ctx;
    int op_type;
    storageStatus status;
    uint32_t db_id;
    void *key;
    size_t klen;
    void *value;      /* GET: caller takes ownership (malloc'd) */
    size_t vlen;
    int64_t expire_ms;
    size_t ram_bytes;   /* in-RAM footprint of spilled value; computed on the IO thread at serialize */
} storageCompletion;

typedef void (*storageCompletionFn)(storageCompletion *c, void *privdata);

/* Config passed to open() */
typedef struct storageConfig {
    const char *path;
    size_t capacity_bytes;
    uint32_t num_databases;
    int io_threads;
    int eviction_enabled;  /* 0 = noeviction (flash never deletes data) */
    storageCompletionFn completion_fn;
    void *completion_privdata;
    /* FlashCache tuning (passed through to backend) */
    size_t index_size;                 /* initial index entries per DB */
    uint32_t max_allocated_percent;    /* GC triggers at this % full */
    uint32_t max_in_flight_reads;      /* max concurrent read IO ops */
    uint32_t min_gc_rate;              /* min GC bytes/sec */
    uint32_t max_gc_rate;              /* max GC bytes/sec */
    size_t max_buffered_write_size;    /* staging buffer size */
    size_t buffered_write_flush_threshold; /* flush when buffer hits this */
} storageConfig;

/* Stats */
typedef struct storageStats {
    uint64_t total_puts;
    uint64_t total_gets;
    uint64_t total_dels;
    uint64_t keys_stored;
} storageStats;

/* ---------------------------------------------------------------------------
 * Streaming snapshot sink.
 *
 * The sink is RECORD oriented, not byte oriented. A backend decodes its own
 * on-disk representation and hands up one whole record at a time. Two reasons:
 *
 *  1. Backend independence. The mock does not share the real backend's
 *     serialized item layout, so a byte oriented sink would let mock tests
 *     validate a framing that the real backend never emits.
 *  2. The consumer needs records anyway. The RDB flash section is a sequence
 *     of key/value entries, so an opaque byte image would have to be decoded
 *     downstream regardless.
 *
 * No expiry or LRU metadata crosses this boundary. Values spill, keys do not,
 * so the engine still owns every key's TTL and idle time in its own hashtable
 * and stamps them when it writes the record out.
 *
 * Threading: on_record and writable are invoked on the BACKEND's IO thread.
 * Implementations must not touch engine data structures. The record buffers
 * are owned by the backend and are invalid once on_record returns, so an
 * implementation that needs to retain them must copy.
 * ---------------------------------------------------------------------------*/
typedef struct storageSnapshotSink {
    void *privdata;

    /* Total record COUNT is not knowable up front, but the backend usually
     * knows the byte size of the frozen range. Advisory only; may be 0. */
    void (*set_size_hint)(void *privdata, size_t total_bytes);

    /* Backpressure probe, called before every record. Return 0 to signal "not
     * now". A backend is free to buffer, but buffering is bounded and the
     * backend may abort the snapshot if the sink stays unwritable. */
    int (*writable)(void *privdata);

    /* One record, live as of the snapshot cut. Called exactly once per key. */
    void (*on_record)(void *privdata, uint32_t db_id,
                      const char *key, size_t klen,
                      const char *value, size_t vlen);

    /* Terminal, called exactly once. ok=1 success, ok=0 aborted or failed. */
    void (*complete)(void *privdata, int ok);
} storageSnapshotSink;

/* The pluggable interface */
typedef struct storageType {
    const char *name;
    int version;

    /* Lifecycle */
    void *(*open)(storageConfig *cfg);
    void (*close)(void *ctx);

    /* Sync KV ops (called from middleware IO thread). NULL if async-only. */
    storageStatus (*put)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen,
                         const void *value, size_t vlen, int64_t expire_ms);
    storageStatus (*get)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen,
                         void **value, size_t *vlen, int64_t *expire_ms);
    storageStatus (*del)(void *ctx, uint32_t db_id,
                         const void *key, size_t klen);

    /* Async KV ops (backend owns IO). NULL = use middleware with sync ops. */
    storageStatus (*put_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms, void *request_ctx);
    storageStatus (*get_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               void *request_ctx);
    storageStatus (*del_async)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               void *request_ctx);
    int (*poll_completions)(void *ctx, int max);

    /* Optional */
    int (*cron)(void *ctx);
    void (*get_stats)(void *ctx, storageStats *out);

    /* -----------------------------------------------------------------------
     * Snapshot support (fork-based RDB save with tiered values).
     * All four are optional; a backend that leaves them NULL does not support
     * snapshotting (the engine keeps its fail-loudly gates).
     * -----------------------------------------------------------------------*/

    /* Park the backend's IO thread at a safe point (outside any backend
     * library call, holding no locks) and return once it is parked. Called
     * by the main thread immediately before fork() so the child inherits a
     * consistent backend state. */
    void (*snapshot_hold)(void *ctx);

    /* Release a previously held IO thread. */
    void (*snapshot_release)(void *ctx);

    /* Pause/resume on-storage garbage collection. While paused, the storage
     * locations of existing items are stable (new writes may still append).
     * Pause spans the snapshot child's lifetime. */
    void (*gc_pause)(void *ctx, int paused);

    /* Synchronous, fork-child-safe read of one item's VALUE bytes (the same
     * serialized payload that was stored via put). Runs entirely on the
     * calling thread; never touches the async IO path. Also callable from
     * the parent main thread while the IO thread is held (foreground SAVE).
     * Returns STORAGE_OK and a malloc'd *value (caller free()s), or
     * STORAGE_NOT_FOUND. */
    storageStatus (*fork_read)(void *ctx, uint32_t db_id,
                               const void *key, size_t klen,
                               void **value, size_t *vlen);

    /* -----------------------------------------------------------------------
     * Streaming snapshot (single cut, sink driven). Optional; NULL means the
     * backend cannot stream and the engine falls back to the fork_read path.
     *
     * Contract the backend MUST honour:
     *   - Establish a point in time cut at the instant of the start call.
     *   - Deliver every record that is live at that cut, exactly once, with
     *     its value as of the cut.
     *   - A record mutated, deleted, or relocated after the cut but before
     *     the producer reaches it must still be delivered with its PRE cut
     *     value. (FlashCache satisfies this with its expedite path; RocksDB
     *     would use GetSnapshot plus an iterator; the mock copies its map.)
     *   - Records written after the cut must NOT be delivered.
     *   - Invoke sink->complete exactly once, even on failure.
     *
     * Returns STORAGE_OK if the snapshot was started. The stream then runs
     * asynchronously on the backend IO thread until complete() fires.
     * -----------------------------------------------------------------------*/
    storageStatus (*snapshot_stream_start)(void *ctx, storageSnapshotSink *sink);

    /* Cancel an in flight stream. Idempotent. complete(privdata, 0) still
     * fires. Safe to call after the stream has already finished. */
    void (*snapshot_stream_abort)(void *ctx);
} storageType;

/* ---------------------------------------------------------------------------
 * Engine API (dispatch layer)
 * ---------------------------------------------------------------------------*/
int storageInit(storageType *type, storageConfig *cfg);
void storageShutdown(void);
storageStatus storageSubmitPut(uint32_t db_id, const void *key, size_t klen,
                               const void *value, size_t vlen,
                               int64_t expire_ms, void *request_ctx);
storageStatus storageSubmitGet(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);
storageStatus storageSubmitDel(uint32_t db_id, const void *key, size_t klen,
                               void *request_ctx);
int storagePollCompletions(int max);

/* Snapshot support (see storageType). No-ops / STORAGE_NOT_FOUND when the
 * active backend doesn't implement them. */
int storageSnapshotSupported(void);
void storageSnapshotHold(void);
void storageSnapshotRelease(void);
void storageGcPause(int paused);
storageStatus storageForkRead(uint32_t db_id, const void *key, size_t klen,
                              void **value, size_t *vlen);
void storageCron(void);

/* Streaming snapshot. storageSnapshotStreamSupported() is the capability probe
 * the engine must consult before choosing the streaming path; it returns 0 on
 * any backend that left the ops NULL, including the mock's degraded modes. */
int storageSnapshotStreamSupported(void);
storageStatus storageSnapshotStreamStart(storageSnapshotSink *sink);
void storageSnapshotStreamAbort(void);

/* Backend getters */
storageType *storageGetFlashCacheType(void);          /* in-memory mock (testing) */
storageType *storageGetFlashCacheRealType(void);      /* real — links libflashcache.a */
storageType *storageGetRocksDBAsyncType(void);        /* alias to mock (same impl) */

/* Serialize utility */
void *storageSerializeValue(const void *value, size_t size, size_t *out_size);
void *storageDeserializeValue(const void *data, size_t size, size_t *out_size);
void storageFreeSerializedBytes(void *buf);

#endif /* STORAGE_H */
