/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "ext_snapshot.h"
#include "ext_storage.h"
#include "storage/storage.h"

#include <stdatomic.h>
#include <unistd.h>

extern int ext_data_enabled;

int extSnapshotStreamSupported(void) {
    return ext_data_enabled && storageSnapshotStreamSupported();
}

/* ---------------------------------------------------------------------------
 * Self test sink
 *
 * The sink callbacks run on the storage engine's IO thread, so every field the
 * main thread reads afterwards is atomic and the completion flag is the release
 * point that publishes the rest.
 * ---------------------------------------------------------------------------*/
typedef struct selfTestState {
    _Atomic long long records;
    _Atomic long long key_bytes;
    _Atomic long long value_bytes;
    _Atomic uint_least64_t digest;
    _Atomic int done;      /* 0 = running, 1 = complete() fired */
    _Atomic int ok;
} selfTestState;

static selfTestState g_selftest;

/* FNV-1a over dbid, key bytes and value length. Combined into the running
 * digest with XOR so the result does not depend on delivery order: expedited
 * records are emitted ahead of the cursor, so order is not stable. */
static uint64_t selfTestRecordHash(uint32_t db_id, const char *key, size_t klen, size_t vlen) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)key;
    h ^= (uint64_t)db_id;    h *= 1099511628211ULL;
    for (size_t i = 0; i < klen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    h ^= (uint64_t)vlen;     h *= 1099511628211ULL;
    return h;
}

static int selfTestWritable(void *privdata) {
    (void)privdata;
    return 1; /* no backpressure in the self test */
}

static void selfTestOnRecord(void *privdata, uint32_t db_id,
                             const char *key, size_t klen,
                             const char *value, size_t vlen) {
    selfTestState *st = privdata;
    (void)value;
    atomic_fetch_add_explicit(&st->records, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->key_bytes, (long long)klen, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->value_bytes, (long long)vlen, memory_order_relaxed);
    atomic_fetch_xor_explicit(&st->digest,
                              (uint_least64_t)selfTestRecordHash(db_id, key, klen, vlen),
                              memory_order_relaxed);
}

static void selfTestComplete(void *privdata, int ok) {
    selfTestState *st = privdata;
    atomic_store_explicit(&st->ok, ok, memory_order_relaxed);
    atomic_store_explicit(&st->done, 1, memory_order_release);
}

int extSnapshotStreamSelfTest(int timeout_ms, extSnapshotSelfTestResult *out) {
    if (!out) return C_ERR;
    memset(out, 0, sizeof(*out));
    if (!extSnapshotStreamSupported()) return C_ERR;

    selfTestState *st = &g_selftest;
    atomic_store_explicit(&st->records, 0, memory_order_relaxed);
    atomic_store_explicit(&st->key_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&st->value_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&st->digest, 0, memory_order_relaxed);
    atomic_store_explicit(&st->ok, 0, memory_order_relaxed);
    atomic_store_explicit(&st->done, 0, memory_order_release);

    storageSnapshotSink sink = {
        .privdata = st,
        .set_size_hint = NULL,
        .writable = selfTestWritable,
        .on_record = selfTestOnRecord,
        .complete = selfTestComplete,
    };

    mstime_t start = mstime();
    if (storageSnapshotStreamStart(&sink) != STORAGE_OK) return C_ERR;
    out->started = 1;

    while (!atomic_load_explicit(&st->done, memory_order_acquire)) {
        if (mstime() - start > timeout_ms) {
            /* Leave nothing running behind us: the engine still owns the sink
             * pointer until complete() fires. */
            storageSnapshotStreamAbort();
            mstime_t abort_start = mstime();
            while (!atomic_load_explicit(&st->done, memory_order_acquire) &&
                   mstime() - abort_start < 5000)
                usleep(200);
            break;
        }
        usleep(200);
    }

    out->completed = atomic_load_explicit(&st->done, memory_order_acquire);
    out->ok = atomic_load_explicit(&st->ok, memory_order_relaxed);
    out->records = atomic_load_explicit(&st->records, memory_order_relaxed);
    out->key_bytes = atomic_load_explicit(&st->key_bytes, memory_order_relaxed);
    out->value_bytes = atomic_load_explicit(&st->value_bytes, memory_order_relaxed);
    out->digest = (uint64_t)atomic_load_explicit(&st->digest, memory_order_relaxed);
    out->elapsed_ms = (long long)(mstime() - start);
    return C_OK;
}
