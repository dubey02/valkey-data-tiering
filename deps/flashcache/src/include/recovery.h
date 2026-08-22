#ifndef __FLASHCACHE_RECOVERY_H
#define __FLASHCACHE_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Clean-shutdown superblock + log-scan recovery (fast boot, Phase 1 POC).
 *
 * Protocol:
 *  - On clean shutdown, AFTER logFsyncBufferedWrites() has drained the staging
 *    buffer and all in-flight log flushes, logWriteSuperblock() persists the
 *    circular-log window {head_offset, tail_offset} plus item/db counts to a
 *    small sidecar file. Without this window a cold scan cannot order entries
 *    (the log is circular; "later in file" != "written later").
 *  - On the next boot, AFTER flashcacheInit(), logRecoverFromLog() validates
 *    the superblock, restores head/tail, scans the active window tail->head in
 *    write order, deduplicates keys last-write-wins (the engine's overwrite
 *    protocol appends a new copy and index-deletes the old one, leaving stale
 *    copies in the log), rebuilds the in-memory index, and invokes the item
 *    callback once per live item so the hosting engine can rebuild keyspace
 *    metadata without reading values.
 *  - The superblock is unlinked as soon as recovery starts consuming it, so a
 *    later crash can never resurrect a stale window.
 *
 * Known POC limitations (by design, see engine-side docs):
 *  - deletes are index-only (no log tombstones): deleted-but-uncompacted keys
 *    are resurrected. Test workloads must avoid deletes.
 *  - TTLs are not stored in the log.
 * ---------------------------------------------------------------------------*/

struct flashcacheLog;

/* Per-live-item callback fed during recovery finalize.
 * value_first_byte is the first byte of the stored value payload (the hosting
 * engine's DUMP-format type byte); the value itself is NOT materialized. */
typedef void (*flashcacheRecoveryItemCallback)(void *ctx, uint32_t dbid,
        char const *key, size_t key_len, uint8_t value_first_byte,
        size_t value_len);

typedef struct flashcacheRecoveryStats {
    size_t items_scanned;   /* every kv record seen in the active window   */
    size_t items_live;      /* survivors after last-write-wins dedup       */
    size_t items_stale;     /* overwritten copies discarded by dedup       */
    size_t bytes_scanned;   /* active-window bytes walked                  */
    uint64_t scan_us;       /* wall time: scan + dedup                     */
    uint64_t finalize_us;   /* wall time: index rebuild + callbacks        */
} flashcacheRecoveryStats;

/* Write the clean-shutdown superblock sidecar. Caller must have already
 * drained the staging buffer (logFsyncBufferedWrites). Returns 0 on success,
 * -1 on IO error or if the staging buffer is not empty. */
int logWriteSuperblock(struct flashcacheLog *log, char const *superblock_filename);

/* ---------------------------------------------------------------------------
 * Index reflection (fast boot without a log scan).
 *
 * At clean shutdown, AFTER the staging buffer is drained, logWriteIndexFile()
 * serializes the in-memory index of every database (bucket geometry + the
 * 8-byte logEntry per item) plus the SipHash seed and the allocated-bytes
 * accounting to a sidecar file. Bucket indices and collision hashes are
 * seed-dependent, so the seed MUST travel with the index.
 *
 * On the next boot, logRecoverFromIndexFile() restores the hasher seed, the
 * log window (from the superblock), the exact index geometry, and all chains
 * — no log scan, no key bytes touched. Because the index stores hashes only
 * (no keys), this path can NOT feed keyspace entries to the engine: it is
 * only usable when the engine runs in key-spilling mode (implicit keys,
 * dict-miss consult). The per-db item counts are reported via counts_cb.
 * Both sidecar files are consumed (unlinked) at the start of recovery.
 * ---------------------------------------------------------------------------*/

/* Per-db live-item count callback for index-file recovery. */
typedef void (*flashcacheRecoveryCountsCallback)(void *ctx, uint32_t dbid, size_t count);

/* Serialize the index to the sidecar. Pumps any in-progress incremental
 * index growth to completion first (bucket indices must reflect one stable
 * geometry). Returns 0 on success, -1 on failure. */
int logWriteIndexFile(struct flashcacheLog *log, char const *index_filename);

/* Restore the index from the sidecar written at the previous clean shutdown.
 * Must run after logCreate and before any traffic. Requires a valid matching
 * superblock (for the log window). Returns 0 on success; -1 when either file
 * is missing/invalid (caller falls back to log-scan recovery or cold start —
 * the log offsets are left reset in that case). */
int logRecoverFromIndexFile(struct flashcacheLog *log,
        char const *superblock_filename, char const *index_filename,
        flashcacheRecoveryCountsCallback counts_cb, void *counts_cb_ctx);

/* Recover the index (and feed the engine) from an existing log using the
 * superblock. Must run after logCreate and before any write traffic.
 * Returns 0 on success; -1 if the superblock is missing/invalid or the scan
 * hits corruption (caller should fall back to a normal cold start — the log
 * offsets are left reset in that case). */
int logRecoverFromLog(struct flashcacheLog *log, char const *superblock_filename,
        flashcacheRecoveryItemCallback item_cb, void *item_cb_ctx,
        flashcacheRecoveryStats *stats);

#endif /* __FLASHCACHE_RECOVERY_H */
