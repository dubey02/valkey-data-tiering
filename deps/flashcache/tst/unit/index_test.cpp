#include <algorithm>
#include <iterator>
#include <gtest/gtest.h>

#include "flashcache_test_base.hpp"

extern "C" {
#include "include/index.h"
#include "include/hash.h"
#include "include/crc.h"
#include "include/util.h"
#include "include/serialization.h"
#include <malloc.h>

size_t getIndexHash(flashcacheIndex *index, char const *key, size_t key_len);
size_t indexTableSize(flashcacheIndex *index);
void indexResizeTable(flashcacheIndex *index, size_t new_size);
void indexGrowthIterationCallback(void *context, size_t hash_bucket_idx, indexEntry *head_entry);
}

typedef struct itemDetail {
    char *key;
    size_t key_len;
    indexEntry *index_entry;
    stagingBufferEntry *staging_buffer_entry;
} itemDetail;

typedef struct iterationContext {
    bool is_completed;
    size_t num_completion_callback_invoked;
    size_t num_iteration_callback_invoked;
    std::vector<itemDetail> expected_items;
    std::set<indexEntry *> visited_index_entries;
    std::set<size_t> visited_hash_bucket_idx;
} iterationContext;

static void iterationCallback(void *context, size_t hash_bucket_idx, indexEntry *head_entry) {
    iterationContext *ic = reinterpret_cast<iterationContext *>(context);
    ic->visited_hash_bucket_idx.insert(hash_bucket_idx);
    ic->num_iteration_callback_invoked++;

    while (head_entry) {
        ic->visited_index_entries.insert(head_entry);
        head_entry = head_entry->next;
    }
}

static void completionCallback(void *context) {
    iterationContext *ic = reinterpret_cast<iterationContext *>(context);
    ic->is_completed = true;
    ic->num_completion_callback_invoked++;
}

class IndexTest : public testing::TestWithParam<size_t> {
    void SetUp() {
        index_initial_size = 1024;
        size_t num_items = GetParam();
        flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
        index = indexCreate(index_initial_size, hasher->hash_function);
        key_len = 100;
        addItems(num_items);
    }

    void TearDown() {
        releaseItems();
        indexRelease(index);
    }

 public:
    flashcacheIndex *index;
    size_t key_len;
    size_t index_initial_size;
    std::vector<itemDetail> items;

    stagingBufferEntry *createStagingBufferEntry(char *key, size_t key_len) {
        char *item = nullptr;
        size_t item_len = 0;
        serializeKeyValuePair(0, key, key_len, "test", 4, &item,
                              &item_len, flashcacheCrc32c);
        size_t user_data = 847131;  // Arbitary user value
        stagingBufferEntry *entry = static_cast<stagingBufferEntry *>(fcMalloc(sizeof(stagingBufferEntry)));
        flashcacheAssert(entry != NULL);
        entry->item = item;
        entry->item_len = item_len;
        entry->user_data = user_data;
        return entry;
    }

    void addItems(size_t num_items) {
        for (size_t i = 0; i < num_items; ++i) {
            char *key = convertStringToCharArray(generateRandomString(
                        "key-", key_len));
            stagingBufferEntry *staging_buffer_entry = createStagingBufferEntry(key, key_len);
            indexEntry *head = indexGetHeadEntry(index, key, key_len);
            indexEntry *entry = indexAddItem(index, key, key_len, staging_buffer_entry);
            ASSERT_NE(entry, nullptr);
            ASSERT_EQ(entry->next, head);
            ASSERT_EQ(entry->item_entry.staging_buffer_entry, staging_buffer_entry);
            ASSERT_EQ(entry->item_entry.staging_buffer_entry->log_entry.on_flash, 1);

            itemDetail item { key, key_len, entry, staging_buffer_entry };
            items.push_back(item);
        }
    }

    void deleteItem(size_t idx) {
        itemDetail item = items.at(idx);
        indexDeleteItem(index, item.key, item.key_len, item.index_entry);
        items.erase(items.begin() + idx);
        fcFree(item.key);
        fcFree(item.staging_buffer_entry->item);
        fcFree(item.staging_buffer_entry);
    }

    void validateItemsInIndex() {
        for (auto it = items.begin(); it != items.end(); ++it) {
            itemDetail item = *it;
            indexEntry *entry = indexGetHeadEntry(index, item.key, item.key_len);
            while (entry) {
                if (entry == item.index_entry) {
                    break;
                }
                entry = entry->next;
            }

            ASSERT_EQ(entry, item.index_entry);
            ASSERT_EQ(entry->item_entry.staging_buffer_entry, item.staging_buffer_entry);
        }
        ASSERT_EQ(index->num_items, items.size());
        ASSERT_GE(index->num_hash_bucket_used, 0);
    }

    void createPointInTimeIterator(iterationContext *context) {
        context->is_completed = false;
        context->num_completion_callback_invoked = 0;
        context->num_iteration_callback_invoked = 0;
        std::copy(items.begin(), items.end(), std::back_inserter(context->expected_items));

        indexIteratorCallbackDetails callback_details;
        callback_details.iteration_callback = iterationCallback;
        callback_details.completion_callback = completionCallback;
        callback_details.context = static_cast<void *>(context);
        indexCreateCustomIterator(index, &callback_details, 10);
    }

    void validatePointInTimeIteration(iterationContext *context) {
        ASSERT_EQ(1, context->num_completion_callback_invoked);
        ASSERT_EQ(index_initial_size, context->num_iteration_callback_invoked);
        ASSERT_EQ(context->expected_items.size(), context->visited_index_entries.size());
        ASSERT_EQ(index_initial_size, context->visited_hash_bucket_idx.size());

        for (size_t idx = 0; idx < index_initial_size; ++idx) {
            ASSERT_EQ(1, context->visited_hash_bucket_idx.count(idx));
        }

        for (auto it = context->expected_items.begin(); it != context->expected_items.end(); ++it) {
            ASSERT_EQ(1, context->visited_index_entries.count(it->index_entry));
        }
    }

    void validateIndexGrowthIterationCallback(size_t num_items) {
        ASSERT_EQ(index->num_hash_bucket_used, 0);
        index->hash_function = hash_function_for_collision_hash_as_one;
        size_t index_hash = 0;  // index hash will be 0 here because of hash_function_for_collision_hash_as_one.
        addItems(num_items);
        ASSERT_EQ(index->num_hash_bucket_used, (num_items > 0) ? 1 : 0);
        size_t num_hash_bucket_used_before_growth = index->num_hash_bucket_used;
        size_t num_items_before_growth = index->num_items;
        indexResizeTable(index, index_initial_size * 2);
        index->growth_iterator->next_hash_bucket = index_hash + 1;
        indexGrowthIterationCallback(index, index_hash, index->table[index_hash]);
        ASSERT_EQ(index->num_hash_bucket_used, num_hash_bucket_used_before_growth);
        ASSERT_EQ(index->num_items, num_items_before_growth);
    }

    void releaseItems() {
        for (auto it = items.begin(); it != items.end(); ++it) {
            fcFree((*it).key);
            fcFree((*it).staging_buffer_entry->item);
            fcFree((*it).staging_buffer_entry);
        }
        items.clear();
    }
};

class IndexTestWithNoInitialItem : public IndexTest {
};

TEST_P(IndexTest, testAddAndGetItem) {
    validateItemsInIndex();
}

TEST_P(IndexTest, testUpdateItem) {
    auto first_item = items.begin();
    size_t trimmed_offset = 10456;
    memset(&(first_item->index_entry->item_entry.log_entry), 0, sizeof(logEntry));
    first_item->index_entry->item_entry.log_entry.on_flash = 1;
    first_item->index_entry->item_entry.log_entry.trimmed_log_offset = trimmed_offset;
    char *key = convertStringToCharArray(generateRandomString(
            "key-", key_len));
    stagingBufferEntry *new_entry = createStagingBufferEntry(key, key_len);
    indexUpdateItem(index, first_item->key, first_item->key_len,
            expandTrimmedLogOffset(trimmed_offset), new_entry);
    ASSERT_EQ(new_entry, first_item->index_entry->item_entry.staging_buffer_entry);

    memset(&(first_item->index_entry->item_entry.log_entry), 0, sizeof(logEntry));
    first_item->index_entry->item_entry.log_entry.on_flash = 1;
    first_item->index_entry->item_entry.log_entry.trimmed_log_offset = trimmed_offset;
    ASSERT_DEATH(indexUpdateItem(index, first_item->key, first_item->key_len,
                                 expandTrimmedLogOffset(trimmed_offset + 1), new_entry), "");
    fcFree(key);
    fcFree(new_entry->item);
    fcFree(new_entry);
}

TEST_P(IndexTest, testRecreateWithNotRunningStatus) {
    validateItemsInIndex();
    size_t base_size_bits = 4;
    size_t collision_bits_used = 2;

    indexRecreate(index, base_size_bits, collision_bits_used, NOT_RUNNING, 0, index->hash_function);
    ASSERT_EQ(index->num_items, 0);
    ASSERT_EQ(index->num_hash_bucket_used, 0);
    ASSERT_EQ(indexTableSize(index), 1 << (base_size_bits + collision_bits_used));
    ASSERT_EQ(malloc_usable_size(index->table),
              sizeof(indexEntry *) * (1 << (base_size_bits + collision_bits_used)));
    ASSERT_EQ(index->base_size_bits, base_size_bits);
    ASSERT_EQ(index->collision_bits_used, collision_bits_used);
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);
}

TEST_P(IndexTest, testRecreateWithRunningStatus) {
    validateItemsInIndex();
    size_t base_size_bits = 12;
    size_t collision_bits_used = 10;
    indexRecreate(index, base_size_bits, collision_bits_used, RUNNING, 0, index->hash_function);
    ASSERT_EQ(index->num_items, 0);
    ASSERT_EQ(index->num_hash_bucket_used, 0);
    ASSERT_EQ(indexTableSize(index), 1 << (base_size_bits + collision_bits_used));
    ASSERT_EQ(malloc_usable_size(index->table),
              sizeof(indexEntry *) * (1 << (base_size_bits + collision_bits_used + 1)));
    ASSERT_EQ(index->base_size_bits, base_size_bits);
    ASSERT_EQ(index->collision_bits_used, collision_bits_used);
    ASSERT_EQ(index->growth_iterator->status, RUNNING);
    ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);
}

TEST_P(IndexTest, testRecreateWithPausedStatus) {
    validateItemsInIndex();
    size_t base_size_bits = 1;
    size_t collision_bits_used = 15;
    indexRecreate(index, base_size_bits, collision_bits_used, PAUSED, 0, index->hash_function);
    ASSERT_EQ(index->num_items, 0);
    ASSERT_EQ(index->num_hash_bucket_used, 0);
    ASSERT_EQ(indexTableSize(index), 1 << (base_size_bits + collision_bits_used));
    ASSERT_EQ(malloc_usable_size(index->table),
                 sizeof(indexEntry *) * (1 << (base_size_bits + collision_bits_used + 1)));
    ASSERT_EQ(index->base_size_bits, base_size_bits);
    ASSERT_EQ(index->collision_bits_used, collision_bits_used);
    ASSERT_EQ(index->growth_iterator->status, PAUSED);
    ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);
}

TEST_P(IndexTest, testRecreateInvalidScenarios) {
    validateItemsInIndex();
    size_t base_size_bits = 1;
    size_t collision_bits_used = 17;
    ASSERT_DEATH(indexRecreate(index, base_size_bits, collision_bits_used, NOT_RUNNING, 0,
                index->hash_function), "");

    collision_bits_used = 16;
    ASSERT_DEATH(indexRecreate(index, base_size_bits, collision_bits_used, RUNNING, 0,
                index->hash_function), "");
    ASSERT_DEATH(indexRecreate(index, base_size_bits, collision_bits_used, PAUSED, 0,
                index->hash_function), "");
}

TEST_P(IndexTest, testIndexGetItems) {
    auto first_item = items.begin();
    indexEntry *entry = indexGetItem(index, first_item->key, key_len, 0);

    ASSERT_EQ(nullptr, entry);

    size_t trimmed_offset = 77;
    first_item->index_entry->item_entry.log_entry.on_flash = 1;
    first_item->index_entry->item_entry.log_entry.trimmed_log_offset = trimmed_offset;
    entry = indexGetItem(index, first_item->key, key_len, expandTrimmedLogOffset(trimmed_offset));
    ASSERT_EQ(first_item->index_entry, entry);

    trimmed_offset = 20001;
    auto item = items.at(items.size() / 2);
    item.index_entry->item_entry.log_entry.on_flash = 1;
    item.index_entry->item_entry.log_entry.trimmed_log_offset = trimmed_offset;
    entry = indexGetItem(index, item.key, key_len, expandTrimmedLogOffset(trimmed_offset));
    ASSERT_EQ(item.index_entry, entry);
}

TEST_P(IndexTest, testDeleteOneItem) {
    deleteItem(random() % items.size());
    validateItemsInIndex();
}

TEST_P(IndexTest, testDeleteTenPercentItem) {
    size_t total_item = items.size();
    while (items.size() >= 0.9 * total_item) {
        deleteItem(random() % items.size());
    }
    validateItemsInIndex();
}

TEST_P(IndexTest, testDeleteFiftyPercentItem) {
    size_t total_item = items.size();
    while (items.size() >= 0.5 * total_item) {
        deleteItem(random() % items.size());
    }
    validateItemsInIndex();
}

TEST_P(IndexTest, testDeleteNinetyPercentItem) {
    size_t total_item = items.size();
    while (items.size() >= 0.1 * total_item) {
        deleteItem(random() % items.size());
    }
    validateItemsInIndex();
}

TEST_P(IndexTest, testIterationHappyCase) {
    iterationContext context;
    createPointInTimeIterator(&context);
    while (!context.is_completed) {
        indexIterateCustomIteratorIfRequired(index);
    }
    validatePointInTimeIteration(&context);
}

TEST_P(IndexTest, testIteratorReleasedBeforeCompletion) {
    iterationContext context;
    createPointInTimeIterator(&context);
    ASSERT_GT(indexIterateCustomIteratorIfRequired(index), 0);
    indexReleaseCustomIterator(index);
    ASSERT_EQ(0, indexIterateCustomIteratorIfRequired(index));
}

TEST_P(IndexTest, testIterationWithAdditionOfItems) {
    iterationContext context;
    createPointInTimeIterator(&context);
    addItems(200);
    while (!context.is_completed) {
        indexIterateCustomIteratorIfRequired(index);
    }
    validatePointInTimeIteration(&context);
}

TEST_P(IndexTest, testIterationWithDeletionOfItems) {
    iterationContext context;
    createPointInTimeIterator(&context);

    for (int i = 0; i < 200; ++i) {
        if (items.size() == 0) {
            break;
        }
        deleteItem(random() % items.size());
    }

    while (!context.is_completed) {
        indexIterateCustomIteratorIfRequired(index);
    }
    validatePointInTimeIteration(&context);
}

TEST_P(IndexTest, testIterationWithAdditionAndDeletionOfItem) {
    iterationContext context;
    createPointInTimeIterator(&context);
    while (!context.is_completed) {
        indexIterateCustomIteratorIfRequired(index);
        deleteItem(random() % items.size());
        addItems(1);
    }
    validatePointInTimeIteration(&context);
}

TEST_P(IndexTest, testIndexGrowIfRequired) {
    validateItemsInIndex();

    // Added more items in order to breach load factor for initiating growth
    addItems(6 * 1024);

    // Not Running Status
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    size_t collision_bits_used_before_growth = index->collision_bits_used;
    size_t num_hash_bucket_used_before_growth = index->num_hash_bucket_used;
    size_t next_hash_bucket_idx_before_growth = index->growth_iterator->next_hash_bucket;
    indexGrowIfRequired(index);

    ASSERT_EQ(index->growth_iterator->status, RUNNING);
    ASSERT_GE(index->num_hash_bucket_used, num_hash_bucket_used_before_growth);
    ASSERT_GT(index->growth_iterator->next_hash_bucket, next_hash_bucket_idx_before_growth);

    // Running Status
    num_hash_bucket_used_before_growth = index->num_hash_bucket_used;
    next_hash_bucket_idx_before_growth = index->growth_iterator->next_hash_bucket;
    indexGrowIfRequired(index);

    ASSERT_EQ(index->growth_iterator->status, RUNNING);
    ASSERT_GE(index->num_hash_bucket_used, num_hash_bucket_used_before_growth);
    ASSERT_GT(index->growth_iterator->next_hash_bucket, next_hash_bucket_idx_before_growth);

    // Paused Status
    index->growth_iterator->status = PAUSED;
    num_hash_bucket_used_before_growth = index->num_hash_bucket_used;
    next_hash_bucket_idx_before_growth = index->growth_iterator->next_hash_bucket;
    indexGrowIfRequired(index);

    ASSERT_EQ(index->growth_iterator->status, PAUSED);
    ASSERT_EQ(index->num_hash_bucket_used, num_hash_bucket_used_before_growth);
    ASSERT_EQ(index->growth_iterator->next_hash_bucket, next_hash_bucket_idx_before_growth);

    // Completion of growth operation
    index->growth_iterator->status = RUNNING;
    while (index->growth_iterator->status != NOT_RUNNING) {
        indexGrowIfRequired(index);
    }
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);
    ASSERT_EQ(index->collision_bits_used, collision_bits_used_before_growth + 1);
    ASSERT_EQ(index->growth_iterator->next_hash_bucket, 0);

    // When all collision bits are consumed
    index->growth_iterator->status = PAUSED;
    index->collision_bits_used = 16;
    ASSERT_EQ(indexGrowIfRequired(index), 0);

    // Reverting the values changed manually for testing (required for tear up)
    index->collision_bits_used = collision_bits_used_before_growth + 1;
    index->growth_iterator->status = NOT_RUNNING;
}

TEST_P(IndexTest, testIndexPauseGrowth) {
    index->growth_iterator->status = NOT_RUNNING;
    indexPauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);

    index->growth_iterator->status = RUNNING;
    indexPauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, PAUSED);

    index->growth_iterator->status = PAUSED;
    indexPauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, PAUSED);
}

TEST_P(IndexTest, testIndexUnpauseGrowth) {
    index->growth_iterator->status = NOT_RUNNING;
    indexUnpauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, NOT_RUNNING);

    index->growth_iterator->status = RUNNING;
    indexUnpauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, RUNNING);

    index->growth_iterator->status = PAUSED;
    indexUnpauseGrowth(index);
    ASSERT_EQ(index->growth_iterator->status, RUNNING);
}

TEST_P(IndexTestWithNoInitialItem, testIndexGrowthIterationCallbackWithZeroItemInBucket) {
    validateIndexGrowthIterationCallback(0);
}

TEST_P(IndexTestWithNoInitialItem, testIndexGrowthIterationCallbackWithOneItemInBucket) {
    validateIndexGrowthIterationCallback(1);
}

TEST_P(IndexTestWithNoInitialItem, testIndexGrowthIterationCallbackWithTwoItemInBucket) {
    validateIndexGrowthIterationCallback(2);
}

TEST_P(IndexTestWithNoInitialItem, testIndexGrowthIterationCallbackWithFiveItemInBucket) {
    validateIndexGrowthIterationCallback(5);
}

TEST_P(IndexTestWithNoInitialItem, testIndexGrowthIterationCallbackWithDifferentItems) {
    size_t index_hash = 0;
    ASSERT_EQ(index->num_hash_bucket_used, 0);
    index->hash_function = hash_function_for_collision_hash_as_one;
    addItems(1);
    ASSERT_EQ(index->num_hash_bucket_used, 1);
    index->hash_function = hash_function_for_collision_hash_as_zero;
    addItems(1);
    ASSERT_EQ(index->num_hash_bucket_used, 1);
    size_t num_hash_bucket_used_before_growth = index->num_hash_bucket_used;
    size_t num_items_before_growth = index->num_items;
    indexResizeTable(index, index_initial_size * 2);
    index->growth_iterator->next_hash_bucket = index_hash + 1;
    indexGrowthIterationCallback(index, index_hash, index->table[index_hash]);
    ASSERT_EQ(index->num_hash_bucket_used, num_hash_bucket_used_before_growth + 1);
    ASSERT_EQ(index->num_items, num_items_before_growth);
}

INSTANTIATE_TEST_SUITE_P(ParameterizedIndexTest, IndexTest,
        testing::Values(1, 2, 101, 102, 222, 411, 512, 1024, 2048, 4155, 19055, 120044));

INSTANTIATE_TEST_SUITE_P(ParameterizedIndexTest, IndexTestWithNoInitialItem,
        testing::Values(0));
