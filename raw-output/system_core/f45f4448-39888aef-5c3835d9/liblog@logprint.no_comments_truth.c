#define _GNU_SOURCE 
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <cutils/list.h>
#include <log/logd.h>
#include <log/logprint.h>
#define MS_PER_NSEC 1000000
#define US_PER_NSEC 1000
#define WEAK static
typedef struct FilterInfo_t {
    char *mTag;
    android_LogPriority mPri;
    struct FilterInfo_t *p_next;
} FilterInfo;
struct AndroidLogFormat_t {
    android_LogPriority global_pri;
    FilterInfo *filters;
    AndroidLogPrintFormat format;
    bool colored_output;
    bool usec_time_output;
    bool printable_output;
    bool year_output;
    bool zone_output;
    bool epoch_output;
    bool monotonic_output;
};
#define ANDROID_COLOR_BLUE 75
#define ANDROID_COLOR_DEFAULT 231
#define ANDROID_COLOR_GREEN 40
#define ANDROID_COLOR_ORANGE 166
#define ANDROID_COLOR_RED 196
#define ANDROID_COLOR_YELLOW 226
static FilterInfo * filterinfo_new(const char * tag, android_LogPriority pri)
{
    FilterInfo *p_ret;
    p_ret = (FilterInfo *)calloc(1, sizeof(FilterInfo));
    p_ret->mTag = strdup(tag);
    p_ret->mPri = pri;
    return p_ret;
}
static android_LogPriority filterCharToPri (char c)
{
    android_LogPriority pri;
    c = tolower(c);
    if (c >= '0' && c <= '9') {
        if (c >= ('0'+ANDROID_LOG_SILENT)) {
            pri = ANDROID_LOG_VERBOSE;
        } else {
            pri = (android_LogPriority)(c - '0');
        }
    } else if (c == 'v') {
        pri = ANDROID_LOG_VERBOSE;
    } else if (c == 'd') {
        pri = ANDROID_LOG_DEBUG;
    } else if (c == 'i') {
        pri = ANDROID_LOG_INFO;
    } else if (c == 'w') {
        pri = ANDROID_LOG_WARN;
    } else if (c == 'e') {
        pri = ANDROID_LOG_ERROR;
    } else if (c == 'f') {
        pri = ANDROID_LOG_FATAL;
    } else if (c == 's') {
        pri = ANDROID_LOG_SILENT;
    } else if (c == '*') {
        pri = ANDROID_LOG_DEFAULT;
    } else {
        pri = ANDROID_LOG_UNKNOWN;
    }
    return pri;
}
static char filterPriToChar (android_LogPriority pri)
{
    switch (pri) {
        case ANDROID_LOG_VERBOSE: return 'V';
        case ANDROID_LOG_DEBUG: return 'D';
        case ANDROID_LOG_INFO: return 'I';
        case ANDROID_LOG_WARN: return 'W';
        case ANDROID_LOG_ERROR: return 'E';
        case ANDROID_LOG_FATAL: return 'F';
        case ANDROID_LOG_SILENT: return 'S';
        case ANDROID_LOG_DEFAULT:
        case ANDROID_LOG_UNKNOWN:
        default: return '?';
    }
}
static int colorFromPri (android_LogPriority pri)
{
    switch (pri) {
        case ANDROID_LOG_VERBOSE: return ANDROID_COLOR_DEFAULT;
        case ANDROID_LOG_DEBUG: return ANDROID_COLOR_BLUE;
        case ANDROID_LOG_INFO: return ANDROID_COLOR_GREEN;
        case ANDROID_LOG_WARN: return ANDROID_COLOR_ORANGE;
        case ANDROID_LOG_ERROR: return ANDROID_COLOR_RED;
        case ANDROID_LOG_FATAL: return ANDROID_COLOR_RED;
        case ANDROID_LOG_SILENT: return ANDROID_COLOR_DEFAULT;
        case ANDROID_LOG_DEFAULT:
        case ANDROID_LOG_UNKNOWN:
        default: return ANDROID_COLOR_DEFAULT;
    }
}
static android_LogPriority filterPriForTag(
        AndroidLogFormat *p_format, const char *tag)
{
    FilterInfo *p_curFilter;
    for (p_curFilter = p_format->filters
            ; p_curFilter != NULL
            ; p_curFilter = p_curFilter->p_next
    ) {
        if (0 == strcmp(tag, p_curFilter->mTag)) {
            if (p_curFilter->mPri == ANDROID_LOG_DEFAULT) {
                return p_format->global_pri;
            } else {
                return p_curFilter->mPri;
            }
        }
    }
    return p_format->global_pri;
}
int android_log_shouldPrintLine (
        AndroidLogFormat *p_format, const char *tag, android_LogPriority pri)
{
    return pri >= filterPriForTag(p_format, tag);
}
AndroidLogFormat *android_log_format_new()
{
    AndroidLogFormat *p_ret;
    p_ret = calloc(1, sizeof(AndroidLogFormat));
    p_ret->global_pri = ANDROID_LOG_VERBOSE;
    p_ret->format = FORMAT_BRIEF;
    p_ret->colored_output = false;
    p_ret->usec_time_output = false;
    p_ret->printable_output = false;
    p_ret->year_output = false;
    p_ret->zone_output = false;
    p_ret->epoch_output = false;
    p_ret->monotonic_output = android_log_timestamp() == 'm';
    return p_ret;
}
static list_declare(convertHead);
void android_log_format_free(AndroidLogFormat *p_format)
{
    FilterInfo *p_info, *p_info_old;
    p_info = p_format->filters;
    while (p_info != NULL) {
        p_info_old = p_info;
        p_info = p_info->p_next;
        free(p_info_old);
    }
    free(p_format);
    while (!list_empty(&convertHead)) {
        struct listnode *node = list_head(&convertHead);
        list_remove(node);
        free(node);
    }
}
int android_log_setPrintFormat(AndroidLogFormat *p_format,
        AndroidLogPrintFormat format)
{
    switch (format) {
    case FORMAT_MODIFIER_COLOR:
        p_format->colored_output = true;
        return 0;
    case FORMAT_MODIFIER_TIME_USEC:
        p_format->usec_time_output = true;
        return 0;
    case FORMAT_MODIFIER_PRINTABLE:
        p_format->printable_output = true;
        return 0;
    case FORMAT_MODIFIER_YEAR:
        p_format->year_output = true;
        return 0;
    case FORMAT_MODIFIER_ZONE:
        p_format->zone_output = !p_format->zone_output;
        return 0;
    case FORMAT_MODIFIER_EPOCH:
        p_format->epoch_output = true;
        return 0;
    case FORMAT_MODIFIER_MONOTONIC:
        p_format->monotonic_output = true;
        return 0;
    default:
        break;
    }
    p_format->format = format;
    return 1;
}
static const char tz[] = "TZ";
static const char utc[] = "UTC";
AndroidLogPrintFormat android_log_formatFromString(const char * formatString)
{
    static AndroidLogPrintFormat format;
    if (strcmp(formatString, "brief") == 0) format = FORMAT_BRIEF;
    else if (strcmp(formatString, "process") == 0) format = FORMAT_PROCESS;
    else if (strcmp(formatString, "tag") == 0) format = FORMAT_TAG;
    else if (strcmp(formatString, "thread") == 0) format = FORMAT_THREAD;
    else if (strcmp(formatString, "raw") == 0) format = FORMAT_RAW;
    else if (strcmp(formatString, "time") == 0) format = FORMAT_TIME;
    else if (strcmp(formatString, "threadtime") == 0) format = FORMAT_THREADTIME;
    else if (strcmp(formatString, "long") == 0) format = FORMAT_LONG;
    else if (strcmp(formatString, "color") == 0) format = FORMAT_MODIFIER_COLOR;
    else if (strcmp(formatString, "usec") == 0) format = FORMAT_MODIFIER_TIME_USEC;
    else if (strcmp(formatString, "printable") == 0) format = FORMAT_MODIFIER_PRINTABLE;
    else if (strcmp(formatString, "year") == 0) format = FORMAT_MODIFIER_YEAR;
    else if (strcmp(formatString, "zone") == 0) format = FORMAT_MODIFIER_ZONE;
    else if (strcmp(formatString, "epoch") == 0) format = FORMAT_MODIFIER_EPOCH;
    else if (strcmp(formatString, "monotonic") == 0) format = FORMAT_MODIFIER_MONOTONIC;
    else {
        extern char *tzname[2];
        static const char gmt[] = "GMT";
        char *cp = getenv(tz);
        if (cp) {
            cp = strdup(cp);
        }
        setenv(tz, formatString, 1);
        tzset();
        if (!tzname[0]
                || ((!strcmp(tzname[0], utc)
                        || !strcmp(tzname[0], gmt))
                    && strcasecmp(formatString, utc)
                    && strcasecmp(formatString, gmt))) {
            if (cp) {
                setenv(tz, cp, 1);
            } else {
                unsetenv(tz);
            }
            tzset();
            format = FORMAT_OFF;
        } else {
            format = FORMAT_MODIFIER_ZONE;
        }
        free(cp);
    }
    return format;
}
int android_log_addFilterRule(AndroidLogFormat *p_format,
        const char *filterExpression)
{
    size_t tagNameLength;
    android_LogPriority pri = ANDROID_LOG_DEFAULT;
    tagNameLength = strcspn(filterExpression, ":");
    if (tagNameLength == 0) {
        goto error;
    }
    if(filterExpression[tagNameLength] == ':') {
        pri = filterCharToPri(filterExpression[tagNameLength+1]);
        if (pri == ANDROID_LOG_UNKNOWN) {
            goto error;
        }
    }
    if(0 == strncmp("*", filterExpression, tagNameLength)) {
        if (pri == ANDROID_LOG_DEFAULT) {
            pri = ANDROID_LOG_DEBUG;
        }
        p_format->global_pri = pri;
    } else {
        if (pri == ANDROID_LOG_DEFAULT) {
            pri = ANDROID_LOG_VERBOSE;
        }
        char *tagName;
#ifdef HAVE_STRNDUP
        tagName = strndup(filterExpression, tagNameLength);
#else
        tagName = strdup(filterExpression);
        tagName[tagNameLength] = '\0';
#endif
        FilterInfo *p_fi = filterinfo_new(tagName, pri);
        free(tagName);
        p_fi->p_next = p_format->filters;
        p_format->filters = p_fi;
    }
    return 0;
error:
    return -1;
}
int android_log_addFilterString(AndroidLogFormat *p_format,
        const char *filterString)
{
    char *filterStringCopy = strdup (filterString);
    char *p_cur = filterStringCopy;
    char *p_ret;
    int err;
    while (NULL != (p_ret = strsep(&p_cur, " \t,"))) {
        if(p_ret[0] != '\0') {
            err = android_log_addFilterRule(p_format, p_ret);
            if (err < 0) {
                goto error;
            }
        }
    }
    free (filterStringCopy);
    return 0;
error:
    free (filterStringCopy);
    return -1;
}
int android_log_processLogBuffer(struct logger_entry *buf,
                                 AndroidLogEntry *entry)
{
    entry->tv_sec = buf->sec;
    entry->tv_nsec = buf->nsec;
    entry->pid = buf->pid;
    entry->tid = buf->tid;
    if (buf->len < 3) {
        fprintf(stderr, "+++ LOG: entry too small\n");
        return -1;
    }
    int msgStart = -1;
    int msgEnd = -1;
    int i;
    char *msg = buf->msg;
    struct logger_entry_v2 *buf2 = (struct logger_entry_v2 *)buf;
    if (buf2->hdr_size) {
        msg = ((char *)buf2) + buf2->hdr_size;
    }
    for (i = 1; i < buf->len; i++) {
        if (msg[i] == '\0') {
            if (msgStart == -1) {
                msgStart = i + 1;
            } else {
                msgEnd = i;
                break;
            }
        }
    }
    if (msgStart == -1) {
        fprintf(stderr, "+++ LOG: malformed log message\n");
        return -1;
    }
    if (msgEnd == -1) {
        msgEnd = buf->len - 1;
        msg[msgEnd] = '\0';
    }
    entry->priority = msg[0];
    entry->tag = msg + 1;
    entry->message = msg + msgStart;
    entry->messageLen = msgEnd - msgStart;
    return 0;
}
static inline uint32_t get4LE(const uint8_t* src)
{
    return src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
}
static inline uint64_t get8LE(const uint8_t* src)
{
    uint32_t low, high;
    low = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
    high = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    return ((uint64_t) high << 32) | (uint64_t) low;
}
static int android_log_printBinaryEvent(const unsigned char** pEventData,
    size_t* pEventDataLen, char** pOutBuf, size_t* pOutBufLen)
{
    const unsigned char* eventData = *pEventData;
    size_t eventDataLen = *pEventDataLen;
    char* outBuf = *pOutBuf;
    size_t outBufLen = *pOutBufLen;
    unsigned char type;
    size_t outCount;
    int result = 0;
    if (eventDataLen < 1)
        return -1;
    type = *eventData++;
    eventDataLen--;
    switch (type) {
    case EVENT_TYPE_INT:
        {
            int ival;
            if (eventDataLen < 4)
                return -1;
            ival = get4LE(eventData);
            eventData += 4;
            eventDataLen -= 4;
            outCount = snprintf(outBuf, outBufLen, "%d", ival);
            if (outCount < outBufLen) {
                outBuf += outCount;
                outBufLen -= outCount;
            } else {
                goto no_room;
            }
        }
        break;
    case EVENT_TYPE_LONG:
        {
            uint64_t lval;
            if (eventDataLen < 8)
                return -1;
            lval = get8LE(eventData);
            eventData += 8;
            eventDataLen -= 8;
            outCount = snprintf(outBuf, outBufLen, "%" PRId64, lval);
            if (outCount < outBufLen) {
                outBuf += outCount;
                outBufLen -= outCount;
            } else {
                goto no_room;
            }
        }
        break;
    case EVENT_TYPE_FLOAT:
        {
            uint32_t ival;
            float fval;
            if (eventDataLen < 4)
                return -1;
            ival = get4LE(eventData);
            fval = *(float*)&ival;
            eventData += 4;
            eventDataLen -= 4;
            outCount = snprintf(outBuf, outBufLen, "%f", fval);
            if (outCount < outBufLen) {
                outBuf += outCount;
                outBufLen -= outCount;
            } else {
                goto no_room;
            }
        }
        break;
    case EVENT_TYPE_STRING:
        {
            unsigned int strLen;
            if (eventDataLen < 4)
                return -1;
            strLen = get4LE(eventData);
            eventData += 4;
            eventDataLen -= 4;
            if (eventDataLen < strLen)
                return -1;
            if (strLen < outBufLen) {
                memcpy(outBuf, eventData, strLen);
                outBuf += strLen;
                outBufLen -= strLen;
            } else if (outBufLen > 0) {
                memcpy(outBuf, eventData, outBufLen);
                outBuf += outBufLen;
                outBufLen -= outBufLen;
                goto no_room;
            }
            eventData += strLen;
            eventDataLen -= strLen;
            break;
        }
    case EVENT_TYPE_LIST:
        {
            unsigned char count;
            int i;
            if (eventDataLen < 1)
                return -1;
            count = *eventData++;
            eventDataLen--;
            if (outBufLen > 0) {
                *outBuf++ = '[';
                outBufLen--;
            } else {
                goto no_room;
            }
            for (i = 0; i < count; i++) {
                result = android_log_printBinaryEvent(&eventData, &eventDataLen,
                        &outBuf, &outBufLen);
                if (result != 0)
                    goto bail;
                if (i < count-1) {
                    if (outBufLen > 0) {
                        *outBuf++ = ',';
                        outBufLen--;
                    } else {
                        goto no_room;
                    }
                }
            }
            if (outBufLen > 0) {
                *outBuf++ = ']';
                outBufLen--;
            } else {
                goto no_room;
            }
        }
        break;
    default:
        fprintf(stderr, "Unknown binary event type %d\n", type);
        return -1;
    }
bail:
    *pEventData = eventData;
    *pEventDataLen = eventDataLen;
    *pOutBuf = outBuf;
    *pOutBufLen = outBufLen;
    return result;
no_room:
    result = 1;
    goto bail;
}
int android_log_processBinaryLogBuffer(struct logger_entry *buf,
    AndroidLogEntry *entry, const EventTagMap* map, char* messageBuf,
    int messageBufLen)
{
    size_t inCount;
    unsigned int tagIndex;
    const unsigned char* eventData;
    entry->tv_sec = buf->sec;
    entry->tv_nsec = buf->nsec;
    entry->priority = ANDROID_LOG_INFO;
    entry->pid = buf->pid;
    entry->tid = buf->tid;
    eventData = (const unsigned char*) buf->msg;
    struct logger_entry_v2 *buf2 = (struct logger_entry_v2 *)buf;
    if (buf2->hdr_size) {
        eventData = ((unsigned char *)buf2) + buf2->hdr_size;
    }
    inCount = buf->len;
    if (inCount < 4)
        return -1;
    tagIndex = get4LE(eventData);
    eventData += 4;
    inCount -= 4;
    if (map != NULL) {
        entry->tag = android_lookupEventTag(map, tagIndex);
    } else {
        entry->tag = NULL;
    }
    if (entry->tag == NULL) {
        int tagLen;
        tagLen = snprintf(messageBuf, messageBufLen, "[%d]", tagIndex);
        entry->tag = messageBuf;
        messageBuf += tagLen+1;
        messageBufLen -= tagLen+1;
    }
    char* outBuf = messageBuf;
    size_t outRemaining = messageBufLen-1;
    int result;
    result = android_log_printBinaryEvent(&eventData, &inCount, &outBuf,
                &outRemaining);
    if (result < 0) {
        fprintf(stderr, "Binary log entry conversion failed\n");
        return -1;
    } else if (result == 1) {
        if (outBuf > messageBuf) {
            *(outBuf-1) = '!';
        } else {
            *outBuf++ = '!';
            outRemaining--;
        }
        inCount = 0;
    }
    if (inCount == 1 && *eventData == '\n') {
        eventData++;
        inCount--;
    }
    if (inCount != 0) {
        fprintf(stderr,
            "Warning: leftover binary log data (%zu bytes)\n", inCount);
    }
    *outBuf = '\0';
    entry->messageLen = outBuf - messageBuf;
    assert(entry->messageLen == (messageBufLen-1) - outRemaining);
    entry->message = messageBuf;
    return 0;
}
WEAK ssize_t utf8_character_length(const char *src, size_t len)
{
    const char *cur = src;
    const char first_char = *cur++;
    static const uint32_t kUnicodeMaxCodepoint = 0x0010FFFF;
    int32_t mask, to_ignore_mask;
    size_t num_to_read;
    uint32_t utf32;
    if ((first_char & 0x80) == 0) {
        return first_char ? 1 : -1;
    }
    if ((first_char & 0x40) == 0) {
        return -1;
    }
    for (utf32 = 1, num_to_read = 1, mask = 0x40, to_ignore_mask = 0x80;
         num_to_read < 5 && (first_char & mask);
         num_to_read++, to_ignore_mask |= mask, mask >>= 1) {
        if (num_to_read > len) {
            return -1;
        }
        if ((*cur & 0xC0) != 0x80) {
            return -1;
        }
        utf32 = (utf32 << 6) + (*cur++ & 0b00111111);
    }
    if (num_to_read >= 5) {
        return -1;
    }
    to_ignore_mask |= mask;
    utf32 |= ((~to_ignore_mask) & first_char) << (6 * (num_to_read - 1));
    if (utf32 > kUnicodeMaxCodepoint) {
        return -1;
    }
    return num_to_read;
}
static size_t convertPrintable(char *p, const char *message, size_t messageLen)
{
    char *begin = p;
    bool print = p != NULL;
    while (messageLen) {
        char buf[6];
        ssize_t len = sizeof(buf) - 1;
        if ((size_t)len > messageLen) {
            len = messageLen;
        }
        len = utf8_character_length(message, len);
        if (len < 0) {
            snprintf(buf, sizeof(buf),
                     ((messageLen > 1) && isdigit(message[1]))
                         ? "\\%03o"
                         : "\\%o",
                     *message & 0377);
            len = 1;
        } else {
            buf[0] = '\0';
            if (len == 1) {
                if (*message == '\a') {
                    strcpy(buf, "\\a");
                } else if (*message == '\b') {
                    strcpy(buf, "\\b");
                } else if (*message == '\t') {
                    strcpy(buf, "\t");
                } else if (*message == '\v') {
                    strcpy(buf, "\\v");
                } else if (*message == '\f') {
                    strcpy(buf, "\\f");
                } else if (*message == '\r') {
                    strcpy(buf, "\\r");
                } else if (*message == '\\') {
                    strcpy(buf, "\\\\");
                } else if ((*message < ' ') || (*message & 0x80)) {
                    snprintf(buf, sizeof(buf), "\\%o", *message & 0377);
                }
            }
            if (!buf[0]) {
                strncpy(buf, message, len);
                buf[len] = '\0';
            }
        }
        if (print) {
            strcpy(p, buf);
        }
        p += strlen(buf);
        message += len;
        messageLen -= len;
    }
    return p - begin;
}
char *readSeconds(char *e, struct timespec *t)
{
    unsigned long multiplier;
    char *p;
    t->tv_sec = strtoul(e, &p, 10);
    if (*p != '.') {
        return NULL;
    }
    t->tv_nsec = 0;
    multiplier = NS_PER_SEC;
    while (isdigit(*++p) && (multiplier /= 10)) {
        t->tv_nsec += (*p - '0') * multiplier;
    }
    return p;
}
static struct timespec *sumTimespec(struct timespec *left,
                                    struct timespec *right)
{
    left->tv_nsec += right->tv_nsec;
    left->tv_sec += right->tv_sec;
    if (left->tv_nsec >= (long)NS_PER_SEC) {
        left->tv_nsec -= NS_PER_SEC;
        left->tv_sec += 1;
    }
    return left;
}
static struct timespec *subTimespec(struct timespec *result,
                                    struct timespec *left,
                                    struct timespec *right)
{
    result->tv_nsec = left->tv_nsec - right->tv_nsec;
    result->tv_sec = left->tv_sec - right->tv_sec;
    if (result->tv_nsec < 0) {
        result->tv_nsec += NS_PER_SEC;
        result->tv_sec -= 1;
    }
    return result;
}
static long long nsecTimespec(struct timespec *now)
{
    return (long long)now->tv_sec * NS_PER_SEC + now->tv_nsec;
}
static void convertMonotonic(struct timespec *result,
                             const AndroidLogEntry *entry)
{
    struct listnode *node;
    struct conversionList {
        struct listnode node;
        struct timespec time;
        struct timespec convert;
    } *list, *next;
    struct timespec time, convert;
    if (list_empty(&convertHead)) {
        bool suspended_pending = false;
        struct timespec suspended_monotonic = { 0, 0 };
        struct timespec suspended_diff = { 0, 0 };
        FILE *p = popen("/system/bin/dmesg", "r");
        if (p) {
            char *line = NULL;
            size_t len = 0;
            while (getline(&line, &len, p) > 0) {
                static const char suspend[] = "PM: suspend entry ";
                static const char resume[] = "PM: suspend exit ";
                static const char healthd[] = "healthd";
                static const char battery[] = ": battery ";
                static const char suspended[] = "Suspended for ";
                struct timespec monotonic;
                struct tm tm;
                char *cp, *e = line;
                bool add_entry = true;
                if (*e == '<') {
                    while (*e && (*e != '>')) {
                        ++e;
                    }
                    if (*e != '>') {
                        continue;
                    }
                }
                if (*e != '[') {
                    continue;
                }
                while (*++e == ' ') {
                    ;
                }
                e = readSeconds(e, &monotonic);
                if (!e || (*e != ']')) {
                    continue;
                }
                if ((e = strstr(e, suspend))) {
                    e += sizeof(suspend) - 1;
                } else if ((e = strstr(line, resume))) {
                    e += sizeof(resume) - 1;
                } else if (((e = strstr(line, healthd)))
                        && ((e = strstr(e + sizeof(healthd) - 1, battery)))) {
                    e += sizeof(battery) - 1;
                } else if ((e = strstr(line, suspended))) {
                    e += sizeof(suspended) - 1;
                    e = readSeconds(e, &time);
                    if (!e) {
                        continue;
                    }
                    add_entry = false;
                    suspended_pending = true;
                    suspended_monotonic = monotonic;
                    suspended_diff = time;
                } else {
                    continue;
                }
                if (add_entry) {
                    cp = strstr(e, " UTC");
                    if (!cp || ((cp - e) < 29) || (cp[-10] != '.')) {
                        continue;
                    }
                    e = cp - 29;
                    cp = readSeconds(cp - 10, &time);
                    if (!cp) {
                        continue;
                    }
                    cp = strptime(e, "%Y-%m-%d %H:%M:%S.", &tm);
                    if (!cp) {
                        continue;
                    }
                    cp = getenv(tz);
                    if (cp) {
                        cp = strdup(cp);
                    }
                    setenv(tz, utc, 1);
                    time.tv_sec = mktime(&tm);
                    if (cp) {
                        setenv(tz, cp, 1);
                        free(cp);
                    } else {
                        unsetenv(tz);
                    }
                    list = calloc(1, sizeof(struct conversionList));
                    list_init(&list->node);
                    list->time = time;
                    subTimespec(&list->convert, &time, &monotonic);
                    list_add_tail(&convertHead, &list->node);
                }
                if (suspended_pending && !list_empty(&convertHead)) {
                    list = node_to_item(list_tail(&convertHead),
                                        struct conversionList, node);
                    if (subTimespec(&time,
                                    subTimespec(&time,
                                                &list->time,
                                                &list->convert),
                                    &suspended_monotonic)->tv_sec > 0) {
                        subTimespec(&convert, &list->convert, &suspended_diff);
                    } else {
                        convert = list->convert;
                    }
                    time = suspended_monotonic;
                    sumTimespec(&time, &convert);
                    list = calloc(1, sizeof(struct conversionList));
                    list_init(&list->node);
                    list->time = time;
                    list->convert = convert;
                    list_add_tail(&convertHead, &list->node);
                    list = calloc(1, sizeof(struct conversionList));
                    list_init(&list->node);
                    list->time = time;
                    sumTimespec(&list->time, &suspended_diff);
                    list->convert = convert;
                    sumTimespec(&list->convert, &suspended_diff);
                    list_add_tail(&convertHead, &list->node);
                    suspended_pending = false;
                }
            }
            pclose(p);
        }
        list = calloc(1, sizeof(struct conversionList));
        list_init(&list->node);
        clock_gettime(CLOCK_REALTIME, &list->time);
        clock_gettime(CLOCK_MONOTONIC, &convert);
        clock_gettime(CLOCK_MONOTONIC, &time);
        subTimespec(&time, &convert, subTimespec(&time, &time, &convert));
        subTimespec(&list->convert, &list->time, &time);
        list_add_tail(&convertHead, &list->node);
        if (suspended_pending) {
            subTimespec(&convert, &list->convert, &suspended_diff);
            time = suspended_monotonic;
            sumTimespec(&time, &convert);
            list = calloc(1, sizeof(struct conversionList));
            list_init(&list->node);
            list->time = time;
            sumTimespec(&list->time, &suspended_diff);
            list->convert = convert;
            sumTimespec(&list->convert, &suspended_diff);
            list_add_head(&convertHead, &list->node);
            list = calloc(1, sizeof(struct conversionList));
            list_init(&list->node);
            list->time = time;
            list->convert = convert;
            list_add_head(&convertHead, &list->node);
        }
    }
    list = node_to_item(list_head(&convertHead), struct conversionList, node);
    next = NULL;
    list_for_each(node, &convertHead) {
        next = node_to_item(node, struct conversionList, node);
        if (entry->tv_sec < next->time.tv_sec) {
            break;
        } else if (entry->tv_sec == next->time.tv_sec) {
            if (entry->tv_nsec < next->time.tv_nsec) {
                break;
            }
        }
        list = next;
    }
    convert = list->convert;
    if (next) {
        unsigned long long total, run;
        total = nsecTimespec(subTimespec(&time, &next->time, &list->time));
        time.tv_sec = entry->tv_sec;
        time.tv_nsec = entry->tv_nsec;
        run = nsecTimespec(subTimespec(&time, &time, &list->time));
        if (run < total) {
            long long crun;
            float f = nsecTimespec(subTimespec(&time, &next->convert, &convert));
            f *= run;
            f /= total;
            crun = f;
            convert.tv_sec += crun / (long long)NS_PER_SEC;
            if (crun < 0) {
                convert.tv_nsec -= (-crun) % NS_PER_SEC;
                if (convert.tv_nsec < 0) {
                    convert.tv_nsec += NS_PER_SEC;
                    convert.tv_sec -= 1;
                }
            } else {
                convert.tv_nsec += crun % NS_PER_SEC;
                if (convert.tv_nsec >= (long)NS_PER_SEC) {
                    convert.tv_nsec -= NS_PER_SEC;
                    convert.tv_sec += 1;
                }
            }
        }
    }
    result->tv_sec = entry->tv_sec;
    result->tv_nsec = entry->tv_nsec;
    subTimespec(result, result, &convert);
}
char *android_log_formatLogLine (
    AndroidLogFormat *p_format,
    char *defaultBuffer,
    size_t defaultBufferSize,
    const AndroidLogEntry *entry,
    size_t *p_outLength)
{
#if !defined(_WIN32)
    struct tm tmBuf;
#endif
    struct tm* ptm;
    char timeBuf[64];
    char prefixBuf[128], suffixBuf[128];
    char priChar;
    int prefixSuffixIsHeaderFooter = 0;
    char *ret = NULL;
    time_t now;
    unsigned long nsec;
    priChar = filterPriToChar(entry->priority);
    size_t prefixLen = 0, suffixLen = 0;
    size_t len;
    now = entry->tv_sec;
    nsec = entry->tv_nsec;
    if (p_format->monotonic_output) {
        if (android_log_timestamp() != 'm') {
            struct timespec time;
            convertMonotonic(&time, entry);
            now = time.tv_sec;
            nsec = time.tv_nsec;
        }
    }
    if (now < 0) {
        nsec = NS_PER_SEC - nsec;
    }
    if (p_format->epoch_output || p_format->monotonic_output) {
        ptm = NULL;
        snprintf(timeBuf, sizeof(timeBuf),
                 p_format->monotonic_output ? "%6lld" : "%19lld",
                 (long long)now);
    } else {
#if !defined(_WIN32)
        ptm = localtime_r(&now, &tmBuf);
#else
        ptm = localtime(&now);
#endif
        strftime(timeBuf, sizeof(timeBuf),
                 &"%Y-%m-%d %H:%M:%S"[p_format->year_output ? 0 : 3],
                 ptm);
    }
    len = strlen(timeBuf);
    if (p_format->usec_time_output) {
        len += snprintf(timeBuf + len, sizeof(timeBuf) - len,
                        ".%06ld", nsec / US_PER_NSEC);
    } else {
        len += snprintf(timeBuf + len, sizeof(timeBuf) - len,
                        ".%03ld", nsec / MS_PER_NSEC);
    }
    if (p_format->zone_output && ptm) {
        strftime(timeBuf + len, sizeof(timeBuf) - len, " %z", ptm);
    }
    if (p_format->colored_output) {
        prefixLen = snprintf(prefixBuf, sizeof(prefixBuf), "\x1B[38;5;%dm",
                             colorFromPri(entry->priority));
        prefixLen = MIN(prefixLen, sizeof(prefixBuf));
        suffixLen = snprintf(suffixBuf, sizeof(suffixBuf), "\x1B[0m");
        suffixLen = MIN(suffixLen, sizeof(suffixBuf));
    }
    switch (p_format->format) {
        case FORMAT_TAG:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%c/%-8s: ", priChar, entry->tag);
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
        case FORMAT_PROCESS:
            len = snprintf(suffixBuf + suffixLen, sizeof(suffixBuf) - suffixLen,
                "  (%s)\n", entry->tag);
            suffixLen += MIN(len, sizeof(suffixBuf) - suffixLen);
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%c(%5d) ", priChar, entry->pid);
            break;
        case FORMAT_THREAD:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%c(%5d:%5d) ", priChar, entry->pid, entry->tid);
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
        case FORMAT_RAW:
            prefixBuf[prefixLen] = 0;
            len = 0;
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
        case FORMAT_TIME:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%s %c/%-8s(%5d): ", timeBuf, priChar, entry->tag, entry->pid);
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
        case FORMAT_THREADTIME:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%s %5d %5d %c %-8s: ", timeBuf,
                entry->pid, entry->tid, priChar, entry->tag);
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
        case FORMAT_LONG:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "[ %s %5d:%5d %c/%-8s ]\n",
                timeBuf, entry->pid, entry->tid, priChar, entry->tag);
            strcpy(suffixBuf + suffixLen, "\n\n");
            suffixLen += 2;
            prefixSuffixIsHeaderFooter = 1;
            break;
        case FORMAT_BRIEF:
        default:
            len = snprintf(prefixBuf + prefixLen, sizeof(prefixBuf) - prefixLen,
                "%c/%-8s(%5d): ", priChar, entry->tag, entry->pid);
            strcpy(suffixBuf + suffixLen, "\n");
            ++suffixLen;
            break;
    }
    prefixLen += MIN(len, sizeof(prefixBuf) - prefixLen);
    suffixLen = MIN(suffixLen, sizeof(suffixBuf));
    size_t numLines;
    char *p;
    size_t bufferSize;
    const char *pm;
    if (prefixSuffixIsHeaderFooter) {
        numLines = 1;
    } else {
        pm = entry->message;
        numLines = 0;
        while (pm < (entry->message + entry->messageLen)) {
            if (*pm++ == '\n') numLines++;
        }
        if (pm > entry->message && *(pm-1) != '\n') numLines++;
    }
    bufferSize = (numLines * (prefixLen + suffixLen)) + 1;
    if (p_format->printable_output) {
        bufferSize += convertPrintable(NULL, entry->message, entry->messageLen);
    } else {
        bufferSize += entry->messageLen;
    }
    if (defaultBufferSize >= bufferSize) {
        ret = defaultBuffer;
    } else {
        ret = (char *)malloc(bufferSize);
        if (ret == NULL) {
            return ret;
        }
    }
    ret[0] = '\0';
    p = ret;
    pm = entry->message;
    if (prefixSuffixIsHeaderFooter) {
        strcat(p, prefixBuf);
        p += prefixLen;
        if (p_format->printable_output) {
            p += convertPrintable(p, entry->message, entry->messageLen);
        } else {
            strncat(p, entry->message, entry->messageLen);
            p += entry->messageLen;
        }
        strcat(p, suffixBuf);
        p += suffixLen;
    } else {
        while(pm < (entry->message + entry->messageLen)) {
            const char *lineStart;
            size_t lineLen;
            lineStart = pm;
            while (pm < (entry->message + entry->messageLen)
                    && *pm != '\n') pm++;
            lineLen = pm - lineStart;
            strcat(p, prefixBuf);
            p += prefixLen;
            if (p_format->printable_output) {
                p += convertPrintable(p, lineStart, lineLen);
            } else {
                strncat(p, lineStart, lineLen);
                p += lineLen;
            }
            strcat(p, suffixBuf);
            p += suffixLen;
            if (*pm == '\n') pm++;
        }
    }
    if (p_outLength != NULL) {
        *p_outLength = p - ret;
    }
    return ret;
}
int android_log_printLogLine(
    AndroidLogFormat *p_format,
    int fd,
    const AndroidLogEntry *entry)
{
    int ret;
    char defaultBuffer[512];
    char *outBuffer = NULL;
    size_t totalLen;
    outBuffer = android_log_formatLogLine(p_format, defaultBuffer,
            sizeof(defaultBuffer), entry, &totalLen);
    if (!outBuffer)
        return -1;
    do {
        ret = write(fd, outBuffer, totalLen);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fprintf(stderr, "+++ LOG: write failed (errno=%d)\n", errno);
        ret = 0;
        goto done;
    }
    if (((size_t)ret) < totalLen) {
        fprintf(stderr, "+++ LOG: write partial (%d of %d)\n", ret,
                (int)totalLen);
        goto done;
    }
done:
    if (outBuffer != defaultBuffer) {
        free(outBuffer);
    }
    return ret;
}
