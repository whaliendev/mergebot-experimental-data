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

TEST(util, write_file_exist, read_file_success, write_file_binary){
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
  std::string s("hello");
  EXPECT_TRUE(read_file("/proc/version", &s));
  EXPECT_GT(s.length(), 6U);
  EXPECT_EQ('\n', s[s.length() - 1]);
  s[5] = 0;
  EXPECT_STREQ("Linux", s.c_str());
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
}

TEST(util, write_file_exist, read_file_success, write_file_binary){
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
  std::string s("hello");
  EXPECT_TRUE(read_file("/proc/version", &s));
  EXPECT_GT(s.length(), 6U);
  EXPECT_EQ('\n', s[s.length() - 1]);
  s[5] = 0;
  EXPECT_STREQ("Linux", s.c_str());
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
}

TEST(util, write_file_exist, read_file_success, write_file_binary){
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
<<<<<<< HEAD
    std::string s2("");
    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, "1hello1")) << strerror(errno);
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("1hello1", s2.c_str());
    EXPECT_TRUE(write_file(tf.path, "2ll2"));
    EXPECT_TRUE(read_file(tf.path, &s2));
    EXPECT_STREQ("2ll2", s2.c_str());
||||||| 79af3c6d6
  std::string s("hello");
  EXPECT_TRUE(read_file("/proc/version", &s));
  EXPECT_GT(s.length(), 6U);
  EXPECT_EQ('\n', s[s.length() - 1]);
  s[5] = 0;
  EXPECT_STREQ("Linux", s.c_str());
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
=======
    std::string contents("abcd");
    contents.push_back('\0');
    contents.push_back('\0');
    contents.append("dcba");
    ASSERT_EQ(10u, contents.size());

    TemporaryFile tf;
    ASSERT_TRUE(tf.fd != -1);
    EXPECT_TRUE(write_file(tf.path, contents)) << strerror(errno);

    std::string read_back_contents;
    EXPECT_TRUE(read_file(tf.path, &read_back_contents)) << strerror(errno);
    EXPECT_EQ(contents, read_back_contents);
    EXPECT_EQ(10u, read_back_contents.size());
>>>>>>> ef52ae14
}

selabel_handle* sehandle;

TEST(util, mkdir_recursive) {
    TemporaryDir test_dir;
    std::string path = android::base::StringPrintf("%s/three/directories/deep", test_dir.path);
    EXPECT_EQ(0, mkdir_recursive(path, 0755));
    std::string path1 = android::base::StringPrintf("%s/three", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
    std::string path2 = android::base::StringPrintf("%s/three/directories", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
    std::string path3 = android::base::StringPrintf("%s/three/directories/deep", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
}TEST(util, mkdir_recursive_extra_slashes){
    TemporaryDir test_dir;
    std::string path = android::base::StringPrintf("%s/three////directories/deep//", test_dir.path);
    EXPECT_EQ(0, mkdir_recursive(path, 0755));
    std::string path1 = android::base::StringPrintf("%s/three", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
    std::string path2 = android::base::StringPrintf("%s/three/directories", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
    std::string path3 = android::base::StringPrintf("%s/three/directories/deep", test_dir.path);
    EXPECT_TRUE(is_dir(path1.c_str()));
}