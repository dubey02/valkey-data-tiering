#include <gtest/gtest.h>

extern "C" {
#include "include/hash.h"
}

class HashTest : public testing::Test {
};

TEST_F(HashTest, testSiphashWithoutSeed) {
    std::string str1 = "HelloWorld", str2 = "helloworld", str3 = "CaptainAmerica";
    uint64_t hash1, hash2, hash3;

    flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher->init(nullptr);
    ASSERT_EQ(FLASHCACHE_SIPHASH_HASHER, hasher->get_type());

    hash1 = hasher->hash_function(str1.c_str(), str1.length());
    hash2 = hasher->hash_function(str2.c_str(), str2.length());
    hash3 = hasher->hash_function(str3.c_str(), str3.length());
    ASSERT_NE(hash1, hash2);
    ASSERT_NE(hash1, hash3);
    ASSERT_NE(hash2, hash3);

    ASSERT_EQ(hash1, hasher->hash_function(str1.c_str(), str1.length()));
    ASSERT_EQ(hash2, hasher->hash_function(str2.c_str(), str2.length()));
    ASSERT_EQ(hash3, hasher->hash_function(str3.c_str(), str3.length()));
}

TEST_F(HashTest, testSiphashWithSeed) {
    std::string str1 = "HelloWorld";
    uint64_t hash1, hash2;
    uint8_t fetched_seed1[FLASHCACHE_HASHER_SEED_SIZE], fetched_seed2[FLASHCACHE_HASHER_SEED_SIZE];

    flashcacheHasher *hasher = flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher->init(nullptr);
    hasher->get_seed(fetched_seed1);
    hash1 = hasher->hash_function(str1.c_str(), str1.length());

    std::string seed = "0123456789012345";
    hasher->init(reinterpret_cast<const uint8_t *>(seed.c_str()));
    hash2 = hasher->hash_function(str1.c_str(), str1.length());
    ASSERT_NE(hash1, hash2);

    hasher->get_seed(fetched_seed2);
    ASSERT_EQ(0, memcmp(fetched_seed2, seed.c_str(), FLASHCACHE_HASHER_SEED_SIZE));

    hasher->init(fetched_seed1);
    ASSERT_EQ(hash1, hasher->hash_function(str1.c_str(), str1.length()));

    hasher->init(fetched_seed2);
    ASSERT_EQ(hash2, hasher->hash_function(str1.c_str(), str1.length()));
}

uint32_t invalid_get_type_func() {
    return 2;
}

TEST_F(HashTest, testValidation) {
    flashcacheHasher hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    flashcacheHasherValidate(&hasher);

    hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher.init = nullptr;
    ASSERT_DEATH(flashcacheHasherValidate(&hasher), "");

    hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher.get_seed = nullptr;
    ASSERT_DEATH(flashcacheHasherValidate(&hasher), "");

    hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher.hash_function = nullptr;
    ASSERT_DEATH(flashcacheHasherValidate(&hasher), "");

    hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher.get_type = nullptr;
    ASSERT_DEATH(flashcacheHasherValidate(&hasher), "");

    hasher = *flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER);
    hasher.get_type = invalid_get_type_func;
    ASSERT_DEATH(flashcacheHasherValidate(&hasher), "");
}
