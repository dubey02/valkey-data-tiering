/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Data tiering storage-engine-side entry points. Present only in a build with
 * BUILD_EXT_STORAGE, which defines USE_EXT_STORAGE. */

#ifndef VALKEY_EXT_STORAGE_H
#define VALKEY_EXT_STORAGE_H

#ifdef USE_EXT_STORAGE

#include "server.h"
#include "storage/storage.h"

/* Opens the configured storage engine. */
void extStorageInit(void);

/* Closes the storage engine. Outstanding requests are handled first. */
void extStorageDeinit(void);

/* Handles finished requests. */
void extStorageBeforeSleep(void);

/* Crash handler only. Stops the storage engine threads with no cleanup. */
void extStorageKillThreads(void);

/* Returns 1 while a storage engine is open and ready. */
int extStorageIsActive(void);

/* Appends the Ext_storage INFO fields, the ones the valkey server counts. */
sds extStorageInfoString(sds info);

/* Returns 1 while an open storage engine has INFO fields to report. The
 * Storage_engine section is printed only then. */
int extStorageEngineHasInfo(void);

/* Appends the Storage_engine INFO fields, the ones the storage engine reports. */
sds extStorageEngineInfoString(sds info);

/* The serialization callbacks. */
const storageSerializer *extStorageSerializer(void);

/* The same callbacks, except that serializing a value also sizes it for the
 * spill accounting below. Every put to a storage engine opened with these
 * passes a spillSizing as its request_ctx. */
const storageSerializer *extStorageSpillSerializer(void);

storageState getStorageState(const robj *o);
void setStorageState(robj *o, storageState state);
robj *spillSelectCandidate(int *dbid);

/* Spill accounting: what spills in flight will free. */

/* A spill's place in the accounting, passed as its put_async request_ctx. The
 * serializer fills it in on the storage engine's thread, before the engine
 * posts the completion. */
typedef struct spillSizing {
    bool sized;       /* The serializer has measured the value. */
    size_t ram_bytes; /* What freeing the value frees, once sized. */
} spillSizing;

/* Counts a spill as in flight. Called before put_async. */
void spillAccountSubmit(spillSizing *s);

/* Takes a spill back out, once its completion is polled or put_async refuses it. */
void spillAccountFinish(spillSizing *s);

/* What spills in flight will free: their exact sizes once sized, the mean before. */
size_t spillAccountInflightRamBytes(void);

/* Clears the accounting. Nothing may be in flight. */
void spillAccountReset(void);

typedef struct spillAccountStats {
    unsigned long inflight_requests; /* Spills submitted and not finished. */
    unsigned long unsized_requests;  /* Of those, the ones the serializer hasn't sized. */
    size_t sized_ram_bytes;          /* What the sized ones will free. */
    size_t ram_mean;                 /* Mean measured size. Each unsized spill counts as this. */
} spillAccountStats;

void spillAccountGetStats(spillAccountStats *out);

#endif /* USE_EXT_STORAGE */

#endif /* VALKEY_EXT_STORAGE_H */
