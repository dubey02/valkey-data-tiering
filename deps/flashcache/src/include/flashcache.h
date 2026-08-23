#ifndef __FLASHCACHE_H
#define __FLASHCACHE_H

#include <stdlib.h>

#include "include/flashcache_common.h"

/*!\brief Initializes flashcache store
 *
 * @param db_filename the name of the file used to store data on flash
 * @param db_size_bytes the size of file used to store data on flash
 * @param initial_index_size_per_db the initial size of in-memory dictionary used to store the index of a database
 * @param num_databases number of databases
 * @param max_allocated_db_size_percent maximum allowed allocated log size as a percentage of db_size_bytes.
 *        When the allocated db size becomes greater than this threshold, keys are evicted from the db.
 * @param max_num_in_flight_read_requests The maximum allowed number of in-flight READ item requests in the queue
 *        before the read requests start to get throttled
 * param evict_under_max_logsize_time_limit longest amount of time that eviction under max logsize can
 *       run without signal from redis
 * @param hash_function function pointer used for hashing keys
 * @param crc_function function pointer used for computing checksum of data stored on flash
 * @param eviction_details the details of callback called when a key is evicted
 * @param logger log function to be used by this library
 * @param asio_control_msg_callback_details details of the ASIO callback to handle control messages
 *        in a timely fashion.
 */
flashcacheReturnCode flashcacheInit(char const *db_filename,
        size_t db_size_bytes,
        size_t initial_index_size_per_db,
        uint32_t num_databases,
        uint32_t max_allocated_db_size_percent,
        uint32_t max_num_in_flight_read_requests,
        uint32_t min_garbage_collection_rate,
        uint32_t evict_under_max_logsize_time_limit,
        uint8_t optimized_delete_enabled,
        flashcache_monotonic_clock_us monotonic_clock_us,
        flashcacheEvictionDetails *eviction_details,
        flashcache_logger logger,
        flashcacheAsioControlMsgCallbackDetails *asio_control_msg_callback_details);

/*!\brief Stores the provided value against the provided key and db id
 *
 * The key and value is copied internally. The caller can free them
 * immediately after calling this API.
 */
flashcacheReturnCode flashcachePutItem(uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len);

/*!\brief Retrieve the value against the provided key and db id
 *
 * The key is copied internally. The caller can free them immediately
 * after calling this API. This is an asynchronous API. Once the value is
 * retrieved from flash, the provided callback is called with the
 * retrieved value. If a value is not found, the provided callback is called
 * with NULL as value.
 */
flashcacheReturnCode flashcacheGetItem(uint32_t dbid, char const *key, size_t key_len,
        flashcacheReadTypes read_type, void *request_context, flashcache_get_item_callback completion_callback);

/*!\brief Starts taking a point in time snapshot in a snapshot file
 *
 * This function starts taking a point in time snapshot of the store. All changes performed by the read (which
 * deletes item) and write request before this call is present in the snapshot. The snapshot is stored in the
 * specified snapshot file. Once the snapshot file is created, the completion callback is invoked.
 * @Returns : Void
 * @param snapshot_filename : Name of a file in which snapshot will be captured
 * @param snapshot_secret : Snapshot Secret which will be used for preparing rdb/fdb correlation secret.
 * @param completion_callback_details : Callback function which will be called after completion of file based snapshot.
 * @param checksum_verification_enabled : A flag for whether checksum verification for the whole snapshot is needed.
 * @param snapshot_version : The version of the snapshot to use in the current save attempt.
 */
void flashcacheStartFileBasedSave(char const *snapshot_filename,
        flashcacheSnapshotSecret *snapshot_secret,
        flashcacheSnapshotCallbackDetails *completion_callback_details,
        int checksum_verification_enabled,
        flashcacheSnapshotVersion snapshot_version,
        flashcacheSnapshotSaveType snapshot_save_type);

/*!\brief Cancels any ongoing snapshot */
void flashcacheCancelSave();

/*!\brief Load the snapshot from the specified file. The current data in the store is removed during loading the
 * snapshot
 * @param snapshot_filename: name of the file to load snapshot from.
 * @param secret_response: an output parameter to return the secret in the snapshot to the caller.
 * @param checksum_comparison_result: an output parameter to return the result of checksum verification to the caller.
 * */
void flashcacheLoadSnapshot(char const *snapshot_filename,
                            flashcacheSnapshotSecret *secret_response,
                            int *checksum_comparison_result);

/*!\brief Runs cron tasks like garbage collection.
 *
 * This API also processes the pending get data requests that are waiting
 * on the data being fetched from flash. The expectation from the caller
 * to invoke this API after every GetItem or PutItem request.
 */
/* Snapshot support: pause/resume the garbage collector. While paused,
 * on-flash item offsets are stable (writes still append at the log tail),
 * making it safe for a fork()ed snapshot child to pread frozen offsets. */
void flashcacheSetGcPaused(int paused);
int flashcacheGetGcPaused(void);

/* Snapshot support: synchronous, fork-child-safe single-item read. Walks the
 * (CoW) index and reads with pread(2) -- never touches the async IO ring.
 * Returns FC_OK with a malloc'd *out_item (header+key+value; caller frees),
 * or FC_ERR_CATCH_ALL. */
flashcacheReturnCode flashcacheForkChildReadItem(uint32_t dbid, char const *key,
        size_t key_len, char **out_item, size_t *out_len);

flashcacheReturnCode flashcacheRunCronTasks();

/*!\brief Returns the value associated with the specified metric */
size_t flashcacheGetCountBasedMetric(flashcacheCountBasedMetrics metric);

/*!\brief Fetches histogram for the specified histogram metric */
void flashcacheGetHistogramMetrics(flashcacheHistogramMetrics metric,
        unsigned long long histogram[], const size_t histogram_size);

/*!\brief Fetches histogram interval for the specified histogram metric */
void flashcacheGetHistogramIntervals(flashcacheHistogramMetrics metric,
        flashcacheHistogramInterval histogram_interval[], const size_t interval_size);

/*!\brief Tear down the current instance of flashcache. */
flashcacheReturnCode flashcacheTearDown();

/*!\brief Delete all keys for a given db id on flashcache layer
 *
 * This API works synchronously so the caller will be blocked while
 * all pending reads complete, pending snapshots get cancelled.
 */
flashcacheReturnCode flashcacheFlushDB(uint32_t dbid);

/*!\brief Delete all keys for all DBs
 *
 * This API works synchronously so the caller will be blocked while
 * all pending reads complete, pending snapshots get cancelled.
 */
flashcacheReturnCode flashcacheFlushAllDBs();

/*!\brief Returns 1 if there are pending cron tasks that needs to be processed immediately else returns 0
 *
 * This API helps the caller know that there are no more pending cron task left at the moment. when this API return
 * 0, the caller can choose to sleep for a small amount of time. This helps the caller to avoid spinning continuously
 * when flashcache do not have any pending cron task. The recommended amount to sleep is <= 50us as a longer sleep
 * will delay starting garbage collection in the database.
 */
int flashcacheShouldRunCronTasksImmediately();

/**!\brief Starts taking a point in time snapshot using snapshot writer
 *
 * This API starts taking a point in time stream based snapshot of the store. It uses snapshot writer to write
 * snapshot chunks. This is primarily used for replication.
 * @Returns : Void
 * @param snapshot_secret : Snapshot Secret which will be used for preparing rdb/fdb correlation secret.
 * @param snapshot_writer : Pointer of a flashcacheSnapshotWriter which contains bunch of APIs for stream based snapshot.
 * @param snapshot_version : The version of the snapshot to use in the current save attempt.
 * @param log_iteration_completion_callback_details : Callback which needs to be called after completion of log
 *                          iteration in Threadsave replication. Note: we do shallow copy of this callback currently
 *                          but deep copy might be required if we change it in future by adding any allocated memory.
 */
void flashcacheStartStreamBasedSave(flashcacheSnapshotSecret *snapshot_secret,
                                    flashcacheSnapshotWriter *snapshot_writer,
                                    flashcacheSnapshotVersion snapshot_version,
                                    flashcacheSnapshotSaveType snapshot_save_type,
                                    flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details);

/**!\brief Set the specified config
 *
 * @Returns : Void
 * @param :
 * config: The details of the config to set.
 */
void flashcacheSetConfig(flashcacheConfig *config);

/**!\brief Get the value for the specified key in the configuration. The retrieved value is set in the provided config
 * itself.
 *
 * @Returns : Void
 * @param :
 * config: The details of the config to get.
 */
void flashcacheGetConfig(flashcacheConfig *config);

/**!\brief Flush buffered writes to disk
 *
 * @Returns : Void
 */
void flashcacheFsyncBufferedWrites();

/* ---------------------------------------------------------------------------
 * Clean-shutdown superblock + log-scan recovery (fast boot).
 * See include/recovery.h for the protocol and limitations.
 * ---------------------------------------------------------------------------*/
typedef void (*flashcacheRecoveryItemCallback)(void *ctx, uint32_t dbid,
        char const *key, size_t key_len, uint8_t value_first_byte,
        size_t value_len);

typedef struct flashcacheRecoveryStats flashcacheRecoveryStats;

/*!\brief Persist the clean-shutdown superblock. Call after
 * flashcacheFsyncBufferedWrites() and before flashcacheTearDown().
 * Returns 0 on success, -1 on failure. */
int flashcacheWriteSuperblock(char const *superblock_filename);

/*!\brief Rebuild the index (and feed the engine one callback per live item)
 * by scanning the existing log, using the superblock written at the previous
 * clean shutdown. Must be called after flashcacheInit() and before any
 * traffic. The superblock is consumed (unlinked) by this call. Returns 0 on
 * success; -1 when no valid superblock exists or the scan failed (the store
 * is then in normal cold-start state). */
int flashcacheRecoverFromLog(char const *superblock_filename,
        flashcacheRecoveryItemCallback item_cb, void *item_cb_ctx);

/*!\brief Serialize the in-memory index to a sidecar file (call after
 * flashcacheFsyncBufferedWrites, alongside the superblock). Returns 0/-1. */
int flashcacheWriteIndexFile(char const *index_filename);

/* Per-db live-item count callback for index-file recovery. */
typedef void (*flashcacheRecoveryCountsCallback)(void *ctx, uint32_t dbid, size_t count);

/*!\brief Restore the index directly from the sidecar written at the previous
 * clean shutdown — no log scan, no key bytes. Only usable when the hosting
 * engine tracks keys implicitly (key-spilling): the index holds hashes, not
 * keys. Consumes both sidecar files. Returns 0 on success, -1 to fall back. */
int flashcacheRecoverFromIndexFile(char const *superblock_filename,
        char const *index_filename,
        flashcacheRecoveryCountsCallback counts_cb, void *counts_cb_ctx);

/* Fast-boot durability (crash-safe recovery, steps 1+2):
 *  - SetFastBootDurability(1) turns on delete tombstones in the log and
 *    head-journal appends at every staging-flush completion.
 *  - HeadJournalConfigure sets/opens the journal sidecar (<flash>.headj);
 *    call BEFORE recovery so a crashed previous run's window can be read.
 *  - HeadJournalReset voids all prior records and stamps the current window;
 *    call AFTER recovery (any outcome) so stale windows are never replayed.
 * With these enabled, flashcacheRecoverFromLog also recovers after a crash
 * (no superblock) using the journal's durable window. */
void flashcacheSetFastBootDurability(int enabled);
int flashcacheHeadJournalConfigure(char const *headj_filename);
void flashcacheHeadJournalReset();

/**!\brief Notifies Redis layer Snapshot completion
 *
 * @Returns : Void
 */
void flashcacheNotifyRedisLayerSnapshotCompletion();


/**!\brief Invokes the snapshot export process for FDB
 *
 * @Returns : 0 for success; -1 for failure
 * @param source_fdb_filename : the name of source fdb snapshot file
 * @param target_rdb_filename : filename to which the data will be written into
 * @param metadata : Contains RDB secret, running checksum, the CRC64 checksum function callback,
 *                   and the callback to obtain the TTL and the customer DB ID for a given key
 */
int flashcacheStartSnapshotExport(const char *source_fdb_filename,
                                   const char *target_rdb_filename,
                                   flashcacheSnapshotExportMetadata *metadata);

/*!\brief Check if a key exists in the FlashCache index (no disk I/O). */
int flashcacheKeyExists(uint32_t dbid, char const *key, size_t key_len);

#endif  // __FLASHCACHE_H
