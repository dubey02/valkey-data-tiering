#include <cstdio>
#include <vector>
#include <tuple>

#include "clock_mock.hpp"
#include "flashcache_test_base.hpp"
#include "snapshot_writer_mock.hpp"

#define MEGABYTE_TO_BYTES     (1LL << 20)
#define NUM_DATABASES         (600)
#define LOG_SIZE_BYTES        (100LL * 1024 * 1024)
#define MAX_DYNAMIC_GC_RATE   (30 * 1024 * 1024)

extern "C" {
#include "include/util.h"
#include "include/log.h"
#include "include/snapshot_manager.h"
#include "include/snapshot_version_one.h"
#include "include/snapshot_version_two.h"
#include "include/serialization.h"

    extern void resetGarbageCollectionTimeWindowIfRequired(uint64_t current_time_second, garbageCollectorInfo *gc_info);
    extern size_t computeGarbageCollectionBlockSizeInBytes(flashcacheLog *log);
    extern void updateGarbageCollectionRate(flashcacheLog *log);
    extern void updateWriteRateInfo(uint64_t current_time_second, writeRateInfo *info, size_t item_len);
    size_t indexTableSize(flashcacheIndex *index);
    snapshotManagerInfo *getSnapshotManagerInfo();
    extern flashcacheSnapshotConfig flashcache_snapshot_config;
    extern snapshotMetrics snapshot_metrics;
    extern void snapshotThrottleLoadWrite(unsigned int pause_duration);
    extern void snapshotUnthrottleLoadWrite();
    void adjustSnapshotVersion(flashcacheSnapshotVersion new_snapshot_version);
    void waitTillNoPendingIoAndGarbageCollection(flashcacheLog *log, int is_empty_staging_buffer_required);
    extern int collisionHashCount(flashcacheLog *log, uint32_t dbid, char *key, size_t key_len);
    extern int shouldResubmitRequestForExpedition(size_t log_offset, int requested_item_with_value);
    extern int needToPerformEviction(flashcacheLog *log);
}

static inline uint64_t single_bucket_hash_function(char const *key, size_t key_len) {
    (void(key));
    (void(key_len));

    return 17;
}

static inline void get_empty_item_callback(void *request_context, char *value,
        size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb;
    ASSERT_NE(request_context, nullptr);
    ASSERT_EQ(value, nullptr);
    ASSERT_EQ(value_len, 0);
    requestContext *context = static_cast<requestContext *>(request_context);
    context->num_callback_triggered++;
}

static void addDbidToKey(char *key, uint32_t dbid) {
    int i = 0;
    while (dbid) {
        key[i++] = ((dbid % 10) + '0');
        dbid /= 10;
    }
    key[i] = '-';
}

static uint32_t getDbidFromKey(char *key) {
    int i = 0;
    uint32_t dbid = 0;
    uint32_t tens = 1;
    while (key[i] != '-') {
        dbid = dbid + (key[i++] - '0') * tens;
        tens *= 10;
    }
    return dbid;
}

class LogTest : public flashcache::FlashcacheTestBase, public testing::TestWithParam<
                std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, flashcache_hash_function, size_t, bool,
                bool, ssize_t, flashcacheSnapshotVersion, flashcacheSnapshotSaveType, flashcacheReadTypes>> {
 public:
    flashcacheLog *log;
    char log_filename[FILENAME_MAX] = "log.txt";
    char snapshot_filename[FILENAME_MAX] = "snapshot.txt";
    evictionContext eviction_context;
    uint32_t num_databases;
    flashcacheSnapshotWriter *snapshot_writer = NULL;
    uint32_t max_allocated_log_size_percent = 50;
    uint32_t max_num_in_flight_read_requests = 128;
    uint32_t min_garbage_collection_rate = 4096;
    uint32_t evict_under_max_logsize_time_limit = 100 * 1000;
    uint8_t optimized_delete_enabled = 1;
    size_t log_size_bytes;
    flashcacheSnapshotVersion flashcache_snapshot_version;
    flashcacheSnapshotSaveType flashcache_snapshot_save_type;
    flashcacheReadTypes read_type = std::get<13>(GetParam());
    flashcache_get_item_callback completion_callback =
        (read_type == FC_READ ? get_item_callback : delete_item_callback);

    void setHashFunction(flashcacheLog *log, flashcache_hash_function hash_function) {
        if (hash_function == nullptr) {
            return;
        }

        log->hasher.hash_function = hash_function;
        for (size_t i = 0; i < log->num_databases; ++i) {
            log->index_list[i]->hash_function = hash_function;
        }
    }

    void setSnapshotWriter(bool should_set_snapshot_writer,
            const char *filename) {
        if (should_set_snapshot_writer) {
            snapshot_writer = getMockSnapshotWriter();
            initializeSnapshotWriterMock(filename);
        }
    }

    void setFlashCacheSnapshotSaveType(flashcacheSnapshotSaveType snapshot_save_type) {
        flashcache_snapshot_save_type = snapshot_save_type;
    }

    void initializeLog(size_t log_size_bytes_, uint32_t num_databases_) {
        flashcache::FlashcacheTestBase::SetUp();
        num_databases = num_databases_;
        log_size_bytes = log_size_bytes_;

        enableMockFio(std::get<7>(GetParam()), std::get<8>(GetParam()), "");
        flashcacheEvictionDetails eviction_details = {0};
        eviction_details.context = &eviction_context;
        eviction_details.callback = eviction_callback;
        mockFioSetLogSize(log_size_bytes);
        flashcache_snapshot_version = std::get<11>(GetParam());
        flashcache_snapshot_save_type = std::get<12>(GetParam());
        ASSERT_EQ(logCreate(&log, log_filename, log_size_bytes, 128, num_databases,
                    FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD, max_allocated_log_size_percent,
                    max_num_in_flight_read_requests, min_garbage_collection_rate,
                    evict_under_max_logsize_time_limit, optimized_delete_enabled,
                    flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER),
                    mockClockGetTimeUs, &eviction_details), FC_OK);

        log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 0;
        setKeyLen(std::get<0>(GetParam()));
        setValueLen(std::get<1>(GetParam()));
        log->garbage_collector_info.required_garbage_collection_bytes_per_second = std::get<2>(GetParam());
        log->staging_buffer_flush_size_threshold_bytes = std::get<3>(GetParam());
        log->head_offset = log->tail_offset = std::get<4>(GetParam());
        getSnapshotManagerInfo()->snapshot_version_one_info->serialized_index_buffer_len = std::get<5>(GetParam());
        setSnapshotWriter(std::get<9>(GetParam()), snapshot_filename);
        setHashFunction(log, std::get<6>(GetParam()));

        flashcacheConfig config = {};
        config.key = FC_CONFIG_KEY_MAX_SNAPSHOT_BUFFER_SIZE_BYTES;
        config.numeric_value = std::get<10>(GetParam());
        logSetConfig(log, &config);
        config.numeric_value = 0;
        logGetConfig(log, &config);
        ASSERT_EQ(config.numeric_value, std::get<10>(GetParam()));
    }

    void SetUp() {
        initializeLog(LOG_SIZE_BYTES, NUM_DATABASES);
    }

    void TearDown() {
        flashcache::FlashcacheTestBase::TearDown();
        releaseMockSnapshotWriter(snapshot_writer);
        snapshot_writer = NULL;
        logRelease(log);
        disableMockFio();
        ASSERT_EQ(0, eviction_context.key_list.size());
    }

    void freeEvictionContext() {
        for (auto it = eviction_context.key_list.begin(); it != eviction_context.key_list.end(); it++) {
            delete[] std::get<1>(*it);
        }
        eviction_context.key_list.clear();
    }

    void logStartSave(flashcacheLog *log, flashcacheSnapshotSecret *snapshot_secret,
            flashcacheSnapshotCallbackDetails *snapshot_callback_details,
            flashcacheSnapshotWriter *snapshot_writer, snapshotContext *snapshot_context,
            const char *filename) {
        if (snapshot_writer == NULL) {
            logStartFileBasedSave(log, filename,
                                  snapshot_secret,
                                  snapshot_callback_details, 1,
                                  flashcache_snapshot_version,
                                  flashcache_snapshot_save_type);
        } else {
            // Setting context in order to have same set of assertion for both file and stream based save
            snapshot_writer->callback_context = snapshot_context;
            snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
            snapshot_context->snapshot_version = flashcache_snapshot_version;
            // Construct Log Iteration completion callback details
            flashcacheLogIterationCallbackDetails log_iteration_completion_callback_details = { 0 };
            log_iteration_completion_callback_details.context = static_cast<void *>(snapshot_context);
            log_iteration_completion_callback_details.callback = logIterationCompletionCallback;
            logStartStreamBasedSave(log, snapshot_secret, snapshot_writer, flashcache_snapshot_version,
                                    flashcache_snapshot_save_type, &log_iteration_completion_callback_details);

            // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 1 in case of Threadsave
            if (flashcache_snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE) {
                flashcacheConfig config = {};
                config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
                config.numeric_value = 1;
                logSetConfig(log, &config);
            }
        }
    }

    // Write the specified number of items to log
    void writeItemsToLog(flashcacheLog *log, size_t num_databases, size_t num_items) {
        char *key, *value;
        size_t key_len = 0, value_len = 0;
        size_t current_time = getLogicalTime();
        for (size_t i = current_time; i < (current_time + num_items); ++i) {
            generateRandomItem(key, key_len, value, value_len);
            ASSERT_EQ(logWrite(log, i % num_databases, key, key_len, value, value_len),
                    FC_OK);
            logRunCronTasks(log);
        }
    }

    // Read the item written at the specified logical time
    requestContext *readItemFromLog(flashcacheLog *log, size_t num_databases, size_t time_of_creation,
            flashcache_get_item_callback get_item_callback) {
        char *key = nullptr, *value = nullptr;
        size_t key_len = 0, value_len = 0;
        getGeneratedItemAtTime(time_of_creation, key, key_len, value, value_len);
        requestContext *context = new requestContext;
        context->actual_value = value;
        context->actual_value_len = value_len;
        context->num_callback_triggered = 0;
        while (true) {
            flashcacheReturnCode rc = logRead(log, time_of_creation % num_databases, key, key_len,
                    FC_READ, static_cast<void *>(context), get_item_callback);
            if (rc == FC_OK) {
                break;
            }
            flashcacheAssert(rc == FC_ERR_THROTTLED);
            logRunCronTasks(log);
        }
        logRunCronTasks(log);
        return context;
    }

    // Read the item written at the specified logical time
    requestContext *deleteItemFromLog(flashcacheLog *log, size_t num_databases, size_t time_of_creation,
            flashcache_get_item_callback delete_item_callback) {
        char *key = nullptr, *value = nullptr;
        size_t key_len = 0, value_len = 0;
        getGeneratedItemAtTime(time_of_creation, key, key_len, value, value_len);
        requestContext *context = new requestContext;
        // Value and value length are not verified for deletions
        context->actual_value = NULL;
        context->actual_value_len = 0;
        context->num_callback_triggered = 0;
        while (true) {
            flashcacheReturnCode rc = logRead(log, time_of_creation % num_databases, key, key_len,
                    FC_DELETE, static_cast<void *>(context), delete_item_callback);
            if (rc == FC_OK) {
                break;
            }
            flashcacheAssert(rc == FC_ERR_THROTTLED);
            logRunCronTasks(log);
        }
        logRunCronTasks(log);
        return context;
    }

    void validateAllReadRequestCompleted(std::vector<requestContext *> request_contexts) {
        while (true) {
            bool processed_all_requests = true;
            for (size_t i = 0; i < request_contexts.size(); ++i) {
                size_t num_callback_triggered = request_contexts.at(i)->num_callback_triggered;
                ASSERT_LE(num_callback_triggered, 1);
                if (num_callback_triggered == 0) {
                    processed_all_requests = false;
                    break;
                }
            }

            if (processed_all_requests) {
                break;
            }
            logRunCronTasks(log);
        }
    }

    void cleanUpRequestContexts(std::vector<requestContext *> request_contexts) {
        for (size_t i = 0; i < request_contexts.size(); ++i) {
            delete request_contexts.at(i);
        }
    }

    void testItemRead(size_t num_items, size_t start_time) {
        std::vector<requestContext *> request_contexts;
        for (size_t i = start_time; i < (start_time + num_items); ++i) {
            request_contexts.push_back(readItemFromLog(log, num_databases, i, get_item_callback));
        }

        validateAllReadRequestCompleted(request_contexts);
        cleanUpRequestContexts(request_contexts);
    }

    void testItemDelete(size_t num_items, size_t start_time) {
        std::vector<requestContext *> request_contexts;
        for (size_t i = start_time; i < (start_time + num_items); ++i) {
            request_contexts.push_back(deleteItemFromLog(log, num_databases, i, delete_item_callback));
        }

        validateAllReadRequestCompleted(request_contexts);
        cleanUpRequestContexts(request_contexts);
    }

    void testReadAndWriteWithLargeNumberOfItems(size_t num_items, bool should_reset_head_tail_offset) {
        if (!should_reset_head_tail_offset) {
            // Disable resetting head and tail offset as here we write and delete item which will reset
            // the head and tail offset which will avoid scenarios like wrapping head to front of the log.
            log->should_reset_head_tail_offset_of_log = 0;
        }
        size_t start_time = getLogicalTime();
        writeItemsToLog(log, num_databases, num_items);
        testItemRead(num_items, start_time);
        log->should_reset_head_tail_offset_of_log = 1;  // Enable resetting head and tail offset
    }

    void waitForGarbageCollectionToCleanUpLog() {
        // Increase garbage collection speed to at least 1 MiB/sec to speed up the test.
        if (log->garbage_collector_info.required_garbage_collection_bytes_per_second < MEGABYTE_TO_BYTES) {
            log->garbage_collector_info.required_garbage_collection_bytes_per_second = MEGABYTE_TO_BYTES;
        }

        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES));
        while (log->head_offset != log->tail_offset) {
            logRunCronTasks(log);
        }
    }

    void validateSnapshotWriterCallbackInvocation(size_t num_set_snapshot_size_invocation,
                                                  size_t num_completed_invocation) {
        if (snapshot_writer == NULL) {
            return;
        }
        ASSERT_EQ(snapshotWriterMockGetNumSetSnapshotSizeInvocation(), num_set_snapshot_size_invocation);
        ASSERT_GT(snapshotWriterMockGetNumIsWritableInvocation(), 0);
        ASSERT_GT(snapshotWriterMockGetNumWriteInvocation(), 0);
        ASSERT_EQ(snapshotWriterMockGetNumCompleteInvocation(), num_completed_invocation);
    }

    void loadSnapshot(flashcacheLog *log_to_load, size_t expected_num_items,
            uint8_t *expected_hasher_seed) {
        flashcacheSnapshotSecret snapshot_secret_from_load = {0};
        int checksum_verification_result = 0;
        logLoadSnapshot(log_to_load,
                        snapshot_filename,
                        &snapshot_secret_from_load,
                        &checksum_verification_result);
        ASSERT_EQ(expected_num_items, logGetCountBasedMetric(log_to_load, FC_NUM_ITEMS));
        ASSERT_EQ(snapshot_secret_from_load.size, FC_SNAPSHOT_MAX_SECRET_SIZE);
        ASSERT_EQ(memcmp(&snapshot_secret, &snapshot_secret_from_load,
                    snapshot_secret_from_load.size), 0);

        UNUSED(expected_hasher_seed);
        if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
            uint8_t seed_after_loading[FLASHCACHE_HASHER_SEED_SIZE];
            log_to_load->hasher.get_seed(seed_after_loading);
            ASSERT_EQ(0, memcmp(expected_hasher_seed, seed_after_loading, FLASHCACHE_HASHER_SEED_SIZE));
        }
    }

    bool isTestingThreadsaveReplication() {
        return snapshot_writer != NULL && flashcache_snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE;
    }

    bool isTestingReplicationWithThreadsaveAndVersionOne() {
        return isTestingThreadsaveReplication() &&
               flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE;
    }

    void testSnapshotCompletionAndLoadingWithReadAndWrite(size_t num_items, bool load_to_new_log,
            uint32_t new_log_num_databases) {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

        size_t current_time = getLogicalTime();
        writeItemsToLog(log, num_databases, num_items);
        ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

        snapshotContext context = { 0 };
        snapshotContext *snapshot_context = &context;
        snapshot_context->expected_completion_status = 1;
        snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
        snapshot_context->snapshot_version = flashcache_snapshot_version;

        flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
        snapshot_callback_details.context = static_cast<void *>(snapshot_context);
        snapshot_callback_details.callback = snapshotCompletionCallback;

        logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                snapshot_context, snapshot_filename);
        std::vector<requestContext *> request_contexts;
        size_t write_item_time = current_time + num_items;
        size_t read_item_time = current_time + num_items / 2;

        // Read and write item from the log during snapshotting
        snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
        snapshotVersionTwoInfo *snapshot_info = snapshot_manager_info->snapshot_version_two_info;
        snapshotManagerSetHasSnapshottingCompletedInEngineLayer(0);
        while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
            request_contexts.push_back(readItemFromLog(log, num_databases, read_item_time++, get_item_callback));
            writeItemsToLog(log, num_databases, 1);
            write_item_time++;
            if (snapshot_info->is_waiting_for_engine_snapshotting_completion) {
                snapshotManagerSetHasSnapshottingCompletedInEngineLayer(1);
            }
        }

        // Extract the hasher seed from the snapshotted log
        uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
        log->hasher.get_seed(expected_hasher_seed);

        // Capture number of all deleted items before going into a new log
        size_t expected_num_items_after_loading = num_items
                                - logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD)
                                - logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE);

        if (load_to_new_log) {
            // Release the old log before creating a new log
            logRelease(log);
            log = nullptr;
            flashcacheEvictionDetails eviction_details = { 0 };
            eviction_details.context = &eviction_context;
            eviction_details.callback = eviction_callback;
            ASSERT_EQ(logCreate(&log, "new_log.db", log_size_bytes, 128, new_log_num_databases,
                        FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD,
                        max_allocated_log_size_percent, max_num_in_flight_read_requests,
                        min_garbage_collection_rate, evict_under_max_logsize_time_limit,
                        optimized_delete_enabled,
                        flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER),
                        mockClockGetTimeUs, &eviction_details), FC_OK);
        }

        // Load the snapshot
        loadSnapshot(log, expected_num_items_after_loading, expected_hasher_seed);

        // If we are doing THREADSAVE stream-based replication, we do not check for
        // point in time nature of items.
        if (!isTestingThreadsaveReplication()) {
            // Expect all the items to be present in the snapshot that were written before
            // (current_time + num_times)
            for (size_t i = current_time; i < (current_time + num_items); ++i) {
                request_contexts.push_back(readItemFromLog(log, num_databases, i, get_item_callback));
            }
        }

        // Expect all the new items to not be present in the snapshot
        for (size_t i = current_time + num_items; i < write_item_time; ++i) {
            request_contexts.push_back(readItemFromLog(log, num_databases, i,
                        get_empty_item_callback));
        }

        validateSnapshotWriterCallbackInvocation(1, 1);
        validateAllReadRequestCompleted(request_contexts);
        cleanUpRequestContexts(request_contexts);
        logFlush(log, FC_FLUSH_ALL_DBIDS);
    }

    void logRunCronTasksInALoop(flashcacheLog *log) {
        while (true) {
            logRunCronTasks(log);
        }
    }
};

class SingleDbLogTest : public LogTest {
 public:
    void SetUp() {
        initializeLog(LOG_SIZE_BYTES, 1);
    }
};

class LogTestWithDifferentGarbageCollectionRate : public LogTest {
};

class LogTestWithDifferentItemSize : public LogTest {
};

class LogTestWithSnapshotting : public LogTest {
 public:
    void SetUp() {
        initializeLog(LOG_SIZE_BYTES, NUM_DATABASES);
        // Snapshot V1 does not support THREADSAVE replication, FlashCache
        // will crash if we start snapshotting with these configurations.
        if (isTestingReplicationWithThreadsaveAndVersionOne()) {
            GTEST_SKIP();
        }
    }
};

class LogTestWithReplication : public LogTest {
};

class LogTestWithSnapshottingBufferLimit : public LogTest {
 public:
    void SetUp() {
        initializeLog(LOG_SIZE_BYTES, NUM_DATABASES);
        // Snapshot V1 does not support THREADSAVE replication, FlashCache
        // will crash if we start snapshotting with these configurations.
        if (isTestingReplicationWithThreadsaveAndVersionOne()) {
            GTEST_SKIP();
        }
    }
};

class LogTestWithLargeNumberOfItems : public LogTest {
 public:
     void SetUp() {
         // Use 1GiB of log size
         initializeLog(1024UL * 1024 * 1024, NUM_DATABASES);
     }
};

TEST_P(LogTest, happyCaseTest) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));

    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(getTotalItemLen(key_len, value_len),
            logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    ASSERT_EQ(1, context.num_callback_triggered);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Next access should not find the item as the first get should remove the
    // item from the store.
    requestContext empty_context = { 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&empty_context), get_item_callback), FC_OK);
    ASSERT_EQ(1, empty_context.num_callback_triggered);

    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Happy path for deleting the item
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(getTotalItemLen(key_len, value_len),
            logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));

    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_DELETE,
                static_cast<void *>(&context), delete_item_callback), FC_OK);
    ASSERT_EQ(2, context.num_callback_triggered);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_DELETE_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&empty_context), get_item_callback), FC_OK);
    ASSERT_EQ(2, empty_context.num_callback_triggered);
}

TEST_P(LogTest, happyCaseZeroLengthKeyReadWriteTest) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));

    uint32_t dbid = 0;
    char const *key = "";
    char const *value = "myvalue123";
    size_t key_len = 0, value_len = 10;
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len), FC_OK);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(getTotalItemLen(key_len, value_len),
            logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                      static_cast<void *>(&context), get_item_callback), FC_OK);
    ASSERT_EQ(1, context.num_callback_triggered);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Next access should not find the item as the first get should remove the
    // item from the store.
    requestContext empty_context = { 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                      static_cast<void *>(&empty_context), get_item_callback), FC_OK);
    ASSERT_EQ(1, empty_context.num_callback_triggered);

    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(LogTest, testWithIOFailureOnCreate) {
    flashcacheLog *new_log;
    std::string new_log_filename = "file_with_io_error.db";
    enableMockFio(0, true, new_log_filename);
    flashcacheEvictionDetails eviction_details = { 0 };
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;
    ASSERT_DEATH(logCreate(&new_log, new_log_filename.c_str(), log_size_bytes, 128, num_databases,
                FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD, max_allocated_log_size_percent,
                max_num_in_flight_read_requests, min_garbage_collection_rate,
                evict_under_max_logsize_time_limit, optimized_delete_enabled,
                flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER), mockClockGetTimeUs,
                &eviction_details), "");
}

TEST_P(LogTest, testWithIOFailureOnReadWrite) {
    log->staging_buffer_flush_size_threshold_bytes = 0;
    uint32_t dbid = 0;
    char *key, *value;
    setValueLen(256 * FC_PAGESIZE);
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    logRunCronTasks(log);

    enableMockFio(0, true, log_filename);
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    ASSERT_DEATH(logRunCronTasks(log), "");

    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    ASSERT_DEATH(logRunCronTasks(log), "");
    enableMockFio(0, true, "");
    // Drain the read result in this function as context variable is on the stack of this function.
    while (context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testLargeItemReadAndWrite) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));

    log->staging_buffer_flush_size_threshold_bytes = 0;
    uint32_t dbid = 0;
    char *key, *value;
    setValueLen(256 * FC_PAGESIZE);
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    logRunCronTasks(log);

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    ASSERT_EQ(FC_PAGESIZE, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));
}

TEST_P(LogTest, testFlushToLogNotAllowedWhenLogFull) {
    ASSERT_EQ(0, log->head_offset);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));
    log->garbage_collector_info.is_running = 1;

    log->staging_buffer_flush_size_threshold_bytes = 0;
    uint32_t dbid = 0;
    char *key, *value;
    setValueLen(256 * FC_PAGESIZE);
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    // Validate that data was not written to disk
    ASSERT_TRUE(fioRequestIsEmpty(&(log->log_flush_fio_request)));
    ASSERT_GT(logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES), 0);
    logRunCronTasks(log);
    // Validate that data was not written to disk after running cron task
    ASSERT_TRUE(fioRequestIsEmpty(&(log->log_flush_fio_request)));
    ASSERT_GT(logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES), 0);
    log->garbage_collector_info.is_running = 0;
}

TEST_P(LogTest, testWriteWhenMaxBufferedWriteSizeLessThanFlushThreshold) {
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES;
    config.numeric_value = 10;
    logSetConfig(log, &config);

    config.key = FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES;
    config.numeric_value = 1024 * 1024;
    logSetConfig(log, &config);

    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len),
            FC_OK);
    while (logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES) > 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
}

TEST_P(LogTest, testLargeItemReadAndWriteWithHeaderSplitInMultiplePages) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));

    // Write the first item so that large item header splits in 2 pages
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    setKeyLen(16);
    setValueLen(FC_PAGESIZE - 72);  // 72 = 40 (Header of first item) + 16 (Key size) + 16 (Portion
                                    // of next large item header)
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);

    // Set the staging buffer flush size threshold to 0 so that the 2 items are flushed as soon as
    // the next large item is written
    log->staging_buffer_flush_size_threshold_bytes = 0;
    setValueLen(256 * FC_PAGESIZE);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    logRunCronTasks(log);

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    // Since the header does not fit in one page, we expect 2 pages to have been read initially
    ASSERT_EQ(2 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));
}

TEST_P(LogTest, testLargeItemReadAndWriteWithKeySplitInMultiplePages) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));

    // Write the first item so that large item key splits in 2 pages
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    setKeyLen(16);
    setValueLen(FC_PAGESIZE - 104);  // 104 = 40 (Header of first item) + 16 (Key size) + 40 (Next
                                     // large item header) + 8 (Portion of next large item key)
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);

    // Set the staging buffer flush size threshold to 0 so that the 2 items are flushed as soon as
    // the next large item is written
    log->staging_buffer_flush_size_threshold_bytes = 0;
    setValueLen(256 * FC_PAGESIZE);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    logRunCronTasks(log);
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));
    // Since the header fits in the first page, we expect only one page to be partially read
    ASSERT_EQ(1 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_PARTIAL_ITEM_READ_BYTES));
}

TEST_P(LogTest, testDifferentLogCreateParam) {
    // Release the global log to before creating a local one to avoid memory leak.
    logRelease(log);

    size_t log_size = (12LL << 20);  // 12 MiB
    size_t index_size = 256;
    size_t staging_buffer_flush_size_threshold = 16;
    uint32_t max_allocated_log_size_percent = 70;
    flashcacheEvictionDetails eviction_details = { 0 };
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;

    flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    ASSERT_EQ(logCreate(&log, log_filename, log_size, index_size, num_databases, staging_buffer_flush_size_threshold,
                max_allocated_log_size_percent, max_num_in_flight_read_requests, min_garbage_collection_rate,
                evict_under_max_logsize_time_limit, optimized_delete_enabled,
                hasher, mockClockGetTimeUs, &eviction_details), FC_OK);

    ASSERT_EQ(log->log_size_bytes, log_size);
    ASSERT_EQ(log->staging_buffer_flush_size_threshold_bytes, staging_buffer_flush_size_threshold);
    for (size_t i = 0; i < num_databases; ++i) {
        ASSERT_EQ(indexTableSize(log->index_list[i]), index_size);
    }
    ASSERT_EQ(log->eviction_details.context, &eviction_context);
    ASSERT_EQ(log->eviction_details.callback, eviction_callback);
    ASSERT_EQ(log->max_allocated_log_size_percent, max_allocated_log_size_percent);
    ASSERT_EQ(log->hasher.hash_function, hasher->hash_function);
}

TEST_P(LogTest, testParallelReadForItemWithSameCollisionHash) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_TOTAL_DISK_WRITE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_DISK_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_TOTAL_DISK_READ_BYTES));

    setHashFunction(log, &single_bucket_hash_function);
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Avoid running garbage collection by setting value of garbage collected so far equal
    // to the required garbage collection bytes per second
    log->garbage_collector_info.garbage_collection_bytes_in_current_second =
        log->garbage_collector_info.required_garbage_collection_bytes_per_second;
    mockClockSetIncrementPerGetTimeCallUs(0);

    uint32_t dbid = 0;
    char *key1, *value1;
    size_t key1_len = 0, value1_len = 0;
    generateRandomItem(key1, key1_len, value1, value1_len);
    ASSERT_EQ(logWrite(log, dbid, key1, key1_len, value1, value1_len),
            FC_OK);
    logRunCronTasks(log);

    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
    ASSERT_EQ(FC_PAGESIZE, logGetCountBasedMetric(log, FC_TOTAL_DISK_WRITE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_DISK_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_TOTAL_DISK_READ_BYTES));

    char *key2, *value2;
    size_t key2_len = 0, value2_len = 0;
    generateRandomItem(key2, key2_len, value2, value2_len);
    ASSERT_EQ(logWrite(log, dbid, key2, key2_len, value2, value2_len),
            FC_OK);
    logRunCronTasks(log);

    size_t total_item_size = getTotalItemLen(key1_len, value1_len) +
        getTotalItemLen(key2_len, value2_len);
    ASSERT_EQ(LOG_SIZE_BYTES, logGetCountBasedMetric(log, FC_TOTAL_DB_SIZE_BYTES));
    ASSERT_EQ(2 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES));
    ASSERT_EQ(total_item_size, logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
    ASSERT_EQ(2 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_TOTAL_DISK_WRITE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_DISK_READ));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_TOTAL_DISK_READ_BYTES));

    requestContext context2{ value2, value2_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key2, key2_len, FC_READ,
                static_cast<void *>(&context2), get_item_callback), FC_OK);

    requestContext context1{ value1, value1_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key1, key1_len, FC_READ,
                static_cast<void *>(&context1), get_item_callback), FC_OK);

    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_IN_FLIGHT));
    while (context1.num_callback_triggered == 0 ||
            context2.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_READ_IN_FLIGHT));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION));
    ASSERT_EQ(FC_PAGESIZE, logGetCountBasedMetric(log, FC_UNUSED_DISK_READ_BYTES_HASH_COLLISION));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
    ASSERT_EQ(2 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_TOTAL_DISK_WRITE_BYTES));
    ASSERT_EQ(3, logGetCountBasedMetric(log, FC_NUM_DISK_READ));
    ASSERT_EQ(3 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_TOTAL_DISK_READ_BYTES));
}

TEST_P(LogTest, testResetGarbageCollectionTimeWindowIfRequired) {
    garbageCollectorInfo gc_info = { 0 };
    gc_info.garbage_collection_bytes_in_current_second = 100;
    gc_info.required_garbage_collection_bytes_per_second = 10;

    uint64_t current_time_us = 0;
    resetGarbageCollectionTimeWindowIfRequired(current_time_us, &gc_info);
    ASSERT_EQ(0, gc_info.total_garbage_collected_bytes);
    ASSERT_EQ(100, gc_info.garbage_collection_bytes_in_current_second);

    // Increase time by 1 second
    current_time_us += FC_SECOND_TO_MICROSECOND;

    resetGarbageCollectionTimeWindowIfRequired(current_time_us, &gc_info);
    ASSERT_EQ(100, gc_info.total_garbage_collected_bytes);
    ASSERT_EQ(0, gc_info.garbage_collection_bytes_in_current_second);

    gc_info.garbage_collection_bytes_in_current_second = 20;

    // Increase time by 5 second
    current_time_us += 5 * FC_SECOND_TO_MICROSECOND;

    resetGarbageCollectionTimeWindowIfRequired(current_time_us, &gc_info);
    ASSERT_EQ(120, gc_info.total_garbage_collected_bytes);
    ASSERT_EQ(0, gc_info.garbage_collection_bytes_in_current_second);
}

TEST_P(LogTest, testGarbageCollectLastItemBeforeNextPage) {
    // Reduce the threshold for flushing data to flash to 1 to immediately trigger a flush.
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Write an item that is a bit smaller than 4KB page to the log.
    // It is the last item on the 4KB page because no other items comes after it.
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    setKeyLen(16);
    setValueLen(3000);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len), FC_OK);

    while (mockFioGetNumRequestCompleted(log->fio_context) == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, log->garbage_collector_info.is_running);
    ASSERT_EQ(0, log->log_iterator->partial_item_size_bytes);
    ASSERT_EQ(0, log->staging_buffer->total_item_size);

    // This will call garbage collection for the item in the log
    logRunCronTasks(log);

    // Validate the log states after GC is called on the item
    // The item should now be in the staging buffer with actual size of 3056 Bytes.
    // Log head and tail offsets are both 4KB due to page alignment for the item.
    ASSERT_EQ(log->head_offset, 4096);
    ASSERT_EQ(4096, log->tail_offset);
    ASSERT_EQ(3056, log->staging_buffer->total_item_size);
    ASSERT_EQ(1, log->garbage_collector_info.is_running);

    // The number of bytes processed by the GC log iterator in the current second is 4KB.
    ASSERT_EQ(4096, log->garbage_collector_info.garbage_collection_bytes_in_current_second);
    ASSERT_EQ(4096, *log->log_iterator->log_processed_bytes_in_current_second);
    ASSERT_EQ(4096, log->log_iterator->log_data_buffer_offset);
    ASSERT_EQ(4096, *log->log_iterator->log_processed_offset);

    // Partial item size bytes is 0
    ASSERT_EQ(0, log->log_iterator->partial_item_size_bytes);
}

TEST_P(LogTest, testPartialItemResetAfterLogFlush) {
    // Reduce the threshold for flushing data to flash to 1 to immediately trigger a flush.
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Write a big item so that we can have a partial item read.
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    setKeyLen(4096 * 4);
    setValueLen(4096 * 4);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len), FC_OK);

    while (mockFioGetNumRequestCompleted(log->fio_context) == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, log->garbage_collector_info.is_running);
    ASSERT_EQ(0, log->log_iterator->partial_item_size_bytes);

    // During flush, we restrict the GC to start and process the GC pending_read_buffer.
    // As item was big, partial_item_size_bytes will be set for GC next block computation. Earlier this
    // partial_item_size_bytes was not being reset which was causing crash. Now, while resetting head and tail
    // offset, partial_item_size_bytes will be reset too.
    logFlush(log, FC_FLUSH_ALL_DBIDS);
    ASSERT_EQ(0, log->head_offset);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(0, log->garbage_collector_info.is_running);
    ASSERT_EQ(0, log->log_iterator->partial_item_size_bytes);

    // Write a new item so that GC can be started
    key_len = 0, value_len = 0;
    setKeyLen(16);
    setValueLen(200);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len), FC_OK);
    while (mockFioGetNumRequestCompleted(log->fio_context) == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, log->garbage_collector_info.is_running);
    ASSERT_EQ(0, log->log_iterator->partial_item_size_bytes);

    // This will call garbage collection and as partial item read size bytes has been reset, it will not crash.
    logRunCronTasks(log);
    ASSERT_EQ(log->head_offset, 4096);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(1, log->garbage_collector_info.is_running);
}

TEST_P(LogTest, testComputeGarbageCollectionBlockSizeInBytes) {
    if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_TWO) {
        // This tests the GC block size calculation, no need for
        // testing with multiple snapshot versions.
        return;
    }
    adjustSnapshotVersion(flashcache_snapshot_version);
    flashcacheLog log = {};
    stagingBuffer staging_buffer = {};
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    snapshotVersionOneInfo *snapshot_info = snapshot_manager_info->snapshot_version_one_info;
    flashcacheLogIterator log_iterator = {};
    log.log_iterator = &log_iterator;
    log.staging_buffer = &staging_buffer;
    garbageCollectorInfo *gc_info = &(log.garbage_collector_info);
    log.log_size_bytes = (1024LL * 1024 * 1024);  // 1 GiB

    gc_info->garbage_collection_bytes_in_current_second = 100;
    gc_info->required_garbage_collection_bytes_per_second = 10;
    log.tail_offset = 8150;
    log.head_offset = 8192;
    ASSERT_EQ(0, computeGarbageCollectionBlockSizeInBytes(&log));

    gc_info->garbage_collection_bytes_in_current_second = 2000;
    gc_info->required_garbage_collection_bytes_per_second = 4078;
    ASSERT_EQ(4096, computeGarbageCollectionBlockSizeInBytes(&log));

    // Set required garbage collection rate at 120 MiB/sec
    gc_info->required_garbage_collection_bytes_per_second = (220LL * 1024 * 1024);  // 220 MiB
    log.tail_offset = log.log_size_bytes - (240LL * 1024 * 1024);  // 140 MiB
    // Verify maximum block size restriction of 200 MB is applied
    ASSERT_EQ((200LL * 1024 * 1024), computeGarbageCollectionBlockSizeInBytes(&log));

    log.tail_offset = 0;
    log.head_offset = 16384;
    gc_info->required_garbage_collection_bytes_per_second = 8192;
    gc_info->garbage_collection_bytes_in_current_second = 0;
    ASSERT_EQ(gc_info->required_garbage_collection_bytes_per_second,
           logGetCountBasedMetric(&log, FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND));
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    log.tail_offset = 192;
    log.head_offset = 8192;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    log.tail_offset = 0;
    log.head_offset = 16384;
    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes = 20480;
    snapshot_info->snapshot_file_current_log_data_write_offset = 4096;
    snapshot_info->log_file_current_offset = 4096;
    ASSERT_EQ(0, computeGarbageCollectionBlockSizeInBytes(&log));

    snapshot_info->snapshot_common.log_file_processed_offset = 8192;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    snapshot_info->snapshot_common.log_file_processed_offset = 12288;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    log.tail_offset = 16384;
    log.head_offset = 8192;
    snapshot_info->snapshot_common.log_file_processed_offset = 4094;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    snapshot_info->snapshot_common.is_running = 0;
    gc_info->garbage_collection_bytes_in_current_second = 2000;
    log_iterator.partial_item_size_bytes = 8000;
    gc_info->required_garbage_collection_bytes_per_second = 3000;
    log.head_offset = 409600;
    log.tail_offset = 16384;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));

    // If we need to read certain number of bytes to read the item (indicated by partial item read size), we still
    // read the whole item else we can get into situation of repeated failed read.
    gc_info->extra_garbage_collected_bytes = 900;
    ASSERT_EQ(8192, computeGarbageCollectionBlockSizeInBytes(&log));
    ASSERT_EQ(0, gc_info->extra_garbage_collected_bytes);
    ASSERT_EQ(2900, gc_info->garbage_collection_bytes_in_current_second);

    gc_info->garbage_collection_bytes_in_current_second = 2000;
    gc_info->required_garbage_collection_bytes_per_second = 3000;
    gc_info->extra_garbage_collected_bytes = 1000;
    ASSERT_EQ(0, computeGarbageCollectionBlockSizeInBytes(&log));
    ASSERT_EQ(0, gc_info->extra_garbage_collected_bytes);
    ASSERT_EQ(3000, gc_info->garbage_collection_bytes_in_current_second);

    gc_info->garbage_collection_bytes_in_current_second = 2000;
    gc_info->required_garbage_collection_bytes_per_second = 3000;
    gc_info->extra_garbage_collected_bytes = 1400;
    ASSERT_EQ(0, computeGarbageCollectionBlockSizeInBytes(&log));
    ASSERT_EQ(400, gc_info->extra_garbage_collected_bytes);
    ASSERT_EQ(3000, gc_info->garbage_collection_bytes_in_current_second);
}

TEST_P(LogTest, readAndWriteLargeNumberOfItemWithSlowedGarbageCollectionProcessing) {
    // Slow down garbage collection processing to 1 second so that only a single item gets processed
    // in every batch
    mockClockSetIncrementPerGetTimeCallUs(FC_SECOND_TO_MICROSECOND);

    testReadAndWriteWithLargeNumberOfItems(30000, true);
}

TEST_P(LogTest, testItemMovedDuringGarbageCollection) {
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, 100);
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = FC_PAGESIZE;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));
    size_t expected_num_items_moved_by_garbage_collector = 10;
    while (logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED) <
            expected_num_items_moved_by_garbage_collector) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(getTotalItemLen() * expected_num_items_moved_by_garbage_collector,
            logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_WRITE_BYTES));
}

TEST_P(LogTest, testPointInTimeSnapshotWithItemMovedDuringGC) {
    if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        // This tests the point in time snapshot with item being moved
        // because of GC at the same time in Snapshot Version Two. No need to
        // run it for Snapshot Version One
        return;
    }
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, 1000);
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 10 * 4096;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(log->tail_offset, 0);
    // Save snapshot and load it back
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);

    // Change the tail offset to move ahead of Snapshot Iterator which will start from 0 offset.
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), (10 * FC_PAGESIZE));
    log->tail_offset = log->head_offset - (10 * FC_PAGESIZE);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }
    ASSERT_GT(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_WRITE_BYTES), 0);
    ASSERT_GT(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED), 0);

    // Load the snapshot and it should contain all 1000 items.
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, 1000, expected_hasher_seed);
}

TEST_P(LogTest, testItemWithZeroLengthKeyMovedDuringGarbageCollection) {
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Write items with empty key names to each database
    char const *key = "";
    size_t key_len = 0;
    for (int i = 0; i < num_databases; ++i) {
        if (i > 0) {
            ASSERT_EQ(logWrite(log, i, key, key_len, "value", 5), FC_OK);
        } else {
            ASSERT_EQ(logWrite(log, i, key, key_len, "", 0), FC_OK);
        }
        logRunCronTasks(log);
    }
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = FC_PAGESIZE;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));
    // Here we do garbage collection on 10 items. The first item has empty key and empty value.
    // The next 9 items each has an empty key and value with 5 characters
    while (logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED) < 10) {
        logRunCronTasks(log);
    }
    size_t total_item_len = getTotalItemLen(0, 0) + 9 * getTotalItemLen(0, 5);
    ASSERT_EQ(total_item_len, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_WRITE_BYTES));
}

TEST_P(LogTest, testLastItemBeforeNextPageBoundary) {
    // Reduce the threshold for flushing data to flash to 1 to immediately trigger a flush.
    log->staging_buffer_flush_size_threshold_bytes = 0;

    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);

    while (mockFioGetNumRequestCompleted(log->fio_context) == 0) {
        logRunCronTasks(log);
    }

    ASSERT_TRUE(mockFioVerifyItem(log->fio_context, 0, dbid, key, key_len, value, value_len,
                FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY, log->crc_function));
}

TEST_P(LogTest, testSkipSegmentMarkerAtLogEnd) {
    // Reduce the threshold for flushing data to flash to 1 to immediately trigger a flush.
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Move the head pointer to the start of last page
    size_t head_offset = log->tail_offset = log->head_offset = (log->log_size_bytes - FC_PAGESIZE);

    uint32_t dbid = 0;
    char const *key = "key";
    size_t key_len = strlen(key);

    // Create a large value so that the item does not fit in the last page
    char *value = static_cast<char *>(fcMalloc(FC_PAGESIZE));
    size_t value_len = FC_PAGESIZE;

    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);

    while (mockFioGetNumRequestCompleted(log->fio_context) != 1) {
        logRunCronTasks(log);
    }

    ASSERT_TRUE(mockFioVerifyHeaderFlag(log->fio_context, head_offset, FC_SKIP_SEGMENT));
    fcFree(value);

    // Wait for garbage collector to the collect the last page that contains the marker
    while (log->garbage_collector_info.total_garbage_collected_bytes == 0) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testSkipSegmentMarkerAtLogMiddle) {
    // Reduce the threshold for flushing data to flash to 1 to immediately trigger a flush.
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Reduce the log size to half so that the skip segment mark is written at the middle of the log with item written
    // after it
    log->log_size_bytes = log->log_size_bytes / 2;

    // Move the head pointer to the start of last page
    size_t head_offset = log->tail_offset = log->head_offset = (log->log_size_bytes - FC_PAGESIZE);

    uint32_t dbid = 0;
    char const *key = "key";
    size_t key_len = strlen(key);

    // Create a large value so that the item does not fit in the last page
    char *value = static_cast<char *>(fcMalloc(FC_PAGESIZE));
    size_t value_len = FC_PAGESIZE;
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
            FC_OK);
    fcFree(value);

    // Reset the head offset after flush as we will increase the log size. This will ensure that the new write happens
    // after the skip segment
    log->head_offset_after_flush_succeed = log->log_size_bytes;
    log->log_size_bytes *= 2;
    while (mockFioGetNumRequestCompleted(log->fio_context) != 1) {
        logRunCronTasks(log);
    }
    ASSERT_TRUE(mockFioVerifyHeaderFlag(log->fio_context, head_offset, FC_SKIP_SEGMENT));

    // Wait for garbage collector to the collect the last page that contains the marker
    while (log->garbage_collector_info.total_garbage_collected_bytes == 0) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testGarbageCollectionRateWithInverseWithFreeSpaceAlgorithm) {
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 1;
    log->garbage_collector_info.garbage_collection_rate_algorithm = FC_GARBAGE_COLLECTION_INVERSE_WITH_FREE_SPACE;

    // Log size is defaulted to 104857600
    log->head_offset = 1500;
    log->tail_offset = 127;
    // Low overhead defaults to the min GC rate (4kb/s)
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // 100% overhead ratio/100% log util should boost GC rate to max dynamic GC rate (30mb/s)
    log->log_size_bytes = 1373;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(MAX_DYNAMIC_GC_RATE, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // With 0% overhead ratio, linear logic should kick in
    log->head_offset = 127;
    log->tail_offset = 1500;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 627;  // We change the allocated to create no garbage
    updateGarbageCollectionRate(log);
    ASSERT_EQ(12466, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 2000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(273000, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->allocated_log_size_bytes = 0;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1500;
    log->tail_offset = 127;
    log->log_size_bytes = 4000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    log->allocated_log_size_bytes = 1373;  // We change the allocated to create no garbage
    updateGarbageCollectionRate(log);
    ASSERT_EQ(5263, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // create garbage, dynamic logic should kick in. 27.1668% overhead, 34.25% util
    log->allocated_log_size_bytes = 1000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(2933391, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1500;
    log->tail_offset = 0;
    log->log_size_bytes = 1500;
    log->allocated_log_size_bytes = 1500;  // We change the allocated to create no garbage
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(100700, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // set overhead ratio to 50%, dynamic logic should kick in
    log->allocated_log_size_bytes = 750;
    log->log_size_bytes = 1500;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(15 * 1024 * 1024, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // create no garbage for the remaining tests, inverse logic should kick in
    log->allocated_log_size_bytes = 1450;
    log->head_offset = 1450;
    log->tail_offset = 0;
    log->log_size_bytes = 1500;
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(100700, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 0;
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 6000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(6000, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 300;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
}

TEST_P(LogTest, testGarbageCollectionRateWithLinearWithFreeSpaceAlgorithm) {
    // We don't set the GC algorithm as Linear with free space algorithm is the default algorithm
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 1;
    log->max_allocated_log_size_percent = 50;

    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Overhead ratio is at 100%, we expect GC rate to be 30mb/s
    log->head_offset = 1500;
    log->tail_offset = 127;
    log->log_size_bytes = 1373;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(MAX_DYNAMIC_GC_RATE, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
    // create no garbage, we expect the linear algorithm calc to resume
    log->log_size_bytes = 2000;
    log->allocated_log_size_bytes = 1373;
    log->write_rate_info.last_window_data_written_bytes_per_second = 4096;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(10207, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->allocated_log_size_bytes = 627;  // create no garbage to allow inverse logic to kick in
    log->head_offset = 127;
    log->tail_offset = 1500;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(17117, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 2000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(136500, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->allocated_log_size_bytes = 0;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1500;
    log->tail_offset = 127;
    log->log_size_bytes = 4000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    log->allocated_log_size_bytes = 1373;  // create no garbage to allow inverse logic to kick in
    updateGarbageCollectionRate(log);
    ASSERT_EQ(6913, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1500;
    log->tail_offset = 0;
    log->log_size_bytes = 1500;
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    log->allocated_log_size_bytes = 1500;  // create no garbage to allow inverse logic to kick in
    updateGarbageCollectionRate(log);
    ASSERT_EQ(50350, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->head_offset = 1450;
    log->tail_offset = 0;
    log->log_size_bytes = 1500;
    log->write_rate_info.last_window_data_written_bytes_per_second = 10070;
    log->allocated_log_size_bytes = 1450;  // create no garbage to allow inverse logic to kick in
    updateGarbageCollectionRate(log);
    ASSERT_EQ(47664, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->max_allocated_log_size_percent = 75;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(44979, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 0;
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 6000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(6000, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 300;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
}

TEST_P(LogTest, testUpdateRateInfo) {
    writeRateInfo info;
    info.last_window_data_written_bytes_per_second = 27300;
    info.current_window_data_written = 72000;
    info.window_size_us = 2 * FC_SECOND_TO_MICROSECOND;
    info.current_window_start_time_us = 0;
    info.last_window_data_written_bytes_per_second = 120;

    uint64_t current_time_us = 0;
    updateWriteRateInfo(current_time_us, &info, 600000);
    ASSERT_EQ(672000, info.current_window_data_written);
    ASSERT_EQ(120, info.last_window_data_written_bytes_per_second);
    ASSERT_EQ(0, info.current_window_start_time_us);

    current_time_us += info.window_size_us;
    updateWriteRateInfo(current_time_us, &info, 700);
    ASSERT_EQ(700, info.current_window_data_written);
    ASSERT_EQ(336000, info.last_window_data_written_bytes_per_second);
    ASSERT_EQ(current_time_us, info.current_window_start_time_us);
}

TEST_P(LogTest, testEviction) {
    log->max_allocated_log_size_percent = 0;
    log->log_size_bytes = 4096;
    log->staging_buffer_flush_size_threshold_bytes = 0;

    uint32_t dbid = 0;
    char const *key1 = "Hello", *key2 = "Hey";
    char const *value1 = "World!", *value2 = "zzzz";

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_READ_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_DISK_READ));

    logWrite(log, dbid, key1, strlen(key1), value1, strlen(value1));
    logWrite(log, dbid, key2, strlen(key2), value2, strlen(value2));
    while (eviction_context.key_list.size() < 2) {
        logRunCronTasks(log);
    }

    ASSERT_EQ(log->head_offset, log->tail_offset);
    ASSERT_EQ(0, log->allocated_log_size_bytes);
    ASSERT_EQ(0, log->head_offset);

    ASSERT_EQ(dbid, std::get<0>(eviction_context.key_list.at(0)));
    ASSERT_EQ(0, memcmp(std::get<1>(eviction_context.key_list.at(0)), key1, strlen(key1)));
    ASSERT_EQ(strlen(key1), std::get<2>(eviction_context.key_list.at(0)));
    ASSERT_EQ(dbid, std::get<0>(eviction_context.key_list.at(1)));
    ASSERT_EQ(0, memcmp(std::get<1>(eviction_context.key_list.at(1)), key2, strlen(key2)));
    ASSERT_EQ(strlen(key2), std::get<2>(eviction_context.key_list.at(1)));

    size_t total_item_size = getTotalItemLen(strlen(key1), strlen(value1)) +
        getTotalItemLen(strlen(key2), strlen(value2));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    ASSERT_EQ(total_item_size, logGetCountBasedMetric(log, FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_DISK_READ));
    ASSERT_EQ(2 * FC_PAGESIZE, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_READ_BYTES));

    freeEvictionContext();
}

TEST_P(LogTest, testEvictionConfig) {
    // Disable eviction
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_EVICTION_ENABLED;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    log->max_allocated_log_size_percent = 0;
    log->log_size_bytes = 4096;
    log->staging_buffer_flush_size_threshold_bytes = 0;

    uint32_t dbid = 0;
    char const *key1 = "Hello", *key2 = "Hey";
    char const *value1 = "World!", *value2 = "zzzz";

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    logWrite(log, dbid, key1, strlen(key1), value1, strlen(value1));
    logWrite(log, dbid, key2, strlen(key2), value2, strlen(value2));
    for (int i = 0; i < 1000; ++i) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    ASSERT_EQ(0, eviction_context.key_list.size());

    // Enable eviction
    config.numeric_value = 1;
    logSetConfig(log, &config);
    while (logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED) < 2) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(2, eviction_context.key_list.size());
    freeEvictionContext();
}

TEST_P(LogTest, testEvictionUnderMaxLogsizeConfig) {
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_EVICTION_ENABLED;
    config.numeric_value = 1;
    logSetConfig(log, &config);

    // Disable eviction under max logsize
    config.key = FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_RATE;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    // Set configs
    log->max_allocated_log_size_percent = 50;
    log->log_size_bytes = 4096;
    log->evict_under_max_logsize_time_limit = 50 * 1000 * 1000;
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Add items
    uint32_t dbid = 0;
    char const *key1 = "Hello", *key2 = "Hey";
    char const *value1 = "World!", *value2 = "zzzz";
    ASSERT_EQ(0, log->last_evict_under_max_logsize_start);
    logWrite(log, dbid, key1, strlen(key1), value1, strlen(value1));
    logWrite(log, dbid, key2, strlen(key2), value2, strlen(value2));

    // Garbage collection rate should be unchanged from default
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Number of items evicted under max logsize should start at and remain 0
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_IS_EVICTING_UNDER_MAX_LOGSIZE));
    for (int i = 0; i < 1000; ++i) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_IS_EVICTING_UNDER_MAX_LOGSIZE));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE));
    ASSERT_EQ(0, eviction_context.key_list.size());

    // Enable eviction
    uint32_t override_value = 10 * 1024 * 1024;
    config.numeric_value = override_value;
    logSetConfig(log, &config);
    ASSERT_LE(0, log->last_evict_under_max_logsize_start);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_IS_EVICTING_UNDER_MAX_LOGSIZE));

    // Garbage collection rate should be updated to value set by FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_RATE
    // Items should be evicted
    updateGarbageCollectionRate(log);
    ASSERT_EQ(override_value, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
    while (logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE) < 2) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(2, eviction_context.key_list.size());

    // Verify that evict under max logsize resets after time limit
    log->evict_under_max_logsize_time_limit = 0;
    ASSERT_EQ(0, needToPerformEviction(log));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_IS_EVICTING_UNDER_MAX_LOGSIZE));
    freeEvictionContext();
}

TEST_P(LogTest, testOverrideAllocatableDbSizeConfig) {
    log->log_size_bytes = 10240;
    log->max_allocated_log_size_percent = 100;

    // Set log size to 8Kib
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_OVERRIDEN_ALLOCATABLE_DB_SIZE_BYTES;
    config.numeric_value = 8192;
    logSetConfig(log, &config);

    flashcacheConfig result = {};
    result.key = FC_CONFIG_KEY_OVERRIDEN_ALLOCATABLE_DB_SIZE_BYTES;
    logGetConfig(log, &result);
    ASSERT_EQ(result.numeric_value, config.numeric_value);

    // Expected values succeed.
    config.numeric_value = -1;
    logSetConfig(log, &config);
    logGetConfig(log, &result);
    ASSERT_EQ(result.numeric_value, config.numeric_value);
    config.numeric_value = 0;
    logSetConfig(log, &config);
    logGetConfig(log, &result);
    ASSERT_EQ(result.numeric_value, config.numeric_value);

    // Out of bound value.
    config.numeric_value = -2;
    ASSERT_DEATH(logSetConfig(log, &config), "");

    // Value more than the current log size.
    config.numeric_value = log->log_size_bytes * 10;
    ASSERT_DEATH(logSetConfig(log, &config), "");
}

TEST_P(LogTest, testEvictionWithSameKeyWithDifferentDbid) {
    log->max_allocated_log_size_percent = 1;
    log->log_size_bytes = 4096;
    log->staging_buffer_flush_size_threshold_bytes = 0;

    uint32_t dbid1 = 5;
    uint32_t dbid2 = 133;
    char const *key = "Hello", *value = "World!";
    size_t key_len = strlen(key), value_len = strlen(value);

    logWrite(log, dbid1, key, key_len, value, value_len);
    logWrite(log, dbid2, key, key_len, value, value_len);
    while (eviction_context.key_list.size() < 1) {
        logRunCronTasks(log);
    }

    requestContext empty_context = { 0 };
    ASSERT_EQ(logRead(log, dbid1, key, key_len, FC_READ,
                static_cast<void *>(&empty_context), get_item_callback), FC_OK);
    ASSERT_EQ(1, empty_context.num_callback_triggered);

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid2, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    ASSERT_EQ(1, context.num_callback_triggered);

    freeEvictionContext();
}

TEST_P(LogTest, testEvictionLargeNumberOfItems) {
    log->max_allocated_log_size_percent = 20;
    log->log_size_bytes = 10 * MEGABYTE_TO_BYTES;
    log->staging_buffer_flush_size_threshold_bytes = 4096;

    char *key, *value;
    size_t key_len = 0, value_len = 0;
    setValueLen(400);
    setKeyLen(72);
    uint32_t dbid;
    for (size_t i = 0; i < 10000; ++i) {  // Writing more than 5 MiB
        dbid = i % num_databases;
        generateRandomItem(key, key_len, value, value_len);
        addDbidToKey(key, dbid);
        ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len),
                FC_OK);
        logRunCronTasks(log);
    }

    size_t expected_allocated_log_size_bytes = 2 * MEGABYTE_TO_BYTES;
    while (log->allocated_log_size_bytes > expected_allocated_log_size_bytes) {
        logRunCronTasks(log);
    }

    // Running log cron multiple times to ensure that we don't have unwanted eviction
    for (int i = 0; i < 1000; ++i) {
        logRunCronTasks(log);
    }

    size_t expected_num_evicted_items = 5904;
    ASSERT_EQ(expected_allocated_log_size_bytes, log->allocated_log_size_bytes);
    ASSERT_EQ(expected_num_evicted_items, eviction_context.key_list.size());
    ASSERT_EQ(expected_num_evicted_items, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));

    for (size_t i = 0; i < eviction_context.key_list.size(); i++) {
        key = std::get<1>(eviction_context.key_list.at(i));
        dbid = getDbidFromKey(key);
        ASSERT_EQ(std::get<0>(eviction_context.key_list.at(i)), dbid);
    }
    freeEvictionContext();
}

TEST_P(LogTest, testLogStateAfterSnapshotLoading) {
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    size_t num_items = 1;
    writeItemsToLog(log, num_databases, num_items);

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    // Save snapshot and load it back
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    ASSERT_EQ(log->allocated_log_size_bytes, getTotalItemLen());

    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));

    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    log->log_iterator->partial_item_size_bytes = 16 * 1024;
    loadSnapshot(log, num_items, expected_hasher_seed);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), getTotalItemLen());
    ASSERT_EQ(log->log_iterator->partial_item_size_bytes, 0);
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        ASSERT_EQ(log->tail_offset, 0);
        ASSERT_EQ(log->head_offset, FC_PAGESIZE);
    } else if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_TWO) {
        ASSERT_EQ(log->tail_offset, 0);
        ASSERT_EQ(log->head_offset, 0);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));

    // Run the Cron task multiple time to ensure that garbage collection is able to work properly after loading a new
    // snapshot
    for (int i = 0; i < 1000; ++i) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testLogFlushAllDBs) {
    // Write to a number of dbs and make sure all items are wiped
    // after logFlush(ALL_DBIDS)
    size_t num_items = 5000;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    logFlush(log, FC_FLUSH_ALL_DBIDS);
    for (int i = 0; i < num_databases; i++) {
        flashcacheIndex *index = log->index_list[i];
        ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
        ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);
    }
    ASSERT_EQ(0, log->head_offset);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);
    waitForGarbageCollectionToCleanUpLog();
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(LogTest, testLogFlushAllDBsEmptyLog) {
    // Make sure you can flush on an empty log
    logFlush(log, FC_FLUSH_ALL_DBIDS);
    ASSERT_EQ(0, log->head_offset);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);
    waitForGarbageCollectionToCleanUpLog();
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(LogTest, testLogFlushDBid) {
     // Test ability to flush a single db at at time
    size_t num_items = 500;
    size_t num_databases = 2;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    log->index_list[0]->growth_iterator->status = RUNNING;
    logFlush(log, 0);  // Flush one db expect 1/2 items left
    ASSERT_EQ(num_items / num_databases, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(log->index_list[0]->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(log->index_list[0]->growth_iterator->next_hash_bucket, 0);

    log->index_list[1]->growth_iterator->status = RUNNING;
    logFlush(log, 1);  // Flush remaining db
    ASSERT_EQ(log->index_list[1]->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(log->index_list[1]->growth_iterator->next_hash_bucket, 0);
    waitForGarbageCollectionToCleanUpLog();
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(LogTest, testLogFlushAllDBsWithSnapshot) {
    // Test flush alldbs with snapshot pending
    size_t num_items = 5000;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 0;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    // Save snapshot to have one outstanding when we flushLog
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    logFlush(log, FC_FLUSH_ALL_DBIDS);
    ASSERT_EQ(0, log->head_offset);
    ASSERT_EQ(0, log->tail_offset);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);
    waitForGarbageCollectionToCleanUpLog();
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
}

TEST_P(LogTest, testZeroAllocatedSizeWhenGCIsRunning) {
    // Test scenario when allocated size become 0 when gc is running (without flush)
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    log->head_offset = log->tail_offset = FC_PAGESIZE;  // Just to make sure it does not start from 0
    writeItemsToLog(log, num_databases, 1000);
    ASSERT_EQ(1000, logGetCountBasedMetric(log, FC_NUM_DISK_WRITE));
    ASSERT_EQ(1000, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_GT(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);

    log->garbage_collector_info.required_garbage_collection_bytes_per_second = FC_PAGESIZE;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));

    // Wait for gc to start progress
    logRunCronTasks(log);
    ASSERT_EQ(1, log->garbage_collector_info.is_running);
    ASSERT_LT(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED), 5);
    log->allocated_log_size_bytes = 0;  // Set Allocated log size bytes to 0 in middle of GC
    logRunCronTasks(log);
    ASSERT_EQ(1, log->garbage_collector_info.is_running);
    ASSERT_NE(0, log->head_offset);
    ASSERT_NE(0, log->tail_offset);
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);
}

TEST_P(LogTest, testZeroAllocatedSizeWhenSnapshotIsRunning) {
    // Test scenario when allocated size become 0 when snapshotting is running (without flush)
    size_t num_items = 5000;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_GT(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(1, snapshotManagerIsRunning());
    log->allocated_log_size_bytes = 0;

    // Wait for snapshotting to complete
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        ASSERT_NE(0, log->head_offset);
        ASSERT_NE(0, log->tail_offset);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0);
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testGarbageDataNotIncludedInSnapshotIfNoItemsInFlash) {
    // Test that garbage data is not included in Snapshots if there is no item in flash
    testReadAndWriteWithLargeNumberOfItems(1000, false);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Ensure there is no live item and only a garbage data in log
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES));
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), 0);

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Capture snapshot once we only have garbage in log
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(0, log->garbage_collector_info.is_running);
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));

    // Wait for snapshotting to complete
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, 0, expected_hasher_seed);
    validateSnapshotWriterCallbackInvocation(1, 1);

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
}

TEST_P(LogTest, testLogFlushDBidWithSnapshot) {
    // Test flush one db with snapshot pending
    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 0;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    size_t num_items = 500;
    size_t num_databases = 2;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    logFlush(log, 0);
    ASSERT_EQ(num_items / num_databases, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
}

TEST_P(LogTest, testLogShouldRunCronTasksImmediatelyDuringPendingRead) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 0;

    // Write a item to the log that will be read later
    writeItemsToLog(log, num_databases, 1);

    char *key = nullptr, *value = nullptr;
    size_t key_len = 0, value_len = 0;
    getGeneratedItemAtTime(0, key, key_len, value, value_len);
    requestContext context = { 0 };
    context.actual_value = value;
    context.actual_value_len = value_len;
    context.num_callback_triggered = 0;

    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
    ASSERT_EQ(logRead(log, 0, key, key_len, FC_READ, static_cast<void *>(&context),
                get_item_callback), FC_OK);
    ASSERT_TRUE(logShouldRunCronTasksImmediately(log));

    ASSERT_EQ(context.num_callback_triggered, 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_READ), 0);
    while (logGetCountBasedMetric(log, FC_NUM_DISK_READ) == 0) {
        ASSERT_TRUE(logShouldRunCronTasksImmediately(log));
        logRunCronTasks(log);
    }
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
    ASSERT_EQ(context.num_callback_triggered, 1);
}

TEST_P(LogTest, testLogShouldRunCronTasksImmediatelyDuringPendingFlushToLog) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 0;

    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    ASSERT_TRUE(logShouldRunCronTasksImmediately(log));

    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), 0);
    while (logGetCountBasedMetric(log, FC_NUM_DISK_WRITE) == 0) {
        ASSERT_TRUE(logShouldRunCronTasksImmediately(log));
        logRunCronTasks(log);
    }
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
}

TEST_P(LogTest, testLogShouldRunCronTasksImmediatelyDuringGarbageCollection) {
    // As there are no item in the database, garbage collection would not be running
    // so the cron task does not need to be run immediately.
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));

    size_t num_items = 500;
    writeItemsToLog(log, num_databases, num_items);

    while (!log->garbage_collector_info.is_running) {
        ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
        logRunCronTasks(log);
    }
    // Ensure that cron task should be run immediately when garbage collection
    // is running
    ASSERT_TRUE(logShouldRunCronTasksImmediately(log));
}

TEST_P(LogTest, testLogShouldRunCronTasksImmediatelyDuringSnapshotting) {
    // As there are no item in the database, garbage collection would not be running
    // so the cron task does not need to be run immediately.
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    ASSERT_EQ(snapshot_context.num_snapshot_completion_callback_invocation, 0);
    // Ensure that cron task should be run immediately during snapshotting
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        ASSERT_TRUE(logShouldRunCronTasksImmediately(log));
        logRunCronTasks(log);
    }
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
}

TEST_P(LogTest, testFreeDbSize) {
    size_t emptydb_free_db_size_bytes = LOG_SIZE_BYTES / 2;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), emptydb_free_db_size_bytes);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES), 0);

    size_t allocated_log_size_bytes = 1400;
    log->allocated_log_size_bytes = allocated_log_size_bytes;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), allocated_log_size_bytes);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), emptydb_free_db_size_bytes -
            allocated_log_size_bytes);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES), 0);

    size_t buffered_writes_size_bytes = 225;
    size_t allocated_db_size_bytes = allocated_log_size_bytes + buffered_writes_size_bytes;
    log->staging_buffer->total_item_size = buffered_writes_size_bytes;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), allocated_db_size_bytes);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), emptydb_free_db_size_bytes -
            allocated_db_size_bytes);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES), 0);

    log->allocated_log_size_bytes = emptydb_free_db_size_bytes;
    log->staging_buffer->total_item_size = 0;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES), 0);
    log->allocated_log_size_bytes = emptydb_free_db_size_bytes + 1;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES), 1);
}

TEST_P(LogTest, testWaitTillNoPendingIoAndGarbageCollection) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 1000;

    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    // When empty staging buffer is required
    EXPECT_GT(log->staging_buffer->total_item_size, 0u);
    ASSERT_TRUE(fioRequestIsEmpty(&(log->log_flush_fio_request)));
    ASSERT_FALSE(log->num_inflight_item_read_io_request);
    ASSERT_FALSE(log->garbage_collector_info.is_running);

    waitTillNoPendingIoAndGarbageCollection(log, 1);
    EXPECT_EQ(log->staging_buffer->total_item_size, 0u);

    // When empty staging buffer is not required
    key_len = 0; value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    ASSERT_TRUE(fioRequestIsEmpty(&(log->log_flush_fio_request)));
    ASSERT_FALSE(log->num_inflight_item_read_io_request);
    ASSERT_FALSE(log->garbage_collector_info.is_running);
    EXPECT_GT(log->staging_buffer->total_item_size, 0u);
    ASSERT_GT(logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES), 0u);
    waitTillNoPendingIoAndGarbageCollection(log, 0);
    EXPECT_GT(log->staging_buffer->total_item_size, 0u);
}

TEST_P(LogTest, testOverrideAllocatableDbSize) {
    size_t emptydb_free_db_size_bytes = LOG_SIZE_BYTES / 2;
    log->overriden_allocatable_log_size_bytes = -1;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), emptydb_free_db_size_bytes);

    log->overriden_allocatable_log_size_bytes = 1000;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), 1000);

    log->overriden_allocatable_log_size_bytes = -1;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_FREE_ALLOCATABLE_DB_SIZE_BYTES), emptydb_free_db_size_bytes);
}

TEST_P(SingleDbLogTest, testLogShouldRunCronTasksImmediatelyDuringIndexGrowth) {
    // As there are no item in the database, garbage collection would not be running
    // so the cron task does not need to be run immediately.
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    size_t curr_table_size = 128;
    size_t num_items = 5 * curr_table_size + 1;
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    flashcacheIndex *index = log->index_list[0];

    // Write sufficient items to log in order to start index growth
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);

    // Ensure that cron task should be run immediately during index growth
    while (index->growth_iterator->status != NOT_RUNNING) {
        ASSERT_TRUE(logShouldRunCronTasksImmediately(log));
        logRunCronTasks(log);
    }
    ASSERT_FALSE(logShouldRunCronTasksImmediately(log));
}

/**
 * This tests validates the following scenario:
 * 1. Let say we have 3 items (A, B, C) in a bucket that have the same collision hash.
 * 2. The order of the items in the bucket is the following: A -> B -> C
 * 3. A table growth operation is in progress.
 * 4. A read comes for C, but due to the collison A is being read first.
 * 5. At this time, the bucket is processed for table growth.
 * 6. All items are moved to the new bucket.
 * 7. When the read for A completes and we find that it is not the item that the read
 * request is for, the test validates that the system is able to issue read for the
 * other entries and find the item and return the correct value.
 */
TEST_P(SingleDbLogTest, testReadRequestWithItemsHavingSameCollisionHashAndGrowthInProgress) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 0;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t curr_table_size = 128;
    size_t dbid = 0;
    size_t num_items = 5 * curr_table_size;

    flashcacheIndex *index = log->index_list[dbid];
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    // Write sufficient items to log to start index growth later
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);  // Load factor has not been breached yet
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);

    // Paused the index growth
    index->growth_iterator->status = PAUSED;

    // Now Adding 3 items
    // Add first item to log with IndexHash = 0 and CollisionHash = 1
    setHashFunction(log, hash_function_for_collision_hash_as_one);
    num_items++;
    size_t num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    size_t time_of_creation = getLogicalTime();
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Get first key value to read this from this log as the first item added would be more far
    // from the head of the chain in the bucket in comparison to the 2nd and 3rd item added.
    char *first_key, *first_value;
    size_t key_len = 0, value_len = 0;
    getGeneratedItemAtTime(time_of_creation, first_key, key_len, first_value, value_len);

    // Add second item to log with IndexHash = 0 and CollisionHash = 1
    num_items++;
    num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Add third item to log with IndexHash = 0 and CollisionHash = 1
    num_items++;
    num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Trigger requests to read the first item.
    requestContext first_context{ first_value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, first_key, key_len, read_type,
                      static_cast<void *>(&first_context), completion_callback), FC_OK);

    // Unpause the index Growth
    index->growth_iterator->status = NOT_RUNNING;

    // Delaying the FIO processing so the read from flash for the first entry completes
    // after the bucket has been processed for table growth operation.
    mockFioSetMinRequestProcessingDelay(2);

    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION), 0);

    // Process the read request and initiate index growth
    while (first_context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_READ), 3);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION), 2);
}

/**
 * This validates that pending read request which was marked Invalid because of same collision hash
 * should succeed with index growth in place. Lets take an example and understand
 * K1 -> (KEY1, IH, CH) ,  K2 -> (KEY2, IH, CH1) ,  K3 -> (KEY3, IH, CH)
 * Current status of index
 *          Bucket A : K3 -> K2 -> K1
 * Get a read request for K3 and K1 at the same time.
 * Request for K3 will get processed and request for K1 will be marked Invalid.
 * Current status of index
 *          Bucket A : K2 -> K1
 * Now Start Index Growth and K1 will be moved to different bucket
 * Current status of index
 *          Bucket A : K2
 *          Bucket B : K1
 * Now pending read for K1 should succeed
 */
TEST_P(SingleDbLogTest, testReadRequestWithInvalidStateWithIndexGrowthInPlace) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 0;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t curr_table_size = 128;
    size_t dbid = 0;
    size_t num_items = 5 * curr_table_size;

    flashcacheIndex *index = log->index_list[dbid];
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    // Write sufficient items to log to start index growth later
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);  // Load factor has not been breached yet
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);

    // Paused the index growth
    index->growth_iterator->status = PAUSED;

    // Now Adding 3 items
    // Add first item to log with IndexHash = 0 and CollisionHash = 1
    setHashFunction(log, hash_function_for_collision_hash_as_one);
    num_items++;
    size_t num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    size_t time_of_creation = getLogicalTime();
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Get first key value which will be required while reading them.
    char *first_key, *first_value;
    size_t key_len = 0, value_len = 0;
    getGeneratedItemAtTime(time_of_creation, first_key, key_len, first_value, value_len);

    // Add second item to log with IndexHash = 0 and CollisionHash = 0
    setHashFunction(log, hash_function_for_collision_hash_as_zero);
    num_items++;
    num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Add third item to log with IndexHash = 0 and CollisionHash = 1
    setHashFunction(log, hash_function_for_collision_hash_as_one);
    num_items++;
    num_disk_write_before_log_write = logGetCountBasedMetric(log, FC_NUM_DISK_WRITE);
    time_of_creation = getLogicalTime();
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_WRITE), num_disk_write_before_log_write + 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Get third key value which will be required while reading them.
    char *third_key, *third_value;
    key_len = 0; value_len = 0;
    getGeneratedItemAtTime(time_of_creation, third_key, key_len, third_value, value_len);

    // Trigger requests to read the third and first item.
    requestContext first_context{ first_value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, first_key, key_len, read_type,
                      static_cast<void *>(&first_context), completion_callback), FC_OK);
    requestContext third_context{ third_value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, third_key, key_len, read_type,
                      static_cast<void *>(&third_context), completion_callback), FC_OK);

    // Unpause the index Growth
    index->growth_iterator->status = NOT_RUNNING;

    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION), 0);

    mockFioSetMinRequestProcessingDelay(2);  // Delaying the FIO processing

    // Read the third request and initiate index growth
    while (third_context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_READ), 2);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION), 1);

    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);  // Index growth initiated
    ASSERT_GT(index->growth_iterator->next_hash_bucket, 0);  // Bucket 0 has been processed

    // Complete the first request which was marked Invalid due to same collision hash
    ASSERT_EQ(first_context.num_callback_triggered, 0);
    while (first_context.num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(first_context.num_callback_triggered, 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_DISK_READ), 3);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_UNUSED_DISK_READ_HASH_COLLISION), 1);
}

TEST_P(SingleDbLogTest, testLargeItemReadAndWriteWithCompleteIndexGrowthOperation) {
    size_t curr_table_size = 128;
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    flashcacheIndex *index = log->index_list[0];
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    size_t current_time = getLogicalTime();
    size_t num_items = 5 * curr_table_size;

    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);  // Load factor not been breached yet
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);

    // Adding 1 more item to breach the load factor
    num_items++;
    writeItemsToLog(log, num_databases, 1);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);  // Load factor has been breached now.
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);

    while (indexTableSize(index) != curr_table_size * 2) {
        logRunCronTasks(log);
    }

    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    if (read_type == FC_READ) {
        testItemRead(num_items, current_time);
    } else {
        testItemDelete(num_items, current_time);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(SingleDbLogTest, testLargeItemReadAndWriteWithMultipleTimesIndexGrowthOperation) {
    // Turn off garbage collection to prevent it generating the need for running
    // the cron task
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Set staging buffer flush size to 0 so that each item written is flushed to log
    log->staging_buffer_flush_size_threshold_bytes = 0;

    size_t curr_table_size = 128;
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    flashcacheIndex *index = log->index_list[0];
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    size_t current_time = getLogicalTime();
    size_t num_items = 4096 * curr_table_size + 1;

    // Paused the index growth operation till items are written in log
    index->growth_iterator->status = PAUSED;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(index->growth_iterator->status, PAUSED);
    ASSERT_EQ(indexTableSize(index), 128);

    // UnPausing the index growth operation
    index->growth_iterator->status = NOT_RUNNING;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);

    while ((indexTableSize(index) * 5) < num_items) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 10);
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);

    if (read_type == FC_READ) {
        testItemRead(num_items, current_time);
    } else {
        testItemDelete(num_items, current_time);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
}

TEST_P(SingleDbLogTest, testLargeItemReadAndWriteWithPartialIndexGrowthOperation) {
    size_t curr_table_size = 128;
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    flashcacheIndex *index = log->index_list[0];
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    size_t current_time = getLogicalTime();
    size_t num_items = 5 * curr_table_size + 1;

    // Paused Growth operation till items are writen to log
    index->growth_iterator->status = PAUSED;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(indexTableSize(index), curr_table_size);

    // UnPaused Growth operation
    index->growth_iterator->status = NOT_RUNNING;
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);

    // Wait for Partial Index Growth
    while (index->growth_iterator->next_hash_bucket <= 64) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
    ASSERT_LT(index->growth_iterator->next_hash_bucket, curr_table_size - 1);

    // Paused Growth operation till items are read
    index->growth_iterator->status = PAUSED;
    if (read_type == FC_READ) {
        testItemRead(num_items, current_time);
    } else {
        testItemDelete(num_items, current_time);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Resuming the growth operation
    index->growth_iterator->status = RUNNING;
    while (indexTableSize(index) != curr_table_size * 2) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 1);
}

TEST_P(SingleDbLogTest, testIndexGrowthDoesNotStartWhileSnapshotIsRunning) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    size_t curr_table_size = 128;
    size_t num_items = 5 * curr_table_size;
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    flashcacheIndex *index = log->index_list[0];

    // Pause the Growth operation till items gets writen to log
    index->growth_iterator->status = PAUSED;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Unpause the Growth operation
    index->growth_iterator->status = NOT_RUNNING;

    // Start snapshot
    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);
    // Write some item to the log during snapshotting and verify index growth does not start.
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
        writeItemsToLog(log, num_databases, 1);
    }

    // Growth iterator gets started after completion of snapshot
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
}

TEST_P(LogTestWithDifferentGarbageCollectionRate, readAndWriteLargeNumberOfItems) {
    testReadAndWriteWithLargeNumberOfItems(1000, true);
}

TEST_P(LogTestWithDifferentGarbageCollectionRate, readAndWriteLargeNumberOfItemsWithHeadWrappingToFrontOfLogFile) {
    // Set head and tail to the last page of the log file
    log->head_offset = log->tail_offset = (log->log_size_bytes - FC_PAGESIZE);

    testReadAndWriteWithLargeNumberOfItems(1000, false);

    waitForGarbageCollectionToCleanUpLog();
}

TEST_P(LogTestWithDifferentGarbageCollectionRate, testHeadWrappingToFrontOfLogFile) {
    // Reduce the log size to 512 KiB
    log->log_size_bytes = (1LL << 19);

    for (int i = 0; i < 4; ++i) {
        // In each epoch, we write and read (which deletes) 200 KiB of data into log.
        // This operation is performed multiple times so that the we wrap the head multiple times
        // to the front of the log.
        testReadAndWriteWithLargeNumberOfItems(1000, false);
    }

    waitForGarbageCollectionToCleanUpLog();
}

TEST_P(LogTestWithDifferentGarbageCollectionRate,
        readAndWriteLargeNumberOfItemsWithHeadWrappingUsingEndOfLogFileMarker) {
    // Set head and tail to the last page of the log file
    log->head_offset = log->tail_offset = (log->log_size_bytes - FC_PAGESIZE);

    // Set value len to be greater than the page size to ensure the end of log file marker is written
    // in the last page of the log file
    setValueLen(FC_PAGESIZE + 10);

    testReadAndWriteWithLargeNumberOfItems(1000, false);
}

TEST_P(LogTestWithDifferentItemSize, readAndWriteLargeNumberOfItems) {
    // Reduce the log size to 10 MiB
    log->log_size_bytes = 10 * MEGABYTE_TO_BYTES;

    for (int i = 0; i < 20; ++i) {
        testReadAndWriteWithLargeNumberOfItems(100, true);
    }

    waitForGarbageCollectionToCleanUpLog();
}

TEST_P(LogTestWithDifferentItemSize, testReadAndWriteWhenLogWrittenFromNonZeroOffset) {
    size_t total_item_len = getTotalItemLen();
    size_t page_aligned_total_item_len = ((total_item_len / FC_PAGESIZE) +
        (total_item_len % FC_PAGESIZE ? 1 : 0)) * FC_PAGESIZE;
    log->head_offset = log->tail_offset = log->log_size_bytes - page_aligned_total_item_len - 2 * FC_PAGESIZE;

    testReadAndWriteWithLargeNumberOfItems(1000, true);
    waitForGarbageCollectionToCleanUpLog();
}

TEST_P(LogTestWithReplication, testEndToEndThreadsaveInFlashcache) {
    // Start by adding 1 item the the log.
    uint32_t dbid = 0;
    char const *key = "Hello", *value = "World!";
    size_t key_len = strlen(key), value_len = strlen(value);
    ASSERT_EQ(logWrite(log, dbid, key, key_len, value, value_len), FC_OK);

    // Total 51 items written to log.
    size_t num_items = 50;
    writeItemsToLog(log, num_databases, num_items);

    snapshotContext save_context = { 0 };
    snapshotContext *snapshot_context = &save_context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                    snapshot_context, snapshot_filename), "");
        return;
    }
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Run the log cron task a few times to make sure the first item is already iterated.
    for (int i = 0; i < 4; ++i) {
        logRunCronTasks(log);
    }

    // Now the snapshot should not be completed and all the THREADSAVE related metrics
    // should be 0.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    ASSERT_EQ(num_items + 1, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Read the first item (item is processed) to check if addReplicationCommandIfRequired
    // sends the correct DELETE replication command to the snapshot.
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);

    // Wait for snapshotting to complete.
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    // Number of read requests and num items will be the same for
    // BGSAVE and THREADSAVE right after snapshotting.
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // If THREADSAVE replication, we expect DELETE repl command in snapshot and the
    // same item should be sent to RDB too.
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
        // As `has_snapshotting_completed_in_redis_layer` was true in the test,
        // log iteration callback will not be invoked
        ASSERT_EQ(0, snapshot_context->num_log_iteration_completion_callback_invocation);
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    }

    // We ended the snapshot, the curr THREADSAVE metrics should reset to zero.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));

    // Now we try to write 1 new item after the snapshot and read that item.
    char const *new_key = "Hi", *new_value = "America!";
    size_t new_key_len = strlen(new_key), new_value_len = strlen(new_value);
    ASSERT_EQ(logWrite(log, dbid, new_key, new_key_len, new_value, new_value_len), FC_OK);

    // Read the newly added item. This item is not in the snapshot range.
    requestContext new_context{ new_value, new_value_len, 0 };
    ASSERT_EQ(logRead(log, dbid, new_key, new_key_len, FC_READ,
                static_cast<void *>(&new_context), get_item_callback), FC_OK);

    // Since we are reading an item that is not in the snapshot range, we do not send
    // a DELETE replication cmd and do not flag the new item with add_to_rdb flag.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));

    size_t expected_num_items = num_items + 1
                        - logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD)
                        - logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE);

    // Load the snapshot
    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_verification_result = 0;
    logLoadSnapshot(log,
                    snapshot_filename,
                    &snapshot_secret_from_load,
                    &checksum_verification_result);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));
    ASSERT_EQ(expected_num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_DELETE_REQUEST));
        // Key has value NULL since it was deleted by DELETE command.
        requestContext deleted_context{ NULL, 0, 0 };
        ASSERT_EQ(logRead(log, dbid, key, key_len, FC_READ,
                static_cast<void *>(&deleted_context), get_empty_item_callback), FC_OK);
    } else {
        ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    }
}

TEST_P(LogTestWithReplication, testSnapshotWithEvictionOnPastItems) {
    // Skip snapshot version 1 for threadsave
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        return;
    }

    // Total 50 items written to log.
    size_t num_items = 50;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, num_items);
    log->garbage_collector_info.can_start_garbage_collection = 1;

    snapshotContext save_context = { 0 };
    snapshotContext *snapshot_context = &save_context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Capture old tail offset and snapshot start offset
    size_t old_tail_offset = 0, old_log_file_tail_offset = 0;
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    snapshotVersionTwoInfo *snapshot_v2_info;
    if (isTestingThreadsaveReplication()) {
        snapshot_v2_info = snapshot_manager_info->snapshot_version_two_info;
        old_tail_offset = log->tail_offset;
        ASSERT_NE(snapshot_v2_info, nullptr);
        old_log_file_tail_offset = snapshot_v2_info->snapshot_common.log_file_tail_offset;
        ASSERT_EQ(old_log_file_tail_offset, old_tail_offset);
    }

    // Run the log cron task a few times to make sure the first item is already iterated.
    for (int i = 0; i < 4; ++i) {
        logRunCronTasks(log);
    }

    // Now the snapshot should not be completed and all the THREADSAVE related metrics
    // should be 0.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Write a new item, and evict an item from the tail of the log
    log->overriden_allocatable_log_size_bytes = logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES) + 1;
    char const *key = "Hello", *value = "World!";
    size_t key_len = strlen(key), value_len = strlen(value);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    log->garbage_collector_info.garbage_collection_bytes_in_current_second = 0;
    while (eviction_context.key_list.size() < 1) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    // If THREADSAVE replication, we expect DELETE replication command in snapshot and the
    // same item should not be sent to RDB as it is evicted/deleted.
    if (isTestingThreadsaveReplication()) {
        ASSERT_NE(snapshot_v2_info, nullptr);
        ASSERT_EQ(snapshot_v2_info->snapshot_common.log_file_tail_offset, log->tail_offset);
        ASSERT_NE(old_tail_offset, log->tail_offset);
        ASSERT_NE(old_log_file_tail_offset, snapshot_v2_info->snapshot_common.log_file_tail_offset);
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    }

    // Wait for snapshotting to complete.
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    // Number of items will be less due to evictions during snapshotting
    int final_num_items = logGetCountBasedMetric(log, FC_NUM_ITEMS);
    ASSERT_TRUE(final_num_items <= num_items);

    // If THREADSAVE replication, we expect DELETE replication commands in snapshot and the
    // same items should not be sent to RDB.
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(1 + num_items - final_num_items, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    }

    // We ended the snapshot, the curr THREADSAVE metrics should reset to zero.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // Reload the snapshot
    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_verification_result = 0;
    logLoadSnapshot(log, snapshot_filename, &snapshot_secret_from_load, &checksum_verification_result);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(final_num_items - 1, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    } else {
        ASSERT_EQ(50, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    }
    freeEvictionContext();
}

TEST_P(LogTest, testSnapshotWithEvictionOnPendingItems) {
    if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        // This tests the point in time snapshot with item being moved
        // because of GC at the same time in Snapshot Version Two. No need to
        // run it for Snapshot Version One
        return;
    }
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, 1000);
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 10 * 4096;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(log->tail_offset, 0);
    // Save snapshot and load it back
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);

    // Change the tail offset to move ahead of Snapshot Iterator which will start from 0 offset.
    ASSERT_GT(logGetCountBasedMetric(log, FC_ACTIVE_DB_SIZE_BYTES), (10 * FC_PAGESIZE));
    log->tail_offset = log->head_offset - (10 * FC_PAGESIZE);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    // Run eviction. Expect that the eviction of items doesn't propagate the DELETE command
    log->overriden_allocatable_log_size_bytes = logGetCountBasedMetric(log, FC_ALLOCATED_DB_SIZE_BYTES) - 10;
    while (eviction_context.key_list.size() < 1) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_ITEMS_EVICTED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));

    // Wait for snapshot to complete, and reload the snapshot
    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    int num_items = logGetCountBasedMetric(log, FC_NUM_ITEMS);

    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_verification_result = 0;
    logLoadSnapshot(log, snapshot_filename, &snapshot_secret_from_load, &checksum_verification_result);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));
    if (snapshot_writer != NULL) {
        ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    } else {
        ASSERT_EQ(1000, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    }
    freeEvictionContext();
}

TEST_P(LogTestWithReplication, testDisablingGarbageCollectionDuringThreadsaveReplication) {
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, 1000);
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 10 * 4096;
    log->garbage_collector_info.can_start_garbage_collection = 1;

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));

    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                                  &snapshot_context, snapshot_filename), "");
        return;
    }
    // Save snapshot and load it back
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            &snapshot_context, snapshot_filename);

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    if (isTestingThreadsaveReplication()) {
        // Thread save replication case
        ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
        ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 0);
        while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
            ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
            ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 0);
            logRunCronTasks(log);
        }
        ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
        ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 1);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_WRITE_BYTES), 0);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED), 0);
    } else {
        // Other cases like Snapshotting or replication with Bgsave,
        // Snapshotting with Threadsave in both V1 and V2
        ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
        ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 1);
        while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
            ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
            ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 1);
            logRunCronTasks(log);
        }
        ASSERT_EQ(log->garbage_collector_info.can_start_garbage_collection, 1);
        ASSERT_EQ(log->garbage_collector_info.can_do_log_compaction, 1);
        ASSERT_GT(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_WRITE_BYTES), 0);
        ASSERT_GT(logGetCountBasedMetric(log, FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED), 0);
    }

    // Load the snapshot and it should contain all 1000 items.
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, 1000, expected_hasher_seed);
}

TEST_P(LogTestWithSnapshotting, testSnapshotCancellation) {
    size_t num_items = 5000;
    size_t current_time = getLogicalTime();
    writeItemsToLog(log, num_databases, num_items);

    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 0;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCEL_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    size_t num_save_cancellation = 10;
    // Start save and cancel it 10 times. This would ensure that cancelling save does not leave the snapshot info
    // in an unexpected state.
    for (size_t i = 0; i < num_save_cancellation; ++i) {
        if (i % 2 == 0) {
            // Introduce delay every other run so that there are pending request for log read/snapshot write in FIO
            // during cancellation
            mockFioSetMinRequestProcessingDelay(3);
        } else {
            mockFioSetMinRequestProcessingDelay(0);
        }

        logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                snapshot_context, snapshot_filename);
        ASSERT_EQ(i + 1, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));

        // Write few item more item to the log. This triggers the cron task for few cycle.
        writeItemsToLog(log, num_databases, 3);
        num_items += 3;

        ASSERT_EQ(i, snapshot_context->num_snapshot_completion_callback_invocation);
        logCancelSave(log);
        ASSERT_EQ(i + 1, snapshot_context->num_snapshot_completion_callback_invocation);
        ASSERT_EQ(i + 1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCEL_REQUEST));
        ASSERT_EQ(i + 1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    }

    std::vector<requestContext *> request_contexts;
    // Let the save complete this time to ensure that save is able to complete when prior saves were cancelled
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Wait for save to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != (num_save_cancellation + 1)) {
        logRunCronTasks(log);
    }

    // Expect all the items written to be present in the log after completion of snapshot
    for (size_t i = current_time; i < getLogicalTime(); ++i) {
        request_contexts.push_back(readItemFromLog(log, num_databases, i, get_item_callback));
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);

    // Expect all the items written to be present in the log after loading the snapshot
    for (size_t i = current_time; i < getLogicalTime(); ++i) {
        request_contexts.push_back(readItemFromLog(log, num_databases, i, get_item_callback));
    }
    validateSnapshotWriterCallbackInvocation(num_save_cancellation + 1, num_save_cancellation + 1);
    validateAllReadRequestCompleted(request_contexts);
    cleanUpRequestContexts(request_contexts);
}

TEST_P(LogTestWithSnapshotting, testSnapshotCancellationOnLoad) {
    size_t num_items = 5000;
    writeItemsToLog(log, num_databases, num_items);

    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);
    // wait for snapshotting to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    const char *new_snapshot_filename = "another_snapshot.fdb";
    if (snapshot_writer != nullptr) {
        releaseMockSnapshotWriter(snapshot_writer);
        setSnapshotWriter(true, new_snapshot_filename);
    }

    // Start a new snapshotting with the expectation that it would be cancelled
    snapshot_context->expected_completion_status = 0;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, new_snapshot_filename);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));

    // Start loading the old snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);

    // Ensure that the snapshotting was cancelled
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
}

TEST_P(LogTestWithSnapshotting, testSnapshotKeepaliveAllSaveTypes) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t num_items = 10000;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                    snapshot_context, snapshot_filename), "");
        return;
    }
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 0
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    // Run cron tasks to make sure snapshotting completes for V1 and file based snapshotting
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE || snapshot_writer == NULL) {
        while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
            logRunCronTasks(log);
        }
    } else {
        snapshotVersionTwoInfo *snapshot_info = snapshot_manager_info->snapshot_version_two_info;
        snapshot_info->snapshot_keep_alive_msg_interval_us = 1000000;

        // Generally takes ~10 iterations for one message to be sent.
        int count = 100;
        while (count > 0) {
            logRunCronTasks(log);
            count--;
        }

        // Check to see that keepalive message has been sent
        ASSERT_GT(snapshot_manager_info->snapshot_version_two_info->num_replication_link_keep_alive_msg, 0);
        logCompleteForklessSaveReplication(log);
    }
}

TEST_P(LogTestWithReplication, testSnapshotCompletionOnlyAfterRedisCompletesSnapshotting) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t num_items = 5;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    setFlashCacheSnapshotSaveType(FC_SAVE_TYPE_FORKLESS_SAVE);
    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_START_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCEL_REQUEST));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                    snapshot_context, snapshot_filename), "");
        return;
    }
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 0
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    // Run cron tasks to make sure snapshotting completes for V1 and file based snapshotting
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE || snapshot_writer == NULL) {
        while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
            logRunCronTasks(log);
        }
    } else {
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 0);
        int counter = 50000;  //  Generally it takes 19000 iteration for completion of snapshot
        while (counter--) {
            logRunCronTasks(log);
        }
        // Even after multiple iteration of cron job snapshot will not be completed as Redis layer has not completed
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 0);
        ASSERT_EQ(snapshot_context->num_log_iteration_completion_callback_invocation, 1);

        // Make sure that we sent keep alive to the replica's storage IO thread while waiting for the engine snapshot to complete
        ASSERT_GT(snapshot_manager_info->snapshot_version_two_info->num_replication_link_keep_alive_msg, 0);

        logCompleteForklessSaveReplication(log);

        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 1);
        ASSERT_EQ(snapshot_context->num_log_iteration_completion_callback_invocation, 1);
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);
    validateSnapshotWriterCallbackInvocation(1, 1);
}

TEST_P(LogTestWithReplication, testReplicationLinkKeepAliveWithinMaxTimeout) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t num_items = 5;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    setFlashCacheSnapshotSaveType(FC_SAVE_TYPE_FORKLESS_SAVE);
    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                    snapshot_context, snapshot_filename), "");
        return;
    }
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    snapshotVersionTwoInfo *snapshot_info = snapshot_manager_info->snapshot_version_two_info;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 0
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    // Run cron tasks to make sure snapshotting completes for V1 and file based snapshotting
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE || snapshot_writer == NULL) {
        while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
            logRunCronTasks(log);
        }
    } else {
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 0);

        // Keep alive message sent every second, stops after 400 seconds.
        snapshot_info->snapshot_keep_alive_msg_interval_us = 1000000;
        snapshot_info->replication_link_timeout_secs = 400;

        // Every logRunCronTasks increases the clock by 100us,
        // so we let cron runs 20k times to stimulate 2 seconds.
        uint64_t current_time_us = mockClockGetTimeUs();
        mockClockSetIncrementPerGetTimeCallUs(0);
        int count = 20000;
        while (count > 0) {
            logRunCronTasks(log);
            mockClockIncrementTimeInUs(100);
            count--;
        }

        // The current time now should be 2 seconds after we started to wait for Redis snapshotting
        ASSERT_TRUE(mockClockGetTimeUs() >= current_time_us + (2 * FC_SECOND_TO_MICROSECOND)
                    && mockClockGetTimeUs() < current_time_us + (3 * FC_SECOND_TO_MICROSECOND));

        // Total message sent = num secs
        ASSERT_EQ(snapshot_manager_info->snapshot_version_two_info->num_replication_link_keep_alive_msg, 2);

        logCompleteForklessSaveReplication(log);
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 1);
        ASSERT_EQ(snapshot_context->num_log_iteration_completion_callback_invocation, 1);
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);
    validateSnapshotWriterCallbackInvocation(1, 1);
}

TEST_P(LogTestWithReplication, testReplicationLinkKeepAlivePastMaxTimeout) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    size_t num_items = 5;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    setFlashCacheSnapshotSaveType(FC_SAVE_TYPE_FORKLESS_SAVE);
    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Expect crash when we do THREADSAVE stream-based replication in snapshot V1.
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                    snapshot_context, snapshot_filename), "");
        return;
    }
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
    snapshotVersionTwoInfo *snapshot_info = snapshot_manager_info->snapshot_version_two_info;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 0
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
    config.numeric_value = 0;
    logSetConfig(log, &config);

    // Run cron tasks to make sure snapshotting completes for V1 and file based snapshotting
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE || snapshot_writer == NULL) {
        while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
            logRunCronTasks(log);
        }
    } else {
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 0);

        // Keep alive message sent every second, stops after 2 seconds.
        snapshot_info->snapshot_keep_alive_msg_interval_us = 1000000;
        snapshot_info->replication_link_timeout_secs = 2;

        // Every logRunCronTasks increases the clock by 100us,
        // so we let cron runs 100k times to stimulate 10 seconds.
        uint64_t current_time_us = mockClockGetTimeUs();
        mockClockSetIncrementPerGetTimeCallUs(0);
        int count = 100000;
        while (count > 0) {
            logRunCronTasks(log);
            mockClockIncrementTimeInUs(100);
            count--;
        }

        // The current time now should be 10 seconds after we started to wait for Redis snapshotting
        ASSERT_TRUE(mockClockGetTimeUs() >= current_time_us + (10 * FC_SECOND_TO_MICROSECOND)
                    && mockClockGetTimeUs() < current_time_us + (11 * FC_SECOND_TO_MICROSECOND));

        // Total message sent is only 2 because we stopped sending those at the 2nd second.
        ASSERT_EQ(snapshot_manager_info->snapshot_version_two_info->num_replication_link_keep_alive_msg, 2);

        logCompleteForklessSaveReplication(log);
        ASSERT_EQ(snapshot_context->num_snapshot_completion_callback_invocation, 1);
        ASSERT_EQ(snapshot_context->num_log_iteration_completion_callback_invocation, 1);
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);
    validateSnapshotWriterCallbackInvocation(1, 1);
}

TEST_P(LogTestWithSnapshotting, testSnapshotCompletionAndLoadingWithNoItems) {
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);
    // wait for snapshotting to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, 0, expected_hasher_seed);
    validateSnapshotWriterCallbackInvocation(1, 1);
}

TEST_P(LogTestWithSnapshotting, testSnapshotCompletionAndLoadingWithIndexGrowth) {
    adjustSnapshotVersion(flashcache_snapshot_version);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();

    size_t curr_table_size = 128;
    size_t num_items = 12 * curr_table_size;
    size_t num_databases = 2;
    log->num_databases = num_databases;
    snapshot_manager_info->snapshot_version_one_info->snapshot_common.num_databases = num_databases;
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    flashcacheIndex *index = log->index_list[0];

    // Pause the Growth operation till items gets writen to log
    index->growth_iterator->status = PAUSED;
    writeItemsToLog(log, num_databases, num_items);
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    // Unpause the Growth operation
    index->growth_iterator->status = NOT_RUNNING;
    logRunCronTasks(log);

    // Ensure partial growth operation before snapshot starts
    ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);
    ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
    ASSERT_EQ(indexTableSize(index), curr_table_size);
    ASSERT_LT(index->growth_iterator->next_hash_bucket, curr_table_size);

    // Start snapshot
    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);
    size_t next_hash_bucket_at_snapshot = 0;
    size_t collision_bits_used_at_snapshot = 0;
    size_t base_size_bits_at_snapshot = 0;
    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        // Growth iterator gets paused when we start snapshot
        ASSERT_EQ(index->growth_iterator->status, PAUSED);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);

        // Growth Related Values which are written at the time of Snapshot
        next_hash_bucket_at_snapshot = index->growth_iterator->next_hash_bucket;
        collision_bits_used_at_snapshot = index->collision_bits_used;
        base_size_bits_at_snapshot = index->base_size_bits;
    }
    // Wait for snapshotting to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
            ASSERT_EQ(index->growth_iterator->status, PAUSED);
            ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 0);
            ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
        }
        logRunCronTasks(log);
    }

    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        // Growth iterator gets resumed after completion of snapshot
        ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 0);
    }

    // Load the snapshot
    uint8_t expected_hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];
    log->hasher.get_seed(expected_hasher_seed);
    loadSnapshot(log, num_items, expected_hasher_seed);

    if (snapshot_manager_info->snapshot_version == FC_SNAPSHOT_VERSION_ONE) {
        ASSERT_EQ(log->current_index_growth_dbid, 0);
        index = log->index_list[log->current_index_growth_dbid];

        // Ensure same index growth related info has been loaded as captured during snapshot
        ASSERT_EQ(logGetCountBasedMetric(log, FC_IS_INDEX_GROWING), 1);  // After loading, growth iterator is running.
        ASSERT_EQ(index->growth_iterator->next_hash_bucket, next_hash_bucket_at_snapshot);
        ASSERT_EQ(index->collision_bits_used, collision_bits_used_at_snapshot);
        ASSERT_EQ(index->base_size_bits, base_size_bits_at_snapshot);

        // Wait for index Growth operation to complete
        while (index->growth_iterator->status != NOT_RUNNING) {
            logRunCronTasks(log);
        }
        ASSERT_EQ(index->collision_bits_used, collision_bits_used_at_snapshot + 1);
        ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);
        ASSERT_EQ(log->current_index_growth_dbid, 1);
        ASSERT_EQ(logGetCountBasedMetric(log, FC_NUM_INDEX_GROWTH_RUN), 1);
    }
    validateSnapshotWriterCallbackInvocation(1, 1);
    log->num_databases = this->num_databases;
}

TEST_P(LogTestWithSnapshotting, testLoadingSnapshotWithLessNumDatabases) {
    testSnapshotCompletionAndLoadingWithReadAndWrite(5000, true, log->num_databases + 10);
}

TEST_P(LogTestWithSnapshottingBufferLimit, testLoadingSnapshotWithStagingBufferLimit) {
    // The more reads per write we do, the faster we fill the staging buffer
    const size_t number_of_reads_per_write = 10;
    const size_t buffer_units_in_staging_buffer = 2;  // Keep it small to have short running tests
    // Create enough items to fill at least 10 times staging buffer
    const size_t number_of_items =
        10 * buffer_units_in_staging_buffer * FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES / getValueLen();
    // Set staging buffer to small fraction of total size to ensure the buffer gets filled
    size_t original_staging_buffer_size = flashcache_snapshot_config.load_staging_buffer_max_size;
    flashcache_snapshot_config.load_staging_buffer_max_size =
        buffer_units_in_staging_buffer * FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES;
    size_t original_queue_depth = flashcache_snapshot_config.load_queue_depth;
    flashcache_snapshot_config.load_queue_depth = buffer_units_in_staging_buffer;
    snapshotThrottleLoadWrite(number_of_reads_per_write);
    testSnapshotCompletionAndLoadingWithReadAndWrite(number_of_items, true, log->num_databases + 10);
    ASSERT_GT(snapshot_metrics.max_load_staging_buffer_size, 0);
    // In Snapshot v2 we go past the staging buffer size by the size of FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES
    // by design.
    ASSERT_LE(snapshot_metrics.max_load_staging_buffer_size,
              flashcache_snapshot_config.load_staging_buffer_max_size + FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    // Restore original staging size
    flashcache_snapshot_config.load_staging_buffer_max_size = original_staging_buffer_size;
    flashcache_snapshot_config.load_queue_depth = original_queue_depth;
    snapshotUnthrottleLoadWrite();
}

TEST_P(LogTestWithSnapshotting, testLoadingSnapshotWithGreaterNumDatabases) {
    ASSERT_DEATH(testSnapshotCompletionAndLoadingWithReadAndWrite(5000, true, log->num_databases - 1), "");
}

TEST_P(LogTestWithSnapshotting, testSnapshotCompletionAndLoadingWithReadAndWrite) {
    testSnapshotCompletionAndLoadingWithReadAndWrite(5000, true, log->num_databases);
}

TEST_P(LogTestWithSnapshotting, testSnapshotCompletionAndLoadingWithReadAndWriteAndMultiSnapshotVersion) {
    testSnapshotCompletionAndLoadingWithReadAndWrite(5000, true, log->num_databases);

    // Toggle the snapshot version.
    flashcacheSnapshotVersion new_snapshot_version = (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE ?
                                                              FC_SNAPSHOT_VERSION_TWO : FC_SNAPSHOT_VERSION_ONE);
    adjustSnapshotVersion(new_snapshot_version);

    resetSnapshotWriterCounters();
    testSnapshotCompletionAndLoadingWithReadAndWrite(5000, true, log->num_databases);
}

TEST_P(LogTestWithLargeNumberOfItems, testSnapshotCompletionAndLoadingToExistingLogWithReadAndWrite) {
    testSnapshotCompletionAndLoadingWithReadAndWrite(200000, false, log->num_databases);
}

TEST_P(LogTestWithLargeNumberOfItems, testSnapshotCompletionAndLoadingToNewLogWithReadAndWrite) {
    testSnapshotCompletionAndLoadingWithReadAndWrite(200000, true, log->num_databases);
}

TEST_P(LogTestWithLargeNumberOfItems, testWritePathThrottling) {
    flashcacheConfig conf = {};
    conf.key = FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES;
    logGetConfig(log, &conf);
    ASSERT_EQ(conf.numeric_value, 512 * 1024 * 1024);

    size_t throttling_threshold_bytes = 128 * 1024 * 1024;
    conf.numeric_value = static_cast<ssize_t>(throttling_threshold_bytes);
    logSetConfig(log, &conf);
    conf.numeric_value = 0;
    logGetConfig(log, &conf);
    ASSERT_EQ(conf.numeric_value, throttling_threshold_bytes);

    // Pause the processing of fio requests so that the writes get staged in memory
    mockFioPauseRequestProcessing();
    // Set the value len to 1 MiB so that we reach the throttling limit faster
    setValueLen(1024 * 1024);
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    // As the throttling limit is 128 MiB, we test that we are able to write 128 items
    for (size_t i = 0; i < 128; ++i) {
        generateRandomItem(key, key_len, value, value_len);
        ASSERT_LT(logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES),
                throttling_threshold_bytes);
        ASSERT_EQ(logWrite(log, i % num_databases, key, key_len, value, value_len),
                FC_OK);
    }
    ASSERT_GT(logGetCountBasedMetric(log, FC_ITEM_PENDING_FLUSH_SIZE_BYTES),
            throttling_threshold_bytes);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len),
            FC_ERR_THROTTLED);
    // Unpause the processing of fio requests so that any inflight write is completed during
    // releasing the log
    mockFioUnpauseRequestProcessing();
}

TEST_P(LogTestWithLargeNumberOfItems, testSnapshotCancellationOnIOFailure) {
    size_t num_items = 20000;
    writeItemsToLog(log, num_databases, num_items);

    snapshotContext context = { 0 };
    snapshotContext *snapshot_context = &context;
    snapshot_context->expected_completion_status = 0;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Allow successful start of snapshotting
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Fail the snapshotting during writing to snapshot file
    enableMockFio(0, true, snapshot_filename);

    // wait for snapshotting to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    // Ensure that the snapshotting was cancelled
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));

    // Fail the snapshotting at the start of snapshotting
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // wait for snapshotting to complete
    while (snapshot_context->num_snapshot_completion_callback_invocation != 2) {
        logRunCronTasks(log);
    }

    // Ensure that the snapshotting was cancelled
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_SAVE_NUM_CANCELLED));

    // Ensure that failure in creating a fio context for reading log file causes a crash
    enableMockFio(0, true, log_filename);
    ASSERT_DEATH(logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
                snapshot_context, snapshot_filename), "");

    // Allow successful start of snapshotting
    enableMockFio(0, true, "");
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Ensure that failure in reading log file causes a crash
    enableMockFio(0, true, log_filename);
    ASSERT_DEATH(logRunCronTasksInALoop(log), "");

    // Wait for snapshotting to complete as snapshot_callback_details is on the stack of this function.
    enableMockFio(0, true, "");
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;
    while (snapshot_context->num_snapshot_completion_callback_invocation != 3) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testMinGarbageCollectionRateConfig) {
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 1;
    log->max_allocated_log_size_percent = 50;

    // verify GC defaults to min gc rate (4 KiB per second)
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Increase the min garbage collection rate config to 8KiB per second
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_MIN_GARBAGE_COLLECTION_RATE;
    config.numeric_value = 8192;
    logSetConfig(log, &config);
    updateGarbageCollectionRate(log);
    ASSERT_EQ(8192, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set storage overhead ratio to 100%. Verify
    log->head_offset = 1500;
    log->tail_offset = 127;
    log->log_size_bytes = 1373;
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(MAX_DYNAMIC_GC_RATE, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // See if new min config is still activated
    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(8192, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Update minGC one more time and verify the new value is used
    config.key = FC_CONFIG_KEY_MIN_GARBAGE_COLLECTION_RATE;
    config.numeric_value = 16384;
    logSetConfig(log, &config);
    updateGarbageCollectionRate(log);
    ASSERT_EQ(16384, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
}

TEST_P(LogTest, testDynamicGarbageCollectionRateDifferentOverheads) {
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 1;
    log->max_allocated_log_size_percent = 50;

    // Run garbage collection calculations using default gc rate (4 KiB per second)
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set storage overhead ratio to 100% and garbage in log ratio to 100%. max Dynamic gc logic should activate
    log->head_offset = 1500;
    log->tail_offset = 128;
    log->log_size_bytes = 1372;
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(MAX_DYNAMIC_GC_RATE, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set storage overhead ratio to 50%. Dynamic gc logic should activate making GC rate 15mb/s
    log->log_size_bytes = 2744;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(15 * 1024 * 1024, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set overhead to be 36%, garbage in log ratio to be 31.35%.
    // We therefore anticipate dynamic GC to be 30MB/s * 0.36 * 0.3135 = roughly 3.57MB/s
    log->head_offset = 127;
    log->tail_offset = 1500;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 400;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(3570401, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set overhead ratio to 0%. We should not be using dynamic GC
    // And existing logic should kick in, with an expected value of 136.5KiB/s
    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 2000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(136500, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
}

TEST_P(LogTest, testDynamicGarbageCollectionRateConfigUpdate) {
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 1;
    log->max_allocated_log_size_percent = 50;

    // Run garbage collection calculations using default gc rate (4 KiB per second)
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Set storage overhead ratio to 100%. Dynamic gc logic should activate making GC rate 30mb/s
    log->head_offset = 1500;
    log->tail_offset = 127;
    log->log_size_bytes = 1373;
    log->write_rate_info.last_window_data_written_bytes_per_second = 273;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(MAX_DYNAMIC_GC_RATE, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Increase the dynamic gc rate config to 31mb/s per second
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE;
    config.numeric_value = 31 * 1024 * 1024;
    logSetConfig(log, &config);
    updateGarbageCollectionRate(log);
    ASSERT_EQ(31 * 1024 * 1024, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Test minGC logic still kicks in
    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(4096, log->garbage_collector_info.required_garbage_collection_bytes_per_second);

    // Test regular logic kicks still kicks in
    log->head_offset = 1207;
    log->tail_offset = 1207;
    log->log_size_bytes = 2000;
    log->write_rate_info.last_window_data_written_bytes_per_second = 27300;
    log->allocated_log_size_bytes = 2000;
    updateGarbageCollectionRate(log);
    ASSERT_EQ(136500, log->garbage_collector_info.required_garbage_collection_bytes_per_second);
}

TEST_P(LogTestWithReplication, testSnapshotWithDeleteion) {
    // Skip snapshot version 1 for threadsave
    if (isTestingReplicationWithThreadsaveAndVersionOne()) {
        return;
    }

    // Total 50 items written to log.
    size_t num_items = 50;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    writeItemsToLog(log, num_databases, num_items);
    log->garbage_collector_info.can_start_garbage_collection = 1;

    snapshotContext save_context = { 0 };
    snapshotContext *snapshot_context = &save_context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Run the log cron task a few times to make sure the first item is already iterated.
    for (int i = 0; i < 35; ++i) {
        logRunCronTasks(log);
    }

    // Now the snapshot should not be completed and all the THREADSAVE related metrics
    // should be 0.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    ASSERT_EQ(num_items, logGetCountBasedMetric(log, FC_NUM_ITEMS));

    log->garbage_collector_info.garbage_collection_bytes_in_current_second = 0;
    // Delete the item at the tail of the log because it has been written to the FDB
    // Delete an item that has not yet been written to the FDB
    testItemDelete(1, 0);
    testItemDelete(1, 45);

    // Verify that the items have been deleted
    ASSERT_EQ(num_items - 2, logGetCountBasedMetric(log, FC_NUM_ITEMS));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_SAVE_NUM_COMPLETED));

    // If THREADSAVE replication, we expect DELETE replication command in snapshot and the
    // same item should not be sent to RDB as it is deleted.
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    }

    // Wait for snapshotting to complete.
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }

    // If THREADSAVE replication, we expect DELETE repl command in snapshot, and we do
    // not expedite the item to the RDB
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
        // As `has_snapshotting_completed_in_redis_layer` was true in the test,
        // log iteration callback will not be invoked
        ASSERT_EQ(0, snapshot_context->num_log_iteration_completion_callback_invocation);
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
    }

    // We ended the snapshot, the curr THREADSAVE metrics should reset to zero.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));

    // Number of items will be less due to the deletion during snapshotting
    int final_num_items = logGetCountBasedMetric(log, FC_NUM_ITEMS);
    ASSERT_TRUE(final_num_items == num_items - 2);

    // If THREADSAVE replication, we expect DELETE replication commands in snapshot and the
    // same items should not be sent to RDB.
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(1, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
        ASSERT_EQ(432, logGetCountBasedMetric(log, FC_ITEM_BYTES_DELETED_FROM_DISK));
    } else {
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_DELETE_REPL_CMD));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG));
        ASSERT_EQ(0, logGetCountBasedMetric(log, FC_ITEM_BYTES_DELETED_FROM_DISK));
    }

    // We ended the snapshot, the curr THREADSAVE metrics should reset to zero.
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // Reload the snapshot
    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_verification_result = 0;
    logLoadSnapshot(log, snapshot_filename, &snapshot_secret_from_load, &checksum_verification_result);
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_LOAD_REQUEST));
    if (isTestingThreadsaveReplication()) {
        ASSERT_EQ(num_items - 2, logGetCountBasedMetric(log, FC_NUM_ITEMS));
        // Confirm all FDB items were properly written and loaded
        testItemRead(44, 1);
        testItemRead(4, 46);
    } else {
        ASSERT_EQ(50, logGetCountBasedMetric(log, FC_NUM_ITEMS));
        testItemRead(50, 0);
    }
    freeEvictionContext();
}

static void dummyhashInit(const uint8_t *seed) {
    (void) seed;
    return;
}

static void dummyhashGetSeed(uint8_t *seed) {
    (void) seed;
}

static uint64_t dummyhashHash(const char *key, size_t key_len) {
    (void) key;
    // Note: 48 == ((sizeof(long unsigned int) * 8) - FC_LOG_ENTRY_COLLISION_HASHBITS)
    // Using 48 and the bit mask allows us to get a clean collision bit only hash
    return ((key_len % 4) << 48) & 0xFFFF000000000000;
}

static uint32_t dummyhashGetType() {
    return FLASHCACHE_SIPHASH_HASHER;
}

TEST_P(LogTest, testDeletionWithHashCollisionUsingNonUniformKeys) {
    flashcacheHasher dummy_hasher = {dummyhashInit, dummyhashGetSeed, dummyhashHash, dummyhashGetType};
    // Release the old log before creating a new log
    logRelease(log);
    log = nullptr;
    flashcacheEvictionDetails eviction_details = { 0 };
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;
    ASSERT_EQ(logCreate(&log, "new_log.db", log_size_bytes, 32768, 2,
                FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD,
                max_allocated_log_size_percent, max_num_in_flight_read_requests,
                min_garbage_collection_rate,
                evict_under_max_logsize_time_limit, optimized_delete_enabled, &dummy_hasher,
                mockClockGetTimeUs, &eviction_details), FC_OK);

    log->index_list[0]->collision_bits_used = 1;
    log->index_list[0]->base_size_bits = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;

    char *key1, *key2, *key3, *value;
    size_t key_len = 0, value_len = 0;
    // item 1 spans about a quarter of page 1
    setKeyLen(256);
    setValueLen(1024);
    generateRandomItem(key1, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key1, key_len, value, value_len), FC_OK);

    // item 2 spans to about half of page 1
    setKeyLen(512);
    setValueLen(512);
    generateRandomItem(key2, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key2, key_len, value, value_len), FC_OK);

    // item 3's offset starts about half way through page 1. It's key is
    // long enough to span into page 2. No other key would span into page
    // 2 from item 3's offset.
    setKeyLen(2776);
    setValueLen(8);
    generateRandomItem(key3, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key3, key_len, value, value_len), FC_OK);

    // flushing the staging buffer
    log->staging_buffer_flush_size_threshold_bytes = 0;
    while (stagingBufferGetTotalItemSize(log->staging_buffer) != 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(collisionHashCount(log, 0, key3, key_len), 3);

    // When attempting to delete key1, the index for key3 will be iterated over first
    // due to a hash collision. FlashCache will under request the number of pages
    // to read the entire key3 from flash because the number of pages will be calculated
    // using key3's offset and key1's key length. This should not force an engine crash.
    requestContext *context = new requestContext;
    context->num_callback_triggered = 0;
    ASSERT_EQ(logRead(log, 0, key1, 256,
            FC_DELETE, static_cast<void *>(context), delete_item_callback), FC_OK);
    while (context->num_callback_triggered == 0) {
        logRunCronTasks(log);
    }

    delete context;
}

TEST_P(LogTest, testDeletionWithHashCollisionUsingNonUniformKeysZeroOffset) {
    flashcacheHasher dummy_hasher = {dummyhashInit, dummyhashGetSeed, dummyhashHash, dummyhashGetType};
    // Release the old log before creating a new log
    logRelease(log);
    log = nullptr;
    flashcacheEvictionDetails eviction_details = { 0 };
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;
    ASSERT_EQ(logCreate(&log, "new_log.db", log_size_bytes, 32768, 2,
                FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD,
                max_allocated_log_size_percent, max_num_in_flight_read_requests,
                min_garbage_collection_rate,
                evict_under_max_logsize_time_limit, optimized_delete_enabled, &dummy_hasher,
                mockClockGetTimeUs, &eviction_details), FC_OK);

    log->index_list[0]->collision_bits_used = 1;
    log->index_list[0]->base_size_bits = 0;
    log->garbage_collector_info.can_start_garbage_collection = 0;
    log->staging_buffer_flush_size_threshold_bytes = 0;

    char *key1, *value1, *key2, *value2, *key3, *value3;
    size_t key1_len = 256, value1_len = 256, key2_len = 5000, value2_len = 5000,
            key3_len = 256, value3_len = 256;

    // writing items and flushing the staging buffer. This causes each item to
    // have an offset of 0 within each page. Items 1 and 3 have keys that are
    // short, while key 2 has a large key that spans a page boundary.
    setKeyLen(key1_len);
    setValueLen(value1_len);
    generateRandomItem(key1, key1_len, value1, value1_len);
    ASSERT_EQ(logWrite(log, 0, key1, key1_len, value1, value1_len), FC_OK);
    while (stagingBufferGetTotalItemSize(log->staging_buffer) != 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(collisionHashCount(log, 0, key1, key1_len), 1);

    // key 2 has a key that spans two pages.
    setKeyLen(key2_len);
    setValueLen(value2_len);
    generateRandomItem(key2, key2_len, value2, value2_len);
    ASSERT_EQ(logWrite(log, 0, key2, key2_len, value2, value2_len), FC_OK);
    while (stagingBufferGetTotalItemSize(log->staging_buffer) != 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(collisionHashCount(log, 0, key2, key2_len), 2);

    setKeyLen(key3_len);
    setValueLen(value3_len);
    generateRandomItem(key3, key3_len, value3, value3_len);
    ASSERT_EQ(logWrite(log, 0, key3, key3_len, value3, value3_len), FC_OK);
    while (stagingBufferGetTotalItemSize(log->staging_buffer) != 0) {
        logRunCronTasks(log);
    }
    ASSERT_EQ(collisionHashCount(log, 0, key3, key3_len), 3);

    // When attempting to delete key1 there will be a hash collision with key2.
    // Insufficient pages will be initially read for key2, so instead of resubmitting,
    // the engine will recognize the read attempt as a collision miss and will iterate
    // to the next item. The engine should not crash due to the resubmission.
    requestContext *context = new requestContext;
    context->num_callback_triggered = 0;
    ASSERT_EQ(logRead(log, 0, key1, key1_len,
            FC_DELETE, static_cast<void *>(context), delete_item_callback), FC_OK);
    while (context->num_callback_triggered == 0) {
        logRunCronTasks(log);
    }

    delete context;
}

TEST_P(LogTest, testResubmitInflightDeletionWhenPITSnapshottingStarts) {
    // Skip this test if not SV2 with point-in-time snapshotting
    if (flashcache_snapshot_version != FC_SNAPSHOT_VERSION_TWO ||
            flashcache_snapshot_save_type != FC_SAVE_TYPE_FORKLESS_SAVE) {
        return;
    }

    // Write a key and flush it to the log
    log->staging_buffer_flush_size_threshold_bytes = 0;
    char *key, *value;
    size_t key_len = 4000, value_len = 256;
    setKeyLen(key_len);
    setValueLen(value_len);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    logRunCronTasks(log);

    // Start snapshotting
    snapshotContext save_context = { 0 };
    snapshotContext *snapshot_context = &save_context;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;
    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;
    logStartSave(log, &snapshot_secret, &snapshot_callback_details, snapshot_writer,
            snapshot_context, snapshot_filename);

    // Get the log offset of the written key
    indexEntry *index_entry = indexGetHeadEntry(log->index_list[0], key, key_len);
    size_t log_offset = expandTrimmedLogOffset(index_entry->item_entry.log_entry.trimmed_log_offset);

    // Emulate an optimized deletion request in logRunCronTasks with snapshotting running
    int requested_item_with_value = 0;
    ASSERT_TRUE(shouldResubmitRequestForExpedition(log_offset, requested_item_with_value));

    // Finish the snapshotting process for teardown purposes
    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        logRunCronTasks(log);
    }
}

TEST_P(LogTest, testReadAndDeleteLargeItems) {
    log->garbage_collector_info.can_start_garbage_collection = 0;
    log->garbage_collector_info.is_running = 0;
    requestContext *context = new requestContext;
    context->num_callback_triggered = 0;

    // Write an item with a small key and a large value to the log
    log->staging_buffer_flush_size_threshold_bytes = 0;
    char *key, *value;
    size_t key_len = 20, value_len = (1024 * 1024 * 2);
    setKeyLen(key_len);
    setValueLen(value_len);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    logRunCronTasks(log);
    context->actual_value = value;
    context->actual_value_len = value_len;

    // Attempt to delete the item from the log
    ASSERT_EQ(logRead(log, 0, key, key_len,
            FC_DELETE, static_cast<void *>(context), delete_item_callback), FC_OK);
    while (context->num_callback_triggered == 0) {
        logRunCronTasks(log);
    }
    // Re-write the item to the log and then read it
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    logRunCronTasks(log);
    ASSERT_EQ(logRead(log, 0, key, key_len,
            FC_READ, static_cast<void *>(context), get_item_callback), FC_OK);
    while (context->num_callback_triggered == 1) {
        logRunCronTasks(log);
    }

    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_DELETE_REQUEST));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_OPTIMIZED_DELETES));
    ASSERT_EQ(1, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));

    // Write an item with a large key and a small value to the log
    key_len = (1024 * 1024 * 2);
    value_len = 20;
    setKeyLen(key_len);
    setValueLen(value_len);
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    logRunCronTasks(log);
    context->actual_value = value;
    context->actual_value_len = value_len;

    // Attempt to delete the item from the log
    ASSERT_EQ(logRead(log, 0, key, key_len,
            FC_DELETE, static_cast<void *>(context), delete_item_callback), FC_OK);
    while (context->num_callback_triggered == 2) {
        logRunCronTasks(log);
    }
    // Re-write the item to the log and then read it
    ASSERT_EQ(logWrite(log, 0, key, key_len, value, value_len), FC_OK);
    logRunCronTasks(log);
    ASSERT_EQ(logRead(log, 0, key, key_len,
            FC_READ, static_cast<void *>(context), get_item_callback), FC_OK);
    while (context->num_callback_triggered == 3) {
        logRunCronTasks(log);
    }

    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_READ_REQUEST));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_DELETE_REQUEST));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_OPTIMIZED_DELETES));
    ASSERT_EQ(2, logGetCountBasedMetric(log, FC_NUM_PARTIAL_ITEM_READ));

    delete context;
}

TEST_P(LogTest, testHashCollisionCount) {
    flashcacheHasher dummy_hasher = {dummyhashInit, dummyhashGetSeed, dummyhashHash, dummyhashGetType};
    // Release the old log before creating a new log
    logRelease(log);
    log = nullptr;
    flashcacheEvictionDetails eviction_details = { 0 };
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;
    ASSERT_EQ(logCreate(&log, "new_log.db", log_size_bytes, 128, 2,
                FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD,
                max_allocated_log_size_percent, max_num_in_flight_read_requests,
                min_garbage_collection_rate,
                evict_under_max_logsize_time_limit, optimized_delete_enabled, &dummy_hasher,
                mockClockGetTimeUs, &eviction_details), FC_OK);
    log->index_list[0]->collision_bits_used = 2;
    log->index_list[0]->base_size_bits = 0;

    ASSERT_EQ(logWrite(log, 0, "1", 1, "1", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "2", 1, "1", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "3", 1, "1", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "4", 1, "1", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "11", 2, "2", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "12", 2, "2", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "22", 2, "2", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "123", 3, "3", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "321", 3, "3", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "111", 3, "3", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "1234", 4, "4", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "4321", 4, "4", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "1111", 4, "4", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "2222", 4, "4", 1), FC_OK);
    ASSERT_EQ(logWrite(log, 0, "3333", 4, "4", 1), FC_OK);

    // No collisions because all the items are in the staging buffer
    char text[] = "1\0\0\0";
    ASSERT_EQ(collisionHashCount(log, 0, text, 1), 0);
    text[1] = '2';
    ASSERT_EQ(collisionHashCount(log, 0, text, 2), 0);
    text[2] = '3';
    ASSERT_EQ(collisionHashCount(log, 0, text, 3), 0);
    text[3] = '4';
    ASSERT_EQ(collisionHashCount(log, 0, text, 4), 0);

    // Flush the staging buffer
    log->staging_buffer_flush_size_threshold_bytes = 0;
    log->garbage_collector_info.garbage_collection_bytes_in_current_second = 0;
    while (stagingBufferGetTotalItemSize(log->staging_buffer) != 0) {
        logRunCronTasks(log);
    }

    // Verify the hash collision counts of the log items
    text[1] = '\0';
    text[2] = '\0';
    text[3] = '\0';
    ASSERT_EQ(collisionHashCount(log, 0, text, 1), 4);
    text[1] = '2';
    ASSERT_EQ(collisionHashCount(log, 0, text, 2), 3);
    text[2] = '3';
    ASSERT_EQ(collisionHashCount(log, 0, text, 3), 3);
    text[3] = '4';
    ASSERT_EQ(collisionHashCount(log, 0, text, 4), 5);
}

TEST_P(LogTest, testOptimizedDeleteConfig) {
    size_t num_items = 50;
    writeItemsToLog(log, num_databases, num_items);

    log->optimized_delete_enabled = 0;
    testItemDelete(1, 0);
    ASSERT_EQ(0, log->metrics.num_optimized_deletes);

    // Increase the min garbage collection rate config to 8KiB per second
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_OPTIMIZED_DELETE_ENABLED;
    config.numeric_value = 1;
    logSetConfig(log, &config);
    ASSERT_EQ(1, log->optimized_delete_enabled);

    testItemDelete(1, 1);
    ASSERT_EQ(1, log->metrics.num_optimized_deletes);

    // Update minGC one more time and verify the new value is used
    config.key = FC_CONFIG_KEY_OPTIMIZED_DELETE_ENABLED;
    config.numeric_value = 0;
    logSetConfig(log, &config);
    ASSERT_EQ(0, log->optimized_delete_enabled);

    testItemDelete(1, 2);
    ASSERT_EQ(1, log->metrics.num_optimized_deletes);
}

std::vector<size_t> key_len_list { 16, 10023, 65537 };
std::vector<size_t> value_len_list { 200, 10023, 65537 };
std::vector<size_t> log_start_offset_list { 0, (LOG_SIZE_BYTES - 100 * FC_PAGESIZE), (LOG_SIZE_BYTES - FC_PAGESIZE) };
std::vector<flashcache_hash_function> hash_function_override_list {
    single_bucket_hash_function, nullptr };

std::vector<size_t> required_garbage_collection_bytes_per_second_list { 100, 1024, 4096, 8001,
    (4LL << 15),    // 128 KiB/sec
    (8LL << 20),    // 8 MiB/sec
    (100LL << 20),  // 100 MiB/sec
    (200LL << 20)   // 200 MiB/sec
};

std::vector<size_t> staging_buffer_flush_size_threshold_bytes_list { 0, 4096,
    (4LL << 15),  // 128 KiB
    (1LL << 20),  // 1 MiB
    (4LL << 20)   // 4 MiB
};

std::vector<flashcacheSnapshotVersion> flashcache_snapshot_versions_list {
        FC_SNAPSHOT_VERSION_ONE, FC_SNAPSHOT_VERSION_TWO };

std::vector<flashcacheSnapshotSaveType> flashcache_snapshot_save_types_list {
        FC_SAVE_TYPE_BGSAVE, FC_SAVE_TYPE_FORKLESS_SAVE };

std::vector<flashcacheReadTypes> flashcache_read_type { FC_READ, FC_DELETE };

// Argument semantics for parametrixed tests:
// <0> Key length
// <1> Value len
// <2> Collection bytes per second
// <3> Flush size threshold
// <4> Log start offset
// <5> index buffer length
// <6> Hash function
// <7> enableMockFio min_delay_in_request_processing
// <8> enableMockFio fifo_ordering
// <9> setSnapshotWriter should_set_snapshot_writer
// <10> numeric_value
// <11> Snapshot Version
// <12> Snapshot save type
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTest, LogTest,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096),
            testing::Values(4096),
            testing::Values(0),
            testing::Values(4096),
            testing::Values(nullptr),
            testing::Values(0),
            testing::Values(true),
            testing::Values(false),
            testing::Values(100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedSingleDbLogTest, SingleDbLogTest,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096),
            testing::Values(4096),
            testing::Values(0),
            testing::Values(4096),
            testing::Values(nullptr),
            testing::Values(0),
            testing::Values(true),
            testing::Values(false),
            testing::Values(100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::ValuesIn(flashcache_read_type)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithDifferentGarbageCollectionRate,
        LogTestWithDifferentGarbageCollectionRate,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::ValuesIn(required_garbage_collection_bytes_per_second_list),
            testing::ValuesIn(staging_buffer_flush_size_threshold_bytes_list),
            testing::Values(0),
            testing::Values(4096),
            testing::ValuesIn(hash_function_override_list),
            testing::Values(10),
            testing::Values(false),
            testing::Values(false),
            testing::Values(100 * 1024 * 1024),
            testing::Values(FC_SNAPSHOT_VERSION_TWO),
            testing::Values(FC_SAVE_TYPE_BGSAVE),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithDifferentItemSize, LogTestWithDifferentItemSize,
        testing::Combine(testing::ValuesIn(key_len_list),
            testing::ValuesIn(value_len_list),
            testing::Values(4096),
            testing::Values(MEGABYTE_TO_BYTES),
            testing::Values(0),
            testing::Values(4096),
            testing::ValuesIn(hash_function_override_list),
            testing::Values(10),
            testing::Values(true, false),
            testing::Values(false),
            testing::Values(100 * 1024 * 1024),
            testing::Values(FC_SNAPSHOT_VERSION_TWO),
            testing::Values(FC_SAVE_TYPE_BGSAVE),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithSnapshotting,
        LogTestWithSnapshotting,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096, (10LL * 1024 * 1024)),
            testing::Values(4096),
            testing::ValuesIn(log_start_offset_list),
            testing::Values(4096, (1LL * 1024 * 1024)),
            testing::Values(nullptr),
            testing::Values(10),
            testing::Values(false),
            testing::Values(false, true),
            testing::Values(100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithReplication,
        LogTestWithReplication,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096),
            testing::Values(4096),
            testing::Values(0),
            testing::Values(4096, (1LL * 1024 * 1024)),
            testing::Values(nullptr),
            testing::Values(0),
            testing::Values(true, false),
            testing::Values(true, false),
            testing::Values(100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithSnapshottingBufferLimit,
        LogTestWithSnapshottingBufferLimit,
            testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096),
            testing::Values(4096),
            testing::Values(0),
            testing::Values(4096),
            testing::Values(nullptr),
            testing::Values(10),
            testing::Values(false),
            testing::Values(true),
            testing::Values(100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::Values(FC_READ)));
INSTANTIATE_TEST_SUITE_P(ParameterizedLogTestWithLargeNumberOfItems,
        LogTestWithLargeNumberOfItems,
        testing::Combine(testing::Values(16),
            testing::Values(200),
            testing::Values(4096),
            testing::Values(4096),
            testing::Values(0),
            testing::Values(4096),
            testing::Values(nullptr),
            testing::Values(300),
            testing::Values(false),
            testing::Values(false),
            testing::Values(1, 100 * 1024 * 1024),
            testing::ValuesIn(flashcache_snapshot_versions_list),
            testing::ValuesIn(flashcache_snapshot_save_types_list),
            testing::Values(FC_READ)));
