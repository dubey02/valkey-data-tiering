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
void extStorageCron(void);

/* Crash handler only. Stops the storage engine threads with no cleanup. */
void extStorageKillThreads(void);

/* Returns 1 while a storage engine is open and ready. */
int extStorageIsActive(void);

/* Appends the Ext_storage INFO fields, the ones the valkey server counts. */
sds extStorageInfoString(sds info);

/* Appends the Storage_engine INFO fields, the ones the storage engine reports.
 * Appends nothing when no storage engine is open. */
sds extStorageEngineInfoString(sds info);

/* The serialization callbacks. */
const storageSerializer *extStorageSerializer(void);

storageState getStorageState(const robj *o);
void setStorageState(robj *o, storageState state);
robj *spillSelectCandidate(int *dbid);

#endif /* USE_EXT_STORAGE */

#endif /* VALKEY_EXT_STORAGE_H */
