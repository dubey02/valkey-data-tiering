#include <gtest/gtest.h>

#include "flashcache_test_base.hpp"

extern "C" {
#include "include/serialization.h"
#include "include/util.h"
#include "include/crc.h"
#include "include/hash.h"
}

#define DB_ID (42)

class SerializationTest : public flashcache::FlashcacheTestBase, public testing::Test {
    void TearDown() {
        fcFree(serialized_item);
    }

 public:
    char *serialized_item;
    size_t serialized_item_len;
};

TEST_F(SerializationTest, testcompareKeyLengthAndDbidInSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(1, compareKeyLengthAndDbidInSerializedItem(serialized_item,
                DB_ID, key_len));
    ASSERT_EQ(0, compareKeyLengthAndDbidInSerializedItem(serialized_item,
                DB_ID * 2, key_len));
    ASSERT_EQ(0, compareKeyLengthAndDbidInSerializedItem(serialized_item,
                DB_ID, key_len + 1));
}

TEST_F(SerializationTest, testCompareKeyAndDbidInSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(1, compareKeyAndDbidInSerializedItem(serialized_item,
                DB_ID, key, key_len));
    ASSERT_EQ(0, compareKeyAndDbidInSerializedItem(serialized_item,
                DB_ID * 2, key, key_len));
    ASSERT_EQ(0, compareKeyAndDbidInSerializedItem(serialized_item,
                DB_ID, "Hey|", key_len));
    ASSERT_EQ(0, compareKeyAndDbidInSerializedItem(serialized_item,
                DB_ID, "Heyy!", 5));
}

TEST_F(SerializationTest, testExtractValueFromSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    char *received_value;
    size_t received_value_len;
    extractValueFromSerializedItem(serialized_item, &received_value,
            &received_value_len);
    ASSERT_EQ(value_len, received_value_len);
    ASSERT_EQ(0, memcmp(value, received_value, value_len));
}

TEST_F(SerializationTest, testExtractKeyFromSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    char *received_key;
    size_t received_key_len;
    extractKeyFromSerializedItem(serialized_item, &received_key,
            &received_key_len);
    ASSERT_EQ(key_len, received_key_len);
    ASSERT_EQ(0, memcmp(key, received_key, key_len));
}

TEST_F(SerializationTest, testExtractTotalLenFromSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(FC_ITEM_HEADER_LEN + key_len + value_len,
            extractTotalLenFromSerializedItem(serialized_item));
    ASSERT_EQ(FC_ITEM_HEADER_LEN + key_len,
            extractTotalLenWithoutValueFromSerializedItem(serialized_item));
}

TEST_F(SerializationTest, testExtractDbidFromSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);
    ASSERT_EQ(DB_ID, extractDbidFromSerializedItem(serialized_item));
}

TEST_F(SerializationTest, testComputeCollisionHashOfKeyInSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    ASSERT_EQ(computeCollisionHash(hasher->hash_function, key, key_len),
            computeCollisionHashOfKeyInSerializedItem(serialized_item,
                hasher->hash_function));
}

TEST_F(SerializationTest, testSerializedItemValidation) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[0] += 1;
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 0);
}

TEST_F(SerializationTest, testSerializedItemValidationWithInvalidKeyAndValue) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[FC_ITEM_HEADER_LEN] = 'Z';
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[FC_ITEM_HEADER_LEN] = 'H';
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[FC_ITEM_HEADER_LEN + key_len + 1] = 'w';
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 0);
}

TEST_F(SerializationTest, testUpdateFlagInSerializedItem) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = " I am Alice!";
    size_t value_len = strlen(value);

    serializeKeyValuePair(DB_ID, key, key_len, value, value_len,
            &serialized_item, &serialized_item_len, flashcacheCrc32c);

    ASSERT_EQ(0, getFlagInSerializedItem(serialized_item));
    updateFlagInSerializedItem(serialized_item, FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY,
            flashcacheCrc32c);
    ASSERT_EQ(FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY, getFlagInSerializedItem(serialized_item));
}

TEST_F(SerializationTest, testSkipSegmentInLogFileMarker) {
    serialized_item = static_cast<char *>(fcMalloc(FC_ITEM_HEADER_LEN));
    size_t segment_size = FC_PAGESIZE * 2;
    skipSegmentInLogFileMarker(serialized_item, segment_size, flashcacheCrc32c);
    ASSERT_EQ(FC_SKIP_SEGMENT, getFlagInSerializedItem(serialized_item));
    ASSERT_EQ(segment_size, extractTotalLenFromSerializedItem(serialized_item));
    ASSERT_EQ(sizeof(itemHeader), extractTotalLenWithoutValueFromSerializedItem(serialized_item));

    ASSERT_DEATH(skipSegmentInLogFileMarker(serialized_item, sizeof(itemHeader) - 1, flashcacheCrc32c), "");
}

TEST_F(SerializationTest, testSerializeKeyValuePairWithFlagWithEmptyStringsValue) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = "";
    size_t value_len = strlen(value);

    serializeKeyValuePairWithFlag(DB_ID, key, key_len, value, value_len, &serialized_item, &serialized_item_len,
                                  flashcacheCrc32c, FC_REPL_CMD_DELETE);

    // Validate the flag, item, header, key, value.
    ASSERT_EQ(FC_REPL_CMD_DELETE, getFlagInSerializedItem(serialized_item));
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[0] += 1;
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 0);

    // Crash when flag is not FC_REPL_CMD_DELETE
    ASSERT_DEATH(serializeKeyValuePairWithFlag(DB_ID, key, key_len, value, value_len, &serialized_item,
                                               &serialized_item_len, flashcacheCrc32c, 0), "");
}

TEST_F(SerializationTest, testSerializeKeyValuePairWithFlagWithNullValue) {
    char const *key = "Hey!";
    size_t key_len = strlen(key);
    char const *value = NULL;
    size_t value_len = 0;

    serializeKeyValuePairWithFlag(DB_ID, key, key_len, value, value_len, &serialized_item, &serialized_item_len,
                                  flashcacheCrc32c, FC_REPL_CMD_DELETE);

    // Validate the flag, item, header, key, value.
    ASSERT_EQ(FC_REPL_CMD_DELETE, getFlagInSerializedItem(serialized_item));
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateKeyInSerializedItem(serialized_item, flashcacheCrc32c), 1);
    ASSERT_EQ(validateValueInSerializedItem(serialized_item, flashcacheCrc32c), 1);

    serialized_item[0] += 1;
    ASSERT_EQ(validateSerializedItem(serialized_item, flashcacheCrc32c), 0);
    ASSERT_EQ(validateHeaderInSerializedItem(serialized_item, flashcacheCrc32c), 0);

    // Crash when flag is not FC_REPL_CMD_DELETE
    ASSERT_DEATH(serializeKeyValuePairWithFlag(DB_ID, key, key_len, value, value_len, &serialized_item,
                                               &serialized_item_len, flashcacheCrc32c, 0), "");
}
