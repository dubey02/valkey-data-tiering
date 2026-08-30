/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Token bucket client throttler for external storage (flash-tiering).
 *
 * Matches dt-poc's amzThrottle pattern:
 * - Dynamic max TPS derived from measured command execution latency
 * - Proportional throttle: allowed_tps = max_tps * (1 - throttle_rate)
 * - No fixed TPS floor — throttle can go as low as needed
 * - Hook point: readQueryFromClient (BEFORE command is read/parsed)
 * - When throttled: remove read handler, queue client
 * - Timer event: refill tokens, release clients, re-install read handler
 */

#include "ext_storage_throttle.h"
#include "ext_storage.h"
#include "server.h"
#include "connection.h"
#include <math.h>

/* --------------------------------------------------------------------------
 * Configuration (matching dt-poc defaults)
 * -------------------------------------------------------------------------- */

#define THROTTLE_ABSOLUTE_MAX_TPS  200000.0  /* Upper bound when not throttling */
#define THROTTLE_MIN_CMD_MAX_TPS   150000.0  /* Floor for TPS — raised from 50K to allow higher throughput under tiering */
#define THROTTLE_ADJUST_INTERVAL   10        /* Adjust rate every 10 commands (dt-poc: 10) */
#define THROTTLE_TIMER_MS          1         /* Timer fires every 1ms */
#define THROTTLE_MAX_RELEASE_PER_TICK 500    /* Max clients to release per timer tick */
#define CMD_STAT_WINDOW_MS         1000      /* Rolling window for min command latency (1s) */

/* --------------------------------------------------------------------------
 * Token bucket
 * -------------------------------------------------------------------------- */

typedef struct {
    double tokens;
    double max_tokens;
    double tokens_per_ms;
    monotime last_refill;
} TokenBucket;

static void tb_init(TokenBucket *tb, double tps) {
    tb->max_tokens = tps / 1000.0 * 50;   /* Burst: 50ms worth */
    tb->tokens = tb->max_tokens;
    tb->tokens_per_ms = tps / 1000.0;
    elapsedStart(&tb->last_refill);
}

static void tb_setRate(TokenBucket *tb, double tps) {
    if (tps < 1.0) tps = 1.0; /* Never go to zero — would deadlock */
    tb->tokens_per_ms = tps / 1000.0;
    tb->max_tokens = tps / 1000.0 * 50;
    /* Don't let existing tokens exceed new max */
    if (tb->tokens > tb->max_tokens) tb->tokens = tb->max_tokens;
}

static void tb_refill(TokenBucket *tb) {
    monotime now;
    elapsedStart(&now);
    double elapsed_ms = (double)elapsedUs(tb->last_refill) / 1000.0;
    if (elapsed_ms > 0.0) {
        tb->tokens += elapsed_ms * tb->tokens_per_ms;
        if (tb->tokens > tb->max_tokens) tb->tokens = tb->max_tokens;
        tb->last_refill = now;
    }
}

static int tb_tryConsume(TokenBucket *tb) {
    if (tb->tokens >= 1.0) {
        tb->tokens -= 1.0;
        return 1;
    }
    return 0;
}


/* --------------------------------------------------------------------------
 * Throttle state
 * -------------------------------------------------------------------------- */

typedef struct {
    TokenBucket bucket;
    list *client_queue;
    long long timer_event_id;
    double allowed_tps;
    double measured_max_tps;   /* Derived from min command latency */
    double throttle_rate;
    int is_throttling;
    int commands_since_adjust;

    /* Command latency tracking (rolling window, matching dt-poc) */
    long long min_cmd_exec_us_current_window;   /* Min latency in current window */
    long long min_cmd_exec_us_previous_window;  /* Min latency in previous window (used for TPS calc) */
    monotime window_start_time;

    /* Metrics */
    long long total_throttled;
    long long total_released;
} ThrottleState;

static ThrottleState ts;

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

void readQueryFromClient(connection *conn);

/* --------------------------------------------------------------------------
 * Timer: refill tokens and release queued clients
 * -------------------------------------------------------------------------- */

static long long throttleTimerProc(struct aeEventLoop *el, long long id, void *data) {
    UNUSED(el); UNUSED(id); UNUSED(data);

    tb_refill(&ts.bucket);

    int released = 0;
    while (listLength(ts.client_queue) > 0 && released < THROTTLE_MAX_RELEASE_PER_TICK) {
        if (!tb_tryConsume(&ts.bucket)) break;

        listNode *ln = listFirst(ts.client_queue);
        client *c = listNodeValue(ln);
        listDelNode(ts.client_queue, ln);
        ts.total_released++;
        released++;

        /* Re-install the read handler AND queue the client so whatever is
         * already sitting in its query buffer actually gets processed.
         *
         * Re-arming on its own is not enough, and that was a permanent client
         * wedge. The throttle hook runs at the TOP of readQueryFromClient,
         * before readToQueryBuf/processInputBuffer, so a complete command that
         * an earlier read left in querybuf is stranded when we disarm. A normal
         * request/response client has sent that command and is now waiting for
         * its reply -- it sends nothing further, so the socket never becomes
         * readable again, readQueryFromClient is never called, and the buffered
         * command is never executed. The client waits forever while the server
         * stays healthy for every other connection. Observed as
         * qbuf=1069 (one whole SET), blocked_clients=0, idle climbing.
         *
         * queueClientForReprocessing puts it on server.unblocked_clients, and
         * processUnblockedClients re-arms the handler if needed and then calls
         * processPendingCommandAndInputBuffer, which drains querybuf. That path
         * does not re-enter the throttle, which is correct: this client's token
         * was already consumed above, so its buffered command must not be
         * charged again. */
        if (c->conn) {
            connSetReadHandler(c->conn, readQueryFromClient);
        }
        queueClientForReprocessing(c);
    }

    if (listLength(ts.client_queue) > 0) {
        return THROTTLE_TIMER_MS; /* Keep timer alive */
    }

    ts.timer_event_id = AE_ERR;
    return AE_NOMORE;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void extStorageThrottle_init(void) {
    tb_init(&ts.bucket, THROTTLE_ABSOLUTE_MAX_TPS);
    ts.client_queue = listCreate();
    ts.timer_event_id = AE_ERR;
    ts.allowed_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    ts.measured_max_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    ts.throttle_rate = 0.0;
    ts.is_throttling = 0;
    ts.commands_since_adjust = 0;
    ts.min_cmd_exec_us_current_window = 0;
    ts.min_cmd_exec_us_previous_window = 0;
    elapsedStart(&ts.window_start_time);
    ts.total_throttled = 0;
    ts.total_released = 0;
}

/**
 * Record a command's execution duration for dynamic TPS calculation.
 * Called after each command completes (from server.c call() or similar).
 * This tracks the minimum command latency over a rolling window.
 */
void extStorageThrottle_recordCommandLatency(long long duration_us) {
    if (duration_us <= 0) return;
    if (ts.min_cmd_exec_us_current_window == 0 ||
        duration_us < ts.min_cmd_exec_us_current_window) {
        ts.min_cmd_exec_us_current_window = duration_us;
    }
}

/**
 * Adjust the throttle rate based on current memory pressure.
 *
 * Dynamic max TPS (matching dt-poc):
 *   max_tps = 1_000_000 / min_cmd_exec_duration_previous_window
 *   allowed_tps = max_tps * (1 - throttle_rate)
 *
 * This means: if commands are fast (10µs), max_tps = 100K.
 * If commands are slow (1ms, e.g. blocked on disk), max_tps = 1K.
 * Under full throttle (rate=1.0), allowed_tps approaches 0.
 */
/* LEGACY (COUPLED) rate controller — selected when throttling_strategy == COUPLED.
 * Throttle band is [band_start, band_end] × maxmemory on raw used_memory (defaults: 1.0x, 1.2x), and it drives the
 * dynamic spill-concurrency cap via extStorageUpdateSpillConcurrency(rate). This is
 * the pre-Smith behavior, restored for A/B measurement. */
void extStorageThrottle_adjustRate(void) {
    if (server.maxmemory == 0) return;

    /* Slide the rolling window if needed */
    long long elapsed_ms = (long long)(elapsedUs(ts.window_start_time) / 1000);
    if (elapsed_ms >= CMD_STAT_WINDOW_MS) {
        ts.min_cmd_exec_us_previous_window = ts.min_cmd_exec_us_current_window;
        ts.min_cmd_exec_us_current_window = 0;
        elapsedStart(&ts.window_start_time);
    }

    /* Compute measured max TPS from minimum command latency */
    if (ts.min_cmd_exec_us_previous_window > 0) {
        ts.measured_max_tps = 1000000.0 / (double)ts.min_cmd_exec_us_previous_window;
    } else {
        ts.measured_max_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    }
    /* Apply floor — never estimate max_tps below MIN_CMD_MAX_TPS */
    if (ts.measured_max_tps < THROTTLE_MIN_CMD_MAX_TPS) {
        ts.measured_max_tps = THROTTLE_MIN_CMD_MAX_TPS;
    }
    /* Cap at absolute max */
    if (ts.measured_max_tps > THROTTLE_ABSOLUTE_MAX_TPS) {
        ts.measured_max_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    }

    /* Compute throttle rate from memory pressure (configurable band) */
    size_t used_mem = zmalloc_used_memory();
    size_t throttle_start = (size_t)(server.maxmemory * ext_storage_throttle_band_start / 100);
    size_t throttle_max   = (size_t)(server.maxmemory * ext_storage_throttle_band_end / 100);

    double rate;
    if (used_mem <= throttle_start) {
        rate = 0.0;
    } else if (used_mem >= throttle_max) {
        rate = 1.0;
    } else {
        rate = (double)(used_mem - throttle_start) / (double)(throttle_max - throttle_start);
    }

    ts.throttle_rate = rate;

    if (rate <= 0.0) {
        if (ts.is_throttling) {
            ts.is_throttling = 0;
            ts.allowed_tps = ts.measured_max_tps;
            tb_setRate(&ts.bucket, ts.measured_max_tps);
        }
    } else {
        ts.is_throttling = 1;
        /* allowed_tps = max_tps * (1 - throttle_rate); at rate=1.0 (1.1x maxmem):
         * throttle to ~1% of max TPS. Dynamic proportional reduction across band. */
        double new_tps = ts.measured_max_tps * (1.0 - rate * 0.99);
        if (new_tps < 1000.0) new_tps = 1000.0; /* Absolute minimum: 1K TPS to avoid deadlock */
        ts.allowed_tps = new_tps;
        tb_setRate(&ts.bucket, new_tps);
    }

    /* Legacy coupling: drive dynamic spill concurrency from the throttle rate. */
    extStorageUpdateSpillConcurrency(rate);
}

void extStorageThrottle_adjustRateV2(void) {
    if (server.maxmemory == 0) return;

    /* Slide the rolling window if needed */
    long long elapsed_ms = (long long)(elapsedUs(ts.window_start_time) / 1000);
    if (elapsed_ms >= CMD_STAT_WINDOW_MS) {
        ts.min_cmd_exec_us_previous_window = ts.min_cmd_exec_us_current_window;
        ts.min_cmd_exec_us_current_window = 0;
        elapsedStart(&ts.window_start_time);
    }

    /* Compute measured max TPS from minimum command latency */
    if (ts.min_cmd_exec_us_previous_window > 0) {
        ts.measured_max_tps = 1000000.0 / (double)ts.min_cmd_exec_us_previous_window;
    } else {
        ts.measured_max_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    }
    /* Apply floor — never estimate max_tps below MIN_CMD_MAX_TPS */
    if (ts.measured_max_tps < THROTTLE_MIN_CMD_MAX_TPS) {
        ts.measured_max_tps = THROTTLE_MIN_CMD_MAX_TPS;
    }
    /* Cap at absolute max */
    if (ts.measured_max_tps > THROTTLE_ABSOLUTE_MAX_TPS) {
        ts.measured_max_tps = THROTTLE_ABSOLUTE_MAX_TPS;
    }

    /* Throttle band reads RAW used_memory (NOT projected). With the spill controller
     * pinning projected at maxmemory, (used_memory - maxmemory) is the in-flight
     * backlog, so raw used_memory climbing toward band_end IS the disk-saturation
     * signal. Band is configurable via ext-storage-throttle-band-start/end
     * (defaults: 100/120 = 1.0x/1.2x maxmemory). The two
     * controllers are decoupled — coupling is implicit via M. */
    size_t used_mem = zmalloc_used_memory();
    size_t throttle_start = (size_t)(server.maxmemory * ext_storage_throttle_band_start / 100);
    size_t throttle_max   = (size_t)(server.maxmemory * ext_storage_throttle_band_end / 100);

    double rate;
    if (used_mem <= throttle_start) {
        rate = 0.0;
    } else if (used_mem >= throttle_max) {
        rate = 1.0;
    } else {
        rate = (double)(used_mem - throttle_start) / (double)(throttle_max - throttle_start);
    }

    ts.throttle_rate = rate;

    if (rate <= 0.0) {
        if (ts.is_throttling) {
            ts.is_throttling = 0;
            ts.allowed_tps = ts.measured_max_tps;
            tb_setRate(&ts.bucket, ts.measured_max_tps);
        }
    } else {
        ts.is_throttling = 1;
        /* allowed_tps = max_tps * (1 - throttle_rate)
         * rate ramps linearly across [band_start, band_end] × maxmemory (defaults: 1.0x, 1.2x); at rate=1.0
         * throttle to ~1% of max TPS (1.2x is also the OOM hard-cap reject point). */
        double new_tps = ts.measured_max_tps * (1.0 - rate * 0.99);
        if (new_tps < 1000.0) new_tps = 1000.0; /* Absolute minimum: 1K TPS to avoid deadlock */
        ts.allowed_tps = new_tps;
        tb_setRate(&ts.bucket, new_tps);
    }

    /* No spill-concurrency coupling: the spill controller is cap-less and
     * projected-gated, fully decoupled from the throttle (coupling is implicit
     * through memory). extStorageUpdateSpillConcurrency was removed. */
}

int extStorageThrottle_shouldThrottle(client *c) {
    /* Periodic rate adjustment */
    ts.commands_since_adjust++;
    if (ts.commands_since_adjust >= THROTTLE_ADJUST_INTERVAL) {
        ts.commands_since_adjust = 0;
        if (ext_storage_throttling_strategy == THROTTLING_STRATEGY_V1)
            extStorageThrottle_adjustRate();
        else
            extStorageThrottle_adjustRateV2();
    }

    if (!ts.is_throttling) return 0;

    /* Deliberately does NOT inspect c->flag.pending_command.
     *
     * That flag belongs to the blocking framework: it means "this client has a
     * PARSED command awaiting execution" (see blockClientInUseOnKeys, which
     * asserts it is 1, and processPendingCommandAndInputBuffer, which calls
     * processCommandAndResetClient on the strength of it). The throttle runs
     * before parsing, so it never has a parsed command to describe, and reading
     * plus clearing the flag here silently stole it from the blocking path.
     *
     * The "skip the throttle once after release" behaviour it was implementing
     * is now achieved properly: a released client's buffered command is drained
     * via processInputBuffer, which never enters this function, so it is not
     * charged a second token. Genuinely new socket data is throttled normally,
     * which is what should happen. */

    /* If there are already queued clients, new clients must queue too (FIFO fairness) */
    if (listLength(ts.client_queue) > 0) {
        goto throttle;
    }

    /* Try to consume a token */
    tb_refill(&ts.bucket);
    if (tb_tryConsume(&ts.bucket)) {
        return 0; /* Allowed */
    }

throttle:
    ts.total_throttled++;

    /* Remove read handler — stop reading from this client's socket */
    if (c->conn) {
        connSetReadHandler(c->conn, NULL);
    }

    /* Queue the client */
    listAddNodeTail(ts.client_queue, c);

    /* Ensure timer is running */
    if (ts.timer_event_id == AE_ERR) {
        ts.timer_event_id = aeCreateTimeEvent(
            server.el, THROTTLE_TIMER_MS, throttleTimerProc, NULL, NULL);
    }

    return 1;
}

void extStorageThrottle_removeClient(client *c) {
    listNode *ln;
    listIter li;
    listRewind(ts.client_queue, &li);
    while ((ln = listNext(&li))) {
        if (listNodeValue(ln) == c) {
            listDelNode(ts.client_queue, ln);
            break;
        }
    }
}

long long extStorageThrottle_getThrottledCount(void) { return ts.total_throttled; }
long long extStorageThrottle_getQueuedClients(void) { return (long long)listLength(ts.client_queue); }
double extStorageThrottle_getCurrentRate(void) { return ts.throttle_rate; }
double extStorageThrottle_getAllowedTps(void) { return ts.allowed_tps; }
