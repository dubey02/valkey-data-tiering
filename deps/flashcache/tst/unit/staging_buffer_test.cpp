#include <gtest/gtest.h>

#include "flashcache_test_base.hpp"

extern "C" {
#include "include/staging_buffer.h"
}

typedef struct stagingBufferEntryWithItem {
    stagingBufferEntry *entry;
    char *item;
    size_t item_len;
} stagingBufferEntryWithItem;

class StagingBufferTest : public flashcache::FlashcacheTestBase, public testing::Test {
    void SetUp() {
        staging_buffer = stagingBufferCreate();
    }

    void TearDown() {
        stagingBufferRelease(staging_buffer);
    }

 public:
    stagingBuffer *staging_buffer;
};

TEST_F(StagingBufferTest, testAddGetAndDeleteSingleItem) {
    char *item = nullptr;
    size_t item_len = cloneCharArray("item1", &item);
    size_t user_data = 847131;  // Arbitary user value

    stagingBufferEntry *entry = stagingBufferAddItem(staging_buffer, item, item_len, user_data);
    ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->next, nullptr);
    ASSERT_EQ(entry->prev, nullptr);
    ASSERT_EQ(entry->index_entry, nullptr);
    ASSERT_EQ(entry->item_len, item_len);
    ASSERT_EQ(entry->user_data, user_data);
    ASSERT_EQ(0, memcmp(entry->item, item, item_len));

    ASSERT_EQ(entry, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ(entry, stagingBufferGetTail(staging_buffer));
    ASSERT_EQ(item_len, stagingBufferGetTotalItemSize(staging_buffer));

    stagingBufferDeleteEntry(staging_buffer, entry, 1);
    ASSERT_EQ(NULL, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ(NULL, stagingBufferGetTail(staging_buffer));
}

TEST_F(StagingBufferTest, testDeleteWithoutFreeingSerializedItem) {
    char *item = nullptr;
    size_t item_len = cloneCharArray("item1", &item);

    stagingBufferEntry *entry = stagingBufferAddItem(staging_buffer, item, item_len, 0);
    ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->next, nullptr);
    ASSERT_EQ(entry->prev, nullptr);
    ASSERT_EQ(entry->index_entry, nullptr);
    ASSERT_EQ(entry->item_len, item_len);
    ASSERT_EQ(0, memcmp(entry->item, item, item_len));

    ASSERT_EQ(entry, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ(entry, stagingBufferGetTail(staging_buffer));
    ASSERT_EQ(item_len, stagingBufferGetTotalItemSize(staging_buffer));

    stagingBufferDeleteEntry(staging_buffer, entry, 0);
    ASSERT_EQ(NULL, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ(NULL, stagingBufferGetTail(staging_buffer));
    fcFree(item);
}

TEST_F(StagingBufferTest, testAddAndGetMultipleItem) {
    size_t num_items = 1000;
    std::vector<stagingBufferEntryWithItem> entries;
    size_t total_size = 0;
    for (size_t i = 0; i < num_items; ++i) {
        char *item = nullptr;
        size_t item_len = cloneCharArray("itemz", &item);
        stagingBufferEntry *entry = stagingBufferAddItem(staging_buffer, item,
                item_len, i);
        stagingBufferEntryWithItem entry_with_item { entry, item, item_len };
        entries.push_back(entry_with_item);
        total_size += item_len;
    }

    ASSERT_EQ(total_size, stagingBufferGetTotalItemSize(staging_buffer));
    ASSERT_EQ((*(entries.rbegin())).entry, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ((*(entries.begin())).entry, stagingBufferGetTail(staging_buffer));
    stagingBufferEntry *entry = stagingBufferGetHead(staging_buffer);
    stagingBufferEntry *tail = entry;
    size_t expected_user_data = num_items;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        tail = entry;
        ASSERT_EQ((*it).entry, entry);
        ASSERT_EQ((*it).item_len, entry->item_len);
        ASSERT_EQ(0, memcmp((*it).item, entry->item, entry->item_len));
        ASSERT_EQ(--expected_user_data, entry->user_data);
        entry = entry->next;
    }
    ASSERT_EQ(entry, nullptr);

    entry = tail;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        ASSERT_EQ((*it).entry, entry);
        entry = entry->prev;
    }
    ASSERT_EQ(entry, nullptr);
}

TEST_F(StagingBufferTest, testDeleteEntry) {
    size_t num_items = 1000;
    std::vector<stagingBufferEntryWithItem> entries;
    size_t total_size = 0;
    for (size_t i = 0; i < num_items; ++i) {
        char *item = nullptr;
        size_t item_len = cloneCharArray("itemz", &item);
        stagingBufferEntry *entry = stagingBufferAddItem(staging_buffer, item,
                item_len, i);
        stagingBufferEntryWithItem entry_with_item { entry, item, item_len };
        entries.push_back(entry_with_item);
        total_size += item_len;
    }

    ASSERT_EQ(total_size, stagingBufferGetTotalItemSize(staging_buffer));

    // Delete the head
    size_t head_pos = entries.size() - 1;
    stagingBufferDeleteEntry(staging_buffer, entries.at(head_pos).entry, 1);
    total_size -= entries.at(head_pos).item_len;
    entries.erase(entries.begin() + head_pos);

    // Delete the tail
    size_t tail_pos = 0;
    stagingBufferDeleteEntry(staging_buffer, entries.at(tail_pos).entry, 1);
    total_size -= entries.at(tail_pos).item_len;
    entries.erase(entries.begin());

    // Delete entry from the middle
    size_t pos = 257;
    stagingBufferDeleteEntry(staging_buffer, entries.at(pos).entry, 1);
    total_size -= entries.at(pos).item_len;
    entries.erase(entries.begin() + pos);

    ASSERT_EQ(total_size, stagingBufferGetTotalItemSize(staging_buffer));
    ASSERT_EQ((*(entries.rbegin())).entry, stagingBufferGetHead(staging_buffer));
    ASSERT_EQ((*(entries.begin())).entry, stagingBufferGetTail(staging_buffer));
    stagingBufferEntry *entry = stagingBufferGetHead(staging_buffer);
    stagingBufferEntry *tail = entry;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        tail = entry;
        ASSERT_EQ((*it).entry, entry);
        ASSERT_EQ((*it).item_len, entry->item_len);
        ASSERT_EQ(0, memcmp((*it).item, entry->item, entry->item_len));
        entry = entry->next;
    }
    ASSERT_EQ(entry, nullptr);

    entry = tail;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        ASSERT_EQ((*it).entry, entry);
        entry = entry->prev;
    }
    ASSERT_EQ(entry, nullptr);
}
