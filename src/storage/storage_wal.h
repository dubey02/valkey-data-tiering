/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Client-ack WAL (fast-boot Phase 3, steps 6-7).
 *
 * Contract: a client-visible reply to a write implies the write is durably
 * committed. The main thread builds one framed record blob per execution
 * unit (an EXEC, a script invocation, or a single command) and pushes it to
 * a dedicated WAL writer thread, which appends and group-commits with one
 * fdatasync per drain cycle, then publishes the durable LSN. Reply release
 * is gated on the durable LSN in networking.c.
 *
 * The writer is a DEDICATED thread (not the FC io thread as the design doc
 * first sketched): an fdatasync stall must never queue behind -- or ahead
 * of -- cold-fetch reads on the FC ring, and the WAL has no ordering
 * dependency on the main log (retirement in step 8 is offset-based).
 *
 * On-disk format
 *   file header : "FCWAL001" (8 bytes)
 *   record      : { u32 crc32c; u32 body_len; u64 lsn; u8 kind; u8 flags;
 *                   u16 reserved; } (20 bytes) + body
 *                 crc covers header-after-crc + body.
 *   item body   : { u32 dbid; u32 klen; u32 vlen; key; value }
 *                 vlen == 0 && (flags & WAL_RFLAG_TOMBSTONE) => delete.
 *   GROUP_BEGIN body : { u64 group_seq }
 *   GROUP_COMMIT body: { u64 group_seq; u32 record_count; u32 group_crc }
 *                 group_crc = crc32c chained over member record CRCs.
 *
 * Framing rules (see docs/fast-boot-durable-index.md):
 *   - a unit emitting exactly one record is STANDALONE (zero overhead);
 *   - >1 record => GROUP_BEGIN + IN_GROUP... + GROUP_COMMIT, contiguous by
 *     construction (single main thread builds the whole blob, writer
 *     appends blobs whole);
 *   - the ack stamp is the GROUP_COMMIT record's LSN;
 *   - replay (step 8) applies STANDALONE immediately, buffers IN_GROUP
 *     until a matching GROUP_COMMIT, truncates at an unterminated group.
 */

#ifndef STORAGE_WAL_H
#define STORAGE_WAL_H

#include <stddef.h>
#include <stdint.h>

#define WAL_FILE_MAGIC "FCWAL001"
#define WAL_FILE_MAGIC_LEN 8
#define WAL_RECORD_HDR_SIZE 20

/* Record kinds */
#define WAL_REC_STANDALONE 1
#define WAL_REC_GROUP_BEGIN 2
#define WAL_REC_IN_GROUP 3
#define WAL_REC_GROUP_COMMIT 4

/* Record flags */
#define WAL_RFLAG_TOMBSTONE 0x01

/* fsync policy */
#define WAL_FSYNC_ALWAYS 0
#define WAL_FSYNC_EVERYSEC 1

/* ---- Blob builder (main thread) ----
 * Accumulates the records of one execution unit, then finalizes into a
 * single contiguous byte buffer with framing applied. */
typedef struct walBlobBuilder walBlobBuilder;

walBlobBuilder *walBlobNew(void);
void walBlobAddItem(walBlobBuilder *b, uint32_t dbid, const void *key,
                    size_t klen, const void *val, size_t vlen, int tombstone);
/* Assigns LSNs, applies framing, and hands the buffer to the caller.
 * Returns the commit LSN (LSN of the last record: the item for STANDALONE,
 * GROUP_COMMIT for a group); 0 if the blob is empty. *buf is malloc'd and
 * ownership passes to walSubmit (freed by the writer thread). */
uint64_t walBlobFinalize(walBlobBuilder *b, char **buf, size_t *len);
void walBlobFree(walBlobBuilder *b);

/* ---- Writer (dedicated thread) ---- */
/* Opens <path>, starts the writer thread. wakeup_fd_out receives the read
 * end of a pipe: one byte is written whenever the durable LSN advances, so
 * the event loop wakes to release gated replies. fsync_policy is
 * WAL_FSYNC_*. Returns 0 on success. */
int walOpen(const char *path, int fsync_policy, int *wakeup_fd_out);
/* Push a finalized blob (ownership transferred). Spins briefly on a full
 * ring: ring-full backpressure is ack latency by design. max_lsn is the
 * blob's commit LSN. Returns 0. */
int walSubmit(char *buf, size_t len, uint64_t max_lsn);
/* Highest LSN known durable (fsync'd). Atomic read; 0 before any commit. */
uint64_t walDurableLsn(void);
/* Highest LSN assigned (for INFO). */
uint64_t walLastAssignedLsn(void);
/* Runtime fsync-policy switch (MODIFIABLE config). */
void walSetFsyncPolicy(int fsync_policy);
/* Drain + fsync + stop the writer thread, close fds. */
void walClose(void);
int walIsOpen(void);

/* ---- Boot replay + retirement (step 8) ---- */
/* Called per surviving record, in file order (file order == LSN order, so
 * applying sequentially is last-write-wins). Framing-aware: STANDALONE
 * applies immediately, group members only once their GROUP_COMMIT validates;
 * a torn tail or torn group truncates the rest. */
typedef void (*walReplayRecordFn)(void *ctx, uint32_t dbid, const void *key,
                                  size_t klen, const void *val, size_t vlen,
                                  int tombstone);
/* Standalone (writer need not be open). Returns applied record count, or -1
 * if the file cannot be opened / has a bad magic. */
long walReplayFile(const char *path, walReplayRecordFn cb, void *ctx);
/* Truncate the ACTIVE WAL back to the file header at a writer-safe point
 * (ring empty, durable == last). Caller must guarantee all current content
 * is durable elsewhere. Returns 0 on success, -1 on timeout. */
int walTruncateActive(void);
/* Bytes appended to the active WAL since open (reset by truncate). */
uint64_t walActiveBytes(void);

/* Stats (INFO) */
typedef struct walStats {
    uint64_t records, groups, units, bytes, fsyncs, ring_full_stalls;
} walStats;
void walGetStats(walStats *out);

/* crc32c (software, Castagnoli) -- exposed for the dump/validator tool. */
uint32_t walCrc32c(uint32_t crc, const void *buf, size_t len);

#endif /* STORAGE_WAL_H */
