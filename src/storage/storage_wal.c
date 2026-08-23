/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Client-ack WAL core. See storage_wal.h for the format and architecture.
 * Self-contained (libc + pthread only) so the dump/validator tool can link
 * this file standalone. */

#define _GNU_SOURCE /* pthread_setname_np */

#include "storage_wal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * crc32c (Castagnoli), software table. Records are small and value bytes are
 * CRC'd once per unit; table crc32c is plenty for the POC.
 * ---------------------------------------------------------------------------*/
static uint32_t crc32c_table[256];
static pthread_once_t crc32c_once = PTHREAD_ONCE_INIT;

static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78U ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
}

uint32_t walCrc32c(uint32_t crc, const void *buf, size_t len) {
    pthread_once(&crc32c_once, crc32c_init);
    const unsigned char *p = buf;
    crc = ~crc;
    while (len--) crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------------------------------------------------------------------
 * Record encoding
 * ---------------------------------------------------------------------------*/
/* Header layout (little-endian, packed by hand to avoid struct padding
 * ambiguity): [0..3] crc, [4..7] body_len, [8..15] lsn, [16] kind,
 * [17] flags, [18..19] reserved. */
static void wal_put_u32(char *p, uint32_t v) { memcpy(p, &v, 4); }
static void wal_put_u64(char *p, uint64_t v) { memcpy(p, &v, 8); }

/* Writes a full record (header + body) into dst; returns record size and the
 * record's crc via *crc_out. */
static size_t wal_encode_record(char *dst, uint8_t kind, uint8_t flags,
                                uint64_t lsn, const char *body,
                                size_t body_len, uint32_t *crc_out) {
    char *h = dst;
    wal_put_u32(h + 4, (uint32_t)body_len);
    wal_put_u64(h + 8, lsn);
    h[16] = (char)kind;
    h[17] = (char)flags;
    h[18] = 0;
    h[19] = 0;
    if (body_len) memcpy(dst + WAL_RECORD_HDR_SIZE, body, body_len);
    uint32_t crc = walCrc32c(0, h + 4, WAL_RECORD_HDR_SIZE - 4);
    if (body_len) crc = walCrc32c(crc, body, body_len);
    wal_put_u32(h, crc);
    *crc_out = crc;
    return WAL_RECORD_HDR_SIZE + body_len;
}

/* ---------------------------------------------------------------------------
 * Blob builder (main thread)
 * ---------------------------------------------------------------------------*/
typedef struct walPendingItem {
    uint32_t dbid;
    uint32_t klen;
    uint32_t vlen;
    uint8_t tombstone;
    char *bytes; /* key || value, malloc'd */
} walPendingItem;

struct walBlobBuilder {
    walPendingItem *items;
    size_t count, cap;
    size_t body_bytes; /* sum of item body sizes */
};

/* Global LSN + group-seq counters. LSNs are assigned at finalize time on the
 * (single) main thread; atomics keep INFO reads from other contexts sane. */
static _Atomic uint64_t wal_next_lsn = 1;
static _Atomic uint64_t wal_next_group_seq = 1;

walBlobBuilder *walBlobNew(void) {
    walBlobBuilder *b = calloc(1, sizeof(*b));
    return b;
}

void walBlobAddItem(walBlobBuilder *b, uint32_t dbid, const void *key,
                    size_t klen, const void *val, size_t vlen, int tombstone) {
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->items = realloc(b->items, b->cap * sizeof(walPendingItem));
    }
    walPendingItem *it = &b->items[b->count++];
    it->dbid = dbid;
    it->klen = (uint32_t)klen;
    it->vlen = (uint32_t)vlen;
    it->tombstone = tombstone ? 1 : 0;
    it->bytes = malloc(klen + vlen);
    memcpy(it->bytes, key, klen);
    if (vlen) memcpy(it->bytes + klen, val, vlen);
    b->body_bytes += 12 + klen + vlen; /* dbid+klen+vlen prefix + payload */
}

uint64_t walBlobFinalize(walBlobBuilder *b, char **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (b->count == 0) return 0;

    int grouped = b->count > 1;
    /* Worst-case size: every item record + optional BEGIN/COMMIT records. */
    size_t max = b->count * WAL_RECORD_HDR_SIZE + b->body_bytes +
                 (grouped ? 2 * WAL_RECORD_HDR_SIZE + 8 + 16 : 0);
    char *out = malloc(max);
    size_t off = 0;
    uint32_t crc;
    uint64_t commit_lsn = 0;
    uint32_t group_crc = 0;

    uint64_t group_seq = 0;
    if (grouped) {
        group_seq = atomic_fetch_add_explicit(&wal_next_group_seq, 1,
                                              memory_order_relaxed);
        char body[8];
        wal_put_u64(body, group_seq);
        uint64_t lsn = atomic_fetch_add_explicit(&wal_next_lsn, 1,
                                                 memory_order_relaxed);
        off += wal_encode_record(out + off, WAL_REC_GROUP_BEGIN, 0, lsn, body,
                                 sizeof(body), &crc);
    }

    for (size_t i = 0; i < b->count; i++) {
        walPendingItem *it = &b->items[i];
        size_t body_len = 12 + it->klen + it->vlen;
        char *body = malloc(body_len);
        wal_put_u32(body, it->dbid);
        wal_put_u32(body + 4, it->klen);
        wal_put_u32(body + 8, it->vlen);
        memcpy(body + 12, it->bytes, it->klen + it->vlen);
        uint64_t lsn = atomic_fetch_add_explicit(&wal_next_lsn, 1,
                                                 memory_order_relaxed);
        uint8_t kind = grouped ? WAL_REC_IN_GROUP : WAL_REC_STANDALONE;
        uint8_t flags = it->tombstone ? WAL_RFLAG_TOMBSTONE : 0;
        off += wal_encode_record(out + off, kind, flags, lsn, body, body_len,
                                 &crc);
        free(body);
        if (grouped) {
            /* group_crc chains over member record CRCs (doc format). */
            group_crc = walCrc32c(group_crc, &crc, sizeof(crc));
        } else {
            commit_lsn = lsn;
        }
    }

    if (grouped) {
        char body[16];
        wal_put_u64(body, group_seq);
        wal_put_u32(body + 8, (uint32_t)b->count);
        wal_put_u32(body + 12, group_crc);
        uint64_t lsn = atomic_fetch_add_explicit(&wal_next_lsn, 1,
                                                 memory_order_relaxed);
        off += wal_encode_record(out + off, WAL_REC_GROUP_COMMIT, 0, lsn, body,
                                 sizeof(body), &crc);
        commit_lsn = lsn;
    }

    *buf = out;
    *len = off;
    return commit_lsn;
}

void walBlobFree(walBlobBuilder *b) {
    if (!b) return;
    for (size_t i = 0; i < b->count; i++) free(b->items[i].bytes);
    free(b->items);
    free(b);
}

/* ---------------------------------------------------------------------------
 * Writer thread: SPSC blob ring, group-commit fdatasync
 * ---------------------------------------------------------------------------*/
#define WAL_RING_SIZE 4096

typedef struct walBlob {
    char *buf;
    size_t len;
    uint64_t max_lsn;
} walBlob;

typedef struct walState {
    walBlob ring[WAL_RING_SIZE];
    _Atomic int head; /* producer: main thread */
    _Atomic int tail; /* consumer: writer thread */
    int fd;
    int wakeup_pipe[2];
    _Atomic int fsync_policy;
    _Atomic uint64_t durable_lsn;
    _Atomic uint64_t written_lsn; /* high-water mark of LSNs write()n to fd */
    _Atomic int shutdown;
    pthread_t thread;
    int open;
    /* stats (writer thread writes; readers tolerate staleness) */
    _Atomic uint64_t st_records, st_groups, st_units, st_bytes, st_fsyncs;
    _Atomic uint64_t st_ring_full_stalls;
} walState;

static walState W; /* zero-initialized; single WAL per process */

uint64_t walDurableLsn(void) {
    return atomic_load_explicit(&W.durable_lsn, memory_order_acquire);
}

uint64_t walLastAssignedLsn(void) {
    return atomic_load_explicit(&wal_next_lsn, memory_order_relaxed) - 1;
}

void walSetFsyncPolicy(int p) {
    atomic_store_explicit(&W.fsync_policy, p, memory_order_release);
}

int walIsOpen(void) { return W.open; }

void walGetStats(walStats *out) {
    out->records = atomic_load(&W.st_records);
    out->groups = atomic_load(&W.st_groups);
    out->units = atomic_load(&W.st_units);
    out->bytes = atomic_load(&W.st_bytes);
    out->fsyncs = atomic_load(&W.st_fsyncs);
    out->ring_full_stalls = atomic_load(&W.st_ring_full_stalls);
}

/* Counts framing kinds in a finalized blob for stats (cheap: header walk). */
static void wal_count_blob(const char *buf, size_t len) {
    size_t off = 0;
    while (off + WAL_RECORD_HDR_SIZE <= len) {
        uint32_t body_len;
        memcpy(&body_len, buf + off + 4, 4);
        uint8_t kind = (uint8_t)buf[off + 16];
        if (kind == WAL_REC_STANDALONE || kind == WAL_REC_IN_GROUP)
            atomic_fetch_add_explicit(&W.st_records, 1, memory_order_relaxed);
        else if (kind == WAL_REC_GROUP_COMMIT)
            atomic_fetch_add_explicit(&W.st_groups, 1, memory_order_relaxed);
        off += WAL_RECORD_HDR_SIZE + body_len;
    }
}

static void *wal_writer_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "wal_writer");
    struct timespec last_fsync;
    clock_gettime(CLOCK_MONOTONIC, &last_fsync);

    for (;;) {
        int did_work = 0;
        uint64_t batch_max_lsn = 0;

        /* Drain everything available: this IS the group commit batch. */
        int tail = atomic_load_explicit(&W.tail, memory_order_relaxed);
        while (tail != atomic_load_explicit(&W.head, memory_order_acquire)) {
            walBlob *bl = &W.ring[tail];
            const char *p = bl->buf;
            size_t remaining = bl->len;
            while (remaining > 0) {
                ssize_t n = write(W.fd, p, remaining);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    /* Write failure = durability broken. Abort loudly rather
                     * than acking undurable writes. */
                    abort();
                }
                p += n;
                remaining -= (size_t)n;
            }
            atomic_fetch_add_explicit(&W.st_bytes, bl->len,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&W.st_units, 1, memory_order_relaxed);
            wal_count_blob(bl->buf, bl->len);
            if (bl->max_lsn > batch_max_lsn) batch_max_lsn = bl->max_lsn;
            if (bl->max_lsn >
                atomic_load_explicit(&W.written_lsn, memory_order_relaxed))
                atomic_store_explicit(&W.written_lsn, bl->max_lsn,
                                      memory_order_relaxed);
            free(bl->buf);
            tail = (tail + 1) % WAL_RING_SIZE;
            atomic_store_explicit(&W.tail, tail, memory_order_release);
            did_work = 1;
        }

        int policy = atomic_load_explicit(&W.fsync_policy,
                                          memory_order_acquire);
        int shutting_down =
            atomic_load_explicit(&W.shutdown, memory_order_acquire);
        int need_fsync = 0;
        if (did_work && policy == WAL_FSYNC_ALWAYS) need_fsync = 1;
        if (policy == WAL_FSYNC_EVERYSEC) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec != last_fsync.tv_sec &&
                atomic_load_explicit(&W.written_lsn, memory_order_relaxed) >
                    atomic_load_explicit(&W.durable_lsn, memory_order_relaxed))
                need_fsync = 1;
        }
        if (shutting_down && did_work) need_fsync = 1;

        if (need_fsync) {
            if (fdatasync(W.fd) != 0) abort(); /* same rationale as write() */
            clock_gettime(CLOCK_MONOTONIC, &last_fsync);
            atomic_fetch_add_explicit(&W.st_fsyncs, 1, memory_order_relaxed);
            /* Everything write()n to the fd before this fsync is durable.
             * written_lsn (not the assigned-LSN counter) is the honest
             * bound: blobs still in the ring are NOT covered. */
            uint64_t publish =
                atomic_load_explicit(&W.written_lsn, memory_order_relaxed);
            uint64_t cur =
                atomic_load_explicit(&W.durable_lsn, memory_order_relaxed);
            if (publish > cur)
                atomic_store_explicit(&W.durable_lsn, publish,
                                      memory_order_release);
            /* Wake the event loop so gated replies release. */
            char byte = 1;
            ssize_t wr = write(W.wakeup_pipe[1], &byte, 1);
            (void)wr; /* pipe full = wakeup already pending; fine */
        }

        if (shutting_down &&
            atomic_load_explicit(&W.tail, memory_order_relaxed) ==
                atomic_load_explicit(&W.head, memory_order_acquire))
            break;

        if (!did_work) {
            struct timespec ts = {0, 50000}; /* 50us */
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

int walOpen(const char *path, int fsync_policy, int *wakeup_fd_out) {
    if (W.open) return -1;
    memset(&W, 0, sizeof(W));
    /* A leftover WAL from a previous run (crash, or clean shutdown -- steps
     * 6-7 have no replay yet) is rotated aside, not truncated: it preserves
     * crash evidence and step 8 will turn this into real replay. */
    if (access(path, F_OK) == 0) {
        size_t plen = strlen(path);
        char *prev = malloc(plen + 6);
        memcpy(prev, path, plen);
        memcpy(prev + plen, ".prev", 6);
        rename(path, prev); /* best-effort */
        free(prev);
    }
    W.fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (W.fd < 0) return -1;
    if (write(W.fd, WAL_FILE_MAGIC, WAL_FILE_MAGIC_LEN) !=
        WAL_FILE_MAGIC_LEN) {
        close(W.fd);
        return -1;
    }
    if (pipe(W.wakeup_pipe) != 0) {
        close(W.fd);
        return -1;
    }
    /* Non-blocking on both ends: writer must never park on a full pipe, the
     * event-loop handler drains without blocking. */
    fcntl(W.wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(W.wakeup_pipe[1], F_SETFL, O_NONBLOCK);
    atomic_store(&W.fsync_policy, fsync_policy);
    atomic_store(&W.shutdown, 0);
    if (pthread_create(&W.thread, NULL, wal_writer_thread, NULL) != 0) {
        close(W.fd);
        close(W.wakeup_pipe[0]);
        close(W.wakeup_pipe[1]);
        return -1;
    }
    W.open = 1;
    *wakeup_fd_out = W.wakeup_pipe[0];
    return 0;
}

int walSubmit(char *buf, size_t len, uint64_t max_lsn) {
    int head = atomic_load_explicit(&W.head, memory_order_relaxed);
    int next = (head + 1) % WAL_RING_SIZE;
    while (next == atomic_load_explicit(&W.tail, memory_order_acquire)) {
        /* Ring full: backpressure surfaces as ack latency, by design. */
        atomic_fetch_add_explicit(&W.st_ring_full_stalls, 1,
                                  memory_order_relaxed);
        struct timespec ts = {0, 100000}; /* 100us */
        nanosleep(&ts, NULL);
    }
    W.ring[head].buf = buf;
    W.ring[head].len = len;
    W.ring[head].max_lsn = max_lsn;
    atomic_store_explicit(&W.head, next, memory_order_release);
    return 0;
}

void walClose(void) {
    if (!W.open) return;
    atomic_store_explicit(&W.shutdown, 1, memory_order_release);
    pthread_join(W.thread, NULL);
    fdatasync(W.fd);
    close(W.fd);
    close(W.wakeup_pipe[0]);
    close(W.wakeup_pipe[1]);
    W.open = 0;
}
