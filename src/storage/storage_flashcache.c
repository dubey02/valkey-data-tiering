/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FlashCache storage engine.
 *
 * Requests from the main thread go through a request queue to a dedicated IO
 * thread, which executes them and posts the results to a completion queue for
 * the main thread to poll. */

#include "fmacros.h"

#include "storage_flashcache.h"
#include "mutexqueue.h"
#include "serverassert.h"
#include "zmalloc.h"

/* TODO(flashcache): declare the no-argument functions in flashcache.h as (void)
 * and drop this suppression. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "include/flashcache.h"
#pragma GCC diagnostic pop

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Maximum number of requests in flight. */
#define FC_QUEUE_DEPTH 256

/* The idle IO thread runs FlashCache's cron once per tick. A request wakes it
 * at once. The tick only keeps garbage collection going when no requests
 * arrive. */
#define FC_IDLE_TICK_MS 10

/* FlashCache init parameters, fixed until they come from configuration. */
#define FC_INITIAL_INDEX_SIZE 1024    /* index entries per database */
#define FC_MAX_DB_SIZE_PERCENT 90     /* garbage collection starts here */
#define FC_MIN_GC_RATE 0              /* FlashCache picks the floor */
#define FC_EVICT_TIME_LIMIT_US 100000 /* eviction window, 100 ms. Inert until a control message arms it. */
#define FC_OPTIMIZED_DELETE 1         /* a delete reads only the record header */

/* FlashCache writes its log in pages of this size. */
#define FC_PAGE_SIZE 4096

typedef struct fcRequest {
    storageOpType op;
    uint32_t db_id;
    char *key;
    size_t key_len;
    /* The object to store. Only set for a put. */
    const valkeyObject *entry;
    /* The serialized put, kept across a throttled retry. */
    char *put_bytes;
    int put_len;
    /* The completion result, handed back to the main thread. */
    storageCompletion completed_request;
} fcRequest;

/* The counters INFO reports. */
typedef struct fcStatsSnapshot {
    /* Number of records stored on flash. */
    uint64_t num_items;
    /* Bytes of flash the stored records take up. */
    uint64_t allocated_db_size_bytes;
    /* Number of puts FlashCache turned away. Each one was retried later. */
    uint64_t num_requests_throttled;
    /* Number of records FlashCache deleted to make room. */
    uint64_t num_items_evicted;
    /* Number of reads served from the flash device. */
    uint64_t num_disk_reads;
    /* Number of times the write buffer was written to the flash device. */
    uint64_t num_disk_writes;
} fcStatsSnapshot;

/* The IO thread refreshes the snapshot INFO reads at this interval. */
#define FC_STATS_REFRESH_US 100000

/* Tells the IO thread to exit. fcClose queues it only once no request is
 * outstanding, so it is the only request the IO thread sees. */
static fcRequest FC_STOP_REQUEST;

static struct {
    /* Converts objects to and from bytes. */
    storageSerializer serializer;
    /* Thread setup from the valkey server. */
    storageThreadConfig thread_config;
    /* Storage logger. It writes to the valkey server log. */
    storageLogFn logger;
    /* Monotonic clock from the valkey server, in microseconds. */
    storageMonotonicClockFn monotonic_clock_us;
    /* Number of databases. */
    uint32_t num_databases;
    /* The db file, inside the configured directory. */
    char db_path[PATH_MAX];
    /* Set once FlashCache is initialized. Close tears it down. */
    int fc_initialized;

    /* Requests waiting for the IO thread. */
    mutexQueue *requests;
    /* Finished requests waiting for the main thread. */
    mutexQueue *completions;
    /* Requests submitted and not yet polled. */
    size_t outstanding;
    /* Wakes the idle IO thread when a request is queued. */
    pthread_mutex_t wake_mutex;
    pthread_cond_t wake_cond;

    /* Counters the IO thread owns. Nothing else reads them. */
    struct {
        /* Number of puts FlashCache turned away. Each one was retried later. */
        uint64_t num_requests_throttled;
        /* Number of records FlashCache deleted to make room. */
        uint64_t num_items_evicted;
    } io_stats;
    /* The copy INFO reads. The IO thread rewrites it every
     * FC_STATS_REFRESH_US under snap_mutex, and fcInfo copies it out under the
     * same lock. */
    pthread_mutex_t snap_mutex;
    fcStatsSnapshot snap;
    uint64_t snap_refreshed_us;

    /* The IO thread. */
    pthread_t io_thread;
    /* Set while the storage engine is open. */
    int running;
} fc_io;

/* ---------------------------------------------------------------------------
 * IO thread
 * ---------------------------------------------------------------------------*/

/* Runs on the IO thread when a read or a delete finishes. FlashCache frees the
 * value once this returns, so a read deserializes it here. A delete always
 * completes OK. FlashCache returns NULL for a deleted and a missing record
 * alike. */
static void fcGetItemCallback(void *request_context, char *value, size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb; /* snapshot only, not used here */
    fcRequest *r = request_context;

    if (r->op == STORAGE_OP_DEL) {
        r->completed_request.status = STORAGE_OK;
    } else if (value == NULL) {
        r->completed_request.status = STORAGE_NOT_FOUND;
    } else {
        valkeyObject *obj = fc_io.serializer.deserialize(r->key, r->key_len, value, (int)value_len);
        if (obj == NULL) {
            r->completed_request.status = STORAGE_ERR_IO;
        } else {
            r->completed_request.value = obj;
            r->completed_request.status = STORAGE_OK;
        }
    }

    mutexQueueAdd(fc_io.completions, r);
}

/* Rewrites the snapshot from FlashCache's metrics and the IO thread's own
 * counters, once per FC_STATS_REFRESH_US. Both sources belong to this thread,
 * so only the hand-over to INFO needs the lock. */
static void fcRefreshStatsSnapshot(void) {
    uint64_t now = fc_io.monotonic_clock_us();
    if (now - fc_io.snap_refreshed_us < FC_STATS_REFRESH_US) return;
    fc_io.snap_refreshed_us = now;

    fcStatsSnapshot snap = {
        .num_items = flashcacheGetCountBasedMetric(FC_NUM_ITEMS),
        .allocated_db_size_bytes = flashcacheGetCountBasedMetric(FC_ALLOCATED_DB_SIZE_BYTES),
        .num_requests_throttled = fc_io.io_stats.num_requests_throttled,
        .num_items_evicted = fc_io.io_stats.num_items_evicted,
        .num_disk_reads = flashcacheGetCountBasedMetric(FC_NUM_DISK_READ),
        .num_disk_writes = flashcacheGetCountBasedMetric(FC_NUM_DISK_WRITE),
    };
    pthread_mutex_lock(&fc_io.snap_mutex);
    fc_io.snap = snap;
    pthread_mutex_unlock(&fc_io.snap_mutex);
}

/* Hands one request to FlashCache. Returns false when FlashCache throttled it,
 * so the same request is tried again after a cron pass. A put completes here.
 * A read or a delete completes in fcGetItemCallback.
 *
 * The retry is defensive. FC_QUEUE_DEPTH is also FlashCache's read limit, so a
 * read stays under it. A put throttles only past FlashCache's 512 MiB write
 * buffer. */
static bool fcSubmitToFlashCache(fcRequest *r) {
    flashcacheReturnCode rc;

    if (r->op == STORAGE_OP_PUT) {
        if (r->put_bytes == NULL) {
            r->put_len = fc_io.serializer.serialize(r->key, r->key_len, r->entry, &r->put_bytes);
            if (r->put_len < 0 || r->put_bytes == NULL) {
                r->put_bytes = NULL;
                r->completed_request.status = STORAGE_ERR_REJECTED;
                mutexQueueAdd(fc_io.completions, r);
                return true;
            }
        }
        rc = flashcachePutItem(r->db_id, r->key, r->key_len, r->put_bytes, (size_t)r->put_len);
        if (rc == FC_ERR_THROTTLED) return false;
        /* FlashCache copied the key and value. */
        fc_io.serializer.free_serialized(r->put_bytes);
        r->put_bytes = NULL;
        r->completed_request.status = (rc == FC_OK) ? STORAGE_OK : STORAGE_ERR_IO;
        if (rc == FC_OK) r->completed_request.stored_bytes = (size_t)r->put_len;
        mutexQueueAdd(fc_io.completions, r);
        return true;
    }

    /* The callback may run inside this call and the main thread may free the
     * request right after, so the request is not touched once it is handed over. */
    flashcacheReadTypes read_type = (r->op == STORAGE_OP_DEL) ? FC_DELETE : FC_READ;
    rc = flashcacheGetItem(r->db_id, r->key, r->key_len, read_type, r, fcGetItemCallback);
    if (rc == FC_ERR_THROTTLED) return false;
    if (rc != FC_OK) {
        /* No callback on failure. */
        r->completed_request.status = STORAGE_ERR_IO;
        mutexQueueAdd(fc_io.completions, r);
    }
    return true;
}

/* Wakes the IO thread. Called after a request is queued. */
static void fcWakeIoThread(void) {
    pthread_mutex_lock(&fc_io.wake_mutex);
    pthread_cond_signal(&fc_io.wake_cond);
    pthread_mutex_unlock(&fc_io.wake_mutex);
}

/* Waits for a request or the idle tick, whichever comes first. The queue is
 * checked under the wake mutex, which the main thread takes to signal, so a
 * request queued between the check and the wait is not missed. */
static void fcWaitForWork(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += FC_IDLE_TICK_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&fc_io.wake_mutex);
    if (mutexQueueLength(fc_io.requests) == 0) pthread_cond_timedwait(&fc_io.wake_cond, &fc_io.wake_mutex, &deadline);
    pthread_mutex_unlock(&fc_io.wake_mutex);
}

/* The IO thread. It hands requests to FlashCache and pumps FlashCache's cron,
 * which is where pending reads finish. A throttled request is held and retried
 * before any newer one, so order is kept. It waits only when nothing moved. */
static void *fcIOThreadMain(void *arg) {
    (void)arg;
    if (fc_io.thread_config.on_thread_start) fc_io.thread_config.on_thread_start("fc_io");

    fcRequest *held = NULL;
    while (true) {
        bool progressed = false;

        if (held != NULL) {
            if (fcSubmitToFlashCache(held)) {
                held = NULL;
                progressed = true;
            }
        } else {
            fcRequest *r = mutexQueuePop(fc_io.requests, false);
            if (r == &FC_STOP_REQUEST) break;
            if (r != NULL) {
                progressed = true;
                if (!fcSubmitToFlashCache(r)) {
                    /* Counted once per request, not once per retry. */
                    fc_io.io_stats.num_requests_throttled++;
                    held = r;
                }
            }
        }

        /* Finishes reads, flushes writes, runs garbage collection. */
        flashcacheRunCronTasks();
        fcRefreshStatsSnapshot();

        /* A held request is retried on the next pass. Wait only when nothing
         * moved and FlashCache has nothing in flight. */
        if (!progressed && !flashcacheShouldRunCronTasksImmediately()) fcWaitForWork();
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Main thread
 * ---------------------------------------------------------------------------*/

static void fcFreeRequest(fcRequest *r) {
    if (r->put_bytes != NULL) fc_io.serializer.free_serialized(r->put_bytes);
    zfree(r->key);
    zfree(r);
}

/* Queues a request for the IO thread. */
static storageStatus fcSubmit(storageOpType op, uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    if (!fc_io.running) return STORAGE_ERR_REJECTED;
    if (db_id >= fc_io.num_databases) return STORAGE_ERR_REJECTED;
    if (fc_io.outstanding >= FC_QUEUE_DEPTH) return STORAGE_WOULDBLOCK;

    fcRequest *r = zmalloc(sizeof(*r));
    r->op = op;
    r->db_id = db_id;
    /* Always a live pointer, even for an empty key. FlashCache asserts on NULL. */
    r->key = zmalloc(key_len);
    memcpy(r->key, key, key_len);
    r->key_len = key_len;
    r->entry = entry;
    r->put_bytes = NULL;
    r->put_len = 0;
    r->completed_request = (storageCompletion){.request_ctx = request_ctx, .op_type = op, .db_id = db_id};

    fc_io.outstanding++;
    mutexQueueAdd(fc_io.requests, r);
    fcWakeIoThread();
    return STORAGE_OK;
}

static storageStatus fcPutAsync(uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    return fcSubmit(STORAGE_OP_PUT, db_id, key, key_len, entry, request_ctx);
}

static storageStatus fcGetAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return fcSubmit(STORAGE_OP_GET, db_id, key, key_len, NULL, request_ctx);
}

static storageStatus fcDelAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return fcSubmit(STORAGE_OP_DEL, db_id, key, key_len, NULL, request_ctx);
}

/* Reports the last snapshot. The IO thread refreshes it, so a value can be up
 * to FC_STATS_REFRESH_US old. */
static void fcInfo(const storageInfoSink *s) {
    if (!fc_io.running) return;
    pthread_mutex_lock(&fc_io.snap_mutex);
    fcStatsSnapshot snap = fc_io.snap;
    pthread_mutex_unlock(&fc_io.snap_mutex);

    s->add_ulong_long(s->ctx, "total_num_items", snap.num_items);
    s->add_ulong_long(s->ctx, "allocated_db_size_bytes", snap.allocated_db_size_bytes);
    s->add_ulong_long(s->ctx, "total_num_requests_throttled", snap.num_requests_throttled);
    s->add_ulong_long(s->ctx, "total_num_items_evicted", snap.num_items_evicted);
    s->add_ulong_long(s->ctx, "total_num_disk_reads", snap.num_disk_reads);
    s->add_ulong_long(s->ctx, "total_num_disk_writes", snap.num_disk_writes);
}

/* Collects finished requests. */
static int fcPollCompletions(storageCompletion *out, int max) {
    if (!fc_io.running || max <= 0) return 0;
    fifo *done = mutexQueuePopAll(fc_io.completions, false);
    if (done == NULL) return 0;

    int total = 0;
    fcRequest *r;
    while (total < max && fifoPop(done, (void **)&r)) {
        out[total++] = r->completed_request;
        fcFreeRequest(r);
    }

    /* Leftovers go back to the head, so completion order is kept. */
    while (fifoPop(done, (void **)&r)) mutexQueuePushPriority(fc_io.completions, r);
    fifoRelease(done);

    fc_io.outstanding -= (size_t)total;
    return total;
}

/* ---------------------------------------------------------------------------
 * FlashCache callback bridges
 * ---------------------------------------------------------------------------*/

/* Formats FlashCache's printf-style log line and passes it on. FlashCache log
 * levels 0 to 3 match the storage log levels. */
static void fcLoggerBridge(int level, const char *fmt, ...) {
    if (fc_io.logger == NULL) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (level < STORAGE_LOG_DEBUG) level = STORAGE_LOG_DEBUG;
    if (level > STORAGE_LOG_WARNING) level = STORAGE_LOG_WARNING;
    fc_io.logger((storageLogLevel)level, "%s", buf);
}

/* FlashCache and the valkey server read the same clock. */
static uint64_t fcMonotonicClockUs(void) {
    return fc_io.monotonic_clock_us();
}

/* FlashCache reports a key it dropped when flash filled up. The drop is counted
 * and logged once. The in-memory key is not removed yet, so a later fetch of
 * it reports not found. */
static void fcEvictionCb(void *context, uint32_t db_id, char *key, size_t key_len) {
    (void)context;
    (void)key;
    if (fc_io.io_stats.num_items_evicted++ == 0 && fc_io.logger != NULL) {
        fc_io.logger(STORAGE_LOG_WARNING,
                     "FlashCache evicted a record (db %u, key length %zu). Evicted keys stay in memory "
                     "until eviction is wired to the valkey server. Fetches of them will report not found.",
                     (unsigned)db_id, key_len);
    }
}

/* Called during long FlashCache operations. Nothing to do yet, but FlashCache
 * requires a non-NULL callback. */
static void fcControlMsgCb(void *context) {
    (void)context;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

static storageStatus fcClose(void);

/* Creates the directory and db file if missing and sizes the file. FlashCache
 * asserts the file is present and large enough before it maps it. */
static storageStatus fcEnsureDbFileIsPresent(const char *dir, uint64_t capacity_bytes) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: cannot create directory %s, %s", dir, strerror(errno));
        return STORAGE_ERR_IO;
    }
    int n = snprintf(fc_io.db_path, sizeof(fc_io.db_path), "%s/%s", dir, STORAGE_FLASHCACHE_DB_FILE);
    if (n < 0 || (size_t)n >= sizeof(fc_io.db_path)) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: path %s is too long", dir);
        return STORAGE_ERR_REJECTED;
    }

    int fd = open(fc_io.db_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: cannot open %s, %s", fc_io.db_path, strerror(errno));
        return STORAGE_ERR_IO;
    }
    /* posix_fallocate returns the error instead of setting errno. */
    int err = posix_fallocate(fd, 0, (off_t)capacity_bytes);
    if (err != 0) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: cannot size %s to %llu bytes, %s", fc_io.db_path,
                     (unsigned long long)capacity_bytes, strerror(err));
        close(fd);
        return STORAGE_ERR_IO;
    }
    close(fd);
    return STORAGE_OK;
}

/* Sizes the db file, initializes FlashCache, then starts the IO thread. */
static storageStatus fcOpen(const storageConfig *cfg) {
    if (cfg == NULL || cfg->num_databases == 0) return STORAGE_ERR_REJECTED;
    if (cfg->serializer.serialize == NULL || cfg->serializer.deserialize == NULL ||
        cfg->serializer.free_serialized == NULL || cfg->logger == NULL || cfg->monotonic_clock_us == NULL) {
        return STORAGE_ERR_REJECTED;
    }
    if (cfg->path == NULL || cfg->path[0] == '\0') {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: ext-storage-path is required");
        return STORAGE_ERR_REJECTED;
    }
    if (cfg->capacity_bytes == 0) {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: ext-storage-capacity must be more than zero");
        return STORAGE_ERR_REJECTED;
    }
    /* FlashCache asserts on a log size that is not page aligned. */
    if (cfg->capacity_bytes % FC_PAGE_SIZE != 0) {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: ext-storage-capacity must be a multiple of %d bytes", FC_PAGE_SIZE);
        return STORAGE_ERR_REJECTED;
    }
    /* No options are recognized yet. */
    if (cfg->num_options > 0) {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: unknown option \"%s\"", cfg->options[0].name);
        return STORAGE_ERR_REJECTED;
    }

    /* Reopen is refused while a prior instance has unpolled requests. */
    if (fcClose() != STORAGE_OK) return STORAGE_WOULDBLOCK;

    fc_io.serializer = cfg->serializer;
    fc_io.thread_config = cfg->thread_config;
    fc_io.logger = cfg->logger;
    fc_io.monotonic_clock_us = cfg->monotonic_clock_us;
    fc_io.num_databases = cfg->num_databases;

    if (fcEnsureDbFileIsPresent(cfg->path, cfg->capacity_bytes) != STORAGE_OK) {
        memset(&fc_io, 0, sizeof(fc_io));
        return STORAGE_ERR_IO;
    }

    /* FlashCache requires non-NULL callbacks and contexts. */
    static flashcacheEvictionDetails eviction;
    eviction.context = &fc_io;
    eviction.callback = fcEvictionCb;
    static flashcacheStorageIoThreadControlMsgCallbackDetails control_msg;
    control_msg.context = &fc_io;
    control_msg.callback = fcControlMsgCb;

    flashcacheReturnCode rc = flashcacheInit(fc_io.db_path, (size_t)cfg->capacity_bytes, FC_INITIAL_INDEX_SIZE,
                                             cfg->num_databases, FC_MAX_DB_SIZE_PERCENT, FC_QUEUE_DEPTH,
                                             FC_MIN_GC_RATE, FC_EVICT_TIME_LIMIT_US, FC_OPTIMIZED_DELETE,
                                             fcMonotonicClockUs, &eviction, fcLoggerBridge, &control_msg);
    if (rc != FC_OK) {
        cfg->logger(STORAGE_LOG_WARNING, "flashcache: init failed, code %d", (int)rc);
        memset(&fc_io, 0, sizeof(fc_io));
        return STORAGE_ERR_IO;
    }
    fc_io.fc_initialized = 1;

    fc_io.requests = mutexQueueCreate();
    fc_io.completions = mutexQueueCreate();

    /* The idle wait uses the monotonic clock, so a wall-clock jump cannot
     * stretch or cut the tick. */
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&fc_io.wake_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    pthread_mutex_init(&fc_io.wake_mutex, NULL);
    pthread_mutex_init(&fc_io.snap_mutex, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stack_size = 0;
    pthread_attr_getstacksize(&attr, &stack_size);
    if (stack_size < fc_io.thread_config.stack_size) pthread_attr_setstacksize(&attr, fc_io.thread_config.stack_size);
    int err = pthread_create(&fc_io.io_thread, &attr, fcIOThreadMain, NULL);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fc_io.logger(STORAGE_LOG_WARNING, "flashcache: can't start the IO thread: %s", strerror(err));
        flashcacheTearDown();
        pthread_cond_destroy(&fc_io.wake_cond);
        pthread_mutex_destroy(&fc_io.wake_mutex);
        pthread_mutex_destroy(&fc_io.snap_mutex);
        mutexQueueRelease(fc_io.requests);
        mutexQueueRelease(fc_io.completions);
        memset(&fc_io, 0, sizeof(fc_io));
        return STORAGE_ERR_IO;
    }
    fc_io.running = 1;
    return STORAGE_OK;
}

/* Stops the IO thread and tears FlashCache down. */
static storageStatus fcClose(void) {
    if (!fc_io.running) return STORAGE_OK;
    /* An unpolled completion may hold a fetched value nobody else owns. */
    if (fc_io.outstanding > 0) return STORAGE_WOULDBLOCK;

    mutexQueuePushPriority(fc_io.requests, &FC_STOP_REQUEST);
    fcWakeIoThread();
    int join_err = pthread_join(fc_io.io_thread, NULL);
    assert(join_err == 0);
    fc_io.running = 0;

    /* The IO thread was the only caller, so teardown is safe now. */
    if (fc_io.fc_initialized) {
        flashcacheTearDown();
        fc_io.fc_initialized = 0;
    }

    pthread_cond_destroy(&fc_io.wake_cond);
    pthread_mutex_destroy(&fc_io.wake_mutex);
    pthread_mutex_destroy(&fc_io.snap_mutex);
    mutexQueueRelease(fc_io.requests);
    mutexQueueRelease(fc_io.completions);
    memset(&fc_io, 0, sizeof(fc_io));
    return STORAGE_OK;
}

/* Cancels the IO thread without any cleanup. */
static void fcKillThreads(void) {
    if (!fc_io.running) return;
    if (pthread_equal(fc_io.io_thread, pthread_self())) return;
    if (pthread_cancel(fc_io.io_thread) != 0) return;
    pthread_join(fc_io.io_thread, NULL);
}

static const storageEngine flashcache_storage_engine = {
    .name = "flashcache",
    .api_version = VALKEY_STORAGE_API_VERSION,
    .open = fcOpen,
    .close = fcClose,
    .kill_threads = fcKillThreads,
    .put_async = fcPutAsync,
    .get_async = fcGetAsync,
    .del_async = fcDelAsync,
    .poll_completions = fcPollCompletions,
    .info = fcInfo,
};

const storageEngine *storageFlashCacheEngine(void) {
    return &flashcache_storage_engine;
}
