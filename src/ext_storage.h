/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Data tiering storage-engine-side entry points. Present only in a build with
 * BUILD_EXT_STORAGE, which defines USE_EXT_STORAGE. Without it, the hooks the
 * rest of the server calls are stubs. */

#ifndef VALKEY_EXT_STORAGE_H
#define VALKEY_EXT_STORAGE_H

#include "server.h"

#ifdef USE_EXT_STORAGE

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

/* Returns the coldest key that can be spilled and sets *dbid, or NULL. */
robj *extStorageSpillSelectCandidate(int *dbid);

/* Submits a key's value to the storage engine. Nothing changes unless it
 * returns STORAGE_OK. */
storageStatus extStorageSpillItem(int dbid, robj *entry);

/* DEBUG SPILL <key> [<key> ...]. */
void debugSpillCommand(client *c);

/* Whether the value is only in DRAM, so it can be moved or freed. */
static inline bool extStorageIsMemoryOnly(const robj *o) {
    return o->storage_state == STORAGE_STATE_ONLY_MEMORY;
}

/* Waits for every spill in flight to finish. */
void extStorageSpillDrain(void);

#else

static inline bool extStorageIsMemoryOnly(const robj *o) {
    UNUSED(o);
    return true;
}
static inline void extStorageSpillDrain(void) {
}

#endif /* USE_EXT_STORAGE */

#endif /* VALKEY_EXT_STORAGE_H */
