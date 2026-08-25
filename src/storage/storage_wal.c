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
    _Atomic int truncate_req;
    _Atomic uint64_t truncate_gen;
    _Atomic uint64_t st_ring_full_stalls;
    /* Segment rotation: the retire cron asks the writer to rotate the active
     * file aside (rename to <path>.seg.<gen>) so retirement can proceed per
     * segment under sustained load, without requiring a write quiesce. */
    _Atomic int rotate_req;
    _Atomic uint64_t rotate_gen; /* acked rotations (writer increments) */
    _Atomic uint64_t seg_gen;    /* gen of the CURRENT active file */
    char path[4096];             /* active WAL path (writer renames from it) */
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
            if (atomic_load_explicit(&W.truncate_req, memory_order_acquire)) {
                /* Ring is empty and everything submitted has been appended;
                 * the caller verified durable == last before requesting. */
                if (ftruncate(W.fd, WAL_FILE_MAGIC_LEN) == 0) {
                    atomic_store(&W.st_bytes, 0);
                }
                atomic_store_explicit(&W.truncate_req, 0, memory_order_release);
                atomic_fetch_add_explicit(&W.truncate_gen, 1,
                                          memory_order_release);
            }
            struct timespec ts = {0, 50000}; /* 50us */
            nanosleep(&ts, NULL);
        }

        /* Segment rotation: safe at any batch boundary (ring drained above,
         * everything written so far fsynced when policy=always; for everysec
         * we fsync explicitly before the swap so the sealed segment is
         * complete). The sealed file keeps every record it ever had; the new
         * active file starts empty. Gated LSNs are unaffected: durable_lsn
         * only ever grows and fsync ordering is preserved across the swap. */
        if (atomic_load_explicit(&W.rotate_req, memory_order_acquire) &&
            atomic_load_explicit(&W.tail, memory_order_relaxed) ==
                atomic_load_explicit(&W.head, memory_order_acquire)) {
            if (atomic_load_explicit(&W.written_lsn, memory_order_relaxed) >
                atomic_load_explicit(&W.durable_lsn, memory_order_relaxed)) {
                if (fdatasync(W.fd) != 0) abort();
                uint64_t publish =
                    atomic_load_explicit(&W.written_lsn, memory_order_relaxed);
                atomic_store_explicit(&W.durable_lsn, publish,
                                      memory_order_release);
                char byte = 1;
                ssize_t wr = write(W.wakeup_pipe[1], &byte, 1);
                (void)wr;
            }
            uint64_t gen =
                atomic_load_explicit(&W.seg_gen, memory_order_relaxed);
            char seg[4200];
            snprintf(seg, sizeof(seg), "%s.seg.%llu", W.path,
                     (unsigned long long)gen);
            int ok = 0;
            if (rename(W.path, seg) == 0) {
                int nfd = open(W.path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (nfd >= 0 &&
                    write(nfd, WAL_FILE_MAGIC, WAL_FILE_MAGIC_LEN) ==
                        WAL_FILE_MAGIC_LEN) {
                    close(W.fd);
                    W.fd = nfd;
                    atomic_store(&W.st_bytes, 0);
                    atomic_store_explicit(&W.seg_gen, gen + 1,
                                          memory_order_relaxed);
                    ok = 1;
                } else {
                    /* Could not open a fresh active file: roll back the
                     * rename and keep appending to the old fd. */
                    if (nfd >= 0) close(nfd);
                    rename(seg, W.path);
                }
            }
            (void)ok;
            atomic_store_explicit(&W.rotate_req, 0, memory_order_release);
            atomic_fetch_add_explicit(&W.rotate_gen, 1, memory_order_release);
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
    snprintf(W.path, sizeof(W.path), "%s", path);
    atomic_store_explicit(&W.seg_gen, 1, memory_order_relaxed);
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


uint64_t walActiveBytes(void) {
    if (!W.open) return 0;
    return atomic_load_explicit(&W.st_bytes, memory_order_relaxed);
}

int walTruncateActive(void) {
    if (!W.open) return -1;
    uint64_t gen0 = atomic_load_explicit(&W.truncate_gen, memory_order_acquire);
    atomic_store_explicit(&W.truncate_req, 1, memory_order_release);
    for (int i = 0; i < 20000; i++) { /* <=1s */
        if (atomic_load_explicit(&W.truncate_gen, memory_order_acquire) != gen0)
            return 0;
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
    atomic_store_explicit(&W.truncate_req, 0, memory_order_release);
    return -1;
}

/* Ask the writer to seal the active file as <path>.seg.<gen> and start a
 * fresh one. Non-blocking: returns the generation that WILL be sealed, or 0
 * if a rotation is already pending. Poll walRotateGen() for completion. */
uint64_t walRequestRotate(void) {
    if (!W.open) return 0;
    if (atomic_load_explicit(&W.rotate_req, memory_order_acquire)) return 0;
    uint64_t gen = atomic_load_explicit(&W.seg_gen, memory_order_relaxed);
    atomic_store_explicit(&W.rotate_req, 1, memory_order_release);
    return gen;
}

uint64_t walRotateGen(void) {
    return atomic_load_explicit(&W.rotate_gen, memory_order_acquire);
}

/* Generation of the CURRENT active file. Records stamped with this value
 * land in this file or a later one (rotation only moves forward). */
uint64_t walActiveSegGen(void) {
    return atomic_load_explicit(&W.seg_gen, memory_order_relaxed);
}

/* ---------------------------------------------------------------------------
 * Boot replay (step 8). Streaming, framing-aware, torn-tail tolerant.
 * ---------------------------------------------------------------------------*/

typedef struct { uint32_t dbid, klen, vlen; char *body; uint8_t flags; } walReplayRec;

static int wal_replay_parse_item(const char *body, uint32_t body_len,
                                 uint32_t *dbid, uint32_t *klen, uint32_t *vlen) {
    if (body_len < 12) return -1;
    memcpy(dbid, body, 4); memcpy(klen, body + 4, 4); memcpy(vlen, body + 8, 4);
    if ((uint64_t)12 + *klen + *vlen != body_len) return -1;
    return 0;
}

long walReplayFile(const char *path, walReplayRecordFn cb, void *ctx) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return -1;
    char magic[WAL_FILE_MAGIC_LEN];
    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
        memcmp(magic, WAL_FILE_MAGIC, sizeof(magic)) != 0) {
        fclose(fp);
        return -1;
    }
    long applied = 0;
    /* Pending (uncommitted) group buffer */
    walReplayRec *grp = NULL;
    int grp_n = 0, grp_cap = 0;
    uint32_t grp_chain_crc = 0;
    uint64_t grp_seq = 0;
    int in_group = 0;

    for (;;) {
        unsigned char hdr[WAL_RECORD_HDR_SIZE];
        size_t got = fread(hdr, 1, sizeof(hdr), fp);
        if (got != sizeof(hdr)) break; /* clean EOF or torn header: stop */
        uint32_t rec_crc, body_len; uint64_t lsn; uint8_t kind, flags;
        memcpy(&rec_crc, hdr, 4); memcpy(&body_len, hdr + 4, 4);
        memcpy(&lsn, hdr + 8, 8); kind = hdr[16]; flags = hdr[17];
        (void)lsn;
        if (body_len > (256u << 20)) break; /* implausible: torn */
        char *body = malloc(body_len ? body_len : 1);
        if (body_len && fread(body, 1, body_len, fp) != body_len) {
            free(body);
            break; /* torn body */
        }
        uint32_t crc = walCrc32c(0, hdr + 4, sizeof(hdr) - 4);
        crc = walCrc32c(crc, body, body_len);
        if (crc != rec_crc) { free(body); break; } /* torn/corrupt: stop */

        if (kind == WAL_REC_STANDALONE) {
            if (in_group) { free(body); break; } /* framing violation */
            uint32_t dbid, klen, vlen;
            if (wal_replay_parse_item(body, body_len, &dbid, &klen, &vlen) != 0) {
                free(body);
                break;
            }
            cb(ctx, dbid, body + 12, klen, body + 12 + klen, vlen,
               (flags & WAL_RFLAG_TOMBSTONE) != 0);
            applied++;
            free(body);
        } else if (kind == WAL_REC_GROUP_BEGIN) {
            if (in_group || body_len != 8) { free(body); break; }
            memcpy(&grp_seq, body, 8);
            in_group = 1; grp_n = 0; grp_chain_crc = 0;
            free(body);
        } else if (kind == WAL_REC_IN_GROUP) {
            if (!in_group) { free(body); break; }
            uint32_t dbid, klen, vlen;
            if (wal_replay_parse_item(body, body_len, &dbid, &klen, &vlen) != 0) {
                free(body);
                break;
            }
            if (grp_n == grp_cap) {
                grp_cap = grp_cap ? grp_cap * 2 : 8;
                grp = realloc(grp, grp_cap * sizeof(*grp));
            }
            grp[grp_n].dbid = dbid; grp[grp_n].klen = klen;
            grp[grp_n].vlen = vlen; grp[grp_n].body = body;
            grp[grp_n].flags = flags;
            grp_n++;
            grp_chain_crc = walCrc32c(grp_chain_crc, &rec_crc, 4);
        } else if (kind == WAL_REC_GROUP_COMMIT) {
            if (!in_group || body_len != 16) { free(body); break; }
            uint64_t seq; uint32_t count, chain;
            memcpy(&seq, body, 8); memcpy(&count, body + 8, 4);
            memcpy(&chain, body + 12, 4);
            free(body);
            if (seq != grp_seq || (int)count != grp_n || chain != grp_chain_crc)
                break; /* torn group: drop whole, stop */
            for (int i = 0; i < grp_n; i++) {
                cb(ctx, grp[i].dbid, grp[i].body + 12, grp[i].klen,
                   grp[i].body + 12 + grp[i].klen, grp[i].vlen,
                   (grp[i].flags & WAL_RFLAG_TOMBSTONE) != 0);
                applied++;
                free(grp[i].body);
            }
            grp_n = 0; in_group = 0;
        } else {
            free(body);
            break; /* unknown kind: stop */
        }
    }
    /* Unterminated trailing group: drop whole (nothing past GROUP_BEGIN was
     * ever acked -- the ack stamp is the GROUP_COMMIT LSN). */
    for (int i = 0; i < grp_n; i++) free(grp[i].body);
    free(grp);
    fclose(fp);
    return applied;
}
