/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"

#ifdef USE_EXT_STORAGE

#include "ext_storage.h"
#include "ext_storage_throttle.h"
#include "throttle.h"

static const char *const METRICS_NAME = "ext_storage_throttle";

/* Used memory, as a percent of maxmemory, where the cut starts. Spilling
 * starts there too. */
static const double THROTTLE_START_PERCENT = 100.0;

/* Proportional controller state, reported by INFO. */
typedef struct {
    double rate;                     /* Allowed commands per second, THROTTLE_UNLIMITED_RATE while not cutting. */
    double baseline;                 /* Incoming rate the cut applies to, frozen while cutting. */
    double fraction;                 /* Share of the baseline held back, 0 to 1. */
    double memory_percent;           /* Used memory as a percent of maxmemory at the last adjustment. */
    unsigned long activation_events; /* Times the throttle started cutting. */
} extStorageThrottleState;

static extStorageThrottleState state = {0};
static throttler *ext_storage_throttler = NULL;

/* Matches commands that may grow memory: the ones processCommand refuses with
 * OOM, and reads, since a read of a value on flash fetches it into memory.
 * Commands from the primary pass, so a replica keeps up with the replication
 * stream. TODO(#N): reads of values already in memory grow nothing and need
 * not be throttled. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);
    if (mustObeyClient(c)) return false;
    return (getCommandFlags(c) & (CMD_DENYOOM | CMD_READONLY)) != 0;
}

/* The throttler stays registered while the throttle is enabled, at the
 * unlimited rate while it isn't cutting. The framework measures the incoming
 * rate only for a registered throttler, so this keeps the baseline current. */
static void install(void) {
    serverAssert(ext_storage_throttler == NULL);
    ext_storage_throttler = throttle_register(criteriaProc, NULL, METRICS_NAME);
    state.rate = THROTTLE_UNLIMITED_RATE;
}

static void uninstall(void) {
    throttle_deregister(ext_storage_throttler);
    ext_storage_throttler = NULL;
    state.rate = THROTTLE_UNLIMITED_RATE;
    state.fraction = 0;
}

static void setRate(double rate) {
    if (rate == state.rate) return;
    throttle_setRate(ext_storage_throttler, rate);
    state.rate = rate;
}

/* Share of the baseline to hold back. The error is how far used memory is past
 * maxmemory, and the gain makes the cut reach the whole baseline at
 * ext-storage-throttle-max-percent. */
static double cutFraction(double memory_percent) {
    double max = server.ext_storage_throttle_max_percent;
    serverAssert(max > THROTTLE_START_PERCENT);
    double fraction = (memory_percent - THROTTLE_START_PERCENT) / (max - THROTTLE_START_PERCENT);
    return fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
}

void extStorageThrottleAdjust(void) {
    /* Without a storage engine nothing spills, so holding commands frees nothing. */
    if (!extStorageIsActive() || !server.ext_storage_throttle_enabled || !server.maxmemory) {
        if (ext_storage_throttler) uninstall();
        return;
    }
    if (!ext_storage_throttler) install();

    /* The measure spilling starts from, without replica and AOF buffers. The
     * OOM check counts them, so with large buffers it is reached first. */
    size_t used = 0;
    getMaxmemoryState(&used, NULL, NULL, NULL);
    state.memory_percent = 100.0 * (double)used / (double)server.maxmemory;

    double fraction = cutFraction(state.memory_percent);
    if (fraction == 0) {
        /* While cutting, held clients send nothing, so the incoming rate falls
         * to the allowed rate. The baseline follows it only while not cutting.
         * TODO(#N): with no matching commands before the start, the baseline is
         * 0 and every matching command is held until memory falls below it. */
        throttleMetrics metrics;
        throttle_getMetrics(METRICS_NAME, &metrics);
        state.baseline = metrics.incoming_tps;
        state.fraction = 0;
        setRate(THROTTLE_UNLIMITED_RATE);
        return;
    }
    if (state.fraction == 0) state.activation_events++;
    state.fraction = fraction;
    setRate(state.baseline * (1.0 - fraction));
}

sds extStorageThrottleInfoString(sds info) {
    throttleMetrics metrics;
    throttle_getMetrics(METRICS_NAME, &metrics);
    bool cutting = ext_storage_throttler && state.fraction > 0;
    /* Note addtionaly metrics are useful during benchrmarking */
    return sdscatprintf(info,
                        "ext_storage_throttle_rate:%.2f\r\n"
                        "ext_storage_throttle_fraction:%.4f\r\n"
                        "ext_storage_throttle_baseline_ops_per_sec:%.2f\r\n"
                        "ext_storage_throttle_incoming_ops_per_sec:%.2f\r\n"
                        "ext_storage_throttle_memory_percent:%.2f\r\n"
                        "ext_storage_throttle_activation_events:%lu\r\n"
                        "ext_storage_throttle_total_commands:%lld\r\n"
                        "ext_storage_throttle_queued_clients:%d\r\n"
                        "ext_storage_throttle_oldest_delay_us:%ld\r\n",
                        cutting ? state.rate : -1.0, state.fraction, state.baseline, metrics.incoming_tps,
                        state.memory_percent, state.activation_events, metrics.num_commands_throttled,
                        metrics.num_clients_throttled, metrics.oldest_client_delay_us);
}

#endif /* USE_EXT_STORAGE */
