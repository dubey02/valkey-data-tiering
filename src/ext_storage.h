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
} TieringState;

/* tieringStateEntry removed — state stored in robj->tiering_state (3 bits) */

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/
extern int ext_data_enabled;
extern int ext_storage_spill_pool_active;
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

/* State machine API */
TieringState extStorageGetState(serverDb *db, sds key);
void extStorageSetState(serverDb *db, sds key, TieringState state, int inflight_op);
void extStorageRemoveState(serverDb *db, sds key);
int extStorageEvictFlashKey(serverDb *db, sds key);

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
