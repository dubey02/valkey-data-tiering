/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Data tiering throttle, a plug-in for the generic throttler (throttle.h).
 *
 * Past ext-storage-throttle-start-percent of maxmemory it slows writes,
 * scripts, and commands on a value on flash. The rate falls in proportion to
 * used memory past the start, reaching zero at ext-storage-throttle-max-percent.
 * Back under the start it rises step by step.
 */

#ifndef EXT_STORAGE_THROTTLE_H
#define EXT_STORAGE_THROTTLE_H

#include "sds.h"

/* Sets the throttle rate from used memory. Called from serverCron every 100ms. */
void extStorageThrottleAdjust(void);

/* Appends the data tiering throttle metrics to an INFO string. */
sds extStorageThrottleInfoString(sds info);

#endif
