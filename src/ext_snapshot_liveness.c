/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Record liveness for the streaming snapshot -- the "orphan filter".
 *
 * This exists because the streaming snapshot travels in the opposite direction
 * to the fork read path. The storage engine enumerates ITS OWN store, not the
 * engine keyspace, so it can offer a record whose key the engine has already
 * forgotten: the record simply has not had its space reclaimed yet. Writing one
 * into an RDB would resurrect a deleted key on load.
 *
 * The fork read path cannot hit this at all, because it starts from the
 * hashtable and only then reads flash. The streaming path arrives from the
 * other end and has to reject explicitly, which is what this unit does.
 *
 * It is a keyspace question, not a transport question -- no pipes, no framing,
 * no RDB format -- which is why it lives apart from the transport in
 * ext_snapshot.c.
 *
 * Consumer side. In a real save this runs in the fork child, where the
 * hashtable is the copy-on-write snapshot taken at the cut: exactly the state
 * the records were frozen against, which is what makes the verdict correct
 * rather than merely plausible.
 */

#include "server.h"
#include "ext_snapshot.h"
#include "ext_storage.h"

static long long ext_snapshot_orphans_dropped = 0;

long long extSnapshotOrphansDropped(void) { return ext_snapshot_orphans_dropped; }

int extSnapshotRecordResolve(uint32_t physical_db_id, const char *key, size_t klen,
                             int *logical_db, robj **out_entry) {
    int logical = extStorageLogicalDbId((int)physical_db_id);
    if (logical < 0 || logical >= server.dbnum) {
        ext_snapshot_orphans_dropped++;
        return 0;
    }
    serverDb *db = server.db[logical];

    /* dbFind takes an sds, not an robj. */
    sds kn = sdsnewlen(key, klen);
    dbEntry *entry = dbFind(db, kn);

    int live = 1;
    if (entry == NULL) {
        /* The engine has forgotten this key. The record is a leftover whose
         * space has not been reclaimed. Writing it would resurrect the key. */
        live = 0;
    } else if (!objectIsTiered(entry)) {
        /* Superseded: the value is back in memory, so the memory section of the
         * snapshot already carries it and this record is stale. */
        live = 0;
    } else if (entry->tiering_state == TIERING_STATE_PENDING_DELETION) {
        /* A client DEL already removed it logically; the flash copy is being
         * deleted. Same condition extStorageMaterializeTiered rejects. */
        live = 0;
    }

    sdsfree(kn);
    if (!live) {
        ext_snapshot_orphans_dropped++;
        return 0;
    }
    if (logical_db) *logical_db = logical;
    if (out_entry) *out_entry = entry;
    return 1;
}

int extSnapshotRecordIsLive(uint32_t physical_db_id, const char *key, size_t klen) {
    return extSnapshotRecordResolve(physical_db_id, key, klen, NULL, NULL);
}
