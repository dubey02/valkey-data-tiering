/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Spilling: moving cold values from memory to the storage engine. */

#include "server.h"

#ifdef USE_EXT_STORAGE

#include "ext_storage.h"

/* Separate from the eviction pool so spill and eviction don't take each
 * other's keys.
 * TODO(#111): eviction must skip keys that are COPYING_TO_FLASH. */
static struct evictionPoolEntry *spill_pool;

void spillPoolCreate(void) {
    spill_pool = evictionPoolCreate();
}

void spillPoolRelease(void) {
    if (spill_pool == NULL) return;
    evictionPoolRelease(spill_pool);
    spill_pool = NULL;
}

/* Embedded and INT values have nothing separate to free, shared values can't be
 * freed, and module values aren't supported yet (#81). */
bool isSpillCandidate(const robj *o) {
    return getStorageState(o) == STORAGE_STATE_ONLY_MEMORY && !o->hasembval &&
           objectGetEncoding(o) != OBJ_ENCODING_INT && objectGetRefcount(o) == 1 && objectGetType(o) != OBJ_MODULE;
}

/* Samples like performEvictions() and returns the coldest spillable key, or
 * NULL if none was sampled. Only allkeys-lru and allkeys-lfu pick keys. */
robj *spillSelectCandidate(int *dbid) {
    if (server.maxmemory_policy != MAXMEMORY_ALLKEYS_LRU && server.maxmemory_policy != MAXMEMORY_ALLKEYS_LFU) {
        return NULL;
    }
    serverAssert(spill_pool != NULL);

    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        unsigned long db_keys = kvstoreSize(db->keys);
        if (db_keys == 0) continue;

        unsigned long sampled_keys = 0;
        int l = kvstoreNumNonEmptyHashtables(db->keys);
        while (l--) {
            sampled_keys += evictionPoolPopulate(db, db->keys, spill_pool, isSpillCandidate);
            if (sampled_keys >= (unsigned long)server.maxmemory_samples) break;
            if (db_keys < (unsigned long)server.maxmemory_samples * 10) break;
        }
    }

    /* Keys may have changed or been submitted since they were sampled. */
    int slot;
    return evictionPoolPop(spill_pool, isSpillCandidate, dbid, &slot);
}

#endif /* USE_EXT_STORAGE */
