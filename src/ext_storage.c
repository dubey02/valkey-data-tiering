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

/* Storage Context. */
typedef struct extStorageContext {
    const storageEngine *engine;
    /* Whether the storage engine is open. */
    int open;
    /* The byte budget the storage engine was opened with. */
    uint64_t capacity_bytes;
} extStorageContext;

static extStorageContext ext_storage = {0};

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

    serverLog(LL_NOTICE, "Data tiering enabled. Engine \"%s\", capacity %llu bytes.", engine->name,
              (unsigned long long)server.ext_storage_capacity);
}
/* Deinitializes the storage engine */
void extStorageDeinit(void) {
    if (!ext_storage.open) return;
    ext_storage.engine->close();
    ext_storage.open = 0;
    ext_storage.engine = NULL;
}

/* Returns 1 when the storage engine is active, otherwise 0. */
int extStorageIsActive(void) {
    return ext_storage.open;
}

/* Storage Info Metrics. */
sds extStorageInfoString(sds info) {
    if (!extStorageIsActive()) return sdscatprintf(info, "ext_storage_enabled:0\r\n");

    storageStats st = {0};
    ext_storage.engine->get_stats(&st);

    return sdscatprintf(info,
                        "ext_storage_enabled:1\r\n"
                        "ext_storage_engine:%s\r\n"
                        "ext_storage_api_version:%d\r\n"
                        "ext_storage_capacity_bytes:%llu\r\n"
                        "ext_storage_total_num_items:%llu\r\n"
                        "ext_storage_total_num_bytes:%llu\r\n"
                        "ext_storage_total_num_items_spilled_to_storage:%llu\r\n"
                        "ext_storage_total_num_items_fetched_from_storage:%llu\r\n"
                        "ext_storage_total_num_items_deleted_from_storage:%llu\r\n",
                        ext_storage.engine->name, VALKEY_STORAGE_API_VERSION,
                        (unsigned long long)ext_storage.capacity_bytes,
                        (unsigned long long)st.total_num_items, (unsigned long long)st.total_num_bytes,
                        (unsigned long long)st.total_num_items_spilled_to_storage,
                        (unsigned long long)st.total_num_items_fetched_from_storage,
                        (unsigned long long)st.total_num_items_deleted_from_storage);
}

#endif /* USE_EXT_STORAGE */
