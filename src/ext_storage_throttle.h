/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Token bucket client throttler for external storage (key-spilling).
 * Similar to dt-poc's the throttle path but simplified for the POC.
 *
 * When memory pressure is high, clients are queued instead of processed.
 * A timer event periodically refills tokens and releases queued clients.
 * This keeps the event loop free to process completions and spill items.
 */

#ifndef EXT_STORAGE_THROTTLE_H
#define EXT_STORAGE_THROTTLE_H

#include "server.h"

/* Initialize the throttler. Called from extStorage_init(). */
void extStorageThrottle_init(void);

/* Check if a client should be throttled. Returns 1 if client was queued
 * (command should be rejected/deferred), 0 if client can proceed. */
int extStorageThrottle_shouldThrottle(client *c);

/* Adjust the throttle rate based on current memory pressure.
 * Called periodically (e.g., every 10 commands or from timer).
 * adjustRate   = legacy COUPLED controller (1.0x-1.1x band + spill-concurrency coupling).
 * adjustRateV2 = current DECOUPLED controller (1.1x-1.2x band, no spill coupling).
 * The active one is selected by ext_storage_throttling_strategy at the call site. */
void extStorageThrottle_adjustRate(void);
void extStorageThrottle_adjustRateV2(void);

/* Record a command's execution duration for dynamic TPS calculation.
 * Called after each command completes. Tracks min latency over a rolling window. */
void extStorageThrottle_recordCommandLatency(long long duration_us);

/* Remove a client from the throttle queue (e.g., on disconnect). */
void extStorageThrottle_removeClient(client *c);

/* Get metrics for INFO output. */
long long extStorageThrottle_getThrottledCount(void);
int extStorageThrottle_tryAdmitFetch(void); /* per-command flash-read gate: 0 = defer fetch submission */
long long extStorageThrottle_getQueuedClients(void);
double extStorageThrottle_getCurrentRate(void);
double extStorageThrottle_getAllowedTps(void);

#endif /* EXT_STORAGE_THROTTLE_H */
