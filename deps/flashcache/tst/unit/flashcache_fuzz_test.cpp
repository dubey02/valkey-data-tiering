#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <map>
#include <set>
#include <utility>
#include <gtest/gtest.h>

#include "flashcache_test_base.hpp"

extern "C" {
#include "include/flashcache.h"
#include "include/util.h"
}

#define DATA_SCALE_MULTIPLIER (150)
#define ITERATION_SCALE_MULTIPLIER (1)
#define INITIAL_INDEX_SIZE_PER_DB (128)
#define NUM_DATABASES (5u)
#define VALUE_CORPUS_SIZE_BYTES (100ul * 1024 * DATA_SCALE_MULTIPLIER)
#define MAX_VALUE_SIZE_BYTES (400ul * DATA_SCALE_MULTIPLIER)
#define NUM_KEYS (2ul * 1024 * 1024 / DATA_SCALE_MULTIPLIER)
#define DB_SIZE_BYTES (3ul * 1024 * 1024 * 1024)
#define NUM_ITERATIONS (2ul * 1024 * 1024 * ITERATION_SCALE_MULTIPLIER)
#define FLUSH_INTERVAL (128ul * 1024 * ITERATION_SCALE_MULTIPLIER)
#define SNAPSHOT_INTERVAL (256ul * 1024 * ITERATION_SCALE_MULTIPLIER)
#define LOG_STATS_INTERVAL (16ul * 1024 * ITERATION_SCALE_MULTIPLIER)

/*
 * A corpus of characters (value_corpus) is used to generate the value of an item. The key of the item is a randomly
 * generated integer. The value is a sub-string of the value_corpus. The start index and size of the sub-string is
 * generated randomly. The database id of the item is also randomly generated. When an item is written to flashcache,
 * the key, the value start index and value size is also stored in an in-memory map. This map acts as ground truth.
 * When an item is read from flashcache, the value read is compared with the value generated using the in-memory map to
 * validate correctness.
 * When a snapshot is taken, the in-memory map is copied. The copied in-memory map is used to restore the state from
 * the point when the snapshot was taken. This helps validate that load in FlashCache is able to perform a point in
 * time restore.
 */
static std::set<std::pair<uint32_t, std::string>> inflight_read_keys;
static char value_corpus[VALUE_CORPUS_SIZE_BYTES];
static std::map<std::string, std::pair<size_t, size_t>> memdb[NUM_DATABASES];
static std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
static bool snapshot_complete = false;
static bool has_snapshot = false;
static int expected_snapshot_success = 1;
static size_t num_key_evicted = 0;
static size_t num_key_read_started = 0;
static size_t num_key_read_completed = 0;
static size_t num_empty_read = 0;
static size_t num_key_written = 0;
static size_t num_flushdb = 0;
static size_t num_flushalldbs = 0;
static size_t num_save_snapshot_started = 0;
static size_t num_save_snapshot_completed = 0;
static size_t num_load_snapshot = 0;
static std::vector<std::tuple<flashcacheSnapshotVersion, flashcacheSnapshotSaveType>> snapshot_configs = {
    {FC_SNAPSHOT_VERSION_ONE, FC_SAVE_TYPE_BGSAVE},
    {FC_SNAPSHOT_VERSION_ONE, FC_SAVE_TYPE_FORKLESS},
    {FC_SNAPSHOT_VERSION_TWO, FC_SAVE_TYPE_BGSAVE},
    {FC_SNAPSHOT_VERSION_TWO, FC_SAVE_TYPE_FORKLESS}
};

typedef struct {
    std::string key;
    uint32_t dbid;
} readContext;

static void fuzzTestLogger(int level, const char *fmt, ...) {
    if (level < FC_LL_NOTICE) return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static bool memdbKeyExists(uint32_t dbid, std::string& key) {
    return (memdb[dbid].find(key) != memdb[dbid].end());
}

static void resetGlobalVariables() {
    for (uint32_t i = 0; i < NUM_DATABASES; ++i) {
        memdb[i].clear();
    }

    inflight_read_keys.clear();
    snapshot_complete = false;
    has_snapshot = false;
    expected_snapshot_success = 1;
    num_key_evicted = 0;
    num_key_read_started = 0;
    num_key_read_completed = 0;
    num_empty_read = 0;
    num_key_written = 0;
    num_flushdb = 0;
    num_flushalldbs = 0;
    num_save_snapshot_started = 0;
    num_save_snapshot_completed = 0;
    num_load_snapshot = 0;
}

static void evictionCallback(void *context, uint32_t dbid, char *key, size_t key_len) {
    flashcacheAssert(context == nullptr);
    std::string key_str(key, key_len);
    flashcacheAssert(memdbKeyExists(dbid, key_str));
    flashcacheAssert(memdb[dbid].erase(key_str) == 1);
    fuzzTestLogger(FC_LL_VERBOSE, "Key evicted %s, db: %u", key_str.c_str(), dbid);
    num_key_evicted++;
}

static void readCallback(void *request_context, char *value, size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb;
    readContext *read_context = static_cast<readContext *>(request_context);
    uint32_t dbid = read_context->dbid;
    std::string key_str = read_context->key;
    bool key_exists = memdbKeyExists(dbid, key_str);
    std::pair<uint32_t, std::string> key_pair(dbid, key_str);
    fuzzTestLogger(FC_LL_VERBOSE, "Read callback for key: %s, db: %u", key_str.c_str(), dbid);
    flashcacheAssert(inflight_read_keys.erase(key_pair) == 1);
    if (value == NULL) {
        flashcacheAssert(!key_exists);
        num_empty_read++;
    } else {
        flashcacheAssert(key_exists);
        flashcacheAssert(memdb[dbid][key_str].second == value_len);
        flashcacheAssert(memcmp(value, value_corpus + memdb[dbid][key_str].first, value_len) == 0);
        memdb[dbid].erase(key_str);
    }
    delete read_context;
    num_key_read_completed++;
}

void fuzzTestSnapshotCompletionCallback(void *context, int success) {
    flashcacheAssert(context == nullptr);
    flashcacheAssert(success == expected_snapshot_success);
    // Save can be cancelled by a flush. We set has_snapshot to false so that we don't try to load the incomplete
    // snapshot.
    if (expected_snapshot_success == 0) {
        has_snapshot = false;
    }
    snapshot_complete = true;
    fuzzTestLogger(FC_LL_VERBOSE, "Completed snapshot save, success: %d", success);
    num_save_snapshot_completed++;
}

static uint64_t clockGetTimeUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
            start_time).count();
}

class FlashcacheFuzzTest : public flashcache::FlashcacheTestBase, public testing::Test {
 public:
     std::string db_filename;
     std::string snapshot_filename;
     evictionContext eviction_context;
     std::mt19937 rng;
     std::uniform_int_distribution<int> corpus_distribution;
     std::uniform_int_distribution<size_t> value_start_index_distribution;
     std::uniform_int_distribution<size_t> value_size_distribution;
     std::uniform_int_distribution<size_t> key_distribution;
     std::uniform_int_distribution<unsigned int> dbid_distribution;
     std::uniform_int_distribution<unsigned int> flush_dbid_distribution;
     std::map<std::string, std::pair<size_t, size_t>> memdb_snapshot[NUM_DATABASES];
     flashcacheSnapshotSecret last_snapshot_secret;
     flashcacheSnapshotVersion flashcache_snapshot_version;
     flashcacheSnapshotSaveType flashcache_snapshot_save_type;

     void changeSnapshotVersionAndSaveType(size_t iteration) {
         size_t index = iteration % snapshot_configs.size();
         flashcache_snapshot_version = std::get<0>(snapshot_configs[index]);
         flashcache_snapshot_save_type = std::get<1>(snapshot_configs[index]);
     }

     void SetUp() {
         flashcache::FlashcacheTestBase::SetUp();
         snapshot_filename = createTmpFile(0);
         db_filename = createTmpFile(DB_SIZE_BYTES);
         unsigned int curr_time = time(nullptr);
         int seed = rand_r(&curr_time);
         fuzzTestLogger(FC_LL_NOTICE, "Random number generator seed: %d", seed);
         rng.seed(seed);
         value_start_index_distribution = std::uniform_int_distribution(0ul, MAX_VALUE_SIZE_BYTES - 1);
         value_size_distribution = std::uniform_int_distribution(0ul, MAX_VALUE_SIZE_BYTES);
         flush_dbid_distribution = std::uniform_int_distribution(0u, NUM_DATABASES);
         dbid_distribution = std::uniform_int_distribution(0u, NUM_DATABASES - 1);
         key_distribution = std::uniform_int_distribution(0ul, NUM_KEYS);
         corpus_distribution = std::uniform_int_distribution<int>(0, SCHAR_MAX);
         for (size_t i = 0; i < VALUE_CORPUS_SIZE_BYTES; ++i) {
             value_corpus[i] = corpus_distribution(rng);
         }

         flashcache_snapshot_version = FC_SNAPSHOT_VERSION_TWO;  // Default the snapshot version to 2.
         size_t max_allocated_db_size_percent = 50;
         size_t max_num_in_flight_read_requests = 128;
         size_t min_garbage_collection_rate = 4096;
         size_t evict_under_max_logsize_time_limit = 100 * 1000;
         uint8_t optimized_delete_enabled = 1;
         flashcacheEvictionDetails eviction_details = { 0 };
         eviction_details.context = static_cast<void *>(&mockStorageIoThreadContext);
         eviction_details.callback = evictionCallback;
         flashcacheStorageIoThreadControlMsgCallbackDetails storage_io_thread_control_msg_callback =
             {static_cast<void *>(&mockStorageIoThreadContext), mockStorageIoThreadCallback};

         ASSERT_EQ(flashcacheInit(db_filename.c_str(), DB_SIZE_BYTES, INITIAL_INDEX_SIZE_PER_DB, NUM_DATABASES,
                     max_allocated_db_size_percent, max_num_in_flight_read_requests, min_garbage_collection_rate,
                     evict_under_max_logsize_time_limit, optimized_delete_enabled, clockGetTimeUs, &eviction_details,
                     fuzzTestLogger, &storage_io_thread_control_msg_callback), FC_OK);
     }

     void TearDown() {
         flashcache::FlashcacheTestBase::TearDown();
         ASSERT_EQ(flashcacheTearDown(), FC_OK);
         unlink(db_filename.c_str());
         unlink(snapshot_filename.c_str());
         resetGlobalVariables();
     }

     void logStatsIfRequired(int log_level, size_t iteration) {
         if (iteration % LOG_STATS_INTERVAL) {
             return;
         }

         fuzzTestLogger(log_level, "============= STATS BEGIN ===========");
         fuzzTestLogger(log_level, "Num keys in all db: %u", flashcacheGetCountBasedMetric(FC_NUM_ITEMS));
         fuzzTestLogger(log_level, "Used db size in bytes: %u",
                 flashcacheGetCountBasedMetric(FC_ALLOCATED_DB_SIZE_BYTES));
         fuzzTestLogger(log_level, "Num key read started: %u", num_key_read_started);
         fuzzTestLogger(log_level, "Num key read completed: %u", num_key_read_completed);
         fuzzTestLogger(log_level, "Num empty read: %u", num_empty_read);
         fuzzTestLogger(log_level, "Num key written: %u", num_key_written);
         fuzzTestLogger(log_level, "Num key evicted: %u", num_key_evicted);
         fuzzTestLogger(log_level, "Num flushdb: %u", num_flushdb);
         fuzzTestLogger(log_level, "Num flushalldbs: %u", num_flushalldbs);
         fuzzTestLogger(log_level, "Num save snapshot started: %u", num_save_snapshot_started);
         fuzzTestLogger(log_level, "Num save snapshot completed: %u", num_save_snapshot_completed);
         fuzzTestLogger(log_level, "Num load snapshot: %u", num_load_snapshot);
         fuzzTestLogger(log_level, "============= STATS END   ===========");
     }

     void performReadOrWrite() {
         size_t key = key_distribution(rng);
         std::string key_str = std::to_string(key);
         uint32_t dbid = dbid_distribution(rng);
         std::pair<uint32_t, std::string> key_pair(dbid, key_str);
         // If the key is currently being read, avoid reading it again.
         if (inflight_read_keys.find(key_pair) != inflight_read_keys.end()) {
             return;
         }
         // Read the key if the key is present in the database. Else write the key to the database.
         if (memdbKeyExists(dbid, key_str)) {
             readContext *context = new readContext();
             context->dbid = dbid;
             context->key = key_str;
             inflight_read_keys.insert(key_pair);
             while (flashcacheGetItem(dbid, key_str.c_str(), key_str.length(), FC_READ, static_cast<void *>(context),
                         readCallback) != FC_OK) {
                 flashcacheRunCronTasks();
             }
             fuzzTestLogger(FC_LL_VERBOSE, "Reading key: %s, db: %u", key_str.c_str(), dbid);
             num_key_read_started++;
         } else {
             size_t index = value_start_index_distribution(rng);
             size_t value_size = std::min(value_size_distribution(rng), VALUE_CORPUS_SIZE_BYTES - index);
             std::string value(value_corpus + index, value_size);
             memdb[dbid][key_str] = std::make_pair(index, value_size);
             while (flashcachePutItem(dbid, key_str.c_str(), key_str.length(), value_corpus + index, value_size)
                     != FC_OK) {
                 flashcacheRunCronTasks();
             }
             fuzzTestLogger(FC_LL_VERBOSE, "Writing key: %s, db: %u", key_str.c_str(), dbid);
             num_key_written++;
         }
         flashcacheRunCronTasks();
     }

     void flushDbIfRequired(size_t iteration) {
         if (iteration % FLUSH_INTERVAL) {
             return;
         }
         // Incase a save is in progress, expected_snapshot_success is set to 0 so that the save completion handler
         // know that the save is expected to fail.
         expected_snapshot_success = 0;
         uint32_t dbid = flush_dbid_distribution(rng);
         if (dbid < NUM_DATABASES) {
             fuzzTestLogger(FC_LL_VERBOSE, "Flushing db: %u", dbid);
             flashcacheFlushDB(dbid);
             memdb[dbid].clear();
             num_flushdb++;
         } else {
             fuzzTestLogger(FC_LL_VERBOSE, "Flushing all dbs");
             flashcacheFlushAllDBs();
             for (uint32_t i = 0; i < NUM_DATABASES; ++i) {
                 memdb[i].clear();
             }
             num_flushalldbs++;
         }
     }

     flashcacheSnapshotSecret generateSnapshotSecret() {
         flashcacheSnapshotSecret secret;
         secret.size = 32;
         for (size_t i = 0; i < secret.size; ++i) {
             secret.secret[i] = corpus_distribution(rng);
         }
         return secret;
     }

     void saveOrLoadSnapshotIfRequired(size_t iteration) {
         if (iteration % SNAPSHOT_INTERVAL) {
             return;
         }

         if (has_snapshot) {
             // If a snapshot was created, we try to load the snapshot. If the save was started and has not yet
             // completed, we wait for the save to complete before loading the snapshot. After loading the snapshot,
             // we also restore the in-memory state.
             flashcacheSnapshotSecret secret;
             int checksum_comparison_result = 0;
             while (!snapshot_complete) {
                 flashcacheRunCronTasks();
             }
             flashcacheLoadSnapshot(snapshot_filename.c_str(), &secret, &checksum_comparison_result);
             flashcacheAssert(memcmp(&secret, &last_snapshot_secret, sizeof(flashcacheSnapshotSecret)) == 0);
             // Restore the old database from snapshot
             for (uint32_t i = 0; i < NUM_DATABASES; ++i) {
                 memdb[i] = memdb_snapshot[i];
             }
             has_snapshot = false;
             fuzzTestLogger(FC_LL_VERBOSE, "Loaded snapshot");
             num_load_snapshot++;
         } else {
             // If there is no snapshot present or save in progress, a new save is started. A copy of the in-memory
             // state is also created so that we can restore the in-memory state when the snapshot is loaded in
             // FlashCache.
             expected_snapshot_success = 1;
             last_snapshot_secret = generateSnapshotSecret();
             snapshot_complete = false;
             flashcacheSnapshotCallbackDetails callback_details = {};
             callback_details.context = nullptr;
             callback_details.callback = fuzzTestSnapshotCompletionCallback;
             // Toggle the snapshot version every time to cover alternation between all versions
             changeSnapshotVersionAndSaveType(iteration);
             flashcacheStartFileBasedSave(snapshot_filename.c_str(), &last_snapshot_secret,
                     &callback_details, 1, flashcache_snapshot_version, flashcache_snapshot_save_type);
             // Take a copy of the database so that we can restore from it when we load the snapshot
             for (uint32_t i = 0; i < NUM_DATABASES; ++i) {
                 memdb_snapshot[i] = memdb[i];
             }
             has_snapshot = true;
             fuzzTestLogger(FC_LL_VERBOSE, "Start snapshot save");
             num_save_snapshot_started++;
         }
     }
};

TEST_F(FlashcacheFuzzTest, fuzzTest) {
    for (size_t i = 0; i < NUM_ITERATIONS; ++i) {
        performReadOrWrite();
        flushDbIfRequired(i);
        saveOrLoadSnapshotIfRequired(i);
        logStatsIfRequired(FC_LL_VERBOSE, i);
    }
    ASSERT_GT(mockStorageIoThreadContext, 0);  // Callback must have been called at least once here
}
