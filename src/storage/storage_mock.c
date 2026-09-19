/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* In-memory reference storage engine.
 *
 * Keeps serialized values in a per-database chained hash table. It implements
 * both the synchronous and asynchronous paths and links only against storage.h,
 * so unit tests can build it on its own. */

#include "storage.h"

#include <stdlib.h>
#include <string.h>

#define MOCK_BUCKETS 1024
/* Maximum submitted requests awaiting execution before STORAGE_WOULDBLOCK. */
#define MOCK_MAX_INFLIGHT 256
/* Maximum executed requests awaiting a drain by poll_completions. */
#define MOCK_MAX_COMPLETED 256

typedef struct mockRecord {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    struct mockRecord *next;
} mockRecord;

typedef struct mockDb {
    mockRecord *buckets[MOCK_BUCKETS];
} mockDb;

/* A queued async request. */
typedef struct mockRequest {
    storageOpType op;
    uint32_t db_id;
    void *key_obj;
    void *val_obj;
    void *request_ctx;
} mockRequest;

/* The single storage engine instance. A storage engine is a process-wide
 * singleton, so its state lives here rather than behind a handle.
 *
 * The async path uses two rings. A submitted request lands in the request
 * ring. Execution retires it into the completion ring, from which completions
 * are drained to the caller. */
static struct {
    int is_open;
    storageConfig cfg;
    mockDb *dbs;
    uint32_t num_databases;

    /* Submitted, not yet executed. */
    mockRequest requests[MOCK_MAX_INFLIGHT];
    int requests_head;
    int requests_len;

    /* Executed, not yet drained. */
    storageCompletion completions[MOCK_MAX_COMPLETED];
    int completions_head;
    int completions_len;

    storageStats stats;
} mock;

/* FNV-1a hash. Distribution only needs to be reasonable, not cryptographic. */
static size_t mockHash(const char *p, size_t len) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h % MOCK_BUCKETS;
}

static mockRecord **mockFindSlot(uint32_t db_id, const char *key, size_t klen) {
    mockRecord **slot = &mock.dbs[db_id].buckets[mockHash(key, klen)];
    while (*slot) {
        if ((*slot)->klen == klen && memcmp((*slot)->key, key, klen) == 0) return slot;
        slot = &(*slot)->next;
    }
    return slot; /* Points at NULL terminator, ready for insertion. */
}

static void mockFreeRecord(mockRecord *r) {
    free(r->key);
    free(r->val);
    free(r);
}

/* ---------------------------------------------------------------------------
 * Serialized key helper
 * ---------------------------------------------------------------------------*/

/* The serialized bytes of a key, held between acquire and release. */
typedef struct mockKeyBytes {
    char *p;
    int len;
} mockKeyBytes;

static int mockAcquireKey(void *key_obj, mockKeyBytes *out) {
    out->len = mock.cfg.serializer.serialize_key(key_obj, &out->p);
    return out->len >= 0 && out->p != NULL;
}

static void mockReleaseKey(mockKeyBytes *k) {
    if (mock.cfg.serializer.free_serialized_key) mock.cfg.serializer.free_serialized_key(k->p);
}

/* ---------------------------------------------------------------------------
 * Operation executors
 *
 * These carry out a single operation against the store. They are not part of
 * the interface. The completion queue drain calls them when it retires a
 * queued request.
 * ---------------------------------------------------------------------------*/

static storageStatus mockPut(uint32_t db_id, void *key_obj, void *val_obj, size_t *stored_bytes) {
    if (db_id >= mock.num_databases) return STORAGE_ERR_REJECTED;

    char *vbytes = NULL;
    int vlen = mock.cfg.serializer.serialize_value(val_obj, &vbytes);
    if (vlen < 0 || vbytes == NULL) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(key_obj, &k)) {
        mock.cfg.serializer.free_serialized_value(vbytes);
        return STORAGE_ERR_REJECTED;
    }

    storageStatus st = STORAGE_OK;
    mockRecord **slot = mockFindSlot(db_id, k.p, (size_t)k.len);

    /* On overwrite, release old value first so capacity check reflects the
     * final cost. */
    size_t freed = *slot ? (*slot)->vlen : 0;
    if (mock.cfg.capacity_bytes && mock.stats.total_num_bytes - freed + (uint64_t)vlen > mock.cfg.capacity_bytes) {
        st = STORAGE_ERR_FULL;
        goto done;
    }

    if (*slot) {
        free((*slot)->val);
        mock.stats.total_num_bytes -= (*slot)->vlen;
    } else {
        mockRecord *r = calloc(1, sizeof(*r));
        if (r == NULL) {
            st = STORAGE_ERR_IO;
            goto done;
        }
        r->key = malloc((size_t)k.len ? (size_t)k.len : 1);
        if (r->key == NULL) {
            free(r);
            st = STORAGE_ERR_IO;
            goto done;
        }
        memcpy(r->key, k.p, (size_t)k.len);
        r->klen = (size_t)k.len;
        *slot = r;
        mock.stats.total_num_items++;
    }

    (*slot)->val = malloc((size_t)vlen ? (size_t)vlen : 1);
    if ((*slot)->val == NULL) {
        st = STORAGE_ERR_IO;
        goto done;
    }
    memcpy((*slot)->val, vbytes, (size_t)vlen);
    (*slot)->vlen = (size_t)vlen;
    mock.stats.total_num_bytes += (size_t)vlen;
    mock.stats.total_num_items_spilled_to_storage++;
    if (stored_bytes) *stored_bytes = (size_t)vlen;

done:
    mockReleaseKey(&k);
    mock.cfg.serializer.free_serialized_value(vbytes);
    return st;
}

static storageStatus mockGet(uint32_t db_id, void *key_obj, void **val_obj_out) {
    *val_obj_out = NULL;
    if (db_id >= mock.num_databases) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(key_obj, &k)) return STORAGE_ERR_REJECTED;

    storageStatus st;
    mockRecord **slot = mockFindSlot(db_id, k.p, (size_t)k.len);
    if (*slot == NULL) {
        st = STORAGE_NOT_FOUND;
    } else {
        void *obj = mock.cfg.serializer.deserialize_value((*slot)->val, (int)(*slot)->vlen);
        if (obj == NULL) {
            st = STORAGE_ERR_IO;
        } else {
            *val_obj_out = obj;
            mock.stats.total_num_items_fetched_from_storage++;
            st = STORAGE_OK;
        }
    }
    mockReleaseKey(&k);
    return st;
}

static storageStatus mockDel(uint32_t db_id, void *key_obj) {
    if (db_id >= mock.num_databases) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(key_obj, &k)) return STORAGE_ERR_REJECTED;

    storageStatus st;
    mockRecord **slot = mockFindSlot(db_id, k.p, (size_t)k.len);
    if (*slot == NULL) {
        st = STORAGE_NOT_FOUND;
    } else {
        mockRecord *victim = *slot;
        *slot = victim->next;
        mock.stats.total_num_bytes -= victim->vlen;
        mock.stats.total_num_items--;
        mock.stats.total_num_items_deleted_from_storage++;
        mockFreeRecord(victim);
        st = STORAGE_OK;
    }
    mockReleaseKey(&k);
    return st;
}

/* ---------------------------------------------------------------------------
 * Asynchronous path
 *
 * Submit enqueues a request. Execution moves it to the completion ring, and a
 * drain hands completions to the caller. poll_completions does both, so the
 * mock stays live in a running valkey server. The test-only storageMockRunIo
 * executes without draining, so a test can observe the two steps separately.
 * ---------------------------------------------------------------------------*/

static storageStatus mockSubmit(storageOpType op, uint32_t db_id, void *key_obj, void *val_obj, void *request_ctx) {
    if (mock.requests_len == MOCK_MAX_INFLIGHT) return STORAGE_WOULDBLOCK;
    int tail = (mock.requests_head + mock.requests_len) % MOCK_MAX_INFLIGHT;
    mock.requests[tail] = (mockRequest){
        .op = op,
        .db_id = db_id,
        .key_obj = key_obj,
        .val_obj = val_obj,
        .request_ctx = request_ctx};
    mock.requests_len++;
    return STORAGE_OK;
}

static storageStatus mockPutAsync(uint32_t db_id, void *key_obj, void *val_obj, void *request_ctx) {
    return mockSubmit(STORAGE_OP_PUT, db_id, key_obj, val_obj, request_ctx);
}

static storageStatus mockGetAsync(uint32_t db_id, void *key_obj, void *request_ctx) {
    return mockSubmit(STORAGE_OP_GET, db_id, key_obj, NULL, request_ctx);
}

static storageStatus mockDelAsync(uint32_t db_id, void *key_obj, void *request_ctx) {
    return mockSubmit(STORAGE_OP_DEL, db_id, key_obj, NULL, request_ctx);
}

/* The IO thread stand-in. Executes up to max queued requests and enqueues a
 * completion for each. Stops early when the completion ring is full, leaving
 * the request in place for a later tick. Returns the number executed. */
int storageMockRunIo(int max) {
    int n = 0;
    while (n < max && mock.requests_len > 0 && mock.completions_len < MOCK_MAX_COMPLETED) {
        mockRequest *req = &mock.requests[mock.requests_head];
        int tail = (mock.completions_head + mock.completions_len) % MOCK_MAX_COMPLETED;
        storageCompletion *comp = &mock.completions[tail];
        memset(comp, 0, sizeof(*comp));
        comp->request_ctx = req->request_ctx;
        comp->op_type = req->op;
        comp->db_id = req->db_id;

        switch (req->op) {
        case STORAGE_OP_PUT:
            comp->status = mockPut(req->db_id, req->key_obj, req->val_obj, &comp->stored_bytes);
            break;
        case STORAGE_OP_GET: comp->status = mockGet(req->db_id, req->key_obj, &comp->val_obj); break;
        case STORAGE_OP_DEL: comp->status = mockDel(req->db_id, req->key_obj); break;
        }

        mock.requests_head = (mock.requests_head + 1) % MOCK_MAX_INFLIGHT;
        mock.requests_len--;
        mock.completions_len++;
        n++;
    }
    return n;
}

static int mockPollCompletions(storageCompletion *out, int max) {
    /* A single-threaded storage engine does its IO at poll time. Execute
     * pending requests first, then drain their completions. This keeps the
     * mock live in a running valkey server, where nothing calls the test-only
     * storageMockRunIo. */
    storageMockRunIo(max);
    int n = 0;
    while (n < max && mock.completions_len > 0) {
        out[n] = mock.completions[mock.completions_head];
        mock.completions_head = (mock.completions_head + 1) % MOCK_MAX_COMPLETED;
        mock.completions_len--;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

static void mockClose(void);

static storageStatus mockOpen(const storageConfig *cfg) {
    if (cfg == NULL || cfg->num_databases == 0) return STORAGE_ERR_REJECTED;
    /* Serialization callbacks are required. */
    if (cfg->serializer.serialize_key == NULL || cfg->serializer.serialize_value == NULL ||
        cfg->serializer.deserialize_value == NULL || cfg->serializer.free_serialized_value == NULL) {
        return STORAGE_ERR_REJECTED;
    }

    /* This reference storage engine recognizes no options, so any supplied
     * option is unknown and rejected. A real storage engine consumes the
     * names it knows here. */
    if (cfg->num_options > 0) return STORAGE_ERR_REJECTED;

    /* Start from a clean slate in case a prior instance was not closed. */
    mockClose();

    mock.dbs = calloc(cfg->num_databases, sizeof(mockDb));
    if (mock.dbs == NULL) return STORAGE_ERR_IO;
    mock.cfg = *cfg;
    mock.num_databases = cfg->num_databases;
    mock.is_open = 1;
    return STORAGE_OK;
}

static void mockClose(void) {
    if (mock.dbs) {
        for (uint32_t d = 0; d < mock.num_databases; d++) {
            for (int b = 0; b < MOCK_BUCKETS; b++) {
                mockRecord *r = mock.dbs[d].buckets[b];
                while (r) {
                    mockRecord *next = r->next;
                    mockFreeRecord(r);
                    r = next;
                }
            }
        }
        free(mock.dbs);
    }
    memset(&mock, 0, sizeof(mock));
}

static void mockGetStats(storageStats *out) {
    *out = mock.stats;
}

static const storageEngine mock_storage_engine = {
    .name = "mock",
    .api_version = VALKEY_STORAGE_API_VERSION,
    .open = mockOpen,
    .close = mockClose,
    .put_async = mockPutAsync,
    .get_async = mockGetAsync,
    .del_async = mockDelAsync,
    .poll_completions = mockPollCompletions,
    .get_stats = mockGetStats,
};

const storageEngine *storageMockEngine(void) {
    return &mock_storage_engine;
}
