/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Data tiering throttle, a plug-in for the generic throttler (throttle.h).
 *
 * Slows writes that may grow memory, scripts, and any command on a value on
 * flash once used memory passes maxmemory, so spilling can bring it back down. The
 * allowed rate falls in proportion to how far used memory is past maxmemory,
 * reaching nothing at ext-storage-throttle-max-percent. Back under maxmemory,
 * the rate rises step by step until no client is held.
 */

#ifndef EXT_STORAGE_THROTTLE_H
#define EXT_STORAGE_THROTTLE_H

#include "sds.h"

/* Sets the throttle rate from used memory. Called from serverCron every 100ms. */
void extStorageThrottleAdjust(void);

/* Appends the data tiering throttle metrics to an INFO string. */
sds extStorageThrottleInfoString(sds info);

#endif
