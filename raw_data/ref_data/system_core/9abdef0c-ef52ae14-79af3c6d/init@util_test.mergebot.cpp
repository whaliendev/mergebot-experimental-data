/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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