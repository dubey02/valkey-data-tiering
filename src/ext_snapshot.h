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

#endif /* EXT_SNAPSHOT_H */
