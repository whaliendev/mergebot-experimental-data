/*
 * Copyright (C) 2013-2014 The Android Open Source Project
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

#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <gtest/gtest.h>
#include <log/log.h>
#include <log/logger.h>
#include <log/log_read.h>
// enhanced version of LOG_FAILURE_RETRY to add support for EAGAIN and
// non-syscall libs. Since we are only using this in the emergency of
// a signal to stuff a terminating code into the logs, we will spin rather
// than try a usleep.

#define LOG_FAILURE_RETRY(exp) ({  \
    typeof (exp) _rc;              \
    do {                           \
        _rc = (exp);               \
    } while (((_rc == -1)          \
           && ((errno == EINTR)    \
            || (errno == EAGAIN))) \
          || (_rc == -EINTR)       \
          || (_rc == -EAGAIN));    \
    _rc; })

static const char begin[] = "--------- beginning of ";

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}// Return a pointer to each null terminated -v long time field.
char *fgetLongTime(char *buffer, size_t buflen, FILE *fp) {
    while (fgets(buffer, buflen, fp)) {
        char *cp = buffer;
        if (*cp != '[') {
            continue;
        }
        while (*++cp == ' ') {
            ;
        }
        char *ep = cp;
        while (isdigit(*ep)) {
            ++ep;
        }
        if ((*ep != '-') && (*ep != '.')) {
           continue;
        }
        // Find PID field
        while (((ep = strchr(ep, ':'))) && (*++ep != ' ')) {
            ;
        }
        if (!ep) {
            continue;
        }
        ep -= 7;
        *ep = '\0';
        return cp;
    }
    return NULL;
}

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}static void caught_blocking(int /*signum*/)
{
    unsigned long long v = 0xDEADBEEFA55A0000ULL;

    v += getpid() & 0xFFFF;

    LOG_FAILURE_RETRY(__android_log_btwrite(0, EVENT_TYPE_LONG, &v, sizeof(v)));
}

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}static void caught_blocking_tail(int /*signum*/)
{
    unsigned long long v = 0xA55ADEADBEEF0000ULL;

    v += getpid() & 0xFFFF;

    LOG_FAILURE_RETRY(__android_log_btwrite(0, EVENT_TYPE_LONG, &v, sizeof(v)));
}

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}static void caught_blocking_clear(int /*signum*/)
{
    unsigned long long v = 0xDEADBEEFA55C0000ULL;

    v += getpid() & 0xFFFF;

    LOG_FAILURE_RETRY(__android_log_btwrite(0, EVENT_TYPE_LONG, &v, sizeof(v)));
}

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}static bool get_white_black(char **list) {
    FILE *fp;

    fp = popen("logcat -p 2>/dev/null", "r");
    if (fp == NULL) {
        fprintf(stderr, "ERROR: logcat -p 2>/dev/null\n");
        return false;
    }

    char buffer[5120];

    while (fgets(buffer, sizeof(buffer), fp)) {
        char *hold = *list;
        char *buf = buffer;
	while (isspace(*buf)) {
            ++buf;
        }
        char *end = buf + strlen(buf);
        while (isspace(*--end) && (end >= buf)) {
            *end = '\0';
        }
        if (end < buf) {
            continue;
        }
        if (hold) {
            asprintf(list, "%s %s", hold, buf);
            free(hold);
        } else {
            asprintf(list, "%s", buf);
        }
    }
    pclose(fp);
    return *list != NULL;
}

static bool set_white_black(const char *list) {
    FILE *fp;

    char buffer[5120];

    snprintf(buffer, sizeof(buffer), "logcat -P '%s' 2>&1", list ? list : "");
    fp = popen(buffer, "r");
    if (fp == NULL) {
        fprintf(stderr, "ERROR: %s\n", buffer);
        return false;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        char *buf = buffer;
	while (isspace(*buf)) {
            ++buf;
        }
        char *end = buf + strlen(buf);
        while ((end > buf) && isspace(*--end)) {
            *end = '\0';
        }
        if (end <= buf) {
            continue;
        }
        fprintf(stderr, "%s\n", buf);
        pclose(fp);
        return false;
    }
    return pclose(fp) == 0;
}

TEST(logcat, white_black_adjust) {
    char *list = NULL;
    char *adjust = NULL;

    get_white_black(&list);

    static const char adjustment[] = "~! 300/20 300/25 2000 ~1000/5 ~1000/30";
    ASSERT_EQ(true, set_white_black(adjustment));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment, adjust);
    free(adjust);
    adjust = NULL;

    static const char adjustment2[] = "300/20 300/21 2000 ~1000";
    ASSERT_EQ(true, set_white_black(adjustment2));
    ASSERT_EQ(true, get_white_black(&adjust));
    EXPECT_STREQ(adjustment2, adjust);
    free(adjust);
    adjust = NULL;

    ASSERT_EQ(true, set_white_black(list));
    get_white_black(&adjust);
    EXPECT_STREQ(list ? list : "", adjust ? adjust : "");
    free(adjust);
    adjust = NULL;

    free(list);
    list = NULL;
}