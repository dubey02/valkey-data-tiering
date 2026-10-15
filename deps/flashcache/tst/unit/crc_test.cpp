#include <gtest/gtest.h>

extern "C" {
#include "include/crc.h"
}

class CrcTest : public testing::Test {
};

TEST_F(CrcTest, testWithDifferentBufferSize) {
    std::string str = "HelloWorld Flashcache!";
    ASSERT_EQ(2581534242, flashcacheCrc32c(0, str.c_str(), 1));
    ASSERT_EQ(3344299280, flashcacheCrc32c(0, str.c_str(), 2));
    ASSERT_EQ(1851891065, flashcacheCrc32c(0, str.c_str(), 3));
    ASSERT_EQ(2008867263, flashcacheCrc32c(0, str.c_str(), 4));
    ASSERT_EQ(2178485787, flashcacheCrc32c(0, str.c_str(), 5));
    ASSERT_EQ(1589925939, flashcacheCrc32c(0, str.c_str(), 6));
    ASSERT_EQ(1313178122, flashcacheCrc32c(0, str.c_str(), 7));
    ASSERT_EQ(2842827281, flashcacheCrc32c(0, str.c_str(), 8));
    ASSERT_EQ(2623814037, flashcacheCrc32c(0, str.c_str(), 9));
    ASSERT_EQ(1407507230, flashcacheCrc32c(0, str.c_str(), 10));
    ASSERT_EQ(3467985180, flashcacheCrc32c(0, str.c_str(), 11));
    ASSERT_EQ(1752438011, flashcacheCrc32c(0, str.c_str(), 12));
    ASSERT_EQ(343392185, flashcacheCrc32c(0, str.c_str(), 13));
    ASSERT_EQ(191077770, flashcacheCrc32c(0, str.c_str(), 14));
    ASSERT_EQ(3651830901, flashcacheCrc32c(0, str.c_str(), 15));
    ASSERT_EQ(4258427497, flashcacheCrc32c(0, str.c_str(), 16));
    ASSERT_EQ(962767131, flashcacheCrc32c(0, str.c_str(), 17));
    ASSERT_EQ(1212042701, flashcacheCrc32c(0, str.c_str(), 18));
    ASSERT_EQ(1543923613, flashcacheCrc32c(0, str.c_str(), 19));
    ASSERT_EQ(2495491679, flashcacheCrc32c(0, str.c_str(), 20));
    ASSERT_EQ(166235382, flashcacheCrc32c(0, str.c_str(), 21));
    ASSERT_EQ(1433222813, flashcacheCrc32c(0, str.c_str(), 22));
}

TEST_F(CrcTest, testWithPreviousCrc) {
    std::string str = "Flashcache!";
    ASSERT_EQ(1433222813, flashcacheCrc32c(3467985180, str.c_str(), 11));
}
