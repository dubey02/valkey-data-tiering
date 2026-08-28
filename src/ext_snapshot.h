/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Streaming snapshot for tiered values.
 *
 * The fork based path in ext_storage.c materializes each flash resident value
 * from the snapshot child with a synchronous pread. That is correct but costs a
 * full random read pass over the live flash set on every save. The streaming
 * path instead asks the storage engine for a point in time stream of its
 * records before the fork, so the values arrive without a read back.
 *
 * This header exposes only what rdb.c and debug.c need. The transport and the
 * sink live in ext_snapshot.c.
 */

#ifndef EXT_SNAPSHOT_H
#define EXT_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

/* Capability probe. Returns 1 when tiering is on AND the active storage engine
 * implements the streaming ops. Callers must fall back to the fork based path
 * when this returns 0. */
int extSnapshotStreamSupported(void);

/* ---------------------------------------------------------------------------
 * Self test (tests only)
 *
 * Drives storageSnapshotStreamStart() with a counting sink and blocks until the
 * stream completes or the timeout expires. This exists so the streaming
 * contract can be verified against a real storage engine before the RDB
 * plumbing is wired, and so a regression can be attributed to the engine rather
 * than to rdb.c.
 * ---------------------------------------------------------------------------*/
typedef struct extSnapshotSelfTestResult {
    int started;         /* the engine accepted the start call */
    int completed;       /* complete() fired before the timeout */
    int ok;              /* complete(ok) argument */
    long long records;   /* records delivered */
    long long key_bytes;
    long long value_bytes;
    uint64_t digest;     /* order independent digest over (dbid, key, vlen) */
    long long elapsed_ms;
} extSnapshotSelfTestResult;

int extSnapshotStreamSelfTest(int timeout_ms, extSnapshotSelfTestResult *out);

/* ---------------------------------------------------------------------------
 * Phase 2: record transport
 *
 * The storage engine delivers records on ITS IO thread, but the RDB writer is
 * the fork child. A pipe carries records across that boundary: the parent's IO
 * thread encodes each record as a frame and writes it, the child decodes and
 * hands records to the RDB layer.
 *
 * Frame layout (host byte order: producer and consumer are the same build on
 * the same machine, so there is nothing to normalise):
 *
 *   record      : u32 payload_len | u32 dbid | u32 klen | u32 vlen | key | value
 *   terminator  : u32 0           | u64 record_count | u64 digest
 *   poison      : u32 0xFFFFFFFF
 *
 * The terminator carries a count and digest so the consumer can prove it saw
 * every record rather than assuming a clean EOF. Poison marks producer side
 * failure, so a truncated stream is never mistaken for a complete one.
 *
 * Backpressure: the write end is non-blocking with a bounded overflow buffer.
 * Blocking the engine IO thread would also stall spills and fetches, so
 * instead writable() reports "not now" while overflow is pending and the
 * engine simply stops handing over records. Overflow past the cap poisons the
 * stream, matching the storage engine's own abort-rather-than-stall policy.
 * ---------------------------------------------------------------------------*/

/* Arm the transport and the engine stream. Main thread, and it must run in the
 * same event-loop tick as the fork so no mutation lands between cut and fork. */
int extSnapshotTransportArm(void);

/* Consumer end. Valid after Arm until Release. */
int extSnapshotTransportReadFd(void);

/* Post fork fd hygiene: each side drops the end it does not use. */
void extSnapshotTransportCloseReadEnd(void);
void extSnapshotTransportCloseWriteEnd(void);

/* Cancel the engine stream and poison the pipe so the consumer fails loudly
 * instead of treating a partial stream as complete. */
void extSnapshotTransportAbort(void);

/* Tear down. Safe to call whether or not Arm succeeded. */
void extSnapshotTransportRelease(void);

/* One record, as decoded by the consumer. */
typedef void (*extSnapshotRecordFn)(void *privdata, uint32_t db_id,
                                    const char *key, size_t klen,
                                    const char *value, size_t vlen);

/* Drain outcomes. Anything negative is terminal. */
#define EXT_SNAP_DRAIN_WOULDBLOCK   0   /* nothing available right now */
#define EXT_SNAP_DRAIN_DONE        -1   /* terminator seen, counts verified */
#define EXT_SNAP_DRAIN_ERR         -2   /* poison, short read, or count mismatch */

/* Consume up to `budget` records. Returns the number consumed (>0), or one of
 * the EXT_SNAP_DRAIN_* codes. With blocking=0 this returns WOULDBLOCK rather
 * than waiting, so the caller can interleave it with other work. */
int extSnapshotTransportDrain(extSnapshotRecordFn fn, void *privdata,
                              int budget, int blocking);

/* Records handed to the pipe so far. Producer side counter, for INFO. */
long long extSnapshotTransportRecordsSent(void);

/* ---------------------------------------------------------------------------
 * Resurrection barrier
 *
 * A key deleted before the cut whose flash record has not been reclaimed yet
 * would still be inside the frozen range, and writing it to an RDB would bring
 * the key back. Draining in-flight storage IO before arming the cut removes
 * that window. Returns C_OK when the queues settled, C_ERR otherwise (caller
 * must then refuse the snapshot).
 * ---------------------------------------------------------------------------*/
int extSnapshotDrainBarrier(void);

/* ---------------------------------------------------------------------------
 * Transport self test (tests only)
 *
 * Runs the whole producer/consumer path in one process: barrier, arm, then
 * drain from the calling thread while the engine IO thread produces. This is
 * the same interleave the fork child performs, minus the fork, so the framing
 * and backpressure can be verified before rdb.c is involved.
 * ---------------------------------------------------------------------------*/
typedef struct extSnapshotTransportTestResult {
    int armed;
    int ok;                /* terminator seen and counts verified */
    long long sent;
    long long received;
    long long drain_calls;
    long long wouldblocks; /* times the consumer found nothing ready */
    long long orphans;     /* records rejected by the orphan filter */
    long long elapsed_ms;
} extSnapshotTransportTestResult;

int extSnapshotTransportSelfTest(int timeout_ms, extSnapshotTransportTestResult *out);

/* ---------------------------------------------------------------------------
 * Orphan filter (Phase 3 correctness gate)
 *
 * The storage engine enumerates ITS OWN store, not the engine keyspace. A
 * record whose key the engine has already forgotten stays in the store until
 * its space is reclaimed, so streaming it into an RDB would resurrect a
 * deleted key on load. The fork based path cannot hit this, because it looks
 * each key up in the hashtable and only then reads flash. The streaming path
 * arrives from the opposite direction and has to reject explicitly.
 *
 * Returns 1 when the record should be written, 0 when it must be dropped.
 * Mirrors the two conditions extStorageMaterializeTiered already honours: a
 * key pending deletion is logically gone, and a key that is no longer flash
 * resident has been superseded in memory.
 *
 * Consumer side. In a real save this runs in the fork child, where the
 * hashtable is the copy on write snapshot taken at the cut, which is the state
 * the records were frozen against.
 * ---------------------------------------------------------------------------*/
int extSnapshotRecordIsLive(uint32_t physical_db_id, const char *key, size_t klen);

/* Records rejected by the orphan filter. Exposed so tests can assert the
 * filter actually engaged rather than passing vacuously. */
long long extSnapshotOrphansDropped(void);

#endif /* EXT_SNAPSHOT_H */
