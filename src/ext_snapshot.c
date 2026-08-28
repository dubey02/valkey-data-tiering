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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>

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

/* ===========================================================================
 * Phase 2: record transport
 * ===========================================================================*/

#define EXT_SNAP_POISON_LEN   0xFFFFFFFFu
#define EXT_SNAP_OVERFLOW_CAP (64u * 1024 * 1024)   /* Phase 4 makes this a config */

extern void extStorageBridge_drainOnly(void);
extern void processCompletedStorageRequests(void);
extern long long total_items_spilling_to_ext_storage;
extern long long total_items_fetching_from_ext_storage;

typedef struct snapTransport {
    int fds[2];              /* [0] read (consumer), [1] write (producer) */
    int armed;

    /* Producer state. Touched only on the engine IO thread once armed, except
     * the atomics, which the main thread may read. */
    char *ovf;               /* overflow: encoded bytes the pipe would not take */
    size_t ovf_len, ovf_cap;
    _Atomic long long sent;  /* records handed to the pipe */
    uint_least64_t digest;    /* producer side running digest */
    int producer_failed;

    /* Consumer state. Fork child, or the main thread in the self test. */
    char *inbuf;
    size_t in_len, in_cap;
    long long received;
    uint_least64_t rx_digest;
    int saw_terminator;
} snapTransport;

static snapTransport g_tx;

/* --- producer --------------------------------------------------------------*/

/* Push bytes toward the pipe. Anything the pipe will not take right now lands
 * in overflow, which writable() then reports on. Returns 0 on hard failure. */
static int txPush(snapTransport *t, const char *buf, size_t len) {
    if (t->producer_failed) return 0;

    /* Overflow must drain first or frames would be reordered. */
    while (t->ovf_len > 0) {
        ssize_t n = write(t->fds[1], t->ovf, t->ovf_len);
        if (n > 0) {
            memmove(t->ovf, t->ovf + n, t->ovf_len - (size_t)n);
            t->ovf_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        t->producer_failed = 1;
        return 0;
    }

    size_t off = 0;
    if (t->ovf_len == 0) {
        while (off < len) {
            ssize_t n = write(t->fds[1], buf + off, len - off);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            t->producer_failed = 1;
            return 0;
        }
    }
    if (off == len) return 1;

    /* Buffer the remainder. Growing past the cap means the consumer is not
     * keeping up at all, so fail rather than absorb without bound. */
    size_t need = len - off;
    if (t->ovf_len + need > EXT_SNAP_OVERFLOW_CAP) {
        serverLog(LL_WARNING,
            "Snapshot transport overflow exceeded %u bytes, aborting stream",
            EXT_SNAP_OVERFLOW_CAP);
        t->producer_failed = 1;
        return 0;
    }
    if (t->ovf_len + need > t->ovf_cap) {
        size_t cap = t->ovf_cap ? t->ovf_cap : 65536;
        while (cap < t->ovf_len + need) cap *= 2;
        char *nb = zrealloc(t->ovf, cap);
        if (!nb) { t->producer_failed = 1; return 0; }
        t->ovf = nb;
        t->ovf_cap = cap;
    }
    memcpy(t->ovf + t->ovf_len, buf + off, need);
    t->ovf_len += need;
    return 1;
}

static int txWritable(void *privdata) {
    snapTransport *t = privdata;
    if (t->producer_failed) return 0;
    /* Flush what we can, then report readiness. Reporting not-ready while
     * overflow is pending is what throttles the engine. */
    if (t->ovf_len > 0) txPush(t, NULL, 0);
    return t->producer_failed ? 0 : (t->ovf_len == 0);
}

static void txOnRecord(void *privdata, uint32_t db_id,
                       const char *key, size_t klen,
                       const char *value, size_t vlen) {
    snapTransport *t = privdata;
    if (t->producer_failed) return;
    if (klen > UINT32_MAX || vlen > UINT32_MAX) {
        t->producer_failed = 1;
        return;
    }
    uint32_t hdr[4];
    hdr[0] = (uint32_t)(12 + klen + vlen);   /* payload after the length word */
    hdr[1] = db_id;
    hdr[2] = (uint32_t)klen;
    hdr[3] = (uint32_t)vlen;
    if (!txPush(t, (const char *)hdr, sizeof(hdr))) return;
    if (klen && !txPush(t, key, klen)) return;
    if (vlen && !txPush(t, value, vlen)) return;

    t->digest ^= (uint_least64_t)selfTestRecordHash(db_id, key, klen, vlen);
    atomic_fetch_add_explicit(&t->sent, 1, memory_order_relaxed);
}

static void txComplete(void *privdata, int ok) {
    snapTransport *t = privdata;
    if (!ok || t->producer_failed) {
        uint32_t poison = EXT_SNAP_POISON_LEN;
        (void)txPush(t, (const char *)&poison, sizeof(poison));
        t->producer_failed = 1;
        return;
    }
    /* Terminator: zero length, then count and digest so the consumer can
     * verify rather than trust EOF. */
    uint32_t zero = 0;
    long long count = atomic_load_explicit(&t->sent, memory_order_relaxed);
    uint64_t dg = (uint64_t)t->digest;
    if (!txPush(t, (const char *)&zero, sizeof(zero))) return;
    if (!txPush(t, (const char *)&count, sizeof(count))) return;
    (void)txPush(t, (const char *)&dg, sizeof(dg));
}

int extSnapshotTransportArm(void) {
    snapTransport *t = &g_tx;
    if (t->armed) return C_ERR;
    if (!extSnapshotStreamSupported()) return C_ERR;

    memset(t, 0, sizeof(*t));
    t->fds[0] = t->fds[1] = -1;
    atomic_store_explicit(&t->sent, 0, memory_order_relaxed);

    if (pipe(t->fds) != 0) {
        serverLog(LL_WARNING, "Snapshot transport: pipe() failed: %s", strerror(errno));
        return C_ERR;
    }
    /* Non-blocking write end: see the backpressure note in ext_snapshot.h. */
    if (fcntl(t->fds[1], F_SETFL, fcntl(t->fds[1], F_GETFL, 0) | O_NONBLOCK) != 0) {
        close(t->fds[0]); close(t->fds[1]);
        t->fds[0] = t->fds[1] = -1;
        return C_ERR;
    }

    storageSnapshotSink sink = {
        .privdata = t,
        .set_size_hint = NULL,
        .writable = txWritable,
        .on_record = txOnRecord,
        .complete = txComplete,
    };
    t->armed = 1;
    if (storageSnapshotStreamStart(&sink) != STORAGE_OK) {
        t->armed = 0;
        close(t->fds[0]); close(t->fds[1]);
        t->fds[0] = t->fds[1] = -1;
        return C_ERR;
    }
    return C_OK;
}

int extSnapshotTransportReadFd(void) { return g_tx.fds[0]; }

void extSnapshotTransportCloseReadEnd(void) {
    if (g_tx.fds[0] >= 0) { close(g_tx.fds[0]); g_tx.fds[0] = -1; }
}

void extSnapshotTransportCloseWriteEnd(void) {
    if (g_tx.fds[1] >= 0) { close(g_tx.fds[1]); g_tx.fds[1] = -1; }
}

void extSnapshotTransportAbort(void) {
    if (!g_tx.armed) return;
    storageSnapshotStreamAbort();
}

void extSnapshotTransportRelease(void) {
    snapTransport *t = &g_tx;
    if (t->fds[0] >= 0) { close(t->fds[0]); t->fds[0] = -1; }
    if (t->fds[1] >= 0) { close(t->fds[1]); t->fds[1] = -1; }
    zfree(t->ovf); t->ovf = NULL; t->ovf_len = t->ovf_cap = 0;
    zfree(t->inbuf); t->inbuf = NULL; t->in_len = t->in_cap = 0;
    t->armed = 0;
}

long long extSnapshotTransportRecordsSent(void) {
    return atomic_load_explicit(&g_tx.sent, memory_order_relaxed);
}

/* --- consumer --------------------------------------------------------------*/

static int txFill(snapTransport *t, int blocking) {
    if (t->in_len + 65536 > t->in_cap) {
        size_t cap = t->in_cap ? t->in_cap * 2 : 262144;
        char *nb = zrealloc(t->inbuf, cap);
        if (!nb) return -1;
        t->inbuf = nb;
        t->in_cap = cap;
    }
    for (;;) {
        ssize_t n = read(t->fds[0], t->inbuf + t->in_len, t->in_cap - t->in_len);
        if (n > 0) { t->in_len += (size_t)n; return 1; }
        if (n == 0) return 0;                       /* writer closed */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!blocking) return 0;
            struct pollfd p = { .fd = t->fds[0], .events = POLLIN };
            poll(&p, 1, 20);
            continue;
        }
        return -1;
    }
}

int extSnapshotTransportDrain(extSnapshotRecordFn fn, void *privdata,
                              int budget, int blocking) {
    snapTransport *t = &g_tx;
    if (t->fds[0] < 0) return EXT_SNAP_DRAIN_ERR;
    if (t->saw_terminator) return EXT_SNAP_DRAIN_DONE;

    int consumed = 0;
    while (consumed < budget) {
        /* Need a length word to know what comes next. */
        if (t->in_len < sizeof(uint32_t)) {
            int r = txFill(t, blocking);
            if (r < 0) return EXT_SNAP_DRAIN_ERR;
            if (r == 0 && t->in_len < sizeof(uint32_t))
                return consumed ? consumed : EXT_SNAP_DRAIN_WOULDBLOCK;
            continue;
        }
        uint32_t plen;
        memcpy(&plen, t->inbuf, sizeof(plen));

        if (plen == EXT_SNAP_POISON_LEN) {
            serverLog(LL_WARNING, "Snapshot transport: producer poisoned the stream");
            return EXT_SNAP_DRAIN_ERR;
        }

        if (plen == 0) {
            /* Terminator: length word, count, digest. */
            size_t need = sizeof(uint32_t) + sizeof(long long) + sizeof(uint64_t);
            if (t->in_len < need) {
                int r = txFill(t, blocking);
                if (r < 0) return EXT_SNAP_DRAIN_ERR;
                if (r == 0 && t->in_len < need)
                    return consumed ? consumed : EXT_SNAP_DRAIN_WOULDBLOCK;
                continue;
            }
            long long count;
            uint64_t dg;
            memcpy(&count, t->inbuf + 4, sizeof(count));
            memcpy(&dg, t->inbuf + 4 + sizeof(count), sizeof(dg));
            memmove(t->inbuf, t->inbuf + need, t->in_len - need);
            t->in_len -= need;
            t->saw_terminator = 1;
            if (count != t->received || dg != (uint64_t)t->rx_digest) {
                serverLog(LL_WARNING,
                    "Snapshot transport mismatch: sent=%lld recv=%lld digest %llu vs %llu",
                    count, t->received,
                    (unsigned long long)dg, (unsigned long long)t->rx_digest);
                return EXT_SNAP_DRAIN_ERR;
            }
            return consumed ? consumed : EXT_SNAP_DRAIN_DONE;
        }

        /* A record. Wait for the whole frame before decoding. */
        size_t need = sizeof(uint32_t) + plen;
        if (plen < 12) return EXT_SNAP_DRAIN_ERR;   /* malformed */
        if (t->in_len < need) {
            int r = txFill(t, blocking);
            if (r < 0) return EXT_SNAP_DRAIN_ERR;
            if (r == 0 && t->in_len < need)
                return consumed ? consumed : EXT_SNAP_DRAIN_WOULDBLOCK;
            continue;
        }
        uint32_t dbid, klen, vlen;
        memcpy(&dbid, t->inbuf + 4, 4);
        memcpy(&klen, t->inbuf + 8, 4);
        memcpy(&vlen, t->inbuf + 12, 4);
        if ((size_t)klen + vlen + 12 != plen) return EXT_SNAP_DRAIN_ERR;

        const char *key = t->inbuf + 16;
        const char *val = key + klen;
        /* Count and digest EVERY record, including dropped ones: those figures
         * verify the transport against the producer's terminator, so filtering
         * them here would make a correct stream look truncated. The filter only
         * decides whether the record reaches the consumer. */
        t->received++;
        t->rx_digest ^= (uint_least64_t)selfTestRecordHash(dbid, key, klen, vlen);
        if (fn && extSnapshotRecordIsLive(dbid, key, klen))
            fn(privdata, dbid, key, klen, val, vlen);

        memmove(t->inbuf, t->inbuf + need, t->in_len - need);
        t->in_len -= need;
        consumed++;
    }
    return consumed;
}

/* --- resurrection barrier --------------------------------------------------*/

int extSnapshotDrainBarrier(void) {
    /* Completion processing can re-issue transiently rejected requests, so
     * this loops rather than draining once. */
    for (int i = 0; i < 200; i++) {
        extStorageBridge_drainOnly();
        processCompletedStorageRequests();
        if (total_items_spilling_to_ext_storage == 0 &&
            total_items_fetching_from_ext_storage == 0)
            return C_OK;
        usleep(500);
    }
    serverLog(LL_WARNING,
        "Snapshot barrier: storage IO did not settle (spilling=%lld fetching=%lld)",
        total_items_spilling_to_ext_storage, total_items_fetching_from_ext_storage);
    return C_ERR;
}

/* --- orphan filter -------------------------------------------------------- */

static long long ext_snapshot_orphans_dropped = 0;

long long extSnapshotOrphansDropped(void) { return ext_snapshot_orphans_dropped; }

int extSnapshotRecordIsLive(uint32_t physical_db_id, const char *key, size_t klen) {
    int logical = extStorageLogicalDbId((int)physical_db_id);
    if (logical < 0 || logical >= server.dbnum) {
        ext_snapshot_orphans_dropped++;
        return 0;
    }
    serverDb *db = server.db[logical];

    /* dbFind takes an sds, not an robj. */
    sds kn = sdsnewlen(key, klen);
    dbEntry *entry = dbFind(db, kn);

    int live = 1;
    if (entry == NULL) {
        /* The engine has forgotten this key. The record is a leftover whose
         * space has not been reclaimed. Writing it would resurrect the key. */
        live = 0;
    } else if (!objectIsTiered(entry)) {
        /* Superseded: the value is back in memory, so the memory section of the
         * snapshot already carries it and this record is stale. */
        live = 0;
    } else if (entry->tiering_state == TIERING_STATE_PENDING_DELETION) {
        /* A client DEL already removed it logically; the flash copy is being
         * deleted. Same condition extStorageMaterializeTiered rejects. */
        live = 0;
    }

    sdsfree(kn);
    if (!live) ext_snapshot_orphans_dropped++;
    return live;
}

/* --- transport self test ---------------------------------------------------*/

static void txTestCountRecord(void *privdata, uint32_t db_id,
                              const char *key, size_t klen,
                              const char *value, size_t vlen) {
    (void)db_id; (void)key; (void)klen; (void)value; (void)vlen;
    (*(long long *)privdata)++;
}

int extSnapshotTransportSelfTest(int timeout_ms, extSnapshotTransportTestResult *out) {
    if (!out) return C_ERR;
    memset(out, 0, sizeof(*out));
    if (!extSnapshotStreamSupported()) return C_ERR;

    mstime_t start = mstime();
    long long orphans_before = extSnapshotOrphansDropped();
    if (extSnapshotDrainBarrier() != C_OK) return C_ERR;
    if (extSnapshotTransportArm() != C_OK) return C_ERR;
    out->armed = 1;

    long long seen = 0;
    int done = 0;
    while (!done) {
        int r = extSnapshotTransportDrain(txTestCountRecord, &seen, 256, 0);
        out->drain_calls++;
        if (r == EXT_SNAP_DRAIN_DONE) { out->ok = 1; done = 1; break; }
        if (r == EXT_SNAP_DRAIN_ERR) { done = 1; break; }
        if (r == EXT_SNAP_DRAIN_WOULDBLOCK) {
            out->wouldblocks++;
            if (mstime() - start > timeout_ms) {
                serverLog(LL_WARNING, "Snapshot transport self test timed out");
                extSnapshotTransportAbort();
                /* Give the producer a moment to emit poison, then read it so
                 * the failure is observed rather than left in the pipe. */
                mstime_t deadline = mstime() + 2000;
                while (mstime() < deadline) {
                    int rr = extSnapshotTransportDrain(txTestCountRecord, &seen, 256, 0);
                    if (rr == EXT_SNAP_DRAIN_DONE || rr == EXT_SNAP_DRAIN_ERR) break;
                    usleep(1000);
                }
                break;
            }
            usleep(200);
        }
    }

    out->sent = extSnapshotTransportRecordsSent();
    out->received = seen;
    out->orphans = extSnapshotOrphansDropped() - orphans_before;
    out->elapsed_ms = (long long)(mstime() - start);
    extSnapshotTransportRelease();
    return C_OK;
}
