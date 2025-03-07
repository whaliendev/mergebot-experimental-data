#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <android-base/stringprintf.h>
#include <android-base/test_utils.h>
#include <gtest/gtest.h>
TEST(util, decode_uid) {
    EXPECT_EQ(0U, decode_uid("root"));
    EXPECT_EQ(UINT_MAX, decode_uid("toot"));
    EXPECT_EQ(123U, decode_uid("123"));
}
selabel_handle* sehandle;
TEST(util, decode_uid) {
    EXPECT_EQ(0U, decode_uid("root"));
    EXPECT_EQ(UINT_MAX, decode_uid("toot"));
    EXPECT_EQ(123U, decode_uid("123"));
}
TEST(util, decode_uid) {
    EXPECT_EQ(0U, decode_uid("root"));
    EXPECT_EQ(UINT_MAX, decode_uid("toot"));
    EXPECT_EQ(123U, decode_uid("123"));
}
TEST(util, decode_uid) {
    EXPECT_EQ(0U, decode_uid("root"));
    EXPECT_EQ(UINT_MAX, decode_uid("toot"));
    EXPECT_EQ(123U, decode_uid("123"));
}
