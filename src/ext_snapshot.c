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
#include <pthread.h>
#include <string.h>

extern int ext_data_enabled;

/* --- tunables (see ext_snapshot.h) ---------------------------------------- */
int ext_snapshot_stream_enabled = 1;
long long ext_snapshot_stream_overflow_limit = 64 * 1024 * 1024;
int ext_snapshot_stream_stall_timeout_ms = 30000;
long long ext_snapshot_stream_pipe_size = 0;

/* --- counters -------------------------------------------------------------
 *
 * Producer side, so they live in the process that armed the cut and stay
 * meaningful in the parent of a BGSAVE. The consumer's own figures (entries
 * written, orphans dropped) are only in-process for a foreground save, so they
 * are reported in the flash section's log line instead of pretended to here.
 *
 * Written from the storage IO thread, read by the main thread for INFO, hence
 * atomic. ------------------------------------------------------------------*/
static _Atomic long long stream_records_total = 0;   /* records handed to the pipe, all saves */
static _Atomic long long stream_last_records = 0;    /* ... in the most recent save */
static _Atomic long long stream_backpressure = 0;    /* times writable() said "not now" */
static _Atomic long long stream_overflow_peak = 0;   /* high water mark of buffered bytes */
static _Atomic long long stream_oversize_frames = 0; /* frames allowed past the overflow limit */
static _Atomic long long stream_aborts = 0;          /* streams that ended poisoned */
static _Atomic long long stream_last_ms = 0;         /* arm -> complete of the most recent save */

int extSnapshotStreamSupported(void) {
    return ext_data_enabled && storageSnapshotStreamSupported();
}

int extSnapshotStreamEnabled(void) {
    return ext_snapshot_stream_enabled && extSnapshotStreamSupported();
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
    int writer_closed;      /* reader saw EOF: producer is gone, not just slow */
    int is_consumer;        /* this process/thread drains the stream */
    mstime_t armed_ms;      /* for the duration reported in INFO */

    /* The overflow buffer is touched by the storage IO thread (from the sink
     * callbacks) and by the main thread (from the event loop, which has to keep
     * pushing it out because the engine may stop calling back at any point --
     * including permanently, once it has completed). Hence the lock. */
    pthread_mutex_t ovf_lock;
    _Atomic int producer_done;
    int poisoned;           /* poison marker queued; do not queue it twice */
    int write_error;        /* pipe is dead, not merely full */
} snapTransport;

static snapTransport g_tx;

/* --- producer --------------------------------------------------------------*/

/* Move buffered bytes toward the pipe. Deliberately has NO producer_failed
 * check: a poisoned stream still has to get its poison marker out, and that
 * marker lives in this same buffer. Caller must hold t->ovf_lock. */
static void txDrainOvfLocked(snapTransport *t) {
    if (t->fds[1] < 0) return;
    while (t->ovf_len > 0) {
        ssize_t n = write(t->fds[1], t->ovf, t->ovf_len);
        if (n > 0) {
            memmove(t->ovf, t->ovf + n, t->ovf_len - (size_t)n);
            t->ovf_len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        /* EAGAIN just means later; anything else is a dead pipe. */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        t->write_error = 1;
        return;
    }
}

/* Kill the stream and queue the poison marker so the consumer fails at once
 * instead of waiting out its stall deadline.
 *
 * Whatever is still buffered is dropped first: the stream is over, those bytes
 * are a partial frame nobody can use, and leaving them in front of the marker
 * would delay or prevent its delivery. Caller must hold ovf_lock. */
static void txPoisonLocked(snapTransport *t) {
    if (t->poisoned) return;
    t->poisoned = 1;
    t->producer_failed = 1;
    t->ovf_len = 0;
    if (t->ovf_cap < sizeof(uint32_t)) {
        char *nb = zrealloc(t->ovf, 64);
        if (!nb) return;
        t->ovf = nb;
        t->ovf_cap = 64;
    }
    uint32_t poison = EXT_SNAP_POISON_LEN;
    memcpy(t->ovf, &poison, sizeof(poison));
    t->ovf_len = sizeof(poison);
    txDrainOvfLocked(t);

    /* Then close the write end, and rely on that rather than on the marker.
     *
     * The marker alone is not enough. Dropping the buffered bytes above can
     * leave a truncated frame in the pipe, and the consumer waits for the rest
     * of that frame before it will look at anything else -- so the four poison
     * bytes queue up behind a frame that will never complete and are never
     * interpreted. That is exactly how a poisoned stream still cost the full
     * stall deadline. EOF cannot be stuck behind a partial frame: the consumer
     * sees it as writer_closed and fails immediately.
     *
     * Safe because the stream is over: nothing more will ever be sent. */
    if (t->fds[1] >= 0) {
        close(t->fds[1]);
        t->fds[1] = -1;
        t->ovf_len = 0;
    }
}

/* Push bytes toward the pipe. Anything the pipe will not take right now lands
 * in overflow. Returns 0 on hard failure.
 * Caller must hold t->ovf_lock; use txPush(). */
static int txPushLocked(snapTransport *t, const char *buf, size_t len) {
    if (t->producer_failed) return 0;

    /* Overflow must drain first or frames would be reordered. */
    txDrainOvfLocked(t);
    if (t->write_error) {
        txPoisonLocked(t);
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

    /* Buffer the remainder. Growing past the limit means the consumer is not
     * keeping up at all, so fail rather than absorb without bound.
     *
     * One exception, and it is load bearing: when nothing is buffered yet, this
     * is a single frame that on its own exceeds the limit. Refusing it would
     * make snapshots impossible for any keyspace holding a value larger than
     * the limit -- and ext-storage-max-spill-size allows values well past the
     * default -- so let it through and count it. The limit is a bound on how
     * far the consumer may fall BEHIND, not a cap on frame size. */
    size_t need = len - off;
    size_t limit = (size_t)ext_snapshot_stream_overflow_limit;
    if (t->ovf_len + need > limit) {
        if (t->ovf_len == 0) {
            atomic_fetch_add_explicit(&stream_oversize_frames, 1, memory_order_relaxed);
        } else {
            serverLog(LL_WARNING,
                "Snapshot transport buffered %zu bytes past the %lld byte limit, aborting stream",
                t->ovf_len + need, ext_snapshot_stream_overflow_limit);
            txPoisonLocked(t);
            return 0;
        }
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
    if ((long long)t->ovf_len > atomic_load_explicit(&stream_overflow_peak, memory_order_relaxed))
        atomic_store_explicit(&stream_overflow_peak, (long long)t->ovf_len, memory_order_relaxed);
    return 1;
}

/* Lock-taking wrapper. Every path that moves bytes goes through this, because
 * the main thread pushes the buffer out from the event loop while the storage IO
 * thread is still producing into it. */
static int txPush(snapTransport *t, const char *buf, size_t len) {
    pthread_mutex_lock(&t->ovf_lock);
    int r = txPushLocked(t, buf, len);
    pthread_mutex_unlock(&t->ovf_lock);
    return r;
}

/* Always accept, unless the stream is already dead.
 *
 * This used to report "not now" while overflow was pending, on the assumption
 * that the storage engine would back off and re-poll. Real FlashCache does not:
 * a single refusal makes it ABORT the snapshot and call complete(ok=0). The
 * counters made that unambiguous -- one backpressure event, nine records
 * delivered out of two hundred, one abort -- while the mock, which does retry,
 * passed the same test.
 *
 * So refusal is not a usable signal and the transport has to absorb instead.
 * The bound is ext-storage-snapshot-stream-overflow-limit: buffer up to it, and
 * past it poison the stream so the save fails loudly and falls back, rather
 * than growing without limit. The counter below is kept purely as a signal that
 * the consumer is running behind. */
static int txWritable(void *privdata) {
    snapTransport *t = privdata;
    pthread_mutex_lock(&t->ovf_lock);
    if (t->ovf_len > 0) {
        txDrainOvfLocked(t);
        if (t->ovf_len > 0)
            atomic_fetch_add_explicit(&stream_backpressure, 1, memory_order_relaxed);
    }
    int failed = t->producer_failed;
    pthread_mutex_unlock(&t->ovf_lock);
    return failed ? 0 : 1;
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
    atomic_fetch_add_explicit(&stream_records_total, 1, memory_order_relaxed);
}

static void txComplete(void *privdata, int ok) {
    snapTransport *t = privdata;
    atomic_store_explicit(&stream_last_records,
                          atomic_load_explicit(&t->sent, memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&stream_last_ms, (long long)(mstime() - t->armed_ms),
                          memory_order_relaxed);
    if (!ok || t->producer_failed) {
        atomic_fetch_add_explicit(&stream_aborts, 1, memory_order_relaxed);
        pthread_mutex_lock(&t->ovf_lock);
        txPoisonLocked(t);
        pthread_mutex_unlock(&t->ovf_lock);
        atomic_store_explicit(&t->producer_done, 1, memory_order_release);
        return;
    }
    /* Terminator: zero length, then count and digest so the consumer can
     * verify rather than trust EOF. */
    uint32_t zero = 0;
    long long count = atomic_load_explicit(&t->sent, memory_order_relaxed);
    uint64_t dg = (uint64_t)t->digest;
    if (txPush(t, (const char *)&zero, sizeof(zero)) &&
        txPush(t, (const char *)&count, sizeof(count)))
        (void)txPush(t, (const char *)&dg, sizeof(dg));

    /* Hand the overflow buffer to the main thread. Anything still buffered here
     * -- the tail of the last record, or the terminator itself -- would
     * otherwise be stranded forever: overflow only drains from inside a sink
     * callback, and the storage engine stops calling us the moment it has
     * completed. The consumer would then wait out its stall deadline and fail a
     * snapshot that had in fact been produced in full. */
    atomic_store_explicit(&t->producer_done, 1, memory_order_release);
}

/* Main thread. Push out whatever the producer left buffered.
 *
 * Deliberately NOT gated on producer_done: the engine decides when to call
 * writable(), and it may stop for a while or forever, while the consumer keeps
 * draining the pipe and freeing room that only we will use. Cheap no-op when
 * there is nothing to do, so it is fine to call every event loop iteration. */
void extSnapshotTransportFlushPending(void) {
    snapTransport *t = &g_tx;
    /* Checked BEFORE taking the lock, and that ordering is load bearing.
     *
     * A fork child inherits a copy of this mutex in whatever state it was in at
     * fork() -- so if the storage IO thread happened to hold it, the child's
     * copy is locked forever. The child is consumer-only and must therefore
     * never touch this lock at all. It always closes the write end immediately
     * after fork, so fds[1] < 0 is exactly the "I am the child" test, and it has
     * to be answered without locking. A foreground save is unaffected: there is
     * no fork, and its write end is open in this same process. */
    if (!t->armed || t->fds[1] < 0) return;
    if (t->ovf_len == 0) return;
    pthread_mutex_lock(&t->ovf_lock);
    if (t->fds[1] >= 0 && t->ovf_len > 0) txDrainOvfLocked(t);
    pthread_mutex_unlock(&t->ovf_lock);
}

int extSnapshotTransportArm(void) {
    snapTransport *t = &g_tx;
    if (t->armed) return C_ERR;
    if (!extSnapshotStreamSupported()) return C_ERR;

    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->ovf_lock, NULL);
    t->fds[0] = t->fds[1] = -1;
    t->armed_ms = mstime();
    atomic_store_explicit(&t->sent, 0, memory_order_relaxed);

    if (pipe(t->fds) != 0) {
        serverLog(LL_WARNING, "Snapshot transport: pipe() failed: %s", strerror(errno));
        return C_ERR;
    }
    /* A bigger pipe lets the producer run ahead before it has to buffer, which
     * matters most while the consumer is still in the memory section and not
     * draining at all. Advisory: the kernel caps this at
     * /proc/sys/fs/pipe-max-size for unprivileged processes, so a refusal is
     * not fatal -- we just keep the default and say so once. */
    if (ext_snapshot_stream_pipe_size > 0) {
        if (fcntl(t->fds[1], F_SETPIPE_SZ, (int)ext_snapshot_stream_pipe_size) == -1) {
            static int warned = 0;
            if (!warned) {
                serverLog(LL_NOTICE,
                    "Snapshot transport: could not set pipe size to %lld (%s), using the default",
                    ext_snapshot_stream_pipe_size, strerror(errno));
                warned = 1;
            }
        }
    }
    /* Both ends non-blocking.
     *
     * Write end: see the backpressure note in ext_snapshot.h -- blocking the
     * engine IO thread would stall spills and fetches too.
     *
     * Read end: a blocking read end would silently defeat the consumer's
     * non-blocking drain. read() would sleep in the kernel instead of
     * returning EAGAIN, so the drain could never report WOULDBLOCK, its stall
     * deadline would never be evaluated, and a foreground save -- which drains
     * on the main thread -- would hang the whole server on a wedged producer
     * rather than failing after the deadline. */
    if (fcntl(t->fds[0], F_SETFL, fcntl(t->fds[0], F_GETFL, 0) | O_NONBLOCK) != 0 ||
        fcntl(t->fds[1], F_SETFL, fcntl(t->fds[1], F_GETFL, 0) | O_NONBLOCK) != 0) {
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
    /* Only if Arm got as far as initialising it. Release is also the cleanup
     * path for a failed Arm. */
    if (t->armed) pthread_mutex_destroy(&t->ovf_lock);
    zfree(t->ovf); t->ovf = NULL; t->ovf_len = t->ovf_cap = 0;
    zfree(t->inbuf); t->inbuf = NULL; t->in_len = t->in_cap = 0;
    t->armed = 0;
    t->is_consumer = 0;
    t->writer_closed = 0;
}

long long extSnapshotTransportRecordsSent(void) {
    return atomic_load_explicit(&g_tx.sent, memory_order_relaxed);
}

int extSnapshotTransportArmed(void) { return g_tx.armed; }

void extSnapshotTransportBecomeConsumer(void) { g_tx.is_consumer = 1; }

int extSnapshotTransportWriterClosed(void) { return g_tx.writer_closed; }

sds genExtSnapshotStreamInfoString(sds info) {
    return sdscatprintf(info,
        "snapshot_stream_enabled:%d\r\n"
        "snapshot_stream_available:%d\r\n"
        "snapshot_stream_armed:%d\r\n"
        "snapshot_stream_records_total:%lld\r\n"
        "snapshot_stream_last_records:%lld\r\n"
        "snapshot_stream_last_duration_ms:%lld\r\n"
        "snapshot_stream_backpressure_events:%lld\r\n"
        "snapshot_stream_overflow_peak_bytes:%lld\r\n"
        "snapshot_stream_oversize_frames:%lld\r\n"
        "snapshot_stream_aborts:%lld\r\n",
        ext_snapshot_stream_enabled ? 1 : 0,
        extSnapshotStreamSupported() ? 1 : 0,
        g_tx.armed ? 1 : 0,
        atomic_load_explicit(&stream_records_total, memory_order_relaxed),
        atomic_load_explicit(&stream_last_records, memory_order_relaxed),
        atomic_load_explicit(&stream_last_ms, memory_order_relaxed),
        atomic_load_explicit(&stream_backpressure, memory_order_relaxed),
        atomic_load_explicit(&stream_overflow_peak, memory_order_relaxed),
        atomic_load_explicit(&stream_oversize_frames, memory_order_relaxed),
        atomic_load_explicit(&stream_aborts, memory_order_relaxed));
}

int extSnapshotStreamConsumerActive(void) {
    return g_tx.armed && g_tx.is_consumer;
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
        if (n == 0) { t->writer_closed = 1; return 0; }  /* writer closed */
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
        int logical_db = 0;
        robj *entry = NULL;
        int stop = 0;
        if (fn && extSnapshotRecordResolve(dbid, key, klen, &logical_db, &entry))
            stop = fn(privdata, logical_db, entry, key, klen, val, vlen) != 0;

        memmove(t->inbuf, t->inbuf + need, t->in_len - need);
        t->in_len -= need;
        consumed++;
        /* The frame is consumed before bailing out so the stream stays framed if
         * anyone drains again. */
        if (stop) return EXT_SNAP_DRAIN_ERR;
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

int extSnapshotRecordResolve(uint32_t physical_db_id, const char *key, size_t klen,
                             int *logical_db, robj **out_entry) {
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
    if (!live) {
        ext_snapshot_orphans_dropped++;
        return 0;
    }
    if (logical_db) *logical_db = logical;
    if (out_entry) *out_entry = entry;
    return 1;
}

int extSnapshotRecordIsLive(uint32_t physical_db_id, const char *key, size_t klen) {
    return extSnapshotRecordResolve(physical_db_id, key, klen, NULL, NULL);
}

/* --- transport self test ---------------------------------------------------*/

static int txTestCountRecord(void *privdata, int logical_db, robj *entry,
                             const char *key, size_t klen,
                             const char *value, size_t vlen) {
    (void)logical_db; (void)entry; (void)key; (void)klen; (void)value; (void)vlen;
    (*(long long *)privdata)++;
    return 0;
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

/* ---------------------------------------------------------------------------
 * Drain to completion
 * ---------------------------------------------------------------------------*/

/* One loop, two very different consumers, which is why the exit conditions look
 * over-specified:
 *
 *  - BGSAVE child: the write end lives in another process, so EOF is
 *    observable. Seeing it without a terminator means the producer died.
 *  - Foreground save: the write end is open in this same process, so EOF never
 *    arrives however wedged the producer gets. A blocking drain would hang the
 *    server forever, so the stall deadline is the only backstop.
 *
 * The deadline measures time WITHOUT PROGRESS rather than total elapsed. A flash
 * set large enough to take an hour to stream is not a stall, and any total
 * budget would be either wrong for large sets or useless for small ones. */
int extSnapshotDrainAll(extSnapshotRecordFn fn, void *privdata) {
    mstime_t last_progress = mstime();

    for (;;) {
        int r = extSnapshotTransportDrain(fn, privdata, 256, 0);
        if (r == EXT_SNAP_DRAIN_DONE) return C_OK;
        if (r == EXT_SNAP_DRAIN_ERR) return C_ERR;
        if (r > 0) {
            last_progress = mstime();
            continue;
        }
        /* WOULDBLOCK. */
        if (extSnapshotTransportWriterClosed()) {
            serverLog(LL_WARNING,
                "Snapshot transport: stream ended without a terminator");
            return C_ERR;
        }
        /* Foreground save: no event loop is running, so nothing else will push
         * out a terminator the producer left sitting in overflow. Do it here. */
        extSnapshotTransportFlushPending();
        if (mstime() - last_progress > ext_snapshot_stream_stall_timeout_ms) {
            serverLog(LL_WARNING,
                "Snapshot transport: no records for %d ms, aborting",
                ext_snapshot_stream_stall_timeout_ms);
            extSnapshotTransportAbort();
            return C_ERR;
        }
        usleep(200);
    }
}

/* ---------------------------------------------------------------------------
 * Save lifecycle
 * ---------------------------------------------------------------------------*/

static extSnapshotSaveMode g_save_mode = EXT_SNAP_SAVE_NONE;

extSnapshotSaveMode extSnapshotSaveBegin(int will_fork) {
    g_save_mode = EXT_SNAP_SAVE_NONE;

    /* A fork child inherits whichever strategy its parent set up, so it must
     * not set up anything itself. */
    if (server.in_fork_child) return g_save_mode;
    if (!ext_data_enabled || num_items_on_flash <= 0) return g_save_mode;

    /* Preferred: ask the storage engine for a point-in-time stream, so no value
     * has to be read back. The barrier settles in-flight IO first -- a key
     * deleted before the cut whose record has not been reclaimed would still sit
     * inside the frozen range, and writing it would resurrect the key. */
    if (extSnapshotStreamEnabled() && !extStorageSnapshotActive()) {
        if (extSnapshotDrainBarrier() == C_OK && extSnapshotTransportArm() == C_OK) {
            extStorageSnapshotCountStreamSave();
            /* No fork means this thread is the consumer, so claim the role now.
             * A forking save leaves it to the child. */
            if (!will_fork) extSnapshotTransportBecomeConsumer();
            g_save_mode = EXT_SNAP_SAVE_STREAM;
            return g_save_mode;
        }
        extSnapshotTransportRelease();
        serverLog(LL_NOTICE,
            "Snapshot stream unavailable, falling back to the fork read path");
    }

    /* Fallback: quiesce tiering IO, park the storage IO thread and pause on-flash
     * GC before the fork, so the child inherits consistent backend state and
     * stable flash offsets. */
    if (extStorageSnapshotSupported() && !extStorageSnapshotActive() &&
        extStorageSnapshotPrepare() == C_OK) {
        g_save_mode = EXT_SNAP_SAVE_FORKREAD;
        return g_save_mode;
    }

    /* Neither path is available and values do live on flash. Refuse rather than
     * write a snapshot that silently omits them. Rate limited because a save can
     * be attempted every few seconds. */
    static mstime_t last_log = 0;
    if (mstime() - last_log > 60000) {
        serverLog(LL_WARNING,
            "Refusing RDB save: %lld values reside on external storage and the "
            "active backend cannot snapshot them", num_items_on_flash);
        last_log = mstime();
    }
    g_save_mode = EXT_SNAP_SAVE_REFUSE;
    return g_save_mode;
}

void extSnapshotSaveAfterForkChild(void) {
    if (g_save_mode != EXT_SNAP_SAVE_STREAM) return;
    /* Drop the producer end first: leaving it open would stop our own reader
     * ever seeing EOF, which is how a dead parent is detected. */
    extSnapshotTransportCloseWriteEnd();
    extSnapshotTransportBecomeConsumer();
}

void extSnapshotSaveAfterForkParent(void) {
    /* Streaming: the child owns the read end. The write end stays open here
     * because the storage IO thread is still producing into it. */
    if (g_save_mode == EXT_SNAP_SAVE_STREAM) extSnapshotTransportCloseReadEnd();
    /* Fork read: unpark the storage IO thread so normal tiering traffic resumes
     * while the child works. GC stays paused until the child is reaped. */
    if (g_save_mode == EXT_SNAP_SAVE_FORKREAD) extStorageSnapshotResume();
}

void extSnapshotSaveEnd(void) {
    /* On-flash GC may resume. No-op when nothing was prepared. */
    extStorageSnapshotDone();
    /* With the consumer gone nothing will drain the rest of the stream, so
     * cancel it rather than let the engine keep producing into a pipe with no
     * reader. Abort is idempotent once the stream completed, which is the normal
     * success case. */
    if (extSnapshotTransportArmed()) {
        extSnapshotTransportAbort();
        extSnapshotTransportRelease();
    }
    g_save_mode = EXT_SNAP_SAVE_NONE;
}
