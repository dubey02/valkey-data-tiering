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

/* Used memory, as a percent of maxmemory, where throttling starts. Spilling
 * starts there too. */
static const double THROTTLE_START_PERCENT = 100.0;

/* Rate multiplier per adjustment while coming out of throttling. At one
 * adjustment every 100 ms, the allowed rate grows about 2.6 times a second. */
static const double THROTTLE_RAMP_MULTIPLIER = 1.1;

/* Proportional controller state, reported by INFO. */
typedef struct {
    double rate;                     /* Allowed commands per second, THROTTLE_UNLIMITED_RATE while not throttling. */
    double baseline;                 /* Incoming rate the throttle reduces, frozen while throttling. */
    double fraction;                 /* Share of the baseline held back, 0 to 1. */
    double memory_percent;           /* Used memory as a percent of maxmemory at the last adjustment. */
    unsigned long activation_events; /* Times throttling started. */
} extStorageThrottleState;

static extStorageThrottleState state = {0};
static throttler *ext_storage_throttler = NULL;

/* Matches commands that may grow memory: scripts that may write, the commands
 * processCommand refuses with OOM, and any command on a key not in memory,
 * since the command fetches the value back first. Commands from the primary pass, so a replica keeps up with
 * the replication stream. */
static bool criteriaProc(client *c, void *priv_data) {
    UNUSED(priv_data);

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

/* The throttler stays registered while the throttle is enabled, at the
 * unlimited rate while it isn't throttling. The framework measures the incoming
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

/* Calculates the fraction of the baseline TPS to throttle, from 0 at maxmemory
 * rising linearly to 1 at ext-storage-throttle-max-percent:
 * (memory_percent - 100) / (ext-storage-throttle-max-percent - 100). */
static double throttleFraction(double memory_percent) {
    serverAssert(server.ext_storage_throttle_max_percent > THROTTLE_START_PERCENT);

    double memory_above_throttle_start = memory_percent - THROTTLE_START_PERCENT;
    double throttle_window = server.ext_storage_throttle_max_percent - THROTTLE_START_PERCENT;
    double throttle_fraction = memory_above_throttle_start / throttle_window;

    if (throttle_fraction < 0) return 0; /* Clamp to zero below THROTTLE_START_PERCENT */
    if (throttle_fraction > 1) return 1;
    return throttle_fraction;
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

    double fraction = throttleFraction(state.memory_percent);
    if (fraction == 0) {
        /* The biggest limitation of this design is that the baseline TPS is decided at
         * the time of first throttle. An example of a bad edge case is there could be 0
         * TPS, but then something like rehashing causes a memory spike that triggers the
         * throttler. The baseline we are using will be 0, so all writes would be blocked
         * until used_memory is below the threshold for throttling.
         *
         * Another example is once we start throttling, we can never go above our starting
         * TPS unless we stop throttling to get a new baseline.
         * TODO(#146): pick the baseline without freezing it at the first throttle. */
        throttleMetrics metrics;
        throttle_getMetrics(METRICS_NAME, &metrics);
        state.fraction = 0;
        /* Gradually increase the throttling rate instead of jumping to the unlimited rate */
        if (state.rate != THROTTLE_UNLIMITED_RATE && metrics.num_clients_throttled > 0) {
            state.rate = throttle_adjustRate(ext_storage_throttler, THROTTLE_RAMP_MULTIPLIER);
            return;
        }
        state.baseline = metrics.incoming_tps;
        setRate(THROTTLE_UNLIMITED_RATE);
        return;
    }
    /* Count how many times the throttler activates. Coming back over the start
     * while the rate still ramps up continues the same activation. */
    if (state.rate == THROTTLE_UNLIMITED_RATE) state.activation_events++;
    state.fraction = fraction;
    setRate(state.baseline * (1.0 - fraction));
}

sds extStorageThrottleInfoString(sds info) {
    throttleMetrics metrics;
    throttle_getMetrics(METRICS_NAME, &metrics);
    bool is_throttling = ext_storage_throttler && state.rate != THROTTLE_UNLIMITED_RATE;
    /* Note additional metrics are useful during benchmarking */
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
                        is_throttling ? state.rate : -1.0, state.fraction, state.baseline, metrics.incoming_tps,
                        state.memory_percent, state.activation_events, metrics.num_commands_throttled,
                        metrics.num_clients_throttled, metrics.oldest_client_delay_us);
}

#endif /* USE_EXT_STORAGE */
