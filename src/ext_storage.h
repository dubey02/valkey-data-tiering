/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef EXT_STORAGE_H
#define EXT_STORAGE_H

#include "server.h"

/* Module command filter status code */
#define CMD_FILTER_ACCEPT 0
#define CMD_FILTER_REJECT 1

typedef struct serverObject dbEntry;

/* ---------------------------------------------------------------------------
 * Tiering State Machine
 *
 * Each key can be in one of 5 states. Keys in ONLY_MEMORY are NOT stored in
 * the keys_tiering_state hashtable (implicit default). All other states are
 * tracked explicitly.
 * ---------------------------------------------------------------------------*/
typedef enum {
    TIERING_STATE_ONLY_MEMORY = 0,       /* Value in RAM (default, not in HT) */
    TIERING_STATE_COPYING_TO_FLASH = 1,  /* Spill in-flight, value still in RAM */
    TIERING_STATE_ONLY_FLASH = 2,        /* Value on disk (encoding=TIERED) */
    TIERING_STATE_COPYING_TO_MEMORY = 3, /* Fetch or delete in-flight from flash */
    TIERING_STATE_PENDING_EVICT = 4,     /* Eviction requested during fetch */
    TIERING_STATE_PENDING_DELETION = 5,  /* Flash copy deleted for a client DEL;
                                          * entry retained so the re-executed
                                          * DEL removes it with full command-
                                          * layer side effects */
    TIERING_STATE_WARM = 6,              /* promotion=never warm retention: value
                                          * resident in RAM AND a valid (clean)
                                          * copy on flash. Reads hit RAM; drop is
                                          * free (tombstone, no IO); a write
                                          * flips it to ONLY_MEMORY (dirty) */
} TieringState;

/* tieringStateEntry removed — state stored in robj->tiering_state (3 bits) */

/* ---------------------------------------------------------------------------
 * Policy constants
 * ---------------------------------------------------------------------------*/
#define EXT_STORAGE_ADMISSION_DRAM  0
#define EXT_STORAGE_ADMISSION_FLASH 1

#define EXT_STORAGE_PROMOTION_ALWAYS    0
#define EXT_STORAGE_PROMOTION_NEVER     1
#define EXT_STORAGE_PROMOTION_2HIT_50K  2

/* Storage GET flags (duplicated from storage/storage.h for engine-level use) */
#define STORAGE_GET_FLAG_NONE    0  /* Destructive read */
#define STORAGE_GET_FLAG_PEEK    1  /* Non-destructive read */

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/
extern int ext_data_enabled;
extern int ext_storage_debug_pause_completions; /* DEBUG EXT-STORAGE-PAUSE-COMPLETIONS (tests only) */
extern long long num_items_on_flash; /* values currently on external storage */

/* ---------------------------------------------------------------------------
 * Client-ack WAL (fast-boot Phase 3 steps 6-7, ext_storage_wal.c).
 * Contract: a client-visible reply to a write implies durable commit.
 * ---------------------------------------------------------------------------*/
extern int ext_storage_wal_enabled;
extern int ext_storage_wal_fsync; /* 0 = always, 1 = everysec */
int extStorageWalInit(const char *flash_path);
void extStorageWalShutdown(void);
int extStorageWalActive(void);
void extStorageWalSignalDirty(serverDb *db, robj *key); /* signalModifiedKey hook */
void extStorageWalEmitUnit(void);       /* exitExecutionUnit hook (nesting==0) */
int extStorageWalDirtyPending(void);    /* cheap guard for the emit hook */
int extStorageWalReplyGated(client *c); /* networking.c reply-release gate */
void extStorageWalApplyFsyncPolicy(void);
void extStorageWalCron(void);           /* retirement: serverCron hook */
void extStorageWalNoteSpillSubmit(int dbid, const char *key, size_t klen);
void extStorageWalNoteSpillDurable(int dbid, const char *key, size_t klen);
int extStorageSpillKeyAsync(int dbid, sds key);
extern long long ext_storage_checkpoint_mb;
extern long long ext_storage_wal_max_mb;
sds genExtStorageWalInfoString(sds info);

/* Mid-execution synchronous fetch (.agent/knowledge/sync-fetch-design.md).
 * Drives the IO for `key` to completion on the calling (main) thread, deferring
 * other keys' completions. On return the key is either resident or absent —
 * caller must re-find the entry. Never times out. */
void extStorageSyncFetch(serverDb *db, sds key);

int extStorageIsInitialized(void);

/* ---------------------------------------------------------------------------
 * Snapshot support (fork-based RDB save; see ext_storage.c for the protocol)
 * ---------------------------------------------------------------------------*/
int extStorageSnapshotSupported(void);
int extStorageSnapshotActive(void);
int extStorageSnapshotPrepare(void);   /* main thread, before fork/save */
void extStorageSnapshotResume(void);   /* parent, right after fork */
void extStorageSnapshotDone(void);     /* child reaped / foreground save done */
/* Fork-child (or held-worker main-thread) materialization of a tiered value.
 * 1 = *payload/(plen) set ([type][object bytes], zfree() after use);
 * 0 = skip this key. */
int extStorageMaterializeTiered(int dbid, robj *key, robj *val, char **payload, size_t *plen);
sds genExternalStorageSnapshotInfoString(sds info);
/* SWAPDB db-id indirection (see ext_storage.c). */
int extStoragePhysicalDbId(int logical_id);
int extStorageLogicalDbId(int physical_id);
void extStorageSwapDbIds(int id1, int id2);
extern int ext_storage_admission_policy;
extern int ext_storage_promotion_policy;
extern int ext_storage_fast_boot;

/* Fast boot: backend recovery feed callback (one live item per call) and the
 * recovered-keys counter used by loadDataFromDisk's skip log line. */
void extStorageRecoveryItem(void *engine_ctx, uint32_t db_id,
                            const void *key, size_t klen,
                            uint8_t value_first_byte, size_t vlen);
long long extStorageFastBootRecoveredKeys(void);
int extStorageFastBootPerformed(void);
void extStorageRecoveryCounts(void *engine_ctx, uint32_t db_id, size_t count);
extern int ext_key_spill_enabled;
extern int ext_storage_spill_pool_active;

/* ---------------------------------------------------------------------------
 * Key spilling (ext-key-spill-enabled)
 *
 * An ONLY_FLASH key already has key + serialized value + TTL on the backend;
 * the dict retains only key + tiered placeholder. Key spilling reclaims that
 * remainder by dropping the dict entry ("demotion"). The key's state becomes
 * implicit: not in dict, backend has it (tracked per-db in keys_spilled_count).
 *
 * Access path: a dict miss with keys_spilled_count > 0 re-materializes a
 * tiered placeholder (extStorageRematerializePlaceholder) and the standard
 * ONLY_FLASH fetch machinery takes over. A backend NOT_FOUND lands the key in
 * keys_confirmed_absent, which the miss path consumes to serve a true miss.
 * ---------------------------------------------------------------------------*/
extern int ext_key_spill_enabled;
extern int ext_storage_drop_pool_active; /* eviction-pool filter: sample ONLY_FLASH placeholders */

/* Demote an ONLY_FLASH key: remove its dict entry without touching the
 * backend. Returns 0 on success, -1 if the key is not droppable. */
int extStorageDropDictEntry(serverDb *db, sds key);

/* Re-insert a tiered placeholder for a key-spilled key (dict miss path).
 * Returns the new entry in ONLY_FLASH state, or NULL on failure. */
dbEntry *extStorageRematerializePlaceholder(serverDb *db, sds key);

/* Settle a rematerialization probe on fetch completion: found -> the key was
 * key-spilled, decrement the per-db count; NOT_FOUND -> it never was.
 * Returns 1 if a probe was consumed, 0 otherwise. */
int extStorageKeyspillSettleProbe(serverDb *db, sds key, int found_on_flash);

extern char *ext_storage_backend;
extern char *ext_storage_path;
extern long long ext_storage_capacity_mb;
extern long long ext_storage_max_spill_size;
extern int items_spillover_batch_size;
/* FlashCache tuning configs */
extern long long ext_storage_index_size;
extern int ext_storage_max_allocated_percent;
extern int ext_storage_max_in_flight_reads;
extern long long ext_storage_min_gc_rate;
extern long long ext_storage_max_gc_rate;
extern long long ext_storage_max_buffered_write_size;
extern long long ext_storage_buffered_write_flush_threshold;
extern long long total_items_spilling_to_ext_storage;

/* ---------------------------------------------------------------------------
 * Strategy selectors (A/B benchmark switches; set via config, MODIFIABLE).
 *
 * Two independent knobs, each a versioned pair (v2 = current/default, v1 = legacy).
 * "Coupling" is a PROPERTY of throttle v1, not its identity:
 *   - throttling v2 = adjustRateV2: 1.1x..1.2x band on raw used_memory, no spill coupling.
 *   - throttling v1 = legacy adjustRate: 1.0x..1.1x band that ALSO drives the dynamic
 *     spill-concurrency cap (extStorageUpdateSpillConcurrency). That cap is only read by
 *     spilling v1; spilling v2 ignores it, so v1 throttle + v2 spill has no real coupling.
 *   - spilling v2 = spillFillToProjected: cap-less Smith-predictor controller.
 *   - spilling v1 = legacy batch + max_num_concurrent_items_spilled cap.
 * The two knobs are orthogonal; the meaningful designs are the matched pairs
 * (v1+v1 = original pre-Smith, v2+v2 = current).
 * ---------------------------------------------------------------------------*/
typedef enum {
    THROTTLING_STRATEGY_V2 = 0,  /* adjustRateV2 — decoupled 1.1x..1.2x (default) */
    THROTTLING_STRATEGY_V1 = 1,  /* legacy adjustRate — coupled 1.0x..1.1x + drives spill cap */
} ExtStorageThrottlingStrategy;

typedef enum {
    SPILLING_STRATEGY_V2 = 0,   /* spillFillToProjected — cap-less Smith predictor (default) */
    SPILLING_STRATEGY_V1 = 1,   /* legacy batch + concurrency cap */
} ExtStorageSpillingStrategy;

extern int ext_storage_throttling_strategy;
extern int ext_storage_spilling_strategy;
extern int ext_storage_throttle_band_start;
extern int ext_storage_throttle_band_end;

void extStorage_init(void);

int preCommandExec(client *c);

int processCompletedStorageRequestsAndSpillOldItems(void);
int processCompletedStorageRequestsAndSpillOldItemsAggressive(void);
void processCompletedStorageRequests(void);
sds genExternalStorageInfoString(sds info);

/* Transient promotion: free values after processUnblockedClients completes */
void extStorageFreeTransientValues(void);
void extStorageMarkTransientDirty(serverDb *db, robj *key);

/* Warm retention (promotion=never + ext-storage-warm-retention):
 * dirty flip on write, flash-copy delete on key delete. */
void extStorageWarmMarkDirty(serverDb *db, robj *key);
void extStorageWarmOnDelete(serverDb *db, sds key);
extern int ext_storage_warm_retention;
extern int ext_storage_warm_pool_active;
extern long long warm_keys_resident;

/* State machine API */
TieringState extStorageGetState(serverDb *db, sds key);
void extStorageSetState(serverDb *db, sds key, TieringState state, int inflight_op);
void extStorageRemoveState(serverDb *db, sds key);
int extStorageEvictFlashKey(serverDb *db, sds key);

/* Flash admission: spill a just-created key immediately if policy is FLASH */
void extStorageMaybeFlashAdmit(client *c, serverDb *db, sds key);

/* Eviction override: returns 1 if tiered storage handled eviction decision,
 * 0 if standard eviction should proceed. Sets *result to EVICT_OK or EVICT_FAIL. */
int extStoragePerformEvictions(int *result);

/* Called from throttle layer to update dynamic spill concurrency */
void extStorageOnSpillSubmit(void);
void extStorageOnSpillSerialize(size_t bytes);

/* Legacy (COUPLED throttling) dynamic spill concurrency actuator. Called by the
 * legacy throttle adjustRate when throttling_strategy == COUPLED; sets the cap the
 * ITEM_COUNT spill strategy reads. No-op effect under the PROJECTED spill strategy
 * (which ignores the cap). */
void extStorageUpdateSpillConcurrency(double throttle_rate);

/* tieringStateHashtableType removed — no longer needed */

/* Serialization callbacks for external storage module */
int extStorageSerializeKey(void *key, char **serialized_key);
int extStorageSerializeValue(void *value, char **serialized_value);
void *extStorageDeserializeKey(char *key, int length);
void *extStorageDeserializeValue(char *value, int length);
void extStorageFreeSerializedKey(void *key);
void extStorageFreeSerializedValue(void *value);
void extStorageInflightAddRam(size_t bytes);
size_t extStorageProjectedMemory(void);

#endif
