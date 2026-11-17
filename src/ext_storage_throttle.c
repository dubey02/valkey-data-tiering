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

/* Rate multiplier per adjustment while unthrottling, about 2.6x a second. */
static const double THROTTLE_RAMP_MULTIPLIER = 1.1;

/* Throttle state, reported by INFO. */
typedef struct {
    double rate;                     /* Allowed commands per second, THROTTLE_UNLIMITED_RATE when not throttling. */
    double baseline;                 /* Rate the cut applies to, frozen while throttling. */
    double fraction;                 /* Share of the baseline held back, 0 to 1. */
    double memory_percent;           /* Used memory as a percent of maxmemory. */
    unsigned long activation_events; /* Times throttling started. */
    bool measuring;                  /* Whether commands are checked. */
} extStorageThrottleState;

static extStorageThrottleState state = {0};
static throttler *ext_storage_throttler = NULL;

/* Matches commands that may grow memory: scripts, commands refused with OOM,
 * and commands on values not in memory. Replication and AOF loading pass. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);

    /* Skips the key lookups below the measure percent. */
    if (!state.measuring) return false;

    if (mustObeyClient(c)) return false; /* primary or aof client */

    /* Every script is matched, with or without a shebang, since it may write,
     * or read values on flash, through keys the checks below don't see. */
    if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand || c->cmd->proc == fcallCommand) return true;

    uint64_t cmd_flags = getCommandFlags(c);

    /* If the command is known to increase memory, we decide to throttle */
    if (cmd_flags & CMD_DENYOOM) return true;

    /* Only throttle other commands on items that are not in memory. Preferably we do this last since it's more expensive. */
    if (!doesCommandHaveKeys(c->cmd)) return false; /* Skip keyless commands */
    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
    bool fetches = false;
    for (int i = 0; i < numkeys && !fetches; i++) {
        robj *item = dbFind(c->db, objectGetVal(c->argv[result.keys[i].pos]));
        fetches = item && !extStorageIsMemoryOnly(item);
    }
    getKeysFreeResult(&result);
    return fetches;
}

/* Stays registered while enabled, so the framework keeps measuring the
 * incoming rate. */
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
    state.measuring = false;
}

static void setRate(double rate) {
    if (rate == state.rate) return;
    throttle_setRate(ext_storage_throttler, rate);
    state.rate = rate;
}

/* 0 at the start percent, rising linearly to 1 at the max percent. */
static double throttleFraction(double memory_percent) {
    double start = server.ext_storage_throttle_start_percent;
    double max = server.ext_storage_throttle_max_percent;
    serverAssert(extStorageThrottleBandIsValid());

    double memory_above_throttle_start = memory_percent - start;
    double throttle_window = max - start;
    double throttle_fraction = memory_above_throttle_start / throttle_window;

    if (throttle_fraction < 0) return 0;
    if (throttle_fraction > 1) return 1;
    return throttle_fraction;
}

void extStorageThrottleAdjust(void) {
    /* Nothing spills without a storage engine, so holding commands frees nothing. */
    if (!extStorageIsActive() || !server.ext_storage_throttle_enabled || !server.maxmemory) {
        if (ext_storage_throttler) uninstall();
        return;
    }
    if (!ext_storage_throttler) install();

    /* Same measure as spilling. The OOM check also counts replica and AOF buffers. */
    size_t used = 0;
    getMaxmemoryState(&used, NULL, NULL, NULL);
    state.memory_percent = 100.0 * (double)used / (double)server.maxmemory;
    state.measuring = state.memory_percent >= server.ext_storage_throttle_measure_percent ||
                      state.rate != THROTTLE_UNLIMITED_RATE;

    double fraction = throttleFraction(state.memory_percent);
    if (fraction == 0) {
        /* The baseline freezes when throttling starts and reads low after little
         * traffic. TODO(#146): track demand while throttling. */
        throttleMetrics metrics;
        throttle_getMetrics(METRICS_NAME, &metrics);
        state.fraction = 0;
        /* Raises the rate step by step while clients are held. */
        if (state.rate != THROTTLE_UNLIMITED_RATE && metrics.num_clients_throttled > 0) {
            state.rate = throttle_adjustRate(ext_storage_throttler, THROTTLE_RAMP_MULTIPLIER);
            return;
        }
        state.baseline = MAX(metrics.incoming_tps, (double)server.ext_storage_throttle_min_baseline);
        setRate(THROTTLE_UNLIMITED_RATE);
        return;
    }
    /* Crossing the start again while ramping up is the same activation. */
    if (state.rate == THROTTLE_UNLIMITED_RATE) state.activation_events++;
    state.fraction = fraction;
    setRate(state.baseline * (1.0 - fraction));
}

sds extStorageThrottleInfoString(sds info) {
    if (!extStorageIsActive()) return info;
    throttleMetrics metrics;
    throttle_getMetrics(METRICS_NAME, &metrics);
    bool is_throttling = ext_storage_throttler && state.rate != THROTTLE_UNLIMITED_RATE;
    return sdscatprintf(info,
                        "ext_storage_throttle_rate:%.2f\r\n"
                        "ext_storage_throttle_fraction:%.4f\r\n"
                        "ext_storage_throttle_baseline_ops_per_sec:%.2f\r\n"
                        "ext_storage_throttle_incoming_ops_per_sec:%.2f\r\n"
                        "ext_storage_throttle_memory_percent:%.2f\r\n"
                        "ext_storage_throttle_measuring:%d\r\n"
                        "ext_storage_throttle_activation_events:%lu\r\n"
                        "ext_storage_throttle_total_commands:%lld\r\n"
                        "ext_storage_throttle_queued_clients:%d\r\n"
                        "ext_storage_throttle_oldest_delay_us:%ld\r\n",
                        is_throttling ? state.rate : -1.0, state.fraction, state.baseline, metrics.incoming_tps,
                        state.memory_percent, state.measuring, state.activation_events, metrics.num_commands_throttled,
                        metrics.num_clients_throttled, metrics.oldest_client_delay_us);
}

#endif /* USE_EXT_STORAGE */
