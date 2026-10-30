/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Data tiering: storage-engine-side wiring.
 * Connects the valkey server to a storage engine. */

#include "server.h"

#ifdef USE_EXT_STORAGE

#include "ext_storage.h"
#include "rdb.h"
#include "storage/storage_flashcache.h"
#include "storage/storage_mock.h"

/* Holds a storage engine registered by a module. Built-in storage engines are
 * selected by name in extStorageInit and are not held here. */

static const storageEngine *registered_engine = NULL;

storageStatus storageRegisterEngine(const storageEngine *engine) {
    if (engine == NULL || engine->name == NULL || engine->name[0] == '\0') return STORAGE_ERR_REJECTED;
    if (engine->api_version != VALKEY_STORAGE_API_VERSION) return STORAGE_ERR_REJECTED;
    if (registered_engine != NULL) return STORAGE_ERR_REJECTED;
    registered_engine = engine;
    return STORAGE_OK;
}

const storageEngine *storageLookupEngine(const char *name) {
    if (name == NULL || registered_engine == NULL) return NULL;
    if (strcmp(registered_engine->name, name) == 0) return registered_engine;
    return NULL;
}

/* Test-only reset. Allows test fixtures to isolate storage engine registration. */
void extStorageTestResetRegistry(void) {
    registered_engine = NULL;
}

/* Counted on the main thread as completions are handled. */
typedef struct extStorageStats {
    uint64_t total_num_items_spilled;
    uint64_t total_num_items_fetched;
    uint64_t total_num_items_deleted;
} extStorageStats;

/* Storage Context. */
typedef struct extStorageContext {
    const storageEngine *engine;
    /* Set while the storage engine is open. */
    int open;
    /* The byte budget the storage engine was opened with. */
    uint64_t capacity_bytes;
    extStorageStats stats;
} extStorageContext;

static extStorageContext ext_storage = {0};

/* Completions handled per poll. */
#define EXT_STORAGE_POLL_BATCH 64

/* Guards rdbLoadObject during deserialization. */
static pthread_mutex_t ext_storage_loading_mutex = PTHREAD_MUTEX_INITIALIZER;

static int extStorageTryAcquireLoadingLock(void) {
    return pthread_mutex_trylock(&ext_storage_loading_mutex) == 0;
}

static void extStorageReleaseLoadingLock(void) {
    pthread_mutex_unlock(&ext_storage_loading_mutex);
}

/* Serialization and Deserialization Functions
 * The storage engine may call these from its own IO thread. */

/* Serialize in the DUMP wire format, so every type and encoding is handled. */
static int extStorageSerialize(const char *key, size_t key_len, const valkeyObject *entry, char **out) {
    sds key_sds = sdsnewlen(key, key_len);
    robj key_obj;
    initStaticStringObject(key_obj, key_sds);

    rio payload;
    createDumpPayload(&payload, (robj *)entry, &key_obj, -1);
    sdsfree(key_sds);

    size_t len = sdslen(payload.io.buffer.ptr);
    if (len > INT_MAX) {
        sdsfree(payload.io.buffer.ptr);
        *out = NULL;
        return -1;
    }
    *out = payload.io.buffer.ptr;
    return (int)len;
}

static valkeyObject *extStorageDeserialize(const char *key, size_t key_len, const char *bytes, int len) {
    if (len < 0) return NULL;
    /* Verify the DUMP footer (version + CRC) before loading, so a corrupt or
     * truncated record returns NULL instead of garbage, as restoreCommand does. */
    if (verifyDumpPayload((unsigned char *)bytes, (size_t)len, NULL) == C_ERR) return NULL;
    rio payload;
    sds buf = sdsnewlen(bytes, (size_t)len);
    rioInitWithBuffer(&payload, buf);
    int type = rdbLoadType(&payload);
    robj *o = NULL;
    if (type != -1) {
        sds key_sds = sdsnewlen(key, key_len);
        if (extStorageTryAcquireLoadingLock()) {
            o = rdbLoadObject(type, &payload, key_sds, -1, NULL, RDBFLAGS_NONE, 0);
            extStorageReleaseLoadingLock();
        }
        sdsfree(key_sds);
    }
    sdsfree(buf);
    return o;
}

static void extStorageFreeSerialized(char *bytes) {
    if (bytes) sdsfree((sds)bytes);
}

static const storageSerializer ext_storage_serializer = {
    .serialize = extStorageSerialize,
    .deserialize = extStorageDeserialize,
    .free_serialized = extStorageFreeSerialized,
};

const storageSerializer *extStorageSerializer(void) {
    return &ext_storage_serializer;
}

/* Runs first on every storage engine thread. */
static void extStorageOnThreadStart(const char *name) {
    valkey_set_thread_title(name);
    serverSetCpuAffinity(server.ext_storage_cpulist);
    makeThreadKillable();

    /* Only the main thread may receive the watchdog signal. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    int err = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
    if (err) serverLog(LL_WARNING, "Warning: can't mask SIGALRM in the %s thread: %s", name, strerror(err));
}

static const storageThreadConfig ext_storage_thread_config = {
    .stack_size = VALKEY_THREAD_STACK_SIZE,
    .on_thread_start = extStorageOnThreadStart,
};

/* Forwards a storage engine log line to the valkey server log. */
static void extStorageLog(storageLogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void extStorageLog(storageLogLevel level, const char *fmt, ...) {
    int server_level;
    switch (level) {
    case STORAGE_LOG_DEBUG: server_level = LL_DEBUG; break;
    case STORAGE_LOG_VERBOSE: server_level = LL_VERBOSE; break;
    case STORAGE_LOG_NOTICE: server_level = LL_NOTICE; break;
    default: server_level = LL_WARNING; break;
    }
    if (server_level < server.verbosity) return;

    char msg[LOG_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    serverLogRaw(server_level, msg);
}

/* Storage Engine Initialization. */
void extStorageInit(void) {
    serverAssert(server.ext_storage_enabled);

    if (server.ext_storage_engine == NULL || server.ext_storage_engine[0] == '\0') {
        serverLog(LL_WARNING, "ext-storage-enabled is set but ext-storage-engine is empty.");
        exit(1);
    }

    /* Resolve the storage engine. */
    const storageEngine *engine = storageLookupEngine(server.ext_storage_engine);
    if (engine == NULL && strcmp(server.ext_storage_engine, "mock") == 0) {
        engine = storageMockEngine();
    }
    if (engine == NULL && strcmp(server.ext_storage_engine, "flashcache") == 0) {
        engine = storageFlashCacheEngine();
    }
    if (engine == NULL) {
        serverLog(LL_WARNING,
                  "Unknown ext-storage-engine \"%s\". A module storage engine only appears if its module "
                  "was loaded from the configuration file or with --loadmodule.",
                  server.ext_storage_engine);
        exit(1);
    }

    /* Storage engine's API version verification */
    if (engine->api_version != VALKEY_STORAGE_API_VERSION) {
        serverLog(LL_WARNING,
                  "Storage engine \"%s\" reports API version %d, but this valkey server requires %d.", engine->name,
                  engine->api_version, VALKEY_STORAGE_API_VERSION);
        exit(1);
    }

    storageConfig cfg = {
        .path = server.ext_storage_path,
        .capacity_bytes = server.ext_storage_capacity,
        .num_databases = (uint32_t)server.dbnum,
        .serializer = ext_storage_serializer,
        .thread_config = ext_storage_thread_config,
        .logger = extStorageLog,
        .monotonic_clock_us = getMonotonicUs,
        /* No storage-engine-specific options are forwarded yet. The
         * configuration directive that collects them is future work. */
        .options = NULL,
        .num_options = 0,
    };

    if (engine->open(&cfg) != STORAGE_OK) {
        serverLog(LL_WARNING, "Storage engine \"%s\" failed to open (path=\"%s\", capacity=%llu bytes).",
                  engine->name, server.ext_storage_path ? server.ext_storage_path : "",
                  (unsigned long long)server.ext_storage_capacity);
        exit(1);
    }

    ext_storage.engine = engine;
    ext_storage.open = 1;
    ext_storage.capacity_bytes = cfg.capacity_bytes;

    serverLog(LL_NOTICE, "Data tiering enabled. Storage engine \"%s\", capacity %llu bytes.", engine->name,
              (unsigned long long)server.ext_storage_capacity);
}

/* Completions
 * extStorageBeforeSleep and the shutdown drain both go through here. */

static void spillComplete(const storageCompletion *comp);

/* Handles one finished request. */
static void extStorageProcessCompletion(const storageCompletion *c) {
    switch (c->op_type) {
    case STORAGE_OP_PUT:
        spillComplete(c);
        break;
    case STORAGE_OP_GET:
        if (c->status == STORAGE_OK) ext_storage.stats.total_num_items_fetched++;
        /* No command path reads a fetched value yet, so it has no owner. */
        if (c->value) decrRefCount((robj *)c->value);
        break;
    case STORAGE_OP_DEL:
        if (c->status == STORAGE_OK) ext_storage.stats.total_num_items_deleted++;
        break;
    }
}

/* Polls the storage engine once and handles what finished. Returns the count. */
static int extStoragePollCompletions(void) {
    storageCompletion done[EXT_STORAGE_POLL_BATCH];
    int n = ext_storage.engine->poll_completions(done, EXT_STORAGE_POLL_BATCH);
    for (int i = 0; i < n; i++) extStorageProcessCompletion(&done[i]);
    return n;
}

/* Runs once per event loop iteration, from beforeSleep. The cron timer wakes
 * the loop at least every tick, so a completion waits at most that long. */
void extStorageBeforeSleep(void) {
    if (!ext_storage.open) return;
    extStoragePollCompletions();
}

/* Closes the storage engine. Outstanding requests are polled and handled first.
 * Nothing new is submitted, so the drain ends when they finish. */
void extStorageDeinit(void) {
    if (!ext_storage.open) return;
    storageStatus st;
    int drained = 0;
    mstime_t drain_start = 0;
    while ((st = ext_storage.engine->close()) == STORAGE_WOULDBLOCK) {
        if (drain_start == 0) {
            drain_start = mstime();
            serverLog(LL_NOTICE, "Storage engine \"%s\" has requests outstanding, draining before close.",
                      ext_storage.engine->name);
        }
        int n = extStoragePollCompletions();
        drained += n;
        if (n == 0) usleep(1000);
    }
    if (drain_start != 0) {
        serverLog(LL_NOTICE, "Storage engine \"%s\" drained %d requests in %lld ms.", ext_storage.engine->name,
                  drained, (long long)(mstime() - drain_start));
    }
    if (st != STORAGE_OK) {
        serverLog(LL_WARNING, "Storage engine \"%s\" failed to close, status %d.", ext_storage.engine->name, (int)st);
    }
    ext_storage.open = 0;
    ext_storage.engine = NULL;
}

/* Crash handler only. Stops the storage engine threads with no cleanup. */
void extStorageKillThreads(void) {
    if (!ext_storage.open) return;
    if (ext_storage.engine->kill_threads == NULL) return;
    ext_storage.engine->kill_threads();
    serverLog(LL_WARNING, "Storage engine \"%s\" threads terminated", ext_storage.engine->name);
}

/* Returns 1 when the storage engine is active, otherwise 0. */
int extStorageIsActive(void) {
    return ext_storage.open;
}

/* INFO
 * The Ext_storage section holds what the valkey server counts. The
 * Storage_engine section holds what the storage engine reports through the
 * sink below. */

sds extStorageInfoString(sds info) {
    if (!extStorageIsActive()) return sdscatprintf(info, "ext_storage_enabled:0\r\n");

    const extStorageStats *st = &ext_storage.stats;
    return sdscatprintf(info,
                        "ext_storage_enabled:1\r\n"
                        "ext_storage_engine:%s\r\n"
                        "ext_storage_api_version:%d\r\n"
                        "ext_storage_capacity_bytes:%llu\r\n"
                        "ext_storage_total_num_items_spilled_to_storage:%llu\r\n"
                        "ext_storage_total_num_items_fetched_from_storage:%llu\r\n"
                        "ext_storage_total_num_items_deleted_from_storage:%llu\r\n",
                        ext_storage.engine->name, VALKEY_STORAGE_API_VERSION,
                        (unsigned long long)ext_storage.capacity_bytes,
                        (unsigned long long)st->total_num_items_spilled,
                        (unsigned long long)st->total_num_items_fetched,
                        (unsigned long long)st->total_num_items_deleted);
}

/* The INFO buffer under construction, handed to the storage engine as the
 * sink's opaque context. */
typedef struct extStorageInfoCtx {
    sds info;
    /* Set while a dict field is open. Fields added inside it print as
     * name=value pairs on the dict's line. */
    int in_dict;
} extStorageInfoCtx;

#define EXT_STORAGE_ENGINE_INFO_PREFIX "storage_engine_"

static void extStorageInfoAddLongLong(void *c, const char *name, long long value) {
    extStorageInfoCtx *ctx = c;
    if (ctx->in_dict) {
        ctx->info = sdscatfmt(ctx->info, "%s=%I,", name, value);
    } else {
        ctx->info = sdscatfmt(ctx->info, EXT_STORAGE_ENGINE_INFO_PREFIX "%s:%I\r\n", name, value);
    }
}

static void extStorageInfoAddULongLong(void *c, const char *name, unsigned long long value) {
    extStorageInfoCtx *ctx = c;
    if (ctx->in_dict) {
        ctx->info = sdscatfmt(ctx->info, "%s=%U,", name, value);
    } else {
        ctx->info = sdscatfmt(ctx->info, EXT_STORAGE_ENGINE_INFO_PREFIX "%s:%U\r\n", name, value);
    }
}

static void extStorageInfoAddDouble(void *c, const char *name, double value) {
    extStorageInfoCtx *ctx = c;
    if (ctx->in_dict) {
        ctx->info = sdscatprintf(ctx->info, "%s=%.17g,", name, value);
    } else {
        ctx->info = sdscatprintf(ctx->info, EXT_STORAGE_ENGINE_INFO_PREFIX "%s:%.17g\r\n", name, value);
    }
}

static void extStorageInfoAddCString(void *c, const char *name, const char *value) {
    extStorageInfoCtx *ctx = c;
    if (ctx->in_dict) {
        ctx->info = sdscatfmt(ctx->info, "%s=%s,", name, value);
    } else {
        ctx->info = sdscatfmt(ctx->info, EXT_STORAGE_ENGINE_INFO_PREFIX "%s:%s\r\n", name, value);
    }
}

static void extStorageInfoEndDict(void *c) {
    extStorageInfoCtx *ctx = c;
    if (!ctx->in_dict) return;
    /* The last pair leaves a trailing comma. */
    if (ctx->info[sdslen(ctx->info) - 1] == ',') sdsIncrLen(ctx->info, -1);
    ctx->info = sdscat(ctx->info, "\r\n");
    ctx->in_dict = 0;
}

static void extStorageInfoBeginDict(void *c, const char *name) {
    extStorageInfoCtx *ctx = c;
    /* A dict cannot nest. Opening one closes the one before it. */
    extStorageInfoEndDict(ctx);
    ctx->info = sdscatfmt(ctx->info, EXT_STORAGE_ENGINE_INFO_PREFIX "%s:", name);
    ctx->in_dict = 1;
}

int extStorageEngineHasInfo(void) {
    return ext_storage.open && ext_storage.engine->info != NULL;
}

sds extStorageEngineInfoString(sds info) {
    if (!extStorageEngineHasInfo()) return info;

    extStorageInfoCtx ctx = {.info = info, .in_dict = 0};
    const storageInfoSink sink = {
        .ctx = &ctx,
        .add_long_long = extStorageInfoAddLongLong,
        .add_ulong_long = extStorageInfoAddULongLong,
        .add_double = extStorageInfoAddDouble,
        .add_cstring = extStorageInfoAddCString,
        .begin_dict = extStorageInfoBeginDict,
        .end_dict = extStorageInfoEndDict,
    };
    ext_storage.engine->info(&sink);
    /* A dict the storage engine left open still ends its line. */
    extStorageInfoEndDict(&ctx);
    return ctx.info;
}

storageState getStorageState(const robj *o) {
    return (storageState)o->storage_state;
}

void setStorageState(robj *o, storageState state) {
    o->storage_state = state;
}

/* True when the value of 'o' may be moved to the storage engine. The value
 * must still be in memory and be the only reference to its allocation.
 * Embedded strings and INT encodings have no separate allocation to free.
 * Module values have no serializer yet (#81). Expired keys are left for
 * expiry to delete. A hash with field expirations stays in DRAM because active
 * field expiry reads and changes its value.
 * TODO(#57): spill it once expiry handles keys on flash. */
static bool isSpillCandidate(const robj *o) {
    if (!extStorageIsMemoryOnly(o) || o->hasembval || objectGetRefcount(o) != 1) return false;
    if (objectGetEncoding(o) == OBJ_ENCODING_INT || objectGetType(o) == OBJ_MODULE) return false;
    if (timestampIsExpired(objectGetExpire(o))) return false;
    return !(objectGetType(o) == OBJ_HASH && hashTypeHasVolatileFields((robj *)o));
}

/* Picks the eviction policy used to sample and rank keys for spilling.
 * Spill ignores maxmemory-policy and ranks every key by how long it has
 * been idle. That maps to allkeys-lru. The one exception is an LFU
 * maxmemory-policy. The lru field then holds an LFU counter instead of an
 * LRU clock, so allkeys-lfu is returned to read that field correctly. */
static int spillSamplingPolicy(void) {
    return (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) ? MAXMEMORY_ALLKEYS_LFU : MAXMEMORY_ALLKEYS_LRU;
}

/* Returns the coldest spillable key, or NULL if none was sampled. */
robj *extStorageSpillSelectCandidate(int *dbid) {
    return evictionPoolPopColdest(spillSamplingPolicy(), isSpillCandidate, dbid);
}

/* A put in flight, passed to the engine as its request_ctx. */
typedef struct spillRequest {
    robj *entry;
} spillRequest;

/* Number of spills submitted and not yet finished. */
static unsigned long inflight_requests;

/* Submits an item to the storage engine, which borrows it until the completion
 * is polled. On any status other than STORAGE_OK nothing changes. */
storageStatus extStorageSpillItem(int dbid, robj *entry) {
    serverAssert(ext_storage.open && isSpillCandidate(entry));

    spillRequest *req = zmalloc(sizeof(*req));
    req->entry = entry;
    sds key = objectGetKey(entry);
    storageStatus status = ext_storage.engine->put_async((uint32_t)dbid, key, sdslen(key), entry, req);
    if (status != STORAGE_OK) {
        zfree(req);
        return status;
    }

    setStorageState(entry, STORAGE_STATE_COPYING_TO_FLASH);
    inflight_requests++;
    return STORAGE_OK;
}

/* DEBUG SPILL <key> [<key> ...]: submits each key's value to the storage engine. */
void debugSpillCommand(client *c) {
    if (!extStorageIsActive()) {
        addReplyError(c, "Data tiering is not enabled");
        return;
    }
    /* Stops at the first key that fails. The keys before it stay submitted. */
    for (int j = 2; j < c->argc; j++) {
        robj *entry = dbFind(c->db, objectGetVal(c->argv[j]));
        if (entry == NULL) {
            addReplyErrorObject(c, shared.nokeyerr);
            return;
        }
        if (!isSpillCandidate(entry)) {
            addReplyError(c, "The key can't be spilled");
            return;
        }
        storageStatus status = extStorageSpillItem(c->db->id, entry);
        if (status == STORAGE_WOULDBLOCK) {
            addReplyError(c, "The storage engine is busy");
            return;
        } else if (status != STORAGE_OK) {
            addReplyError(c, "The storage engine rejected the item");
            return;
        }
    }
    addReply(c, shared.ok);
}

/* Frees the value the way dbSetValue() frees an overwritten one. The object,
 * which holds the key, stays.
 * TODO(#81): pass the key and dbid once module values spill, since a module's
 * free_effort callback may read them. */
static void spillFreeValue(robj *entry) {
    robj *val = createObject(objectGetType(entry), objectGetVal(entry));
    val->encoding = objectGetEncoding(entry);
    objectSetVal(entry, NULL);
    if (server.lazyfree_lazy_server_del)
        freeObjAsync(NULL, val, -1);
    else
        decrRefCount(val);
}

/* Handles a finished write. A key is spilled only when nothing else references
 * it, and nothing may take a reference while it is COPYING_TO_FLASH. So a
 * written value is always freed, and only a failed write keeps it in memory. */
static void spillComplete(const storageCompletion *comp) {
    spillRequest *req = comp->request_ctx;
    robj *entry = req->entry;
    serverAssert(getStorageState(entry) == STORAGE_STATE_COPYING_TO_FLASH);
    serverAssert(objectGetRefcount(entry) == 1);

    if (comp->status == STORAGE_OK) {
        spillFreeValue(entry);
        setStorageState(entry, STORAGE_STATE_ONLY_FLASH);
        ext_storage.stats.total_num_items_spilled++;
    } else {
        setStorageState(entry, STORAGE_STATE_ONLY_MEMORY);
    }
    inflight_requests--;
    zfree(req);
}

/* Waits for every spill in flight to finish. */
void extStorageSpillDrain(void) {
    while (inflight_requests > 0) extStorageBeforeSleep();
}
#endif /* USE_EXT_STORAGE */
