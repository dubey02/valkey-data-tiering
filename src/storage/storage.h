/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Pluggable storage engine interface for data tiering.
 *
 * A storage engine holds items spilled out of memory, addressed by (db_id, key)
 * where the key is raw bytes. It stores the serialized value bytes a callback
 * hands it, so a storage engine need not know the object layout.
 *
 * Every call into a storage engine comes from the main thread, except
 * kill_threads, which the crash handler calls. */

#ifndef VALKEY_STORAGE_H
#define VALKEY_STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* Version of this interface. A storage engine reports the one it was built
 * against. Any layout change in this header bumps it. */
#define VALKEY_STORAGE_API_VERSION 1

/* A valkey object, opaque to a storage engine. */
typedef struct serverObject valkeyObject;

/* Result of a storage operation. Negative values are failures. */
typedef enum {
    STORAGE_OK = 0,
    /* No record for this (db_id, key), on a get. */
    STORAGE_NOT_FOUND = 1,
    /* Busy. The request was not taken and may be retried. Returned by
     * put_async, get_async, del_async and close, never carried by a
     * completion. */
    STORAGE_WOULDBLOCK = 2,
    /* An IO error. */
    STORAGE_ERR_IO = -1,
    /* Out of capacity. */
    STORAGE_ERR_FULL = -2,
    /* Rejected on policy grounds, such as an oversized value. */
    STORAGE_ERR_REJECTED = -3,
} storageStatus;

/* The kind of storage operation. */
typedef enum {
    STORAGE_OP_PUT = 0,
    STORAGE_OP_GET = 1,
    STORAGE_OP_DEL = 2,
} storageOpType;

/* Callbacks that convert between a keyspace entry and its serialized bytes.
 * A storage engine calls them on its own thread, never on the main thread. */
typedef struct storageSerializer {
    /* A keyspace entry to bytes. Returns the byte length, or -1 on failure. A
     * storage engine reports a failure here as STORAGE_ERR_REJECTED.
     * request_ctx is the token from the put_async call that lent the entry. */
    int (*serialize)(const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx, char **out);

    /* Bytes to a keyless value object. Returns NULL on a malformed buffer. */
    valkeyObject *(*deserialize)(const char *key, size_t key_len, const char *bytes, int len);

    /* Frees a buffer from serialize. */
    void (*free_serialized)(char *bytes);
} storageSerializer;

/* ---------------------------------------------------------------------------
 * Completions
 * ---------------------------------------------------------------------------*/

/* A finished asynchronous request. */
typedef struct storageCompletion {
    /* The token supplied to the *_async call. */
    void *request_ctx;
    /* The operation this completes. */
    storageOpType op_type;
    /* The operation's result. */
    storageStatus status;
    /* The database the record belongs to. */
    uint32_t db_id;
    /* The keyless value object, for a successful GET. NULL otherwise. Owned by
     * the caller once polled. */
    valkeyObject *value;
    /* The serialized size of the value, for a PUT. Zero otherwise. */
    size_t stored_bytes;
} storageCompletion;

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/

/* A storage-engine-specific config option, as a name and value pair. */
typedef struct storageOption {
    const char *name;
    const char *value;
} storageOption;

/* Setup for the storage engine's threads. */
typedef struct storageThreadConfig {
    /* Minimum stack size in bytes. Zero means the platform default. */
    size_t stack_size;
    /* Called first on every storage engine thread, with the thread name. May be NULL. */
    void (*on_thread_start)(const char *name);
} storageThreadConfig;

/* Log levels, lowest first. */
typedef enum storageLogLevel {
    STORAGE_LOG_DEBUG = 0,
    STORAGE_LOG_VERBOSE,
    STORAGE_LOG_NOTICE,
    STORAGE_LOG_WARNING
} storageLogLevel;

/* Writes one line to the valkey server log. */
typedef void (*storageLogFn)(storageLogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Reads the valkey server's monotonic clock in microseconds. */
typedef uint64_t (*storageMonotonicClockFn)(void);

/* Settings a storage engine is opened with. */
typedef struct storageConfig {
    /* Directory for the storage engine's data. */
    const char *path;
    /* Byte budget for the storage engine. */
    uint64_t capacity_bytes;
    /* Number of databases. */
    uint32_t num_databases;
    /* Serialization callbacks. */
    storageSerializer serializer;
    /* Thread setup. */
    storageThreadConfig thread_config;
    /* Logging. Never NULL. */
    storageLogFn logger;
    /* Monotonic clock in microseconds. Never NULL. */
    storageMonotonicClockFn monotonic_clock_us;
    /* Storage-engine-specific options, forwarded uninterpreted by the valkey
     * server. Only the storage engine understands them. */
    const storageOption *options;
    /* Number of entries in options. */
    size_t num_options;
} storageConfig;

/* ---------------------------------------------------------------------------
 * INFO
 * ---------------------------------------------------------------------------*/

/* The storage engine reports INFO fields through this sink from inside its
 * info callback. Fields print as storage_engine_<name>:<value>. A dict prints
 * its fields as name=value pairs on one line and cannot nest. */
typedef struct storageInfoSink {
    /* Passed back as the first argument of every call below. */
    void *ctx;
    void (*add_long_long)(void *ctx, const char *name, long long value);
    void (*add_ulong_long)(void *ctx, const char *name, unsigned long long value);
    void (*add_double)(void *ctx, const char *name, double value);
    void (*add_cstring)(void *ctx, const char *name, const char *value);
    void (*begin_dict)(void *ctx, const char *name);
    void (*end_dict)(void *ctx);
} storageInfoSink;

/* ---------------------------------------------------------------------------
 * The storage engine
 * ---------------------------------------------------------------------------*/

/* A storage engine implementation. */
typedef struct storageEngine {
    /* The name that selects this storage engine. */
    const char *name;
    /* The interface version it was built against. */
    int api_version;

    /* Opens the storage engine. */
    storageStatus (*open)(const storageConfig *cfg);
    /* Closes the storage engine. Returns STORAGE_WOULDBLOCK while any request
     * is unpolled, since an unpolled fetch may hold the only copy of a value.
     * The caller polls and retries. */
    storageStatus (*close)(void);
    /* Crash handler only. Stops the storage engine threads with no cleanup.
     * May be NULL. */
    void (*kill_threads)(void);

    /* Asynchronous put, get, and delete. The key is freed by the caller
     * after the call returns. The entry is not modified or freed until its
     * completion is polled. A get consumes the record, so a second get of the
     * same key reports STORAGE_NOT_FOUND until it is put again. A delete
     * completes with STORAGE_OK whether or not a record existed. */
    storageStatus (*put_async)(uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx);
    storageStatus (*get_async)(uint32_t db_id, const char *key, size_t key_len, void *request_ctx);
    storageStatus (*del_async)(uint32_t db_id, const char *key, size_t key_len, void *request_ctx);

    /* Collects finished asynchronous requests, in completion order, at most
     * max. Requests may finish out of submission order. Must not block the
     * valkey server's main thread. */
    int (*poll_completions)(storageCompletion *out, int max);

    /* Reports the storage engine's own counters for INFO. Runs on the main
     * thread, so a storage engine with its own threads reads a snapshot it
     * refreshes itself. May be NULL when there is nothing to report. */
    void (*info)(const storageInfoSink *sink);
} storageEngine;

/* ---------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------------*/

/* Registers a module storage engine. A built-in storage engine is selected by
 * name and is not registered here. Returns STORAGE_OK, or STORAGE_ERR_REJECTED
 * for a bad name, a version mismatch, or a second registration. */
storageStatus storageRegisterEngine(const storageEngine *engine);

/* Returns the registered module storage engine matching the name, or NULL. */
const storageEngine *storageLookupEngine(const char *name);

#endif /* VALKEY_STORAGE_H */
