
#include <gtest/gtest.h>
#include <tuple>
#include "flashcache_test_base.hpp"
#include "clock_mock.hpp"
#include "snapshot_writer_mock.hpp"

#define NUM_DATABASES         (600)
#define LOG_SIZE_BYTES        (100LL * 1024 * 1024)

extern "C" {
#include "include/log.h"
#include "include/snapshot_manager.h"
#include "include/snapshot_version_two.h"
#include "include/staging_buffer.h"

    snapshotManagerInfo *getSnapshotManagerInfo();
    int isUnprocessedItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset);
    int isProcessedItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset);
    int isItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset);
    void addDataToSnapshotForTest(snapshotVersionTwoInfo *snapshot_info, char *buf, size_t buf_size);
}

class SnapshotV2TestBase : public flashcache::FlashcacheTestBase, public testing::Test {
 public:
    size_t offset;
    size_t end_offset;
    int expected;
    snapshotVersionTwoInfo *snapshot_info;

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

    void setHashFunction(flashcacheLog *log, flashcache_hash_function hash_function) {
        if (hash_function == nullptr) {
            return;
        }

        log->hasher.hash_function = hash_function;
        for (size_t i = 0; i < log->num_databases; ++i) {
            log->index_list[i]->hash_function = hash_function;
        }
    }

    void setSnapshotWriter(bool should_set_snapshot_writer, const char *filename) {
        if (should_set_snapshot_writer) {
            snapshot_writer = getMockSnapshotWriter();
            initializeSnapshotWriterMock(filename);
        }
    }

    void initializeLog(size_t log_size_bytes_, uint32_t num_databases_) {
        flashcache::FlashcacheTestBase::SetUp();
        num_databases = num_databases_;
        log_size_bytes = log_size_bytes_;

        flashcacheEvictionDetails eviction_details = {0};
        eviction_details.context = &eviction_context;
        eviction_details.callback = eviction_callback;
        flashcache_snapshot_version = FC_SNAPSHOT_VERSION_TWO;
        ASSERT_EQ(logCreate(&log, log_filename, log_size_bytes, 128, num_databases,
                    FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD, max_allocated_log_size_percent,
                    max_num_in_flight_read_requests, min_garbage_collection_rate,
                    evict_under_max_logsize_time_limit, optimized_delete_enabled,
                    flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER), mockClockGetTimeUs,
                    &eviction_details), FC_OK);

        log->garbage_collector_info.enable_adaptive_garbage_collection_rate = 0;
        setKeyLen(16);
        setValueLen(200);
        log->garbage_collector_info.required_garbage_collection_bytes_per_second = 4096;
        log->staging_buffer_flush_size_threshold_bytes = 4096;
        log->head_offset = log->tail_offset = 0;
        setSnapshotWriter(true, snapshot_filename);
        setHashFunction(log, nullptr);

        flashcacheConfig config = {};
        config.key = FC_CONFIG_KEY_MAX_SNAPSHOT_BUFFER_SIZE_BYTES;
        config.numeric_value = 100 * 1024 * 1024;
        logSetConfig(log, &config);
        config.numeric_value = 0;
        logGetConfig(log, &config);
        ASSERT_EQ(config.numeric_value, 100 * 1024 * 1024);

        snapshotManagerInfo *snapshot_manager_info = getSnapshotManagerInfo();
        snapshot_info = snapshot_manager_info->snapshot_version_two_info;

        // Mock the snapshot writer
        snapshot_info->snapshot_common.snapshot_writer = snapshot_writer;
    }

    void SetUp() {
        initializeLog(LOG_SIZE_BYTES, NUM_DATABASES);
    }

    void TearDown() {
        flashcache::FlashcacheTestBase::TearDown();
        releaseMockSnapshotWriter(snapshot_writer);
        snapshot_writer = NULL;
        logRelease(log);
        ASSERT_EQ(0, eviction_context.key_list.size());
    }
};

class SnapshotV2Test : public SnapshotV2TestBase, public testing::WithParamInterface<
    std::tuple<size_t, size_t, size_t, size_t, size_t, int, int, int>> {
 public:
    void populateSnapshotInfo(snapshotVersionTwoInfo *snapshot_info) {
        snapshot_info->snapshot_data_generated_size_bytes = 0;
        snapshot_info->snapshot_file_data_size_bytes = 5000;
        snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes = 5000;
        // Get parameterized offset values
        // Start offset (S)
        snapshot_info->snapshot_common.log_file_tail_offset = std::get<0>(GetParam());
        // End offset (E) = log_file_pending_processing_bytes + log_processed_offset
        end_offset = std::get<1>(GetParam());
        // Recently processed offset (X)
        *snapshot_info->snapshot_log_iterator->log_processed_offset = std::get<2>(GetParam());
        // Target offset (O)
        offset = std::get<3>(GetParam());
        // Log file size
        *snapshot_info->snapshot_log_iterator->log_file_size_bytes_ptr = std::get<4>(GetParam());
        // Log file Pending Processing Bytes
        size_t tail_offset = snapshot_info->snapshot_common.log_file_tail_offset;
        size_t processed_offset = *snapshot_info->snapshot_log_iterator->log_processed_offset;
        size_t log_size = *snapshot_info->snapshot_log_iterator->log_file_size_bytes_ptr;

        if (tail_offset >= end_offset && processed_offset > tail_offset) {
            // Case 1 : +++++++++End+++++++++Tail+++++PendProcess++++++
            snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes = (log_size - processed_offset)
                    + end_offset;
        } else {
            // Case 1 : +++PendProcess+++++End+++++++++Tail++++++++++++
            // case 2 : +++++++++Tail++++++++++++++PendProcess+++End+++
            snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes = end_offset - processed_offset;
        }
    }

    void SetUp() {
        SnapshotV2TestBase::SetUp();
        populateSnapshotInfo(snapshot_info);
    }
};

TEST_P(SnapshotV2Test, testAddDataToSnapshot1MBData) {
    // Create a buffer with exactly 1MB of data
    int data_size = FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES;
    char *buf = createPageAlignedBuffer(data_size);
    memset(buf, 1, data_size);
    // Set up the snapshot_info to populate it with 1MB of data for the entire snapshot
    snapshot_info->snapshot_data_buffer = createPageAlignedBuffer(FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    snapshot_info->snapshot_data_buffer_size = FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES;
    snapshot_info->snapshot_data_buffer_offset = 0;
    snapshot_info->snapshot_data_generated_size_bytes = 0;
    snapshot_info->snapshot_file_data_size_bytes = data_size - getEOFItemSizeBytes();
    snapshot_info->snapshot_data_buffer_list = stagingBufferCreate();
    addDataToSnapshotForTest(snapshot_info, buf, FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    // Here, the snapshot_data_buffer_list should only have 1 item with 1MB data
    stagingBufferEntry *sb_head = stagingBufferGetHead(snapshot_info->snapshot_data_buffer_list);
    ASSERT_EQ(sb_head->item_len, FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    ASSERT_EQ(sb_head->next, nullptr);
    ASSERT_EQ(sb_head->prev, nullptr);
    stagingBufferEntry *sb_tail = stagingBufferGetTail(snapshot_info->snapshot_data_buffer_list);
    ASSERT_EQ(sb_tail->item_len, FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    ASSERT_TRUE(sb_head == sb_tail);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer, nullptr);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer_size, 0);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer_offset, 0);
    fcFree(buf);
    stagingBufferRelease(snapshot_info->snapshot_data_buffer_list);
}

TEST_P(SnapshotV2Test, testAddDataToSnapshotMoreThan1MBData) {
    // Create a buffer with exactly 1.2 MB of data
    int data_size = FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES * 1.2;
    char *buf = createPageAlignedBuffer(data_size);
    memset(buf, 1, data_size);
    char *snapshot_data_buffer = createPageAlignedBuffer(FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    // Set up the snapshot_info to populate it with all data for the entire snapshot
    snapshot_info->snapshot_data_buffer = snapshot_data_buffer;
    snapshot_info->snapshot_data_buffer_size = FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES;
    snapshot_info->snapshot_data_buffer_offset = 0;
    snapshot_info->snapshot_data_generated_size_bytes = 0;
    snapshot_info->snapshot_file_data_size_bytes = data_size - getEOFItemSizeBytes();
    snapshot_info->snapshot_data_buffer_list = stagingBufferCreate();
    addDataToSnapshotForTest(snapshot_info, buf, data_size);
    // Here, the snapshot_data_buffer_list should only have 2 items, one is 1MB,
    // and the other one containing the rest of the snapshot data
    stagingBufferEntry *sb_head = stagingBufferGetHead(snapshot_info->snapshot_data_buffer_list);
    ASSERT_EQ(sb_head->item_len, getCeilPageAlignedOffset(data_size - FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES));
    ASSERT_EQ(sb_head->prev, nullptr);
    stagingBufferEntry *sb_tail = stagingBufferGetTail(snapshot_info->snapshot_data_buffer_list);
    ASSERT_EQ(sb_tail->item_len, FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES);
    ASSERT_EQ(sb_tail->prev, sb_head);
    ASSERT_EQ(sb_tail->next, nullptr);
    ASSERT_EQ(sb_head->next, sb_tail);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer, nullptr);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer_size, 0);
    ASSERT_EQ(snapshot_info->snapshot_data_buffer_offset, 0);
    fcFree(buf);
    stagingBufferRelease(snapshot_info->snapshot_data_buffer_list);
}

TEST_P(SnapshotV2Test, testIsUnprocessedItemInSnapshotRange) {
    expected = std::get<5>(GetParam());
    ASSERT_EQ(expected, isUnprocessedItemInSnapshotRange(snapshot_info, offset));
}

TEST_P(SnapshotV2Test, testIsProcessedItemInSnapshotRange) {
    expected = std::get<6>(GetParam());
    ASSERT_EQ(expected, isProcessedItemInSnapshotRange(snapshot_info, offset));
}

TEST_P(SnapshotV2Test, testIsItemInSnapshotRange) {
    expected = std::get<7>(GetParam());
    ASSERT_EQ(expected, isItemInSnapshotRange(snapshot_info, offset));
}

TEST_P(SnapshotV2Test, testAddReplicationCommandIfRequired) {
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);
    snapshot_info->snapshot_data_buffer_list = stagingBufferCreate();

    // Snapshot is running.
    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_common.has_failed = 0;

    // Enables Threadsave so the function would be called.
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_FORKLESS_SAVE;
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    snapshotV2AddReplicationCommandIfRequired(snapshot_info, offset, dbid, key, key_len,
                                              value, value_len, log->crc_function);

    // The expected value should be consistent with isProcessedItemInSnapshotRange().
    expected = std::get<6>(GetParam());
    ASSERT_EQ(expected, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // The snapshot_data_generated_size_bytes should be > 0 if we add
    // a replication command to the snapshot.
    if (expected == 1) {
        ASSERT_LT(0, snapshot_info->snapshot_data_generated_size_bytes);
    } else {
        ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    }
    snapshot_info->snapshot_common.is_running = 0;
    free(snapshot_info->snapshot_data_buffer);
    stagingBufferRelease(snapshot_info->snapshot_data_buffer_list);
}

TEST_P(SnapshotV2Test, testAddReplicationCommandIfRequiredNegativeCases) {
    uint32_t dbid = 0;
    char *key, *value;
    size_t key_len = 0, value_len = 0;
    generateRandomItem(key, key_len, value, value_len);

    // Threadsave is not enabled.
    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_common.has_failed = 0;
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_BGSAVE;
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    snapshotV2AddReplicationCommandIfRequired(snapshot_info, offset, dbid, key, key_len,
                                              value, value_len, log->crc_function);
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // Snapshot is not running.
    snapshot_info->snapshot_common.is_running = 0;
    snapshot_info->snapshot_common.has_failed = 0;
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_FORKLESS_SAVE;
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    snapshotV2AddReplicationCommandIfRequired(snapshot_info, offset, dbid, key, key_len,
                                              value, value_len, log->crc_function);
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // Snapshot has failed.
    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_common.has_failed = 1;
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_FORKLESS_SAVE;
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    snapshotV2AddReplicationCommandIfRequired(snapshot_info, offset, dbid, key, key_len,
                                              value, value_len, log->crc_function);
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));

    // Snapshot is not running and has failed.
    snapshot_info->snapshot_common.is_running = 0;
    snapshot_info->snapshot_common.has_failed = 1;
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_FORKLESS_SAVE;
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    snapshotV2AddReplicationCommandIfRequired(snapshot_info, offset, dbid, key, key_len,
                                              value, value_len, log->crc_function);
    ASSERT_EQ(0, snapshot_info->snapshot_data_generated_size_bytes);
    ASSERT_EQ(0, logGetCountBasedMetric(log, FC_CURR_NUM_DELETE_REPL_CMD));
    snapshot_info->snapshot_common.is_running = 0;
}

/*
    Argument semantics for parametrized tests:
    <0> Start offset (S): snapshot_common.log_file_tail_offset
    <1> End offset (E)
    <2> Recently processed offset (X): snapshot_log_iterator->log_processed_offset
    <3> Target offset (O)
    <4> Log file size: *snapshot_log_iterator->log_file_size_bytes_ptr
    <5> Expected return value for isUnprocessedItemInSnapshotRange
    <6> Expected return value for isProcessedItemInSnapshotRange
    <7> Expected return value for isItemInSnapshotRange

    Using this representation figure to formulate the test:
    Example:
       TAIL                           HEAD
        |---S+++++++X+++++O+++++++++E--|
        [        30 offset units       ]

        S offset: 3
        E offset: 27
        X offset: 11
        O offset: 17
        Log file size: 30
*/

// CASE 0:
// Log not wrapped around and O is unprocessed.
// TAIL                           HEAD
//  |---S+++++++X+++++O+++++++++E--|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest0, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(27),
        testing::Values(11),
        testing::Values(17),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 1:
// Log not wrapped around and O is processed.
// TAIL                           HEAD
//  |---S+++++++O+++++X+++++++++E--|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest1, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(27),
        testing::Values(17),
        testing::Values(11),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 2:
// Log wrapped around, X not wrapped around and O is processed.
// TAIL                           HEAD
//  |++++++E----------S+++O++X+++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest2, SnapshotV2Test,
    testing::Combine(
        testing::Values(17),
        testing::Values(5),
        testing::Values(24),
        testing::Values(21),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 3:
// Log wrapped around, X wrapped around and O is processed.
// TAIL                           HEAD
//  |++X++E-----------S+++O++++++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest3, SnapshotV2Test,
    testing::Combine(
        testing::Values(17),
        testing::Values(5),
        testing::Values(2),
        testing::Values(21),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 4:
// Log wrapped around, X wrapped around, O wrapped around and processed.
// TAIL                           HEAD
//  |+++O++X+++E----------S++++++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest4, SnapshotV2Test,
    testing::Combine(
        testing::Values(17),
        testing::Values(5),
        testing::Values(2),
        testing::Values(1),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 5:
// Log wrapped around, X wrapped around and O is not processed.
// TAIL                           HEAD
//  |++++++E----------S+++X+++O++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest5, SnapshotV2Test,
    testing::Combine(
        testing::Values(17),
        testing::Values(5),
        testing::Values(21),
        testing::Values(25),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 6:
// Log wrapped around, X wrapped around and O is not processed.
// TAIL                           HEAD
//  |++++O++++E----------S+++X+++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest6, SnapshotV2Test,
    testing::Combine(
        testing::Values(20),
        testing::Values(5),
        testing::Values(24),
        testing::Values(2),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 7:
// Log wrapped around, X wrapped around and O is not processed.
// TAIL                           HEAD
//  |+++X+++O+++E----------S+++++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest7, SnapshotV2Test,
    testing::Combine(
        testing::Values(22),
        testing::Values(11),
        testing::Values(3),
        testing::Values(7),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 8:
// Log not wrapped around and O is before S.
// TAIL                           HEAD
//  |---O----S++++++++X+++++++++E--|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest8, SnapshotV2Test,
    testing::Combine(
        testing::Values(8),
        testing::Values(27),
        testing::Values(17),
        testing::Values(3),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 9:
// Log not wrapped around and O is after E.
// TAIL                           HEAD
//  |---S++++++++X+++++++++E----O--|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest9, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(22),
        testing::Values(12),
        testing::Values(27),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 10:
// Log wrapped around and O is outside of snapshot range.
// TAIL                           HEAD
//  |++++++E-----O-----S+++++X+++++|
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest10, SnapshotV2Test,
    testing::Combine(
        testing::Values(18),
        testing::Values(5),
        testing::Values(24),
        testing::Values(12),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 11:
// Start offset (S) and end offset (E) overlap wrap around.
// O is unprocessed.
// TAIL                           HEAD
//  |+++++O++++++S+++++++++++X+++++|
//               E
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest11, SnapshotV2Test,
    testing::Combine(
        testing::Values(12),
        testing::Values(12),
        testing::Values(24),
        testing::Values(5),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 12:
// Start offset (S) and end offset (E) overlap.
// O is processed.
// TAIL                           HEAD
//  |+++++++++++S+++++O++++++X+++++|
//              E
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest12, SnapshotV2Test,
    testing::Combine(
        testing::Values(11),
        testing::Values(11),
        testing::Values(24),
        testing::Values(17),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 13:
// Log not wrapped around and O is at X.
// TAIL                           HEAD
//  |---S++++++++X+++++++++E-------|
//               O
// Expectations:
//      isUnprocessedItemInSnapshotRange: true
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest13, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(22),
        testing::Values(12),
        testing::Values(12),
        testing::Values(30),
        testing::Values(1),
        testing::Values(0),
        testing::Values(1)));
// CASE 14:
// Log not wrapped around and O is at E.
// TAIL                           HEAD
//  |---S++++++++X+++++++++E-------|
//                         O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest14, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(22),
        testing::Values(12),
        testing::Values(22),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 15:
// Log not wrapped around and O is at S.
// TAIL                           HEAD
//  |---S++++++++X+++++++++E-------|
//      O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest15, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(22),
        testing::Values(12),
        testing::Values(3),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 16:
// Log not wrapped and X is at E.
// TAIL                           HEAD
//  |---S+++++O+++++++++++++E------|
//                          X
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest16, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(23),
        testing::Values(23),
        testing::Values(9),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 17:
// Log not wrapped around and O, X and E overlaps.
// TAIL                           HEAD
//  |---S+++++++++++++++++++E------|
//                          X
//                          O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest17, SnapshotV2Test,
    testing::Combine(
        testing::Values(3),
        testing::Values(23),
        testing::Values(23),
        testing::Values(23),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 18:
// Log wrapped around and S, E, X and O overlaps.
// TAIL                           HEAD
//  |+++++++++++S++++++++++++++++++|
//              E
//              X
//              O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest18, SnapshotV2Test,
    testing::Combine(
        testing::Values(11),
        testing::Values(11),
        testing::Values(11),
        testing::Values(11),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
// CASE 19:
// Start offset (S) and end offset (E) overlap wrap around.
// O is processed.
// TAIL                           HEAD
//  |+++++X++++++S+++++++++++O+++++|
//               E
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest19, SnapshotV2Test,
        testing::Combine(
            testing::Values(12),
            testing::Values(12),
            testing::Values(5),
            testing::Values(24),
            testing::Values(30),
            testing::Values(0),
            testing::Values(1),
            testing::Values(1)));
// CASE 20:
// Start offset (S) and end offset (E) wrap around.
// E (end offset) and X(processed offset) overlaps. O is processed.
// TAIL                           HEAD
//  |+++++E++++++S+++++++++++O+++++|
//        X
//
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest20, SnapshotV2Test,
        testing::Combine(
            testing::Values(12),
            testing::Values(6),
            testing::Values(6),
            testing::Values(24),
            testing::Values(30),
            testing::Values(0),
            testing::Values(1),
            testing::Values(1)));
// CASE 21:
// Start offset (S) and end offset (E)  wrap around.
// E (end offset), O(offset) and X(processed offset) overlaps.
// TAIL                           HEAD
//  |+++++E++++++S+++++++++++++++++|
//        X
//        O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest21, SnapshotV2Test,
        testing::Combine(
            testing::Values(12),
            testing::Values(6),
            testing::Values(6),
            testing::Values(6),
            testing::Values(30),
            testing::Values(0),
            testing::Values(0),
            testing::Values(0)));
// CASE 22:
// Start offset (S) and end offset (E)  wrap around.
// E (end offset), O(offset) overlaps.
// TAIL                           HEAD
//  |++X++E++++++S+++++++++++++++++|
//        O
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: false
//      isItemInSnapshotRange: false
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest22, SnapshotV2Test,
        testing::Combine(
        testing::Values(12),
        testing::Values(6),
        testing::Values(3),
        testing::Values(6),
        testing::Values(30),
        testing::Values(0),
        testing::Values(0),
        testing::Values(0)));
// CASE 23:
// Start offset (S) and end offset (E)  wrap around & overlaps.
// X (Processed offset) overlaps with Start and End.
// TAIL                           HEAD
//  |+++++O++++++S+++++++++++++++++|
//               X
//               E
// Expectations:
//      isUnprocessedItemInSnapshotRange: false
//      isProcessedItemInSnapshotRange: true
//      isItemInSnapshotRange: true
INSTANTIATE_TEST_SUITE_P(ParameterizedSnapshotOffsetTest23, SnapshotV2Test,
        testing::Combine(
        testing::Values(12),
        testing::Values(12),
        testing::Values(12),
        testing::Values(6),
        testing::Values(30),
        testing::Values(0),
        testing::Values(1),
        testing::Values(1)));
