#include <random>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <map>
#include "Crc32.h"

extern "C" {
#include "monotonic.h"
#include "include/flashcache.h"
}

#define UNUSED(x) (void)(x)

#define MAX_FILENAME_LEN (512)

#define VALUE_MAX_SIZE (5 * 1024 * 1024)
#define VALUE_MIN_SIZE (100)
#define VALUE_STD_DEV (100.0f)
#define LARGE_VALUE_SIZE (100 * 1024 * 1024)  // 100 MiB

#define MAX_KEY_SIZE (16)
#define MAX_NUM_INFLIGHT_READ (128)
#define DBID (0)
#define SECOND_IN_MICROSECOND (1e6)
#define MEBIBYTE_TO_BYTES (1024 * 1024)
#define GIGIBYTE_TO_BYTES (1024 * 1024 * 1024)

typedef enum {
    FC_CONSTANT_VALUE_SIZE,
    FC_NORMAL_DIST_VALUE_SIZE
} valueSizeDistributionType;

typedef enum {
    FC_PREFER_OLD_ITEM,
    FC_PREFER_NEW_ITEM,
    FC_PREFER_MIDDLE_ITEM
} readingPreference;

typedef struct readRequestInfo {
    monotime start_time_us;
} readRequestInfo;

// Snapshot context used for tracking snapshot information
typedef struct {
    int is_running;
    uint64_t start_time_second;
    size_t num_item_read;
    size_t num_item_written;
    size_t num_read_complete;
} snapshotContext;

static std::mt19937 random_number_generator;
static std::uniform_int_distribution<unsigned long> mean_value_picker_dist;
static std::map<int, std::normal_distribution<float>> mean_value_dist_map;

static size_t num_read_inflight = 0;
static char db_filename[MAX_FILENAME_LEN];
static char value[VALUE_MAX_SIZE];
static size_t dbsize = 0;
static size_t max_num_items = 0;
static size_t num_item_written = 0;
static size_t num_item_read = 0;
static size_t prev_stat_collection_time_microsecond = 0;
static size_t prev_num_item_written = 0;
static int num_large_items = 0;
static monotime last_read_item_time_us = 0;
static size_t read_delay_us = 0;
static size_t read_to_write_ratio = 1;
static size_t next_print_stats_num_ops = 0;
static int log_level = FC_LL_NOTICE;

// Interval size of 200us is used for testing with item size <= 20KB. For large items interval size
// of 2000us is used.
static const int read_histogram_interval_size_us = 200;
static std::vector<int> read_histogram(10, 0);

// Interval size of 10us is used for testing with item size <= 20KB. For large items interval size
// of 2000us is used.
static const int write_histogram_interval_size_us = 10;
static std::vector<int> write_histogram(10, 0);

static size_t prev_num_read_complete = 0;
static size_t num_read_complete = 0;
static int num_ops_per_stat_emission = 2000000;
static size_t num_read_throttled = 0;
static size_t num_write_throttled = 0;

static std::vector<int> mean_value_size_list;
static valueSizeDistributionType value_size_distribution_type = FC_NORMAL_DIST_VALUE_SIZE;
static readingPreference reading_preference = FC_PREFER_OLD_ITEM;
static flashcacheSnapshotVersion flashcache_snapshot_version = FC_SNAPSHOT_VERSION_TWO;
static flashcacheSnapshotSaveType flashcache_snapshot_save_type = FC_SAVE_TYPE_BGSAVE;
static snapshotContext snapshot_context = { 0 };
static char snapshot_filename[MAX_FILENAME_LEN] = "/log_store/snapshot.txt";
static const char large_value[LARGE_VALUE_SIZE] = { 0 };

void printStats(bool force);

void logging_function(int level, const char *fmt, ...) {
    if (level < log_level) return;

    va_list args;
    va_start(args, fmt);
    if (level == 0) {
        printf("[DEBUG] ");
    } else if (level == 1) {
        printf("[VERBOSE] ");
    } else if (level == 2) {
        printf("[NOTICE] ");
    } else {
        printf("[WARNING] ");
    }
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

uint64_t timeInMicrosecond() {
    return getMonotonicUs();
}

uint64_t timeInSecond() {
    return timeInMicrosecond() / (1000 * 1000);
}

void updateHistogram(std::vector<int> &histogram, int interval_size, int val) {
    size_t histogram_idx = val / interval_size;
    if (histogram_idx >= histogram.size()) {
        histogram_idx = histogram.size() - 1;
    }
    histogram[histogram_idx]++;
}

void resetHistogram(std::vector<int> &histogram) {
    for (size_t i = 0; i < histogram.size(); ++i) {
        histogram[i] = 0;
    }
}

void printHistogram(std::vector<int> &histogram) {
    double total = 0;
    for (size_t i = 0; i < histogram.size(); ++i) {
        total += histogram[i];
    }

    for (size_t i = 0; i < histogram.size(); ++i) {
        if (i > 0) {
            printf(", ");
        }
        double res = 0;
        if (total > 0) {
            res = histogram[i] / total * 100;
        }
        printf("%.2lf", res);
    }
}

void eviction_callback(void *context, uint32_t dbid, char *key, size_t key_len) {
    UNUSED(context);
    UNUSED(dbid);
    UNUSED(key);
    UNUSED(key_len);
}

void get_item_completion_callback(void *request_context, char *value,
        size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb;
    if (value == NULL || value_len == 0) {
        printf("[ERROR] Received empty value\n");
        exit(1);
    }

    readRequestInfo *request = static_cast<readRequestInfo *>(request_context);
    updateHistogram(read_histogram, read_histogram_interval_size_us,
            elapsedUs(request->start_time_us));

    delete request;
    num_read_inflight--;
    num_read_complete++;
}

void snapshotCompletionCallback(void *context, int completed) {
    snapshotContext *snapshot_context = static_cast<snapshotContext *>(context);

    if (completed == 0) {
        printf("[ERROR] Snapshot completed status is incorrect");
        exit(1);
    }

    snapshot_context->is_running = 0;
    printf("=====================================================================\n");
    printf("Snapshot completion time (seconds): %lu\n", (timeInSecond() - snapshot_context->start_time_second));
    printf("=====================================================================\n");

    printStats(true);
    uint64_t snapshot_load_start_time = timeInSecond();
    printf("=====================================================================\n");
    printf("Start snapshot loading\n");
    printf("=====================================================================\n");

    // Load the new snapshot and update the read and write index based on the read and write when the snapshot was
    // taken
    flashcacheSnapshotSecret secret_response = {0};
    int checksum_comparison_result = 0;
    flashcacheLoadSnapshot(snapshot_filename, &secret_response, &checksum_comparison_result);

    printf("=====================================================================\n");
    printf("Snapshot loading completed, Time taken (seconds): %lu\n", (timeInSecond() - snapshot_load_start_time));
    printf("=====================================================================\n");

    num_item_read = snapshot_context->num_item_read;
    num_read_complete =  snapshot_context->num_read_complete;
    num_item_written = snapshot_context->num_item_written;
    prev_stat_collection_time_microsecond = 0;
    prev_num_read_complete = num_read_complete;
    prev_num_item_written = num_item_written;
    next_print_stats_num_ops = num_item_read + num_item_written;
}

void writeLargeItems() {
    if (num_large_items == 0) {
        return;
    }

    char key[MAX_KEY_SIZE];
    for (int i = 0; i < num_large_items; ++i) {
        size_t key_len = snprintf(key, MAX_KEY_SIZE, "large-%d", i);
        while (flashcachePutItem(DBID, key, key_len, large_value, LARGE_VALUE_SIZE) != FC_OK) {
            flashcacheRunCronTasks();
        }
    }
}

void writeData() {
    // Write after after every 'read_to_write_ratio' number of reads
    if (read_to_write_ratio > 1 && num_item_written > max_num_items &&
            num_item_read % read_to_write_ratio != 0) {
        return;
    }

    char key[MAX_KEY_SIZE];
    ssize_t value_size = 0;
    size_t idx = mean_value_picker_dist(random_number_generator);
    switch (value_size_distribution_type) {
        case FC_CONSTANT_VALUE_SIZE:
            value_size = mean_value_size_list[idx];
            break;
        case FC_NORMAL_DIST_VALUE_SIZE:
            value_size = std::round(mean_value_dist_map[mean_value_size_list[idx]](
                        random_number_generator));
            if (value_size < VALUE_MIN_SIZE) {
                value_size = VALUE_MIN_SIZE;
            }
            if (value_size > VALUE_MAX_SIZE) {
                value_size = VALUE_MAX_SIZE;
            }
            break;
        default:
            printf("Invalid value distribution type: %d", value_size_distribution_type);
            exit(1);
    }

    size_t key_len = snprintf(key, MAX_KEY_SIZE, "key-%lu", num_item_written++);
    monotime start_time_us;
    elapsedStart(&start_time_us);
    bool throttled = false;
    while (flashcachePutItem(DBID, key, key_len, value, value_size) != FC_OK) {
        throttled = true;
        flashcacheRunCronTasks();
    }
    if (throttled) {
        num_write_throttled++;
    }
    updateHistogram(write_histogram, write_histogram_interval_size_us,
            elapsedUs(start_time_us));
}

int readData() {
    // Start reading only after max items has been written.
    if (num_item_written < max_num_items) {
        return 1;
    }

    if (last_read_item_time_us > 0 && elapsedUs(last_read_item_time_us) < read_delay_us) {
        return 0;
    }

    bool throttled = false;
    while (num_read_inflight >= MAX_NUM_INFLIGHT_READ) {
        throttled = true;
        flashcacheRunCronTasks();
    }
    if (throttled) {
        num_read_throttled++;
    }

    size_t key_id;
    switch (reading_preference) {
        case FC_PREFER_NEW_ITEM:
            // Read from the recently written object
            key_id = num_item_written - std::min(100000ul, max_num_items);
            break;
        case FC_PREFER_OLD_ITEM:
            // Read the oldest item
            key_id = num_item_read;
            break;
        case FC_PREFER_MIDDLE_ITEM:
            // Read the item that is in between the newest and the oldest item in age
            key_id = num_item_written - (max_num_items / 2);
            break;
        default:
            printf("Unknown reading preference: %d\n", reading_preference);
            exit(1);
    }
    num_item_read++;

    char key[MAX_KEY_SIZE];
    num_read_inflight++;
    size_t key_len = snprintf(key, MAX_KEY_SIZE, "key-%lu", key_id);
    readRequestInfo *request = new readRequestInfo;
    elapsedStart(&request->start_time_us);
    elapsedStart(&last_read_item_time_us);
    if (flashcacheGetItem(DBID, key, key_len, FC_READ, request, get_item_completion_callback) !=
            FC_OK) {
        printf("Unable to get data into flashcache");
        exit(1);
    }
    return 1;
}

void printStats(bool force) {
    size_t total_ops = num_item_read + num_item_written;
    if (total_ops < next_print_stats_num_ops && !force) {
        return;
    }
    next_print_stats_num_ops += num_ops_per_stat_emission;

    uint64_t current_time = timeInMicrosecond();
    double time_delta_us = current_time - prev_stat_collection_time_microsecond;
    // Don't print stats if the time elapsed since the last time stat was printed is less than a second
    if (time_delta_us < SECOND_IN_MICROSECOND) {
        return;
    }

    static size_t prev_total_bytes_read = 0;
    static size_t prev_total_bytes_written = 0;
    static size_t prev_gc_bytes_read = 0;
    static size_t prev_gc_bytes_written = 0;

    size_t curr_total_bytes_read = flashcacheGetCountBasedMetric(FC_TOTAL_DISK_READ_BYTES);
    size_t curr_total_bytes_written = flashcacheGetCountBasedMetric(FC_TOTAL_DISK_WRITE_BYTES);
    size_t curr_gc_bytes_read = flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_READ_BYTES);
    size_t curr_gc_bytes_written = flashcacheGetCountBasedMetric(FC_GARBAGE_COLLECTION_WRITE_BYTES);

    size_t num_read_since_last_stat = (num_read_complete - prev_num_read_complete);
    size_t num_write_since_last_stat = (num_item_written - prev_num_item_written);
    size_t num_items = flashcacheGetCountBasedMetric(FC_NUM_ITEMS);
    size_t read_tps = num_read_since_last_stat * SECOND_IN_MICROSECOND / time_delta_us;
    size_t write_tps = num_write_since_last_stat * SECOND_IN_MICROSECOND / time_delta_us;
    double read_throughput = (curr_total_bytes_read - prev_total_bytes_read) *
        SECOND_IN_MICROSECOND / time_delta_us / MEBIBYTE_TO_BYTES;
    double write_throughput = (curr_total_bytes_written - prev_total_bytes_written) *
        SECOND_IN_MICROSECOND / time_delta_us / MEBIBYTE_TO_BYTES;
    double gc_read_throughput = (curr_gc_bytes_read - prev_gc_bytes_read) *
        SECOND_IN_MICROSECOND / time_delta_us / MEBIBYTE_TO_BYTES;
    double gc_write_throughput = (curr_gc_bytes_written - prev_gc_bytes_written) *
        SECOND_IN_MICROSECOND / time_delta_us / MEBIBYTE_TO_BYTES;

    // Write amplification is the total bytes written by the actual bytes written
    double write_amp = write_throughput / (write_throughput - gc_write_throughput);

    double used_memory_gb = (static_cast<double>(flashcacheGetCountBasedMetric(
                FC_ACTIVE_MEMORY_SIZE))) / GIGIBYTE_TO_BYTES;
    double active_db_size_gb = (static_cast<double>(flashcacheGetCountBasedMetric(
                FC_ACTIVE_DB_SIZE_BYTES))) / GIGIBYTE_TO_BYTES;
    double allocated_db_size_gb = (static_cast<double>(flashcacheGetCountBasedMetric(
                FC_ALLOCATED_DB_SIZE_BYTES))) / GIGIBYTE_TO_BYTES;
    double curr_gc_rate_mbps = (static_cast<double>(flashcacheGetCountBasedMetric(
                FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND))) / MEBIBYTE_TO_BYTES;

    if (prev_stat_collection_time_microsecond) {
        printf("%-20ld | %-20ld | %-20ld | %-20lf | %-20lf | %-20lf | %-20lf | %-20lf | %-20lf | "
                "%-20lf | %-20lf | %-20ld | %-20ld | %-20lf | ",
                num_items, read_tps, write_tps, read_throughput, write_throughput,
                gc_read_throughput, gc_write_throughput, active_db_size_gb,
                allocated_db_size_gb, curr_gc_rate_mbps, write_amp, num_read_throttled,
                num_write_throttled, used_memory_gb);
        printHistogram(read_histogram);
        printf(" / ");
        printHistogram(write_histogram);
        printf("\n");
    } else {
        printf("%-20s | %-20s | %-20s | %-20s | %-20s | %-20s | %-20s | %-20s | %-20s | %-20s | "
                "%-20s | %-20s | %-20s | %-20s | %s\n",
                "num items", "read tps", "write tps", "disk read MiB/s", "disk write MiB/s",
                "gc read MiB/s", "gc write MiB/s", "active DB size GiB", "allocated DB size GiB",
                "curr gc rate MiB/s", "write amp", "num read throttled", "num write throttled",
                "used memory GiB", "read latency / write latency");
    }
    fflush(stdout);

    prev_stat_collection_time_microsecond = current_time;
    prev_num_read_complete = num_read_complete;
    prev_num_item_written = num_item_written;
    num_read_throttled = 0;
    num_write_throttled = 0;

    prev_total_bytes_read = curr_total_bytes_read;
    prev_total_bytes_written = curr_total_bytes_written;
    prev_gc_bytes_read = curr_gc_bytes_read;
    prev_gc_bytes_written = curr_gc_bytes_written;

    resetHistogram(read_histogram);
    resetHistogram(write_histogram);
}

const char *test_secret =
    "abcdefghijklmnopqrestuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ§!@#$%^&**()_+=-{}[];:|<>?±~"
    "abcdefghijklmnopqrestuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ§!@#$%^&**()_+=-{}[];:|<>?±~"
    "abcdefghijklmnopqrestuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ§!@#$%^&**()_+=-{}[];:|<>?±~"
    "abcdefghijklmnopqrestuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ§!@#$%^&**()_+=-{}[];:|<>?±~";

flashcacheSnapshotSecret shared_secret = {0};

void parseArgs(int argc, char *argv[]) {
    // Parse command line arguments
    if (argc != 13) {
        printf("Invalid number of arguments: Required arguments are:\n");
        printf("<db_filename>: The name of the DB file\n");
        printf("<dbsize_GiB>: The size of DB in GiB\n");
        printf("<max_num_items>: The maximum number of items in DB\n");
        printf("<mean_value_sizes>:<weight>: Comma separated mean value sizes with the value "
                "weight. The weight determines how often the value is used, For in this example: "
                "500:1,200:8,10000:1 value with mean size 500 is written 10%% of time, value with "
                "mean size 200 is written 80%% of time and value with mean size 10000 is written "
                "10%% of time, If weight is not present, it is considered to be 1\n");
        printf("<value_distribution_type>: normal_dist or constant_dist\n");
        printf("<number_of_large_items>: Number of items of size 100 MiB\n");
        printf("<reading_preference>: prefer_old_item or prefer_new_item or prefer_middle_item\n");
        printf("<tps>: expected read/write tps\n");
        printf("<num_ops_per_stat_emission>: Number of ops before emitting stats\n");
        printf("<read_to_write_ratio>: Number of read operations per write operation\n");
        printf("<snapshot_version>: The version of snapshot algorithm to use values 1 or 2.\n");
        printf("<snapshot_save_type>: bgsave or forkless_save\n");
        exit(1);
    }

    int arg_idx = 1;
    // Parse the DB filename
    snprintf(db_filename, MAX_FILENAME_LEN, "%s", argv[arg_idx++]);
    printf("Using DB filename: %s\n", db_filename);

    // Parse size of database
    dbsize = atoi(argv[arg_idx++]) * 1024LL * 1024 * 1024;
    printf("Using DB size %lu bytes\n", dbsize);

    // Parse max number of items
    max_num_items = atoll(argv[arg_idx++]);
    printf("Using max number of items: %lu\n", max_num_items);

    // Parse value mean sizes
    char *arg = argv[arg_idx++];
    char *outer_token_ptr, *inner_token_ptr;
    char *token = strtok_r(arg, ",", &outer_token_ptr);
    while (token) {
        char *inner_token = strtok_r(token, ":", &inner_token_ptr);
        int mean_value_size = atoi(inner_token);

        inner_token = strtok_r(nullptr, ":", &inner_token_ptr);
        int weight = 1;
        if (inner_token != nullptr) {
            weight = atoi(inner_token);
        }
        for (int i = 0; i < weight; ++i) {
            mean_value_size_list.push_back(mean_value_size);
        }

        if (mean_value_dist_map.count(mean_value_size) > 0) {
            printf("Duplicate value present in the mean value sizes: %d\n", mean_value_size);
            exit(1);
        }
        mean_value_dist_map[mean_value_size] = std::normal_distribution(
                static_cast<float>(mean_value_size), VALUE_STD_DEV);
        token = strtok_r(nullptr, ",", &outer_token_ptr);
    }
    mean_value_picker_dist = std::uniform_int_distribution(0ul, mean_value_size_list.size() - 1);

    // Parse the value distribution type
    arg = argv[arg_idx++];
    if (!strcmp(arg, "normal_dist")) {
        value_size_distribution_type = FC_NORMAL_DIST_VALUE_SIZE;
    } else if (!strcmp(arg, "constant_dist")) {
        value_size_distribution_type = FC_CONSTANT_VALUE_SIZE;
    } else {
        printf("Unknown value distribution type: %s", arg);
        exit(1);
    }
    printf("Using value distribution type: %s\n", arg);

    // Parse number of large items
    num_large_items = atoi(argv[arg_idx++]);
    printf("Using %d large items\n", num_large_items);

    // Parse reading order
    arg = argv[arg_idx++];
    if (!strcmp(arg, "prefer_old_item")) {
        reading_preference = FC_PREFER_OLD_ITEM;
    } else if (!strcmp(arg, "prefer_new_item")) {
        reading_preference = FC_PREFER_NEW_ITEM;
    } else if (!strcmp(arg, "prefer_middle_item")) {
        reading_preference = FC_PREFER_MIDDLE_ITEM;
    } else {
        printf("Unknown reading order: %s\n", arg);
        exit(1);
    }
    printf("Using reading order: %s\n", arg);

    // Parse read/write tps
    int tps = atoi(argv[arg_idx++]);
    if (tps > 0) {
        read_delay_us = SECOND_IN_MICROSECOND / tps;
    }

    // Parse number of ops executed between each stat emission
    num_ops_per_stat_emission = atoi(argv[arg_idx++]);
    printf("Using num ops per stat emission: %d\n", num_ops_per_stat_emission);

    // Parse the read to write ratio
    read_to_write_ratio = atoi(argv[arg_idx++]);
    printf("Using read to write ratio: %lu\n", read_to_write_ratio);

    // Parse the snapshot version. Default to version 2.
    arg = argv[arg_idx++];
    if (!strcmp(arg, "1")) {
        flashcache_snapshot_version = FC_SNAPSHOT_VERSION_ONE;
    } else {
        flashcache_snapshot_version = FC_SNAPSHOT_VERSION_TWO;
    }
    printf("Using snapshot version: `%d`\n", flashcache_snapshot_version);

    // Parse the snapshot save type. Default to BGSAVE.
    arg = argv[arg_idx++];
    if (!strcmp(arg, "forkless_save")) {
        flashcache_snapshot_save_type = FC_SAVE_TYPE_FORKLESS_SAVE;
    } else if (!strcmp(arg, "bgsave")) {
        flashcache_snapshot_save_type = FC_SAVE_TYPE_BGSAVE;
    } else {
        printf("Unknown snapshot save type: %s, defaulting to BGSAVE", arg);
    }
    printf("Using snapshot save type: `%s`\n", arg);
}

int mockAsioContext = 0;
void mockAsioCallback(void *context) {
    if (context != static_cast<void *>(&mockAsioContext)) {
        printf("Context used in callback function is incorrect!\n");
        exit(1);
    }
    mockAsioContext++;
}

int main(int argc, char *argv[]) {
    random_number_generator.seed(42);
    printf("Monotonic clock %s\n", monotonicInit());
    flashcacheEvictionDetails eviction_details = { 0 };
    static int eviction_context = 0;
    eviction_details.context = &eviction_context;
    eviction_details.callback = eviction_callback;

    shared_secret.size = FC_SNAPSHOT_MAX_SECRET_SIZE;
    memcpy(&shared_secret.secret, test_secret, FC_SNAPSHOT_MAX_SECRET_SIZE);

    parseArgs(argc, argv);

    memset(value, 'z', VALUE_MAX_SIZE);
    size_t index_size_per_db = (64LL * 1024);
    uint32_t num_databases = 1;
    uint32_t max_allocated_db_size_percent = 50; // 50% of the database is reserved
    size_t max_num_in_flight_read_requests = 128;
    uint32_t min_garbage_collection_rate = 4096;
    uint32_t evict_under_max_logsize_time_limit = 100 * 1000;
    uint8_t optimized_delete_enabled = 1;
    flashcacheAsioControlMsgCallbackDetails asio_control_msg_callback =
        { static_cast<void *>(&mockAsioContext), mockAsioCallback};
    if (flashcacheInit(db_filename, dbsize, index_size_per_db, num_databases,
                max_allocated_db_size_percent, max_num_in_flight_read_requests,
                min_garbage_collection_rate, evict_under_max_logsize_time_limit,
                optimized_delete_enabled,
                timeInMicrosecond,&eviction_details, logging_function,
                &asio_control_msg_callback) != FC_OK) {
        printf("Unable to initialize flashcache");
        exit(1);
    }

    writeLargeItems();

    int snapshot_enabled = 0;
    flashcacheSnapshotCallbackDetails snapshot_callback_details = { 0 };
    snapshot_callback_details.context = static_cast<void *>(&snapshot_context);
    snapshot_callback_details.callback = snapshotCompletionCallback;

    size_t max_num_read_ops = 5LL * 1000 * 1000 * 1000 * 1000;
    while (num_item_read < max_num_read_ops) {
        if ((num_item_written > max_num_items) && !(snapshot_context.is_running) && snapshot_enabled) {
            snapshot_context.is_running = 1;
            snapshot_context.num_item_read = num_item_read;
            snapshot_context.num_item_written = num_item_written;
            snapshot_context.start_time_second = timeInSecond();

            flashcacheStartFileBasedSave(snapshot_filename, &shared_secret, &snapshot_callback_details, 1,
                                         flashcache_snapshot_version, flashcache_snapshot_save_type);
            snapshot_context.num_read_complete = num_read_complete;
        }

        // Write data only when data is read from the DB
        if (readData()) {
            writeData();
        }

        printStats(false);
        flashcacheRunCronTasks();
    }

    while (num_read_inflight > 0) {
        flashcacheRunCronTasks();
    }

    printStats(false);
    if (flashcacheTearDown() != FC_OK) {
        printf("Unable to teardown flashcache");
        exit(1);
    }
    return 0;
}
