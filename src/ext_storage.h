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

#include "sds.h"
#include "storage/storage.h"

/* Opens the configured storage engine. */
void extStorageInit(void);

/* Closes the storage engine. */
void extStorageDeinit(void);

/* Whether a storage engine is open and ready. */
int extStorageIsActive(void);

/* Appends the ext_storage INFO section to the given sds. */
sds extStorageInfoString(sds info);

/* The serialization callbacks. */
const storageSerializer *extStorageSerializer(void);

/* Test-only: clears the registered storage engine. */
void extStorageTestResetRegistry(void);

#endif /* USE_EXT_STORAGE */

#endif /* VALKEY_EXT_STORAGE_H */
