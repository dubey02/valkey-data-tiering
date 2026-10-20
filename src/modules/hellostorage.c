/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A worked example of a storage engine module.
 *
 * Data tiering spills values out of memory into a storage engine, addressed by
 * (db_id, key). A storage engine can be built into the valkey server, or it can
 * arrive as a loadable module. This file is the module path, written to be read.
 * It is a complete, if minimal, in-memory storage engine so a third party can
 * see every part a real storage engine must provide.
 *
 * The storage engine keeps serialized value bytes in a per-database hash table.
 * It never touches disk, so the storage logic stays out of the way of the
 * lesson, which is the shape of the interface in src/storage/storage.h.
 *
 * To load it:
 *   valkey-server --loadmodule hellostorage.so \
 *                 --ext-storage-enabled yes \
 *                 --ext-storage-engine hellostorage
 *
 * The real shippable storage engine, FlashCache, lives in its own repository and
 * follows this same shape at production scale. */

#include "storage/storage.h"
#include "valkeymodule.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Storage
 *
 * A plain chained hash table per database. None of this is part of the
 * interface. A real storage engine replaces it with its own index and media.
 * ---------------------------------------------------------------------------*/

#define HELLO_BUCKETS 1024
/* Submitted requests waiting to run before the storage engine reports it is busy. */
#define HELLO_MAX_INFLIGHT 256
/* Finished requests waiting for the valkey server to collect them. */
#define HELLO_MAX_COMPLETED 256

typedef struct helloRecord {
    char *key;
    size_t key_len;
    char *val;
    size_t val_len;
    struct helloRecord *next;
} helloRecord;

typedef struct helloDb {
    helloRecord *buckets[HELLO_BUCKETS];
} helloDb;

/* One submitted asynchronous request, held until the storage engine runs it. */
typedef struct helloRequest {
    storageOpType op;
    uint32_t db_id;
    char *key;
    size_t key_len;
    /* The entry a PUT spills. It is borrowed, not owned, so the storage engine must
     * serialize it before the request completes and never free it. */
    const valkeyObject *entry;
    void *request_ctx;
} helloRequest;

/* A storage engine is a process-wide singleton, so its state lives here rather
 * than behind a handle.
 *
 * The asynchronous path uses two rings. A submitted request lands in the
 * request ring. Running it moves it to the completion ring, which the valkey
 * server drains with poll_completions. */
static struct {
    storageConfig cfg;
    helloDb *dbs;
    uint32_t num_databases;

    helloRequest requests[HELLO_MAX_INFLIGHT];
    int requests_head;
    int requests_len;

    storageCompletion completions[HELLO_MAX_COMPLETED];
    int completions_head;
    int completions_len;

    storageStats stats;
} hello;

/* FNV-1a hash. */
static size_t helloHash(const char *p, size_t len) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h % HELLO_BUCKETS;
}

/* Returns the slot holding the record for a key, or the empty slot where it
 * would go. */
static helloRecord **helloFindSlot(uint32_t db_id, const char *key, size_t key_len) {
    helloRecord **slot = &hello.dbs[db_id].buckets[helloHash(key, key_len)];
    while (*slot) {
        if ((*slot)->key_len == key_len && memcmp((*slot)->key, key, key_len) == 0) return slot;
        slot = &(*slot)->next;
    }
    return slot;
}

static void helloFreeRecord(helloRecord *r) {
    free(r->key);
    free(r->val);
    free(r);
}

/* An out-of-memory storage engine aborts, matching the allocation contract the rest of
 * the valkey server runs under. */
static void *helloAlloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p == NULL) abort();
    return p;
}

/* ---------------------------------------------------------------------------
 * The three operations
 *
 * Each one does a single put, get, or delete against the store. The valkey
 * server never calls these directly. They run when a queued request is retired.
 * ---------------------------------------------------------------------------*/

static storageStatus helloPut(uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, size_t *stored_bytes) {
    if (db_id >= hello.num_databases) return STORAGE_ERR_REJECTED;

    /* The storage engine stores bytes, not objects. The valkey server hands it a
     * serializer so it never needs to know the object layout. */
    char *bytes = NULL;
    int len = hello.cfg.serializer.serialize(key, key_len, entry, &bytes);
    if (len < 0 || bytes == NULL) return STORAGE_ERR_REJECTED;

    storageStatus st = STORAGE_OK;
    helloRecord **slot = helloFindSlot(db_id, key, key_len);

    /* On overwrite the old bytes are reclaimed, so the capacity check is against
     * the final cost. */
    size_t freed = *slot ? (*slot)->val_len : 0;
    if (hello.cfg.capacity_bytes &&
        hello.stats.total_num_bytes - freed + (uint64_t)len > hello.cfg.capacity_bytes) {
        st = STORAGE_ERR_FULL;
    } else {
        if (*slot) {
            free((*slot)->val);
            hello.stats.total_num_bytes -= (*slot)->val_len;
        } else {
            helloRecord *r = helloAlloc(sizeof(*r));
            r->key = helloAlloc(key_len);
            memcpy(r->key, key, key_len);
            r->key_len = key_len;
            r->val = NULL;
            r->val_len = 0;
            r->next = NULL;
            *slot = r;
            hello.stats.total_num_items++;
        }

        (*slot)->val = helloAlloc((size_t)len);
        memcpy((*slot)->val, bytes, (size_t)len);
        (*slot)->val_len = (size_t)len;
        hello.stats.total_num_bytes += (size_t)len;
        hello.stats.total_num_items_spilled_to_storage++;
        if (stored_bytes) *stored_bytes = (size_t)len;
    }

    hello.cfg.serializer.free_serialized(bytes);
    return st;
}

static storageStatus helloGet(uint32_t db_id, const char *key, size_t key_len, valkeyObject **value_out) {
    *value_out = NULL;
    if (db_id >= hello.num_databases) return STORAGE_ERR_REJECTED;

    helloRecord **slot = helloFindSlot(db_id, key, key_len);
    if (*slot == NULL) return STORAGE_NOT_FOUND;

    /* The storage engine turns stored bytes back into a keyless value object through the
     * same serializer. The valkey server owns the object after this. */
    valkeyObject *obj = hello.cfg.serializer.deserialize(key, key_len, (*slot)->val, (int)(*slot)->val_len);
    if (obj == NULL) return STORAGE_ERR_IO;

    *value_out = obj;
    hello.stats.total_num_items_fetched_from_storage++;
    return STORAGE_OK;
}

static storageStatus helloDel(uint32_t db_id, const char *key, size_t key_len) {
    if (db_id >= hello.num_databases) return STORAGE_ERR_REJECTED;

    helloRecord **slot = helloFindSlot(db_id, key, key_len);
    if (*slot == NULL) return STORAGE_NOT_FOUND;

    helloRecord *victim = *slot;
    *slot = victim->next;
    hello.stats.total_num_bytes -= victim->val_len;
    hello.stats.total_num_items--;
    hello.stats.total_num_items_deleted_from_storage++;
    helloFreeRecord(victim);
    return STORAGE_OK;
}

/* ---------------------------------------------------------------------------
 * The asynchronous path
 *
 * The valkey server submits a request and later collects its completion. It
 * never blocks on the storage engine. This example runs the queued work at poll time,
 * which is the simplest thing that honors the contract. A real storage engine
 * runs it on its own IO threads instead.
 * ---------------------------------------------------------------------------*/

static storageStatus helloSubmit(storageOpType op, uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    /* A full request ring means the storage engine is busy. The valkey server retries. */
    if (hello.requests_len == HELLO_MAX_INFLIGHT) return STORAGE_WOULDBLOCK;

    int tail = (hello.requests_head + hello.requests_len) % HELLO_MAX_INFLIGHT;

    /* The key is borrowed for the call, so the storage engine copies it to keep it. */
    char *key_copy = helloAlloc(key_len);
    memcpy(key_copy, key, key_len);

    hello.requests[tail] = (helloRequest){
        .op = op,
        .db_id = db_id,
        .key = key_copy,
        .key_len = key_len,
        .entry = entry,
        .request_ctx = request_ctx};
    hello.requests_len++;
    return STORAGE_OK;
}

static storageStatus helloPutAsync(uint32_t db_id, const char *key, size_t key_len, const valkeyObject *entry, void *request_ctx) {
    return helloSubmit(STORAGE_OP_PUT, db_id, key, key_len, entry, request_ctx);
}

static storageStatus helloGetAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return helloSubmit(STORAGE_OP_GET, db_id, key, key_len, NULL, request_ctx);
}

static storageStatus helloDelAsync(uint32_t db_id, const char *key, size_t key_len, void *request_ctx) {
    return helloSubmit(STORAGE_OP_DEL, db_id, key, key_len, NULL, request_ctx);
}

/* Runs up to max queued requests, one completion each. Stops early if the
 * completion ring fills, leaving the request for a later pass. */
static int helloRunQueued(int max) {
    int n = 0;
    while (n < max && hello.requests_len > 0 && hello.completions_len < HELLO_MAX_COMPLETED) {
        helloRequest *req = &hello.requests[hello.requests_head];
        int tail = (hello.completions_head + hello.completions_len) % HELLO_MAX_COMPLETED;
        storageCompletion *comp = &hello.completions[tail];
        memset(comp, 0, sizeof(*comp));
        comp->request_ctx = req->request_ctx;
        comp->op_type = req->op;
        comp->db_id = req->db_id;

        switch (req->op) {
        case STORAGE_OP_PUT:
            comp->status = helloPut(req->db_id, req->key, req->key_len, req->entry, &comp->stored_bytes);
            break;
        case STORAGE_OP_GET:
            comp->status = helloGet(req->db_id, req->key, req->key_len, &comp->value);
            break;
        case STORAGE_OP_DEL:
            comp->status = helloDel(req->db_id, req->key, req->key_len);
            break;
        }

        free(req->key);
        hello.requests_head = (hello.requests_head + 1) % HELLO_MAX_INFLIGHT;
        hello.requests_len--;
        hello.completions_len++;
        n++;
    }
    return n;
}

/* Hands finished requests to the valkey server. This example runs the queued
 * work first, then drains it. */
static int helloPollCompletions(storageCompletion *out, int max) {
    helloRunQueued(max);
    int n = 0;
    while (n < max && hello.completions_len > 0) {
        out[n] = hello.completions[hello.completions_head];
        hello.completions_head = (hello.completions_head + 1) % HELLO_MAX_COMPLETED;
        hello.completions_len--;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Lifecycle and stats
 * ---------------------------------------------------------------------------*/

static void helloClose(void);

static storageStatus helloOpen(const storageConfig *cfg) {
    if (cfg == NULL || cfg->num_databases == 0) return STORAGE_ERR_REJECTED;

    /* The serializer moves records between objects and bytes, so all three
     * callbacks are required. */
    if (cfg->serializer.serialize == NULL || cfg->serializer.deserialize == NULL ||
        cfg->serializer.free_serialized == NULL) {
        return STORAGE_ERR_REJECTED;
    }

    /* The valkey server forwards storage-engine-specific options uninterpreted. This
     * example understands none, so any supplied option is unknown and rejected.
     * A real storage engine reads the names it knows here. */
    if (cfg->num_options > 0) return STORAGE_ERR_REJECTED;

    /* Start clean in case a prior instance was not closed. */
    helloClose();

    hello.dbs = helloAlloc(cfg->num_databases * sizeof(helloDb));
    memset(hello.dbs, 0, cfg->num_databases * sizeof(helloDb));
    hello.cfg = *cfg;
    hello.num_databases = cfg->num_databases;
    return STORAGE_OK;
}

static void helloClose(void) {
    for (int i = 0; i < hello.requests_len; i++) {
        free(hello.requests[(hello.requests_head + i) % HELLO_MAX_INFLIGHT].key);
    }
    if (hello.dbs) {
        for (uint32_t d = 0; d < hello.num_databases; d++) {
            for (int b = 0; b < HELLO_BUCKETS; b++) {
                helloRecord *r = hello.dbs[d].buckets[b];
                while (r) {
                    helloRecord *next = r->next;
                    helloFreeRecord(r);
                    r = next;
                }
            }
        }
        free(hello.dbs);
    }
    memset(&hello, 0, sizeof(hello));
}

static void helloGetStats(storageStats *out) {
    *out = hello.stats;
}

/* ---------------------------------------------------------------------------
 * The storage engine, and loading it
 *
 * The vtable gathers every callback above under a name. The module hands this
 * one struct to the valkey server in OnLoad. Config ext-storage-engine
 * hellostorage then selects it.
 * ---------------------------------------------------------------------------*/

static storageEngine hellostorage_engine = {
    .name = "hellostorage",
    .api_version = VALKEY_STORAGE_API_VERSION,
    .open = helloOpen,
    .close = helloClose,
    .put_async = helloPutAsync,
    .get_async = helloGetAsync,
    .del_async = helloDelAsync,
    .poll_completions = helloPollCompletions,
    .get_stats = helloGetStats,
};

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "hellostorage", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    /* The vtable is static, so the pointer the valkey server keeps stays valid
     * after OnLoad returns. */
    if (ValkeyModule_RegisterStorageEngine(ctx, &hellostorage_engine) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}
