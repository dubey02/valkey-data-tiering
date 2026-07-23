/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Bridge between the new pluggable storage interface and ext_storage.c
 */
#ifndef EXT_STORAGE_BRIDGE_H
#define EXT_STORAGE_BRIDGE_H

#include "server.h"

/* Initialize storage backend. Call from extStorage_init(). */
int extStorageBridge_init(const char *backend_name, const char *path, size_t capacity);

/* Check if backend is ready (replaces moduleHasExternalStorageSubscribers) */
int extStorageBridge_isReady(void);

/* Submit operations (replace moduleFireExternalStorageEvent) */
int extStorageBridge_submitPut(int db_id, robj *key, robj *value, int64_t expire_ms);
int extStorageBridge_submitGet(int db_id, sds key);
int extStorageBridge_submitDel(int db_id, sds key);
int extStorageBridge_flushDB(int db_id);
int extStorageBridge_flushAll(void);
void extStorageBridge_drainOnly(void);

/* Poll completions (replaces moduleGetCompletedExternalStorageResponses).
 * Returns ValkeyModuleExternalStorageMsg** for compatibility with existing
 * processCompletedStorageRequests() logic. */
int extStorageBridge_pollCompletions(ValkeyModuleExternalStorageMsg **out, int max);

/* Shutdown */
/* Snapshot support (fork-based RDB save with tiered values) */
int extStorageBridge_snapshotSupported(void);
void extStorageBridge_snapshotHold(void);
void extStorageBridge_snapshotRelease(void);
void extStorageBridge_gcPause(int paused);
int extStorageBridge_forkRead(int db_id, sds key, char **payload, size_t *plen);

void extStorageBridge_shutdown(void);

/* Propagate MODIFIABLE FC tuning configs to live backend */
void extStorageBridge_applyFcConfigs(void);
void storageShutdown(void);

#endif
