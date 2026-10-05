#include <cstdio>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <jemalloc/jemalloc.h>

#include "clock_mock.hpp"
#include "flashcache_test_base.hpp"
#include "snapshot_writer_mock.hpp"

extern "C" {
#include "include/flashcache.h"
#include "include/util.h"

    void flashcacheLogState(int level);
}

#define INITIAL_INDEX_SIZE_PER_DB (128)
#define NUM_DATABASES (20)

static int num_logger_invoked;
void mockLogger(int level, const char *fmt, ...) {
    ASSERT_GE(level, FC_LL_DEBUG);
    ASSERT_LE(level, FC_LL_WARNING);
    char const *expected_prefix = "[FLASHCACHE]";
    ASSERT_EQ(0, memcmp(expected_prefix, fmt, strlen(expected_prefix)));
    num_logger_invoked++;
}

int mockAsioContext = 0;
void mockAsioCallback(void *context) {
    ASSERT_EQ(context, static_cast<void *>(&mockAsioContext));
    mockAsioContext++;
}

class FlashcacheTest : public flashcache::FlashcacheTestBase, public testing::TestWithParam
        <std::tuple<flashcacheSnapshotVersion, flashcacheSnapshotSaveType, size_t>> {
 public:
     std::string db_filename;
     std::string snapshot_filename;
     evictionContext eviction_context;
     size_t checksum_verification_enabled;
     flashcacheSnapshotVersion flashcache_snapshot_version;
     flashcacheSnapshotSaveType flashcache_snapshot_save_type;

     void SetUp() {
         flashcache::FlashcacheTestBase::SetUp();
         snapshot_filename = createTmpFile(0);
         size_t db_size = (5LL << 20);  // 5 MiB
         db_filename = createTmpFile(db_size);
         size_t max_allocated_db_size_percent = 100;
         size_t max_num_in_flight_read_requests = 128;
         size_t min_garbage_collection_rate = 4096;
         size_t evict_under_max_logsize_time_limit = 100 * 1000;
         uint8_t optimized_delete_enabled = 1;
         flashcacheEvictionDetails eviction_details = { 0 };
         eviction_details.context = static_cast<void *>(&eviction_context);
         eviction_details.callback = eviction_callback;
         flashcacheAsioControlMsgCallbackDetails asio_control_msg_callback =
             {static_cast<void *>(&mockAsioContext), mockAsioCallback};
         flashcache_snapshot_version = std::get<0>(GetParam());
         flashcache_snapshot_save_type = std::get<1>(GetParam());
         checksum_verification_enabled = std::get<2>(GetParam());
         ASSERT_EQ(flashcacheInit(db_filename.c_str(), db_size, INITIAL_INDEX_SIZE_PER_DB, NUM_DATABASES,
                     max_allocated_db_size_percent, max_num_in_flight_read_requests, min_garbage_collection_rate,
                     evict_under_max_logsize_time_limit, optimized_delete_enabled, mockClockGetTimeUs,
                     &eviction_details, mockLogger, &asio_control_msg_callback), FC_OK);
     }

     void TearDown() {
         flashcache::FlashcacheTestBase::TearDown();
         ASSERT_EQ(flashcacheTearDown(), FC_OK);
         unlink(db_filename.c_str());
         unlink(snapshot_filename.c_str());
         ASSERT_EQ(0, eviction_context.key_list.size());
         ASSERT_GT(num_logger_invoked, 0);
         ASSERT_EQ(0, flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE));
     }

    int getHistogramTotalCount(unsigned long long histogram[], const size_t histogram_size) {
        int total_count = 0;
        for (int i = 0; i < histogram_size; i++) {
            total_count += histogram[i];
        }
        return total_count;
    }
};

class FlashcacheTestWithSnapshottingVersions : public FlashcacheTest {
};

class FlashcacheTestWithSnapshottingVersionsAndChecksum : public FlashcacheTest {
};


TEST_P(FlashcacheTest, flashcacheRunCronTasksWithoutPutOrGet) {
    for (int i = 0; i < 1000; ++i) {
        flashcacheRunCronTasks();
    }
    // Must have received ASIO callbacks
    ASSERT_GT(mockAsioContext, 0);
}

TEST_P(FlashcacheTest, testLogState) {
    // Dump the state of the DB and ensure that it does not crash
    flashcacheLogState(FC_LL_WARNING);
}

TEST_P(FlashcacheTest, happyCaseTest) {
    ASSERT_EQ(0, flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST));
    ASSERT_EQ(0, flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST));
    ASSERT_EQ(0, flashcacheGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR));

    // Memory allocation for histogram
    const size_t histogram_size = 9;
    unsigned long long read_histogram[histogram_size] = {0};
    unsigned long long write_histogram[histogram_size] = {0};

    flashcacheGetHistogramMetrics(FC_DISK_WRITE_LATENCY_HISTOGRAM, write_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(write_histogram, histogram_size));
    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(read_histogram, histogram_size));

    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(flashcachePutItem(dbid, key, key_len, value, value_len),
            FC_OK);
    ASSERT_EQ(0, flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST));
    ASSERT_EQ(1, flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST));

    // Fetching the key using a different database identifier should not find the item
    requestContext empty_context_1 = { 0 };
    ASSERT_EQ(flashcacheGetItem(dbid + 1, key, key_len, FC_READ,
                static_cast<void *>(&empty_context_1), get_item_callback), FC_OK);
    while (empty_context_1.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }
    ASSERT_EQ(1, flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST));
    ASSERT_EQ(1, flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST));
    flashcacheGetHistogramMetrics(FC_DISK_WRITE_LATENCY_HISTOGRAM, write_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(write_histogram, histogram_size));
    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(read_histogram, histogram_size));

    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(flashcacheGetItem(dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }
    ASSERT_EQ(2, flashcacheGetCountBasedMetric(FC_NUM_READ_REQUEST));
    ASSERT_EQ(1, flashcacheGetCountBasedMetric(FC_NUM_WRITE_REQUEST));
    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(read_histogram, histogram_size));

    // Next access should not find the item as the first get should remove the
    // item from the store.
    requestContext empty_context_2 = { 0 };
    ASSERT_EQ(flashcacheGetItem(dbid, key, key_len, FC_READ,
                static_cast<void *>(&empty_context_2), get_item_callback), FC_OK);
    while (empty_context_2.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }

    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_EQ(0, getHistogramTotalCount(read_histogram, histogram_size));
}

TEST_P(FlashcacheTestWithSnapshottingVersionsAndChecksum, testSnapshotCompletionAndLoading) {
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    const size_t histogram_size = 9;
    unsigned long long read_histogram[histogram_size] = {0};
    unsigned long long write_histogram[histogram_size] = {0};

    ASSERT_EQ(flashcachePutItem(dbid, key, key_len, value, value_len),
            FC_OK);

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 1;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Start snapshotting and wait till it completes
    flashcacheStartFileBasedSave(snapshot_filename.c_str(), &snapshot_secret,
                                 &snapshot_callback_details, checksum_verification_enabled,
                                 flashcache_snapshot_version, flashcache_snapshot_save_type);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 1 in case of Threadsave
    if (flashcache_snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE) {
        flashcacheConfig config = {};
        config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
        config.numeric_value = 1;
        flashcacheSetConfig(&config);
    }

    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }

    // Expected checksum verification result should only be 1 if the
    // checksum is enabled on snapshot version 2. Otherwise 0.
    size_t expected_checksum_result = 0;
    if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_TWO && checksum_verification_enabled == 1) {
        expected_checksum_result = 1;
    }

    // Load the snapshot
    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_comparison_result = 0;
    flashcacheLoadSnapshot(snapshot_filename.c_str(), &snapshot_secret_from_load, &checksum_comparison_result);
    ASSERT_EQ(snapshot_secret_from_load.size, FC_SNAPSHOT_MAX_SECRET_SIZE);
    ASSERT_EQ(memcmp(&snapshot_secret.secret, &snapshot_secret_from_load.secret, snapshot_secret_from_load.size), 0);
    flashcacheGetHistogramMetrics(FC_DISK_WRITE_LATENCY_HISTOGRAM, write_histogram, histogram_size);
    ASSERT_GT(getHistogramTotalCount(write_histogram, histogram_size), 0);
    ASSERT_EQ(checksum_comparison_result, expected_checksum_result);

    // Ensure that the snapshot has the item that was written before taking snapshot
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(flashcacheGetItem(dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }
    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_GT(getHistogramTotalCount(read_histogram, histogram_size), 0);
}

TEST_P(FlashcacheTestWithSnapshottingVersionsAndChecksum, testStreamBasedSnapshotCompletionAndLoading) {
    // Snapshot V1 does not support THREADSAVE replication, FlashCache
    // will crash if we start snapshotting with these configurations.
    if (flashcache_snapshot_version == FC_SNAPSHOT_VERSION_ONE
        && flashcache_snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE) {
        return;
    }
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    const size_t histogram_size = 9;
    unsigned long long read_histogram[histogram_size] = {0};
    unsigned long long write_histogram[histogram_size] = {0};

    ASSERT_EQ(flashcachePutItem(dbid, key, key_len, value, value_len),
            FC_OK);

    snapshotContext snapshot_context_ = { 0 };
    snapshotContext *snapshot_context = &snapshot_context_;
    snapshot_context->expected_completion_status = 1;
    snapshot_context->snapshot_save_type = flashcache_snapshot_save_type;
    snapshot_context->snapshot_version = flashcache_snapshot_version;

    // Start stream based snapshotting and wait till it completes
    flashcacheSnapshotWriter *snapshot_writer = getMockSnapshotWriter();
    initializeSnapshotWriterMock(snapshot_filename.c_str());
    snapshot_writer->callback_context = snapshot_context;

    // Construct log iteration callback details
    flashcacheLogIterationCallbackDetails log_iteration_callback_details = { 0 };
    log_iteration_callback_details.context = static_cast<void *>(snapshot_context);
    log_iteration_callback_details.callback = logIterationCompletionCallback;

    flashcacheStartStreamBasedSave(&snapshot_secret, snapshot_writer, flashcache_snapshot_version,
                                   flashcache_snapshot_save_type, &log_iteration_callback_details);

    // Set FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS config to 1 in case of Threadsave
    if (flashcache_snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE) {
        flashcacheConfig config = {};
        config.key = FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS;
        config.numeric_value = 1;
        flashcacheSetConfig(&config);
    }

    while (snapshot_context->num_snapshot_completion_callback_invocation != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }

    // Load the snapshot
    flashcacheSnapshotSecret snapshot_secret_from_load = {0};
    int checksum_comparison_result = 0;
    flashcacheLoadSnapshot(snapshot_filename.c_str(), &snapshot_secret_from_load, &checksum_comparison_result);
    ASSERT_EQ(snapshot_secret_from_load.size, FC_SNAPSHOT_MAX_SECRET_SIZE);
    ASSERT_EQ(memcmp(&snapshot_secret.secret, &snapshot_secret_from_load.secret, snapshot_secret_from_load.size), 0);
    flashcacheGetHistogramMetrics(FC_DISK_WRITE_LATENCY_HISTOGRAM, write_histogram, histogram_size);
    ASSERT_GT(getHistogramTotalCount(write_histogram, histogram_size), 0);
    ASSERT_EQ(checksum_comparison_result, 0);  // In stream based snapshot checksum result is never touched.

    // Ensure that the snapshot has the item that was written before taking snapshot
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(flashcacheGetItem(dbid, key, key_len, FC_READ,
                                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }
    flashcacheGetHistogramMetrics(FC_DISK_READ_LATENCY_HISTOGRAM, read_histogram, histogram_size);
    ASSERT_GT(getHistogramTotalCount(read_histogram, histogram_size), 0);
    ASSERT_EQ(snapshotWriterMockGetNumSetSnapshotSizeInvocation(), 1);
    ASSERT_GT(snapshotWriterMockGetNumIsWritableInvocation(), 0);
    ASSERT_GT(snapshotWriterMockGetNumWriteInvocation(), 0);
    ASSERT_EQ(snapshotWriterMockGetNumCompleteInvocation(), 1);
    releaseMockSnapshotWriter(snapshot_writer);
}

TEST_P(FlashcacheTestWithSnapshottingVersions, testSnapshotCancel) {
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(flashcachePutItem(dbid, key, key_len, value, value_len),
            FC_OK);

    snapshotContext snapshot_context = { 0 };
    snapshot_context.expected_completion_status = 0;

    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    // Start snapshotting and wait till it completes
    flashcacheStartFileBasedSave(snapshot_filename.c_str(), &snapshot_secret, &snapshot_callback_details, 1,
                                 flashcache_snapshot_version, flashcache_snapshot_save_type);

    // Cancel snapshotting
    flashcacheCancelSave();

    while (snapshot_context.num_snapshot_completion_callback_invocation != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }

    // Ensure that the item that was written before starting and cancelling snapshot is still present in the store
    requestContext context{ value, value_len, 0 };
    ASSERT_EQ(flashcacheGetItem(dbid, key, key_len, FC_READ,
                static_cast<void *>(&context), get_item_callback), FC_OK);
    while (context.num_callback_triggered != 1) {
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }
}

TEST_P(FlashcacheTest, readAndWriteLargeNumberOfItems) {
    size_t num_items = 8000;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    for (size_t i = 0; i < num_items; ++i) {
        generateRandomItem(key, key_len, value, value_len);
        ASSERT_EQ(flashcachePutItem(i % NUM_DATABASES, key, key_len, value, value_len),
                FC_OK);
    }

    std::vector<requestContext *> request_contexts;
    for (size_t i = 0; i < num_items; ++i) {
       getGeneratedItemAtTime(i, key, key_len, value, value_len);
       requestContext *context = new requestContext;
       context->actual_value = value;
       context->actual_value_len = value_len;
       context->num_callback_triggered = 0;
       request_contexts.push_back(context);
       while (true) {
           flashcacheReturnCode rc = flashcacheGetItem(i % NUM_DATABASES, key, key_len, FC_READ,
                   static_cast<void *>(context), get_item_callback);
           if (rc == FC_OK) {
               break;
           }
           ASSERT_EQ(rc, FC_ERR_THROTTLED);
           ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
           flashcacheRunCronTasks();
       }
       flashcacheRunCronTasks();
    }

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
        ASSERT_TRUE(flashcacheShouldRunCronTasksImmediately());
        flashcacheRunCronTasks();
    }

    for (size_t i = 0; i < request_contexts.size(); ++i) {
        delete request_contexts.at(i);
    }
}

TEST_P(FlashcacheTest, testFlashCacheFlushAllDBs) {
    size_t num_items = 800;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    for (size_t i = 0; i < num_items; ++i) {
        generateRandomItem(key, key_len, value, value_len);
        ASSERT_EQ(flashcachePutItem(i % NUM_DATABASES, key, key_len, value, value_len),
                FC_OK);
    }

    ASSERT_EQ(flashcacheFlushAllDBs(), FC_OK);
    ASSERT_EQ(flashcacheGetCountBasedMetric(FC_NUM_ITEMS), 0);
}

TEST_P(FlashcacheTest, testFlashcacheFlushDB) {
    const size_t TWO_DATABASES = 2;
    size_t num_items = 800;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    for (size_t i = 0; i < num_items; ++i) {
        generateRandomItem(key, key_len, value, value_len);
        ASSERT_EQ(flashcachePutItem(i % TWO_DATABASES, key, key_len, value, value_len),
                  FC_OK);
    }

    // Items are in db 0 or db 1

    // Wipe half ot items (in db 0)
    ASSERT_EQ(flashcacheFlushDB(0), FC_OK);
    ASSERT_EQ(flashcacheGetCountBasedMetric(FC_NUM_ITEMS), num_items/2);

    // Wipe the remaining items (in db 1)
    ASSERT_EQ(flashcacheFlushDB(1), FC_OK);
    ASSERT_EQ(flashcacheGetCountBasedMetric(FC_NUM_ITEMS), 0);
}

TEST_P(FlashcacheTest, testActiveMemoryMetrics) {
    size_t initial_size = flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE);
    void *ptr = fcMalloc(8);
    ASSERT_EQ(malloc_usable_size(ptr), flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE) - initial_size);

    fcFree(ptr);
    ASSERT_EQ(initial_size, flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE));

    ptr = fcCalloc(2, 8);
    ASSERT_EQ(malloc_usable_size(ptr), flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE) - initial_size);

    ptr = fcRealloc(ptr, 8);
    ASSERT_EQ(malloc_usable_size(ptr), flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE) - initial_size);

    fcFree(ptr);
    ASSERT_EQ(initial_size, flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE));

    ptr = fcMalloc(1);
    ASSERT_EQ(malloc_usable_size(ptr), flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE) - initial_size);

    fcFree(ptr);
    ASSERT_EQ(initial_size, flashcacheGetCountBasedMetric(FC_ACTIVE_MEMORY_SIZE));
}

TEST_P(FlashcacheTest, testGetHistogramIntervals) {
    const int histogram_size = 9;
    flashcacheHistogramInterval histogram_interval[histogram_size] = {};
    flashcacheGetHistogramIntervals(FC_DISK_READ_LATENCY_HISTOGRAM, histogram_interval, histogram_size);
    ASSERT_EQ(histogram_interval[0].interval_start, 0);
    ASSERT_EQ(histogram_interval[histogram_size-1].interval_end, LLONG_MAX);
}

TEST_P(FlashcacheTest, testSetAndGetConfig) {
    flashcacheConfig config = {};
    config.key = FC_CONFIG_KEY_EVICTION_ENABLED;
    flashcacheGetConfig(&config);
    ASSERT_EQ(1, config.numeric_value);

    config.numeric_value = 0;
    flashcacheSetConfig(&config);
    flashcacheGetConfig(&config);
    ASSERT_EQ(0, config.numeric_value);

    config.key = FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES;
    flashcacheGetConfig(&config);
    ASSERT_EQ(1024 * 1024, config.numeric_value);
    config.numeric_value = 278;
    flashcacheSetConfig(&config);
    flashcacheGetConfig(&config);
    ASSERT_EQ(278, config.numeric_value);

    config.key = static_cast<flashcacheConfigKey>(2048);
    ASSERT_DEATH(flashcacheGetConfig(&config), "");
    ASSERT_DEATH(flashcacheSetConfig(&config), "");
}

TEST_P(FlashcacheTest, testFsyncBufferedWrites) {
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    ASSERT_EQ(flashcachePutItem(dbid, key, key_len, value, value_len), FC_OK);
    ASSERT_GT(flashcacheGetCountBasedMetric(FC_ITEM_PENDING_FLUSH_SIZE_BYTES), 0);
    flashcacheFsyncBufferedWrites();
    ASSERT_EQ(flashcacheGetCountBasedMetric(FC_ITEM_PENDING_FLUSH_SIZE_BYTES), 0);
}

INSTANTIATE_TEST_SUITE_P(ParameterizedFlashcacheTest, FlashcacheTest,
        testing::Combine(
                testing::Values(FC_SNAPSHOT_VERSION_TWO),
                testing::Values(FC_SAVE_TYPE_BGSAVE, FC_SAVE_TYPE_FORKLESS_SAVE),
                testing::Values(0)));
INSTANTIATE_TEST_SUITE_P(ParameterizedFlashcacheTestWithSnapshottingVersions,
                         FlashcacheTestWithSnapshottingVersions,
        testing::Combine(
                testing::Values(FC_SNAPSHOT_VERSION_ONE, FC_SNAPSHOT_VERSION_TWO),
                testing::Values(FC_SAVE_TYPE_BGSAVE, FC_SAVE_TYPE_FORKLESS_SAVE),
                testing::Values(1)));
INSTANTIATE_TEST_SUITE_P(ParameterizedFlashcacheTestWithSnapshottingVersionsAndChecksum,
                         FlashcacheTestWithSnapshottingVersionsAndChecksum,
        testing::Combine(
                testing::Values(FC_SNAPSHOT_VERSION_ONE, FC_SNAPSHOT_VERSION_TWO),
                testing::Values(FC_SAVE_TYPE_BGSAVE, FC_SAVE_TYPE_FORKLESS_SAVE),
                testing::Values(0, 1)));
