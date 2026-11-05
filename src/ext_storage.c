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

/* Counted on the main thread as requests are submitted and completed. */
typedef struct extStorageStats {
    /* Writes submitted and not yet completed. */
    uint64_t num_items_spilling;
    /* Puts that completed successfully. */
    uint64_t total_num_items_spilled;
    /* Fetches that completed successfully. */
    uint64_t total_num_items_fetched;
    /* Deletes that completed successfully. */
    uint64_t total_num_items_deleted;
    /* Fetches the storage engine accepted whose completion is not handled yet. */
    uint64_t num_items_fetching;
    /* Fetches that completed with an error. */
    uint64_t total_num_fetch_errors;
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

/* A put in flight, passed to the engine as its request_ctx. */
typedef struct spillRequest {
    robj *entry;
    extStorageSpillSizing sizing;
} spillRequest;

/* Serialize in the DUMP wire format, so every type and encoding is handled.
 * Records the spill's size in sizing, if it isn't NULL. */
static int serializeEntry(const char *key, size_t key_len, const valkeyObject *entry, extStorageSpillSizing *sizing, char **out) {
    sds key_sds = sdsnewlen(key, key_len);
    robj key_obj;
    initStaticStringObject(key_obj, key_sds);

    rio payload;
    size_t ram_bytes = 0;
    createDumpPayloadSized(&payload, (robj *)entry, &key_obj, -1, sizing ? &ram_bytes : NULL);
    sdsfree(key_sds);

    size_t len = sdslen(payload.io.buffer.ptr);
    if (len > INT_MAX) {
        sdsfree(payload.io.buffer.ptr);
        *out = NULL;
        return -1;
    }
    if (sizing) extStorageSpillAccountSized(sizing, ram_bytes);
    *out = payload.io.buffer.ptr;
    return (int)len;
}

static int extStorageSerialize(const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx, char **out) {
    UNUSED(request_ctx);
    return serializeEntry(key, key_len, entry, NULL, out);
}

/* Like extStorageSerialize(), for puts from extStorageSpillItem(). */
static int spillSerialize(const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx, char **out) {
    spillRequest *req = request_ctx;
    serverAssert(req);
    return serializeEntry(key, key_len, entry, &req->sizing, out);
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

    if (extStorageSpillPolicyConflicts()) {
        serverLog(LL_WARNING, "ext-storage-spill-policy must match an LRU or LFU maxmemory-policy.");
        exit(1);
    }

    storageConfig cfg = {
        .path = server.ext_storage_path,
        .capacity_bytes = server.ext_storage_capacity,
        .num_databases = (uint32_t)server.dbnum,
        .serializer = {spillSerialize, extStorageDeserialize, extStorageFreeSerialized},
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
    ext_storage.stats = (extStorageStats){0};

    serverLog(LL_NOTICE, "Data tiering enabled. Storage engine \"%s\", capacity %llu bytes.", engine->name,
              (unsigned long long)server.ext_storage_capacity);
}

/* Completions
 * extStorageBeforeSleep and the shutdown drain both go through here. */

static void spillComplete(const storageCompletion *comp);
static void handleGetCompletion(const storageCompletion *completion);

/* Handles one finished request. */
static void extStorageProcessCompletion(const storageCompletion *completion) {
    switch (completion->op_type) {
    case STORAGE_OP_PUT:
        spillComplete(completion);
        break;
    case STORAGE_OP_GET:
        handleGetCompletion(completion);
        break;
    case STORAGE_OP_DEL:
        if (completion->status == STORAGE_OK) ext_storage.stats.total_num_items_deleted++;
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

/* Spills over maxmemory without a command, also on replicas, which never evict.
 * TODO(#74): on a replica, a replicated write to a spilled key would block the
 * primary's client, which blockClient() doesn't allow. */
static void spillIfOverMaxmemory(void) {
    if (!extStorageIsActive() || server.loading) return;
    if (getMaxmemoryState(NULL, NULL, NULL, NULL) == C_OK) return;
    extStorageSpillOverMaxmemory();
}

/* Runs once per event loop iteration, from beforeSleep. The cron timer wakes
 * the loop at least every tick, so a completion waits at most that long. */
void extStorageBeforeSleep(void) {
    if (!ext_storage.open) return;
    spillIfOverMaxmemory();
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
                        "ext_storage_num_items_spilling_to_storage:%llu\r\n"
                        "ext_storage_total_num_items_spilled_to_storage:%llu\r\n"
                        "ext_storage_total_num_items_fetched_from_storage:%llu\r\n"
                        "ext_storage_total_num_items_deleted_from_storage:%llu\r\n"
                        "ext_storage_num_items_fetching_from_storage:%llu\r\n"
                        "ext_storage_total_num_fetch_errors:%llu\r\n",
                        ext_storage.engine->name, VALKEY_STORAGE_API_VERSION,
                        (unsigned long long)ext_storage.capacity_bytes,
                        (unsigned long long)st->num_items_spilling,
                        (unsigned long long)st->total_num_items_spilled,
                        (unsigned long long)st->total_num_items_fetched,
                        (unsigned long long)st->total_num_items_deleted,
                        (unsigned long long)st->num_items_fetching,
                        (unsigned long long)st->total_num_fetch_errors);
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
 * Module values have no serializer yet (#81). A key with a TTL, or a hash with
 * field expirations, stays in DRAM because expiry deletes and changes values
 * without checking where they are.
 * TODO(#57): spill them once expiry handles keys on flash. */
static bool isSpillCandidate(const robj *o) {
    if (!extStorageIsMemoryOnly(o) || o->hasembval || objectGetRefcount(o) != 1) return false;
    if (objectGetEncoding(o) == OBJ_ENCODING_INT || objectGetType(o) == OBJ_MODULE) return false;
    if (objectGetExpire(o) != EXPIRY_NONE) return false;
    return !(objectGetType(o) == OBJ_HASH && hashTypeHasVolatileFields((robj *)o));
}

/* Whether data tiering spills by LRU while maxmemory-policy evicts by LFU, or
 * the other way round. A key has one lru field, holding either an LRU clock or
 * an LFU counter, so the two can't rank by different ones. */
bool extStorageSpillPolicyConflicts(void) {
    if (!server.ext_storage_enabled) return false;
    if (!(server.maxmemory_policy & (MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_LFU))) return false;
    return (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) != (server.ext_storage_spill_policy & MAXMEMORY_FLAG_LFU);
}

/* Returns the coldest spillable key, or NULL if none was sampled. Every key is
 * ranked by ext-storage-spill-policy, whatever the maxmemory-policy. */
robj *extStorageSpillSelectCandidate(int *dbid) {
    return evictionPoolPopColdest(server.ext_storage_spill_policy, isSpillCandidate, dbid);
}

/* Submits an item to the storage engine, which borrows it until the completion
 * is polled. On any status other than STORAGE_OK nothing changes. */
storageStatus extStorageSpillItem(int dbid, robj *entry) {
    serverAssert(ext_storage.open && isSpillCandidate(entry));

    spillRequest *req = zmalloc(sizeof(*req));
    req->entry = entry;
    extStorageSpillAccountSubmit(&req->sizing);
    sds key = objectGetKey(entry);
    incrRefCount(entry); /* Released in spillComplete(), keeps defrag from moving the entry. */
    storageStatus status = ext_storage.engine->put_async((uint32_t)dbid, key, sdslen(key), entry, req);
    if (status != STORAGE_OK) {
        decrRefCount(entry);
        extStorageSpillAccountFinish(&req->sizing);
        zfree(req);
        return status;
    }

    setStorageState(entry, STORAGE_STATE_COPYING_TO_FLASH);
    ext_storage.stats.num_items_spilling++;
    return STORAGE_OK;
}

/* Most keys one DEBUG EXT-STORAGE SPILL submits.
 * TODO(#80): drop the cap once spilling is throttled. */
#define DEBUG_SPILL_MAX_KEYS 256

/* DEBUG EXT-STORAGE SPILL <key> [<key> ...]
 * Submits each key's value to the storage engine. */
static void extStorageDebugSpill(client *c) {
    if (c->argc < 4) {
        addReplySubcommandSyntaxError(c);
        return;
    }
    if (c->argc - 3 > DEBUG_SPILL_MAX_KEYS) {
        addReplyErrorFormat(c, "DEBUG EXT-STORAGE SPILL takes at most %d keys", DEBUG_SPILL_MAX_KEYS);
        return;
    }
    /* Stops at the first key that fails. The keys before it stay submitted. */
    for (int j = 3; j < c->argc; j++) {
        sds key = objectGetVal(c->argv[j]);
        robj *entry = dbFind(c->db, key);
        if (entry == NULL) {
            addReplyErrorFormat(c, "No such key '%s'", key);
            return;
        }
        storageState state = getStorageState(entry);
        /* Already on flash or on its way there, so there's nothing to do. */
        if (state == STORAGE_STATE_COPYING_TO_FLASH || state == STORAGE_STATE_ONLY_FLASH) continue;
        if (!isSpillCandidate(entry)) {
            addReplyErrorFormat(c, "The key '%s' can't be spilled", key);
            return;
        }
        if (extStorageSpillItem(c->db->id, entry) != STORAGE_OK) {
            addReplyErrorFormat(c, "Spilling the key '%s' failed", key);
            return;
        }
    }
    addReply(c, shared.ok);
}

/* Handles DEBUG EXT-STORAGE. Returns 0, without replying, for any other DEBUG
 * subcommand. */
int extStorageDebugCommand(client *c) {
    if (c->argc < 3 || strcasecmp(objectGetVal(c->argv[1]), "ext-storage")) return 0;
    if (!ext_storage.open) {
        addReplyError(c, "ext-storage is not enabled");
        return 1;
    }
    const char *subcommand = objectGetVal(c->argv[2]);
    if (!strcasecmp(subcommand, "spill")) {
        extStorageDebugSpill(c);
    } else {
        addReplySubcommandSyntaxError(c);
    }
    return 1;
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

/* Handles a finished write. A write accepted by the storage engine must
 * succeed, so the value is always freed. */
static void spillComplete(const storageCompletion *comp) {
    spillRequest *req = comp->request_ctx;
    robj *entry = req->entry;
    serverAssert(getStorageState(entry) == STORAGE_STATE_COPYING_TO_FLASH);
    serverAssert(comp->status == STORAGE_OK);

    spillFreeValue(entry);
    setStorageState(entry, STORAGE_STATE_ONLY_FLASH);
    extStorageSpillAccountFinish(&req->sizing);
    ext_storage.stats.num_items_spilling--;
    ext_storage.stats.total_num_items_spilled++;

    /* Unblock clients blocked on the key while it was being spilled. */
    sds name = objectGetKey(entry);
    robj *key = createStringObject(name, sdslen(name));
    unblockClientsInUseOnKey(key);
    decrRefCount(key);
    decrRefCount(entry);
    zfree(req);
}

/* ---------------------------------------------------------------------------
 * Fetching Paths
 * ---------------------------------------------------------------------------*/

/* Submits a fetch for one ONLY_FLASH object. */
void fetchItem(serverDb *db, robj *key) {
    robj *o = dbFind(db, objectGetVal(key));
    storageState state = getStorageState(o);
    if (state == STORAGE_STATE_COPYING_TO_MEMORY) return;
    serverAssert(state == STORAGE_STATE_ONLY_FLASH);
    storageStatus st = ext_storage.engine->get_async(db->id, objectGetVal(key), sdslen(objectGetVal(key)), o);
    if (st != STORAGE_OK) {
        /* STORAGE_WOULDBLOCK: queue full. TODO(#136): retry later, or reply with an error. */
        serverPanic("Storage engine rejected the fetch");
    }
    incrRefCount(o); /* Released in handleGetCompletion. */
    setStorageState(o, STORAGE_STATE_COPYING_TO_MEMORY);
    ext_storage.stats.num_items_fetching++;
}

/* Moves a fetched value into its entry, then unblocks the clients waiting on
 * the key. The entry is the request_ctx. fetchItem's reference on it keeps it
 * from being freed or moved by defrag until now. */
static void handleGetCompletion(const storageCompletion *completion) {
    serverAssert(ext_storage.stats.num_items_fetching > 0);
    ext_storage.stats.num_items_fetching--;
    if (completion->status == STORAGE_OK) {
        ext_storage.stats.total_num_items_fetched++;
    } else {
        /* Counted before the panic so it shows in the crash report's INFO. */
        ext_storage.stats.total_num_fetch_errors++;
        serverPanic("Fetch from flash failed"); /* TODO(#137): Proper error handling. */
    }

    robj *entry = completion->request_ctx;
    serverAssert(getStorageState(entry) == STORAGE_STATE_COPYING_TO_MEMORY);
    serverAssert(objectGetVal(entry) == NULL);

    /* Move the fetched value into the entry with a pointer swap. Loading turns
     * a short string into an embedded one, whose bytes live in the shell freed
     * below, so that value is copied instead. */
    robj *fetched = completion->value;
    serverAssert(objectGetType(entry) == objectGetType(fetched));
    if (fetched->hasembval) {
        objectSetVal(entry, sdsdup(objectGetVal(fetched)));
        objectSetEncoding(entry, OBJ_ENCODING_RAW);
    } else {
        objectSetVal(entry, objectGetVal(fetched));
        objectSetEncoding(entry, objectGetEncoding(fetched));
        objectSetVal(fetched, NULL);
    }
    decrRefCount(fetched); /* decrRefCount skips a NULL value, so only the shell is freed. */
    /* A fetched key was just accessed, so it shouldn't be the next spill
     * candidate. Unlike lookupKey, no copy-on-write guard: the header was
     * already written above. */
    entry->lru = lrulfu_touch(entry->lru);
    setStorageState(entry, STORAGE_STATE_ONLY_MEMORY);

    robj key;
    initStaticStringObject(key, objectGetKey(entry));
    unblockClientsInUseOnKey(&key);
    decrRefCount(entry);
}

/* Determines if the client should be blocked on a key. */
static bool shouldBlockClientOnKey(robj *item) {
    if (!item) return false; /* If the item doesn't exist, external storage doesn't handle it */
    storageState state = getStorageState(item);
    return state != STORAGE_STATE_ONLY_MEMORY;
}

/* Determines if a fetchRequest should be submitted. */
static bool shouldFetchItem(robj *item) {
    if (!item) return false; /* If the item doesn't exist, external storage doesn't handle it */
    storageState state = getStorageState(item);
    return state == STORAGE_STATE_ONLY_FLASH;
}

/* Most keys a command can have before the key array is allocated on the heap. */
#define MAX_NUM_KEYS_FOR_STACK_BUFFER 16

/* Returns true if the client was blocked and the command must not run now.
 * TODO(#45): keys fetched while a MULTI command is queued can be spilled again
 * before EXEC, which has no keys of its own to block on. */
bool extStorageBlockClientIfRequired(client *c) {
    if (!ext_storage.open) return false;
    if (!doesCommandHaveKeys(c->cmd)) return false;

    /* If any of the keys are spilled for this command, then block the client for tiered storage. */
    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);

    robj *stackbuf[MAX_NUM_KEYS_FOR_STACK_BUFFER];
    robj **keys = numkeys <= MAX_NUM_KEYS_FOR_STACK_BUFFER ? stackbuf : zmalloc(sizeof(robj *) * numkeys);
    int n = 0;
    for (int i = 0; i < numkeys; i++) {
        robj *key = c->argv[result.keys[i].pos];
        robj *item = dbFind(c->db, objectGetVal(key));
        if (shouldBlockClientOnKey(item)) keys[n++] = key;
        /* fetchItem should go last because it can mutate the storage state. */
        if (shouldFetchItem(item)) fetchItem(c->db, key);
    }
    if (n > 0) blockClientInUseOnKeys(c, n, keys); /* All the keys that spilled. */

    if (keys != stackbuf) zfree(keys);
    getKeysFreeResult(&result);
    return n > 0;
}

/* Keys spilled per call, to bound the time spent selecting. */
#define SPILL_MAX_PER_CALL 64

/* Spills cold keys until used memory, minus what spills in flight will free,
 * is under maxmemory. Spills at most 64, or fewer if no candidate is found or
 * the storage engine refuses one. The first call spills 64, since no spill
 * size is known yet. Returns false when writes must be rejected: used memory
 * is over 1.2x maxmemory, or nothing could be spilled and no spill is in
 * flight.
 * TODO(#110): make the cap a config, and throttle writes below it. A value
 * freed in the background stops counting as in flight before its memory is
 * freed, so a few extra keys may be spilled. */
bool extStorageSpillOverMaxmemory(void) {
    if (!extStorageIsActive()) return true;

    for (int i = 0;
         i < SPILL_MAX_PER_CALL && zmalloc_used_memory() > server.maxmemory + extStorageSpillAccountInflightRamBytes();
         i++) {
        int dbid;
        robj *entry = extStorageSpillSelectCandidate(&dbid);
        if (entry == NULL || extStorageSpillItem(dbid, entry) != STORAGE_OK) break;
    }

    if (zmalloc_used_memory() > server.maxmemory + server.maxmemory / 5) return false; /* Over 1.2x maxmemory. */
    return ext_storage.stats.num_items_spilling > 0;
}

/* Waits for every spill in flight to finish. */
void extStorageSpillDrain(void) {
    if (!ext_storage.open) return;
    while (ext_storage.stats.num_items_spilling > 0) extStoragePollCompletions();
}

/* Spill accounting
 *
 * Estimates the total inflight bytes of all spill requests:
 * - From submit to serialize, a spill isn't sized yet. The number of these
 *   spills is multiplied by the EMA of serialized sizes to estimate their bytes.
 * - From serialize to completion, a spill counts as its measured size.
 * Completion subtracts what a spill added. */
static unsigned long spill_account_requests;         /* Submission increments, completion or refusal decrements. */
static _Atomic unsigned long spill_account_sized;    /* Serialization increments, completion decrements. */
static _Atomic size_t spill_account_sized_ram_bytes; /* Serialization adds, completion subtracts. */
static _Atomic size_t spill_account_ram_mean;        /* Serialization updates, main thread reads. */

/* Weight of a new size in the mean: 1/16. */
#define SPILL_RAM_MEAN_SHIFT 4

/* Moves the mean 1/16 of the way to a new size. The first size sets it. */
static size_t spillRamMeanStep(size_t mean, size_t bytes) {
    if (mean == 0) return bytes;
    if (bytes >= mean) return mean + ((bytes - mean) >> SPILL_RAM_MEAN_SHIFT);
    return mean - ((mean - bytes) >> SPILL_RAM_MEAN_SHIFT);
}

/* Test-only reset, including spills a failed test left inflight. */
void extStorageTestResetSpillAccount(void) {
    spill_account_requests = 0;
    atomic_store_explicit(&spill_account_sized, 0, memory_order_relaxed);
    atomic_store_explicit(&spill_account_sized_ram_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&spill_account_ram_mean, 0, memory_order_relaxed);
}

void extStorageSpillAccountSubmit(extStorageSpillSizing *s) {
    s->sized = false;
    s->ram_bytes = 0;
    spill_account_requests++;
}

/* Adds the spill's measured size and updates the mean. The count is added
 * before the bytes (release), so a reader that sees the bytes sees the count. */
void extStorageSpillAccountSized(extStorageSpillSizing *s, size_t ram_bytes) {
    serverAssert(!s->sized);
    s->ram_bytes = ram_bytes;
    s->sized = true;
    atomic_fetch_add_explicit(&spill_account_sized, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&spill_account_sized_ram_bytes, ram_bytes, memory_order_release);

    size_t mean = atomic_load_explicit(&spill_account_ram_mean, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&spill_account_ram_mean, &mean, spillRamMeanStep(mean, ram_bytes),
                                                  memory_order_relaxed, memory_order_relaxed));
}

void extStorageSpillAccountFinish(extStorageSpillSizing *s) {
    serverAssert(spill_account_requests > 0);
    if (s->sized) {
        atomic_fetch_sub_explicit(&spill_account_sized_ram_bytes, s->ram_bytes, memory_order_relaxed);
        atomic_fetch_sub_explicit(&spill_account_sized, 1, memory_order_relaxed);
    }
    spill_account_requests--;
}

/* Reads the serialized bytes (acquire), then the unserialized count. A spill
 * serialized in between may be missed, but never counted twice. */
static void spillAccountRead(size_t *sized_ram_bytes, unsigned long *unsized_requests) {
    *sized_ram_bytes = atomic_load_explicit(&spill_account_sized_ram_bytes, memory_order_acquire);
    unsigned long sized = atomic_load_explicit(&spill_account_sized, memory_order_relaxed);
    serverAssert(sized <= spill_account_requests);
    *unsized_requests = spill_account_requests - sized;
}

size_t extStorageSpillAccountInflightRamBytes(void) {
    size_t sized_ram_bytes;
    unsigned long unsized_requests;
    spillAccountRead(&sized_ram_bytes, &unsized_requests);
    return sized_ram_bytes + unsized_requests * atomic_load_explicit(&spill_account_ram_mean, memory_order_relaxed);
}

void extStorageSpillAccountGetStats(extStorageSpillAccountStats *out) {
    out->inflight_requests = spill_account_requests;
    spillAccountRead(&out->sized_ram_bytes, &out->unsized_requests);
    out->ram_mean = atomic_load_explicit(&spill_account_ram_mean, memory_order_relaxed);
}

#endif /* USE_EXT_STORAGE */
