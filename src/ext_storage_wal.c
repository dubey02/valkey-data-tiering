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
#include "storage/storage.h"

/* Configs (registered in config.c) */
int ext_storage_wal_enabled = 0;
int ext_storage_wal_fsync = WAL_FSYNC_ALWAYS;

static int wal_active = 0; /* enabled AND successfully opened */

/* Stats */
extern _Atomic long fc_dbg_push_fail_get, fc_dbg_push_fail_del,
       fc_dbg_push_fail_put, fc_dbg_get_submitted, fc_dbg_get_dispatched;
long long ext_storage_checkpoint_mb = 1024; /* config: 0 = disabled */
long long ext_storage_wal_max_mb = 1024;     /* config: retirement threshold */

/* Retirement table (step 8): (dbid,key) -> {acked_lsn, lsn_at_submit}.
 * An entry exists while the key has an acked write whose bytes may not yet
 * be durable in the main log. Erased at spill completion iff no newer ack
 * arrived after the spill was submitted (the spill serialized the current
 * value, covering every ack up to the submission stamp). */
/* Value packs three fields:
 *   bit  0      : covered (a spill/Del serialized the current value after
 *                 the last ack)
 *   bits 1..32  : cover epoch (meaningful when covered; see below)
 *   bits 33..63 : WAL segment generation of the ack that (last) dirtied the
 *                 entry. 0 = boot-replay evidence (.replay file). Used to
 *                 decide which sealed WAL segments still back un-retired
 *                 acks and must be kept for crash replay.
 * The retirement sweep only erases entries covered strictly BEFORE the
 * checkpoint request: those spills preceded the checkpoint's flush barrier
 * (which runs at fork time, after the request) and are therefore durable in
 * the log. Entries covered later may still be staging-resident when the
 * (forked, asynchronous) checkpoint completes -- erasing them would let the
 * WAL retire away their only durable copy. */
static uint64_t wal_cover_epoch = 1;
#define WAL_EPOCH_MASK 0x1FFFFFFFEULL /* bits 1..32 */
#define WAL_SEG_SHIFT 33
#define WAL_DIRTY_VAL(seg) ((uint64_t)(seg) << WAL_SEG_SHIFT)
#define WAL_COVERED_VAL(old) \
    (((old) & ~(WAL_EPOCH_MASK | 1ULL)) | ((wal_cover_epoch << 1) & WAL_EPOCH_MASK) | 1ULL)
#define WAL_IS_COVERED(v) ((v) & 1)
#define WAL_COVER_EPOCH_OF(v) (((v) & WAL_EPOCH_MASK) >> 1)
#define WAL_SEG_OF(v) ((v) >> WAL_SEG_SHIFT)
static dict *wal_dirty_table = NULL;
static char wal_path_buf[4096];
static char wal_replay_path[4096];
static long long wal_truncations = 0;
static long long wal_replayed_records = 0;
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
    int tombstone;
} walDirtyKey;

static walDirtyKey *dirty_keys = NULL;
static int dirty_count = 0;
static int dirty_cap = 0;

int extStorageWalDirtyPending(void) { return dirty_count; }

static sds walDirtyTableKey(int dbid, const char *key, size_t klen) {
    sds k = sdsnewlen(NULL, 0);
    k = sdscatlen(k, &dbid, sizeof(dbid));
    k = sdscatlen(k, key, klen);
    return k;
}

static dictType walDirtyDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = dictEntryDestructorSdsKey,
};

/* Mark (dbid,key) DIRTY: an ack whose bytes may not yet be in the main log.
 * <seg> is the WAL segment generation holding the ack's record (0 = the
 * .replay evidence file, for boot-replayed entries). Stamping an older seg
 * than the record's true location is safe (retains more); newer is not --
 * callers stamp BEFORE submitting the record, and rotation only ever moves
 * later records to later segments, so the stamp is always <= actual. */
static void walDirtyUpsert(int dbid, const char *key, size_t klen, uint64_t seg) {
    if (wal_dirty_table == NULL) return;
    sds k = walDirtyTableKey(dbid, key, klen);
    dictEntry *de = dictFind(wal_dirty_table, k);
    if (de != NULL) {
        dictSetUnsignedIntegerVal(de, WAL_DIRTY_VAL(seg));
        sdsfree(k);
    } else {
        de = dictAddRaw(wal_dirty_table, k, NULL);
        if (de != NULL) dictSetUnsignedIntegerVal(de, WAL_DIRTY_VAL(seg));
        else sdsfree(k);
    }
}

void extStorageWalNoteSpillSubmit(int dbid, const char *key, size_t klen) {
    if (!wal_active || wal_dirty_table == NULL) return;
    sds k = walDirtyTableKey(dbid, key, klen);
    dictEntry *de = dictFind(wal_dirty_table, k);
    if (de != NULL)
        dictSetUnsignedIntegerVal(de,
            WAL_COVERED_VAL(dictGetUnsignedIntegerVal(de)));
    sdsfree(k);
}

void extStorageWalNoteSpillDurable(int dbid, const char *key, size_t klen) {
    if (!wal_active || wal_dirty_table == NULL) return;
    sds k = walDirtyTableKey(dbid, key, klen);
    dictEntry *de = dictFind(wal_dirty_table, k);
    if (de != NULL && WAL_IS_COVERED(dictGetUnsignedIntegerVal(de)))
        dictDelete(wal_dirty_table, k);
    sdsfree(k);
}

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
            dk->key = NULL;
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
            dk->tombstone = 1;
        }
    }

    char *buf = NULL;
    size_t len = 0;
    uint64_t commit_lsn = walBlobFinalize(b, &buf, &len);
    walBlobFree(b);
    /* Retirement bookkeeping (step 8): every key in this unit now has an
     * acked record at commit_lsn. Tombstones are stamped submitted
     * immediately: their backend Del was issued during the unit, so the
     * next checkpoint's flush barrier covers them. */
    for (int i = 0; i < dirty_count; i++) {
        walDirtyKey *dk = &dirty_keys[i];
        if (dk->key == NULL) continue;
        if (commit_lsn != 0) {
            walDirtyUpsert(dk->dbid, dk->key, sdslen(dk->key),
                           walActiveSegGen());
            /* Coverage at emit time:
             * - tombstone: the backend Del was submitted during the unit.
             * - COPYING_TO_FLASH: flash admission submitted the spill during
             *   the unit (setKey CREATE branch), and the state machine
             *   blocks further writes until it completes, so that spill
             *   covers this ack. All other states: a later spill submission
             *   stamps coverage via extStorageWalNoteSpillSubmit(). */
            int covered = dk->tombstone;
            if (!covered) {
                serverDb *cdb = server.db[dk->dbid];
                if (cdb != NULL &&
                    extStorageGetState(cdb, dk->key) == TIERING_STATE_COPYING_TO_FLASH)
                    covered = 1;
            }
            if (covered)
                extStorageWalNoteSpillSubmit(dk->dbid, dk->key, sdslen(dk->key));
        }
        sdsfree(dk->key);
        dk->key = NULL;
        dk->tombstone = 0;
    }
    dirty_count = 0;
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
/* Step 8 boot replay: apply one surviving WAL record into the engine.
 * Items land in the dict as ordinary (dirty) keys -- the normal spill
 * pipeline re-persists them -- and register in the retirement table so the
 * .replay file survives until a checkpoint has flushed them to the log.
 * File order == LSN order, so sequential apply is last-write-wins. */
static void wal_boot_apply(void *ctx, uint32_t dbid, const void *key,
                           size_t klen, const void *val, size_t vlen,
                           int tombstone) {
    (void)ctx;
    if (dbid >= (uint32_t)server.dbnum) return;
    serverDb *db = createDatabaseIfNeeded(dbid);
    robj keyobj;
    sds key_sds = sdsnewlen(key, klen);
    initStaticStringObject(keyobj, key_sds);
    if (tombstone) {
        if (dbFind(db, key_sds) != NULL) dbDelete(db, &keyobj);
        /* Kill any flash copy too (idempotent; appends an FC tombstone). */
        extStorageBridge_submitDel(dbid, key_sds);
        walDirtyUpsert(dbid, key_sds, klen, 0); /* seg 0 = .replay evidence */
        {
            sds tk = walDirtyTableKey(dbid, key_sds, klen);
            dictEntry *tde = dictFind(wal_dirty_table, tk);
            if (tde != NULL)
                dictSetUnsignedIntegerVal(tde,
                    WAL_COVERED_VAL(dictGetUnsignedIntegerVal(tde)));
            sdsfree(tk);
        }
        sdsfree(key_sds);
        wal_replayed_records++;
        return;
    }
    /* Deserialize the DUMP payload the emit path produced. */
    rio payload;
    rioInitWithBuffer(&payload, (sds)val); /* borrowed window */
    payload.io.buffer.ptr = sdsnewlen(val, vlen);
    robj *obj = NULL;
    int type = rdbLoadObjectType(&payload);
    if (type != -1)
        obj = rdbLoadObject(type, &payload, key_sds, dbid, NULL, RDBFLAGS_NONE, 0);
    sdsfree(payload.io.buffer.ptr);
    if (obj == NULL) {
        serverLog(LL_WARNING, "ext-storage-wal: replay: bad payload for a key; skipped");
        sdsfree(key_sds);
        return;
    }
    if (dbFind(db, key_sds) != NULL) dbDelete(db, &keyobj); /* stub or older value */
    if (!dbAddRDBLoad(db, key_sds, &obj)) {
        decrRefCount(obj);
        sdsfree(key_sds);
        return;
    }
    /* key_sds ownership passed to the dict. Register for retirement with
     * seg 0: the record's only durable copy is the .replay evidence file. */
    walDirtyUpsert(dbid, key, klen, 0);
    wal_replayed_records++;
}

/* Merge <wal> onto <wal>.replay (records only; strip the magic of the
 * second file) so exactly one replay file exists across repeated crashes. */
static void wal_merge_into_replay(const char *wal, const char *replay) {
    FILE *src = fopen(wal, "rb");
    if (src == NULL) return;
    FILE *dst = fopen(replay, "ab");
    if (dst == NULL) { fclose(src); return; }
    if (ftell(dst) == 0) {
        /* fresh .replay: keep a valid header */
        fwrite(WAL_FILE_MAGIC, 1, WAL_FILE_MAGIC_LEN, dst);
    }
    fseek(src, WAL_FILE_MAGIC_LEN, SEEK_SET);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(dst);
    fclose(src);
    unlink(wal);
}

int extStorageWalInit(const char *flash_path) {
    if (!ext_storage_wal_enabled) return 0;
    char wal_path[4096];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", flash_path);
    snprintf(wal_path_buf, sizeof(wal_path_buf), "%s", wal_path);
    snprintf(wal_replay_path, sizeof(wal_replay_path), "%s.replay", wal_path);

    wal_dirty_table = dictCreate(&walDirtyDictType);

    /* Step 8: replay acked-but-possibly-unflushed records from the previous
     * run BEFORE opening the fresh WAL. Chronological: .replay (older crash
     * evidence) first, then sealed segments in generation order, then the
     * leftover active WAL. Everything is then merged into .replay -- the
     * evidence is only deleted once a checkpoint's flush barrier has
     * re-persisted its content into the main log. */
    long r1 = 0, r2 = 0;
    if (access(wal_replay_path, F_OK) == 0)
        r1 = walReplayFile(wal_replay_path, wal_boot_apply, NULL);
    /* Sealed segments: <wal>.seg.<gen>. Scan a contiguous range from 1 --
     * the retire cron releases from the low end, so live gens are always a
     * suffix; probe upward past leading holes until a run of misses. */
    {
        char seg[4200];
        int misses = 0;
        for (uint64_t g = 1; misses < 64; g++) {
            snprintf(seg, sizeof(seg), "%s.seg.%llu", wal_path,
                     (unsigned long long)g);
            if (access(seg, F_OK) != 0) {
                misses++;
                continue;
            }
            misses = 0;
            long rs = walReplayFile(seg, wal_boot_apply, NULL);
            if (rs > 0) r2 += rs;
            wal_merge_into_replay(seg, wal_replay_path); /* unlinks seg */
        }
    }
    if (access(wal_path, F_OK) == 0) {
        long ra = walReplayFile(wal_path, wal_boot_apply, NULL);
        serverLog(LL_NOTICE, "wal-debug: REPLAY active file applied=%ld", ra);
        if (ra > 0) r2 += ra;
        wal_merge_into_replay(wal_path, wal_replay_path);
    }
    if (r1 > 0 || r2 > 0) {
        serverLog(LL_NOTICE,
                  "ext-storage-wal: boot replay applied %ld records "
                  "(%ld from prior crash evidence); retained at %s until "
                  "checkpointed",
                  r1 + r2, r1, wal_replay_path);
    }

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
    if (wal_dirty_table != NULL) {
        dictRelease(wal_dirty_table);
        wal_dirty_table = NULL;
    }
    if (!wal_active) return;
    walClose();
    wal_active = 0;
}

int extStorageWalActive(void) { return wal_active; }

/* Runtime fsync-policy propagation (MODIFIABLE config apply hook). */
void extStorageWalApplyFsyncPolicy(void) {
    if (wal_active) walSetFsyncPolicy(ext_storage_wal_fsync);
}

/* Retirement cron (step 8): when the WAL (or leftover replay evidence)
 * exceeds the threshold, request an FC checkpoint (io thread: flush barrier
 * + head journal + index serialize). Once it completes, every table entry
 * whose spill was submitted before the barrier is durable in the log: sweep
 * them; if the table empties, the WAL content is fully covered -> truncate
 * the active file and delete the replay evidence. */
/* Retirement cron (step 8, reworked for sustained load): when the active
 * WAL exceeds the threshold (or sealed segments / replay evidence await
 * release), seal the active file as a segment and request an FC checkpoint
 * (io thread: flush barrier + head journal + index serialize). Once it
 * completes, every table entry whose spill was submitted before the barrier
 * is durable in the log: sweep them, then release every sealed segment older
 * than the oldest surviving entry's segment. No write quiesce is required --
 * under sustained writes new acks land in the NEW active file and never pin
 * the sealed ones. The old design gated release on a fully-empty dirty
 * table, which is unreachable under load; with fork checkpoints (no io
 * stalls to quiesce writers) it degenerated into a continuous checkpoint
 * storm (734 back-to-back forks observed on the r7gd) with unbounded WAL
 * growth. */
static uint64_t wal_ckpt_need_seq = 0;
static uint64_t wal_ckpt_request_epoch = 0;
static int wal_retire_state = 0; /* 0=idle 1=awaiting checkpoint */
static uint64_t wal_seg_lo = 1;  /* lowest sealed segment not yet released */
static uint64_t wal_rotate_wait_gen = 0; /* rotate_gen to wait for; 0=none */
static mstime_t wal_last_ckpt_request_ms = 0;
#define WAL_CKPT_MIN_INTERVAL_MS 2000

void extStorageWalCron(void) {
    if (!wal_active) return;
    int replay_pending = (access(wal_replay_path, F_OK) == 0);
    /* Drain DIRTY entries: keys acked (often boot-replayed) whose spill was
     * never submitted -- without memory pressure the spill controller ignores
     * them, which would pin the WAL forever. Submit spills directly, bounded
     * per tick; ring backpressure just defers to the next tick. Keys no
     * longer in the dict already completed their round trip (spilled +
     * dropped, or deleted with the Del submitted) -> COVERED. */
    if (wal_dirty_table != NULL && dictSize(wal_dirty_table) > 0) {
        /* Cap well below the backend request ring (4096): a pump burst that
         * saturates the ring starves client fetches (measured: 60k futile
         * push attempts + a dropped GET per crash boot at budget 20000).
         * Un-pumped keys simply wait for the next tick. */
        int budget = 1024;
        dictIterator *pit = dictGetSafeIterator(wal_dirty_table);
        dictEntry *pde;
        while (budget > 0 && (pde = dictNext(pit)) != NULL) {
            if (WAL_IS_COVERED(dictGetUnsignedIntegerVal(pde))) continue;
            sds tk = dictGetKey(pde);
            int dbid;
            memcpy(&dbid, tk, sizeof(dbid));
            sds key = sdsnewlen(tk + sizeof(dbid), sdslen(tk) - sizeof(dbid));
            serverDb *db = (dbid >= 0 && dbid < server.dbnum) ? server.db[dbid] : NULL;
            if (db == NULL || dbFind(db, key) == NULL) {
                dictSetUnsignedIntegerVal(pde,
                    WAL_COVERED_VAL(dictGetUnsignedIntegerVal(pde)));
            } else {
                int tstate = extStorageGetState(db, key);
                if (tstate == TIERING_STATE_WARM) {
                    /* Warm retention: the spill COMPLETED and the resident
                     * value is the clean flash copy -- the ack's bytes are
                     * already in the log (staging or flushed); the next
                     * checkpoint's flush barrier makes them provably
                     * durable. Equivalent to a spill submission: cover. */
                    dictSetUnsignedIntegerVal(pde,
                        WAL_COVERED_VAL(dictGetUnsignedIntegerVal(pde)));
                } else if (tstate == TIERING_STATE_ONLY_MEMORY) {
                    /* Only pump state-quiescent keys. Submitting a spill
                     * while a fetch is in flight (COPYING_TO_MEMORY) clobbers
                     * the state machine: the READ completion asserts and the
                     * parked client never wakes (seen on r7gd postboot +
                     * local crash matrix). COPYING_TO_FLASH already covers
                     * us (stamped at emit); other states resolve and get
                     * picked up next tick. Failure here = ring backpressure
                     * or embedded floor: a value below the spill floor is
                     * DRAM-retained by design and pins the WAL until deleted
                     * or grown -- covering DRAM-retained state is checkpoint
                     * work beyond this POC. */
                    extStorageSpillKeyAsync(dbid, key);
                }
            }
            /* on success spillItemAsync stamped COVERED via NoteSpillSubmit */
            sdsfree(key);
            budget--;
        }
        dictReleaseIterator(pit);
    }
    if (wal_retire_state == 0) {
        uint64_t threshold = (uint64_t)ext_storage_wal_max_mb * 1024 * 1024;
        int sealed_pending = (wal_seg_lo < walActiveSegGen());
        int urgent = (walActiveBytes() > threshold || replay_pending);
        if (urgent || sealed_pending) {
            /* Rate limit: a retirement round costs a checkpoint (fork +
             * child serialize -- at 14.5M items that's a real io-thread and
             * CoW hit). Urgent rounds (active file over threshold, replay
             * evidence pending) run at the short interval; rounds whose only
             * purpose is releasing already-sealed segments are housekeeping
             * and must not dominate the fork budget: a pinned min_seg
             * otherwise re-triggers a fork every interval for as long as
             * one un-covered entry exists (observed: fork every 2.8s
             * throughout mixed-full, cratering TPS to 6k). */
            mstime_t now_ms = mstime();
            mstime_t min_interval = urgent ? WAL_CKPT_MIN_INTERVAL_MS
                                           : WAL_CKPT_MIN_INTERVAL_MS * 15;
            if (now_ms - wal_last_ckpt_request_ms < min_interval)
                return;
            wal_last_ckpt_request_ms = now_ms;
            /* Seal the active file if it is over threshold, so this round's
             * sweep can release it; sub-threshold active bytes just wait. */
            wal_rotate_wait_gen = 0;
            if (walActiveBytes() > threshold) {
                if (walRequestRotate() != 0)
                    wal_rotate_wait_gen = walRotateGen() + 1;
            }
            /* The sweep must credit only a checkpoint whose FORK started
             * after this request: an interval checkpoint already in flight
             * right now has a barrier that predates this round's covered
             * spills -- crediting its completion retires acked writes whose
             * bytes are neither in the flushed log nor (after truncate) the
             * WAL. */
            wal_ckpt_need_seq = storageCheckpointForkSeq() + 1;
            /* Coverage boundary: only entries covered before this instant
             * are provably flushed by the checkpoint's fork-time barrier. */
            wal_ckpt_request_epoch = wal_cover_epoch;
            wal_cover_epoch++;
            storageRequestCheckpoint();
            wal_retire_state = 1;
        }
        return;
    }
    if (storageCheckpointDoneSeq() < wal_ckpt_need_seq) return;
    if (wal_rotate_wait_gen != 0 && walRotateGen() < wal_rotate_wait_gen)
        return; /* writer hasn't sealed the segment yet */
    /* Sweep: submitted-before-barrier entries are durable now. Survivors pin
     * their segment (and every later one). */
    uint64_t dbg_swept = 0, dbg_kept = 0;
    uint64_t min_seg = UINT64_MAX;
    dictIterator *it = dictGetSafeIterator(wal_dirty_table);
    dictEntry *de;
    while ((de = dictNext(it)) != NULL) {
        uint64_t v = dictGetUnsignedIntegerVal(de);
        if (WAL_IS_COVERED(v) && WAL_COVER_EPOCH_OF(v) <= wal_ckpt_request_epoch) {
            dictDelete(wal_dirty_table, dictGetKey(de));
            dbg_swept++;
        } else if (WAL_SEG_OF(v) < min_seg) {
            min_seg = WAL_SEG_OF(v);
            dbg_kept++;
        } else {
            dbg_kept++;
        }
    }
    dictReleaseIterator(it);
    serverLog(LL_NOTICE,
              "wal-debug: SWEEP req_epoch=%llu swept=%llu kept=%llu min_seg=%llu "
              "replay_pending=%d durable=%llu assigned=%llu",
              (unsigned long long)wal_ckpt_request_epoch,
              (unsigned long long)dbg_swept, (unsigned long long)dbg_kept,
              (unsigned long long)min_seg, replay_pending,
              (unsigned long long)walDurableLsn(),
              (unsigned long long)walLastAssignedLsn());
    /* Release sealed segments no surviving entry points into. */
    uint64_t release_upto = walActiveSegGen(); /* exclusive */
    if (min_seg < release_upto) release_upto = min_seg;
    while (wal_seg_lo < release_upto) {
        char seg[4200];
        snprintf(seg, sizeof(seg), "%s.seg.%llu", wal_path_buf,
                 (unsigned long long)wal_seg_lo);
        unlink(seg);
        wal_seg_lo++;
        wal_truncations++;
    }
    /* Replay evidence (seg 0) releases as soon as no entry references it. */
    if (replay_pending && min_seg > 0) unlink(wal_replay_path);
    /* Idle bonus: fully-drained table + all LSNs durable -> reclaim the
     * active file bytes too (cheap; the writer ftruncates at a quiesce). */
    if (dictSize(wal_dirty_table) == 0 &&
        walDurableLsn() == walLastAssignedLsn()) {
        if (walTruncateActive() == 0) {
            wal_truncations++;
            serverLog(LL_NOTICE,
                      "wal-debug: TRUNCATE active file (durable=%llu assigned=%llu)",
                      (unsigned long long)walDurableLsn(),
                      (unsigned long long)walLastAssignedLsn());
        }
    }
    wal_retire_state = 0;
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
        "wal_ring_full_stalls:%llu\r\n"
        "wal_active_bytes:%llu\r\n"
        "wal_dirty_keys:%lu\r\n"
        "wal_truncations:%lld\r\n"
        "wal_replayed_records:%lld\r\n"
        "dbg_push_fail_get:%ld\r\n"
        "dbg_push_fail_del:%ld\r\n"
        "dbg_push_fail_put:%ld\r\n"
        "dbg_get_submitted:%ld\r\n"
        "dbg_get_dispatched:%ld\r\n",
        wal_active,
        ext_storage_wal_fsync == WAL_FSYNC_ALWAYS ? "always" : "everysec",
        wal_units_emitted, (unsigned long long)st.records,
        (unsigned long long)st.groups, wal_tombstones,
        (unsigned long long)st.bytes, (unsigned long long)st.fsyncs,
        (unsigned long long)walLastAssignedLsn(),
        (unsigned long long)walDurableLsn(), wal_gated_releases,
        (unsigned long long)st.ring_full_stalls,
        (unsigned long long)walActiveBytes(),
        (unsigned long)(wal_dirty_table ? dictSize(wal_dirty_table) : 0),
        wal_truncations,
        wal_replayed_records,
        atomic_load(&fc_dbg_push_fail_get), atomic_load(&fc_dbg_push_fail_del),
        atomic_load(&fc_dbg_push_fail_put), atomic_load(&fc_dbg_get_submitted),
        atomic_load(&fc_dbg_get_dispatched));
    return info;
}
