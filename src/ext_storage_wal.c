/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Client-ack WAL: engine glue (fast-boot Phase 3, steps 6-7).
 *
 * Contract: a client-visible reply to a write implies the write is durably
 * committed (scope: admission=flash, promotion=never).
 *
 * Mechanics:
 *   - signalModifiedKey() feeds a per-execution-unit dirty-key set
 *     (deduplicated: last-state-wins within the unit, so N SETs of one key
 *     in a MULTI produce ONE record of the final value).
 *   - When the outermost execution unit exits (exitExecutionUnit), each
 *     dirty key's CURRENT value is serialized ON THE MAIN THREAD -- at ack
 *     time, so the WAL records exactly the state being acknowledged (a
 *     deferred/io-thread serialization could capture a NEWER value and tear
 *     group atomicity). Missing key => tombstone. The records are framed
 *     (STANDALONE, or GROUP_BEGIN/IN_GROUP/GROUP_COMMIT for multi-record
 *     units) into one blob and handed to the WAL writer thread.
 *   - Under wal-fsync=always the issuing client is stamped with the blob's
 *     commit LSN; networking.c withholds its reply bytes until
 *     walDurableLsn() covers the stamp. The writer thread's wakeup pipe
 *     (registered as an ae file event) ends the epoll sleep when the
 *     durable LSN advances, so gated replies release promptly even on an
 *     idle server.
 *   - Under wal-fsync=everysec clients are never gated (<=1s loss window);
 *     the writer fsyncs on a timer.
 *
 * Known POC gaps (documented, not silent): FLUSHDB/FLUSHALL bypass
 * signalModifiedKey (no per-key records); TTLs are not carried in WAL
 * records (the flash log has the same gap); replication/AOF-loading
 * contexts are out of scope (standalone only).
 */

#include "server.h"
#include "ext_storage.h"
#include "storage/storage_wal.h"

/* Configs (registered in config.c) */
int ext_storage_wal_enabled = 0;
int ext_storage_wal_fsync = WAL_FSYNC_ALWAYS;

static int wal_active = 0; /* enabled AND successfully opened */

/* Stats */
static long long wal_units_emitted = 0;
static long long wal_tombstones = 0;
static long long wal_gated_releases = 0;

/* ---------------------------------------------------------------------------
 * Per-unit dirty-key set. Small linear-scan dedup: execution units touch few
 * keys; past the cap we append without dedup (replay is last-write-wins and
 * within-blob order preserves it, so duplicates are only wasted bytes).
 * ---------------------------------------------------------------------------*/
#define WAL_DIRTY_DEDUP_MAX 128

typedef struct walDirtyKey {
    int dbid;
    sds key; /* owned copy */
} walDirtyKey;

static walDirtyKey *dirty_keys = NULL;
static int dirty_count = 0;
static int dirty_cap = 0;

int extStorageWalDirtyPending(void) { return dirty_count; }

void extStorageWalSignalDirty(serverDb *db, robj *key) {
    if (!wal_active) return;
    sds keysds = (sds)objectGetVal(key);
    /* Dedup: one record per (db,key) per unit -- final state wins. */
    int scan = dirty_count < WAL_DIRTY_DEDUP_MAX ? dirty_count : WAL_DIRTY_DEDUP_MAX;
    for (int i = 0; i < scan; i++) {
        if (dirty_keys[i].dbid == db->id &&
            sdscmp(dirty_keys[i].key, keysds) == 0)
            return;
    }
    if (dirty_count == dirty_cap) {
        dirty_cap = dirty_cap ? dirty_cap * 2 : 16;
        dirty_keys = zrealloc(dirty_keys, dirty_cap * sizeof(walDirtyKey));
    }
    dirty_keys[dirty_count].dbid = db->id;
    dirty_keys[dirty_count].key = sdsdup(keysds);
    dirty_count++;
    /* Mutations outside any execution unit (tiering completion pumps in
     * beforeSleep, module contexts) must not leak into the NEXT unit's
     * blob -- they'd gate an unrelated client. Emit immediately: the
     * mutation is complete by signalModifiedKey time, and current_client
     * is NULL in these contexts so nothing gets stamped. */
    if (server.execution_nesting == 0) extStorageWalEmitUnit();
}

/* Serialize a value in the same DUMP format the spill path uses -- but
 * WITHOUT the max-spill-size rejection: a WAL record must never be silently
 * dropped, that would be a durability hole. Oversized values simply produce
 * a large WAL record. */
static sds wal_serialize_value(robj *val) {
    rio payload;
    createDumpPayload(&payload, val, NULL, -1);
    return payload.io.buffer.ptr;
}

/* Called by exitExecutionUnit() when the outermost unit ends. Serializes
 * final key states, frames, submits, stamps the current client. */
void extStorageWalEmitUnit(void) {
    if (dirty_count == 0) return;

    walBlobBuilder *b = walBlobNew();
    for (int i = 0; i < dirty_count; i++) {
        walDirtyKey *dk = &dirty_keys[i];
        serverDb *db = server.db[dk->dbid];
        robj *val = db ? dbFind(db, dk->key) : NULL;
        if (val != NULL && objectIsTiered(val)) {
            /* TIERED placeholder (metadata-only mutation of a flash-resident
             * key, e.g. EXPIRE): the value bytes live on flash and are
             * already durable there -- serializing the stub would log
             * garbage, and a tombstone would delete a live key at replay.
             * Skip. (TTL changes are not WAL-carried; same pre-existing gap
             * as the flash log itself.) */
            sdsfree(dk->key);
            continue;
        }
        if (val != NULL) {
            sds ser = wal_serialize_value(val);
            walBlobAddItem(b, (uint32_t)dk->dbid, dk->key, sdslen(dk->key),
                           ser, sdslen(ser), 0);
            sdsfree(ser);
        } else {
            /* Key gone by unit end (deleted, or expired within the unit):
             * durable delete. NOTE: a key demoted to flash mid-unit would
             * also miss dbFind -- under submit-at-unit-end semantics the
             * spill controller runs in beforeSleep, strictly after this
             * hook, so a key dirtied in THIS unit cannot have spilled yet. */
            walBlobAddItem(b, (uint32_t)dk->dbid, dk->key, sdslen(dk->key),
                           NULL, 0, 1);
            wal_tombstones++;
        }
        sdsfree(dk->key);
    }
    dirty_count = 0;

    char *buf = NULL;
    size_t len = 0;
    uint64_t commit_lsn = walBlobFinalize(b, &buf, &len);
    walBlobFree(b);
    if (commit_lsn == 0) return;

    walSubmit(buf, len, commit_lsn);
    wal_units_emitted++;

    /* Gate the issuing client's reply on durability (always policy only).
     * current_client covers direct commands, EXEC, and scripts; cron units
     * (expiry, eviction) have no client -- their records are durable soon,
     * nothing waits. */
    if (ext_storage_wal_fsync == WAL_FSYNC_ALWAYS && server.current_client)
        server.current_client->wal_lsn = commit_lsn;
}

/* Reply-release predicate for networking.c. Returns 1 if this client's
 * replies must be withheld (last write not yet durable). */
int extStorageWalReplyGated(client *c) {
    if (!wal_active || c->wal_lsn == 0) return 0;
    if (walDurableLsn() >= c->wal_lsn) {
        c->wal_lsn = 0; /* durable: clear the stamp, stop checking */
        wal_gated_releases++;
        return 0;
    }
    return 1;
}

/* Wakeup pipe handler: drain the pipe. Its only job is ending the epoll
 * sleep -- the next beforeSleep's handleClientsWithPendingWrites retries
 * gated clients (they stayed in clients_pending_write). */
static void walWakeupReadable(aeEventLoop *el, int fd, void *privdata,
                              int mask) {
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);
    char drain[64];
    while (read(fd, drain, sizeof(drain)) > 0)
        ;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/
int extStorageWalInit(const char *flash_path) {
    if (!ext_storage_wal_enabled) return 0;
    char wal_path[4096];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", flash_path);
    int wakeup_fd = -1;
    if (walOpen(wal_path, ext_storage_wal_fsync, &wakeup_fd) != 0) {
        serverLog(LL_WARNING, "ext-storage-wal: failed to open %s", wal_path);
        return -1;
    }
    if (aeCreateFileEvent(server.el, wakeup_fd, AE_READABLE, walWakeupReadable,
                          NULL) == AE_ERR) {
        serverLog(LL_WARNING, "ext-storage-wal: failed to register wakeup fd");
        walClose();
        return -1;
    }
    wal_active = 1;
    serverLog(LL_NOTICE,
              "ext-storage-wal: enabled at %s (fsync=%s) -- write replies "
              "are gated on durability",
              wal_path,
              ext_storage_wal_fsync == WAL_FSYNC_ALWAYS ? "always"
                                                        : "everysec");
    return 0;
}

void extStorageWalShutdown(void) {
    if (!wal_active) return;
    walClose();
    wal_active = 0;
}

int extStorageWalActive(void) { return wal_active; }

/* Runtime fsync-policy propagation (MODIFIABLE config apply hook). */
void extStorageWalApplyFsyncPolicy(void) {
    if (wal_active) walSetFsyncPolicy(ext_storage_wal_fsync);
}

sds genExtStorageWalInfoString(sds info) {
    if (!ext_storage_wal_enabled) return info;
    walStats st;
    walGetStats(&st);
    info = sdscatprintf(
        info,
        "wal_active:%d\r\n"
        "wal_fsync_policy:%s\r\n"
        "wal_units:%lld\r\n"
        "wal_records:%llu\r\n"
        "wal_groups:%llu\r\n"
        "wal_tombstones:%lld\r\n"
        "wal_bytes:%llu\r\n"
        "wal_fsyncs:%llu\r\n"
        "wal_last_lsn:%llu\r\n"
        "wal_durable_lsn:%llu\r\n"
        "wal_gated_releases:%lld\r\n"
        "wal_ring_full_stalls:%llu\r\n",
        wal_active,
        ext_storage_wal_fsync == WAL_FSYNC_ALWAYS ? "always" : "everysec",
        wal_units_emitted, (unsigned long long)st.records,
        (unsigned long long)st.groups, wal_tombstones,
        (unsigned long long)st.bytes, (unsigned long long)st.fsyncs,
        (unsigned long long)walLastAssignedLsn(),
        (unsigned long long)walDurableLsn(), wal_gated_releases,
        (unsigned long long)st.ring_full_stalls);
    return info;
}
