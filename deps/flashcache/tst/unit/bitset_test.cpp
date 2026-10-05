#include <set>
#include <gtest/gtest.h>

extern "C" {
#include "include/bitset.h"
}

class BitsetTest : public testing::Test {
 public:
     size_t size;
     bitset *bset;

     void SetUp() {
         size = 1024;
         bset = bitsetCreate(size);
     }

     void TearDown() {
         bitsetRelease(bset);
     }
};

TEST_F(BitsetTest, testWithSizeNotMultipleOfEight) {
    size_t size = 45;
    bitset *set = bitsetCreate(size);

    bitsetSet(set, size - 2);
    ASSERT_EQ(1, bitsetGet(set, size - 2));
    ASSERT_EQ(0, bitsetGet(set, size - 1));
    ASSERT_DEATH(bitsetGet(set, size), "");
    ASSERT_DEATH(bitsetGet(set, size + 1), "");
    bitsetRelease(set);
}

TEST_F(BitsetTest, testAllIndexInitializedToZero) {
    for (size_t idx = 0; idx < size; ++idx) {
        ASSERT_EQ(0, bitsetGet(bset, idx));
    }
}

TEST_F(BitsetTest, testOutOfRangeGetSet) {
    ASSERT_DEATH(bitsetGet(bset, size), "");
    ASSERT_DEATH(bitsetGet(bset, size + 1), "");
    ASSERT_DEATH(bitsetGet(bset, 2 * size), "");
    ASSERT_DEATH(bitsetSet(bset, size), "");
    ASSERT_DEATH(bitsetSet(bset, 2 * size), "");
}

TEST_F(BitsetTest, testSetFewValues) {
    size_t set_indices[] = { 5u, 129u, 721u, 1004u, 795u, 976u, 55u };
    size_t set_indices_len = sizeof(set_indices) / sizeof(size_t);
    for (size_t i = 0; i < set_indices_len; ++i) {
        bitsetSet(bset, set_indices[i]);
    }

    for (size_t idx = 0; idx < size; ++idx) {
        uint8_t is_set = 0;
        for (size_t i = 0; i < set_indices_len; ++i) {
            if (set_indices[i] == idx) {
                is_set = 1;
                break;
            }
        }
        ASSERT_EQ(is_set, bitsetGet(bset, idx));
    }
}

TEST_F(BitsetTest, testAllIndexSet) {
    for (size_t idx = 0; idx < size; ++idx) {
        bitsetSet(bset, idx);
    }
    for (size_t idx = 0; idx < size; ++idx) {
        ASSERT_EQ(1, bitsetGet(bset, idx));
    }
}
