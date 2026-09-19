/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Pluggable storage engine interface for data tiering.
 *
 * A storage engine holds items spilled out of memory, addressed by (db_id,
 * key). A key is an sds and a value is a robj, both passed as void * so a
 * storage engine need not know the object layout. */

#ifndef VALKEY_STORAGE_H
#define VALKEY_STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* Version of this interface. A storage engine reports the one it was built
 * against. */
#define VALKEY_STORAGE_API_VERSION 1

/* Result of a storage operation. Negative values are failures. */
typedef enum {
    STORAGE_OK = 0,
    /* No record for this (db_id, key). */
    STORAGE_NOT_FOUND = 1,
    /* Busy. The request was not taken and may be retried. */
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

/* Callbacks that convert between objects and bytes. */
typedef struct storageSerializer {
    /* Object to bytes. Returns the byte length, or -1 on failure. */
    int (*serialize_key)(void *key, char **out);
    int (*serialize_value)(void *val_obj, char **out);

    /* Bytes to object. Returns NULL on a malformed buffer. */
    void *(*deserialize_key)(const char *bytes, int len);
    void *(*deserialize_value)(const char *bytes, int len);

    /* Frees a buffer from the matching serialize callback. */
    void (*free_serialized_key)(char *bytes);
    void (*free_serialized_value)(char *bytes);
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
    /* The value object, for a successful GET. NULL otherwise. */
    void *val_obj;
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
    /* Storage-engine-specific options, forwarded uninterpreted by the valkey
     * server. Only the storage engine understands them. */
    const storageOption *options;
    /* Number of entries in options. */
    size_t num_options;
} storageConfig;

/* Counters reported by a storage engine. */
typedef struct storageStats {
    /* Items spilled to, fetched from, and deleted from storage. */
    uint64_t total_num_items_spilled_to_storage;
    uint64_t total_num_items_fetched_from_storage;
    uint64_t total_num_items_deleted_from_storage;
    /* Items currently held. */
    uint64_t total_num_items;
    /* Serialized bytes currently held. */
    uint64_t total_num_bytes;
} storageStats;

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
    /* Closes the storage engine. */
    void (*close)(void);

    /* Asynchronous put, get, and delete. */
    storageStatus (*put_async)(uint32_t db_id, void *key_obj, void *val_obj, void *request_ctx);
    storageStatus (*get_async)(uint32_t db_id, void *key_obj, void *request_ctx);
    storageStatus (*del_async)(uint32_t db_id, void *key_obj, void *request_ctx);

    /* Collects finished asynchronous requests. */
    int (*poll_completions)(storageCompletion *out, int max);

    /* Reports the storage engine's counters. */
    void (*get_stats)(storageStats *out);
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
