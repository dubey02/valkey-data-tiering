#ifndef __FLASHCACHE_TEST_BASE_HPP
#define __FLASHCACHE_TEST_BASE_HPP

#include <vector>
#include <unordered_set>
#include <cstring>
#include <random>
#include <utility>
#include <climits>
#include <tuple>
#include <gtest/gtest.h>

#include "fio_mock.hpp"

extern "C" {
#include <fcntl.h>
#include "include/serialization.h"
#include "include/util.h"
}

typedef struct requestContext {
    char const *actual_value;
    size_t actual_value_len;
    size_t num_callback_triggered;
} requestContext;

typedef struct evictionContext {
    std::vector<std::tuple<uint32_t, char *, size_t>> key_list;
} evictionContext;

// Snapshot context used for testing completion of snapshotting process
typedef struct {
    size_t num_snapshot_completion_callback_invocation;
    size_t num_log_iteration_completion_callback_invocation;
    int expected_completion_status;
    flashcacheSnapshotSaveType snapshot_save_type;
    flashcacheSnapshotVersion snapshot_version;
} snapshotContext;

extern int mockStorageIoThreadContext;
extern void mockStorageIoThreadCallback(void *context);

static inline void validateValue(char const *actual_value, size_t actual_value_len,
        char const *received_value, size_t received_value_len) {
    ASSERT_EQ(actual_value_len, received_value_len);
    // If one of the pointers is null, the comparison is still valid
    // as long as the number of bytes we are comparing is 0
    if (actual_value == nullptr || received_value == nullptr) {
        ASSERT_EQ(actual_value_len, 0);
        return;
    }
    ASSERT_EQ(0, memcmp(actual_value, received_value, actual_value_len));
}

static inline void eviction_callback(void *context, uint32_t dbid, char *key, size_t key_len) {
    evictionContext *eviction_context = static_cast<evictionContext *>(context);
    char *key_copy = new char[key_len];
    memcpy(key_copy, key, key_len);
    std::tuple<uint32_t, char *, size_t> eviction_tuple;
    std::get<0>(eviction_tuple) = dbid;
    std::get<1>(eviction_tuple) = key_copy;
    std::get<2>(eviction_tuple) = key_len;
    eviction_context->key_list.push_back(eviction_tuple);
}

static inline void get_item_callback(void *request_context, char *value,
        size_t value_len, int add_item_to_rdb) {
    (void)add_item_to_rdb;
    ASSERT_NE(request_context, nullptr);
    requestContext *context = static_cast<requestContext *>(request_context);
    validateValue(context->actual_value, context->actual_value_len,
            value, value_len);
    context->num_callback_triggered++;
}

static inline void delete_item_callback(void *request_context, char *value,
        size_t value_len, int add_item_to_rdb) {
    (void)value;
    (void)value_len;
    (void)add_item_to_rdb;
    // Deletion callback parameters regarding the value are inconsistent and should
    // always be disregarded by the callback
    requestContext *context = static_cast<requestContext *>(request_context);
    context->num_callback_triggered++;
}

static inline size_t cloneCharArray(char const *str, char **arr) {
    size_t str_len = strlen(str);
    char *tmp_arr = static_cast<char *>(fcMalloc(str_len));
    memcpy(tmp_arr, str, str_len);
    *arr = tmp_arr;
    return str_len;
}

static inline char *convertStringToCharArray(std::string str) {
    char *res = static_cast<char *>(fcMalloc(str.length()));
    memcpy(res, str.c_str(), str.length());
    return res;
}

// Character set used for random string generator
static const char rsg_charset[] = "0123456789" \
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
                                  "abcdefghijklmnopqrstuvwxyz";

static inline std::string generateRandomString(std::string prefix, size_t len) {
    std::string random_str(len, 0);
    for (size_t i = 0; i < prefix.length(); ++i) {
        random_str[i] = prefix[i];
    }
    for (size_t i = prefix.length(); i < len; ++i) {
        random_str[i] = rsg_charset[rand() % (strlen(rsg_charset))];
    }
    return random_str;
}

static void snapshotCompletionCallback(void *context, int completed) {
    snapshotContext *snapshot_context = reinterpret_cast<snapshotContext *>(context);
    snapshot_context->num_snapshot_completion_callback_invocation++;
    ASSERT_EQ(snapshot_context->expected_completion_status, completed);
}

static void logIterationCompletionCallback(void *context) {
    snapshotContext *snapshot_context = reinterpret_cast<snapshotContext *>(context);
    ASSERT_EQ(snapshot_context->snapshot_save_type, FC_SAVE_TYPE_FORKLESS_SAVE);
    ASSERT_EQ(snapshot_context->snapshot_version, FC_SNAPSHOT_VERSION_TWO);
    snapshot_context->num_log_iteration_completion_callback_invocation++;
}

// Hash function for allowing index entry to move to different bucket while first index growth iteration
static inline uint64_t hash_function_for_collision_hash_as_one(char const *key, size_t key_len) {
    (void(key));
    (void(key_len));
    return 1LL << 48;
}

// Hash function for restricting index entry to move to different bucket while first index growth iteration
static inline uint64_t hash_function_for_collision_hash_as_zero(char const *key, size_t key_len) {
    (void(key));
    (void(key_len));
    return 0LL;
}

namespace flashcache {

class FlashcacheTestBase {
    struct generatedItem {
        char *key;
        size_t key_len;
        char *value;
        size_t value_len;
    };

    size_t key_len;
    size_t value_len;
    size_t logical_time;
    std::vector<generatedItem> generated_items;
    std::unordered_set<std::string> generated_keys;

    void freeGeneratedItems() {
        for (auto it = generated_items.begin(); it != generated_items.end(); ++it) {
            fcFree(it->key);
            fcFree(it->value);
        }
        generated_items.clear();
    }

 public:
    flashcacheSnapshotSecret snapshot_secret = {0};
    void SetUp() {
        key_len = 16;
        value_len = 200;
        generated_items.clear();
        generated_keys.clear();
        logical_time = 0;
        snapshot_secret.size = FC_SNAPSHOT_MAX_SECRET_SIZE;
        memcpy(snapshot_secret.secret, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ§!"
                                       "@#$%^&**()_+=-{}[];:|<>?±~abcdfghijklmnopqrestuvwxyz012346789ABC",
               FC_SNAPSHOT_MAX_SECRET_SIZE);
    }

    void TearDown() {
        freeGeneratedItems();
        generated_keys.clear();
        logical_time = 0;
    }

    void setKeyLen(size_t key_len) {
        this->key_len = key_len;
    }

    void setValueLen(size_t value_len) {
        this->value_len = value_len;
    }

    size_t getValueLen() {
        return this->value_len;
    }

    size_t getLogicalTime() {
        return logical_time;
    }

    size_t getTotalItemLen(size_t key_len, size_t value_len) {
        size_t item_len = (key_len + value_len + FC_ITEM_HEADER_LEN);
        if (item_len % FC_ITEM_ALIGNMENT_BYTES > 0) {
            item_len = (item_len / FC_ITEM_ALIGNMENT_BYTES + 1) * FC_ITEM_ALIGNMENT_BYTES;
        }
        return item_len;
    }

    size_t getTotalItemLen() {
        return getTotalItemLen(this->key_len, this->value_len);
    }

    void fillRandomData(char *buf, size_t buf_len) {
        memcpy(buf, generateRandomString("", buf_len).c_str(), buf_len);
    }

    // Generate random item and returns the logical time of generation.
    // After generating an item, the logical time is incremented by 1
    size_t generateRandomItem(char * &key, size_t &key_len, char * &value,
            size_t &value_len) {
        key_len = this->key_len;
        value_len = this->value_len;
        std::string key_str;
        do {
            key_str = generateRandomString("key-", key_len);
        } while (generated_keys.count(key_str) > 0);

        std::string value_str = generateRandomString("value::", value_len);

        key = convertStringToCharArray(key_str);
        value = convertStringToCharArray(value_str);

        generated_keys.insert(key_str);
        generatedItem generated_item { key, key_len, value, value_len };
        generated_items.push_back(generated_item);
        return (logical_time++);
    }

    // Get the item generated at the logical time
    void getGeneratedItemAtTime(size_t time, char * &key, size_t &key_len, char * &value,
            size_t &value_len) {
        ASSERT_LT(time, logical_time);
        generatedItem item = generated_items.at(time);
        key = item.key;
        key_len = item.key_len;
        value = item.value;
        value_len = item.value_len;
    }

    // Extracts filename from the specified file
    void extractFilename(int fd, char *filename) {
        char proc_fd_filepath[FILENAME_MAX] = { 0 };
        snprintf(proc_fd_filepath, FILENAME_MAX, "/proc/self/fd/%d", fd);
        ssize_t len = readlink(proc_fd_filepath, filename, FILENAME_MAX - 1);
        if (len != -1) {
            filename[len] = '\0';
        }
    }

    // Create a temp file of specified file size
    std::string createTmpFile(size_t filesize) {
        int retry = 100;
        char filename_template[FILENAME_MAX] = "/tmp/tmpXXXXXX";
        while ((retry--) > 0) {
            int fd = mkstemp(filename_template);
            if (fd < 0) {
                continue;
            }
            if (filesize > 0) {
                fallocate(fd, 0, 0, filesize);
            }
            char filename[FILENAME_MAX];
            extractFilename(fd, filename);
            close(fd);
            return filename;
        }
        flashcacheAssert(0);
        return "";
    }
};

}  // namespace flashcache

#endif  // __FLASHCACHE_TEST_BASE_HPP
