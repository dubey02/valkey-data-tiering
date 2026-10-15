#include <gtest/gtest.h>
#include "flashcache_test_base.hpp"

extern "C" {
#include "include/util.h"
#include "include/hash.h"
}

class UtilTest : public flashcache::FlashcacheTestBase, public testing::Test {
 public:
    size_t key_len;
    char *key;

    void SetUp() {
        key_len = 100;
        key = convertStringToCharArray(generateRandomString("key-", key_len));
    }

    void TearDown() {
        free(key);
    }
};

TEST_F(UtilTest, testComputeIndexHash) {
    size_t hash = 1LL << 49;
    size_t collision_bits_used = 2;
    size_t base_size_bits = 10;
    size_t index_hash = computeIndexHash(hash, collision_bits_used, base_size_bits);
    ASSERT_EQ(index_hash, 1 << 11);
}

TEST_F(UtilTest, testComputeCollisionHash) {
    flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    size_t collision_hash = computeCollisionHash(hasher->hash_function, key, key_len);
    ASSERT_EQ(hasher->hash_function(key, key_len) >> 48, collision_hash);
}

TEST_F(UtilTest, testGetNumberOfBitsRequired) {
    int num = getNumBitsRequired(1ULL << 10);
    ASSERT_EQ(num, 10);

    ASSERT_DEATH(getNumBitsRequired(0), "");

    num = getNumBitsRequired(1ULL << 63);
    ASSERT_EQ(num, 63);

    num = getNumBitsRequired((1ULL << 10) - 1);
    ASSERT_EQ(num, 10);

    num = getNumBitsRequired((1ULL << 5) + 1);
    ASSERT_EQ(num, 6);
}

TEST_F(UtilTest, testGetFileSize) {
    std::string filename;
    size_t filesize = (5LL << 20);  // 5 MiB

    filename = createTmpFile(filesize);
    ASSERT_EQ(getFileSize(filename.c_str()), filesize);
    unlink(filename.c_str());
}

TEST_F(UtilTest, testFlashcacheAssertHandledCrash) {
    // A simple unit test to verify we actually crash when calling flashcacheAssertHandledCrash
    flashcacheAssertHandledCrash(1);
    ASSERT_DEATH(flashcacheAssertHandledCrash(0), "");
}
