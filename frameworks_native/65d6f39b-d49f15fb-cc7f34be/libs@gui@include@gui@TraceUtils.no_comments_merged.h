       
#include <stdarg.h>
#include <cutils/trace.h>
#include <utils/Trace.h>
#define ATRACE_FORMAT(fmt,...) \
    TraceUtils::TraceEnder traceEnder = \
            (CC_UNLIKELY(ATRACE_ENABLED()) && \
                     (TraceUtils::atraceFormatBegin(fmt, ##__VA_ARGS__), true), \
             TraceUtils::TraceEnder())
#define ATRACE_FORMAT_INSTANT(fmt,...) \
    (CC_UNLIKELY(ATRACE_ENABLED()) && (TraceUtils::instantFormat(fmt, ##__VA_ARGS__), true))
#define ALOGE_AND_TRACE(fmt,...) \
    do { \
        ALOGE(fmt, ##__VA_ARGS__); \
        ATRACE_FORMAT_INSTANT(fmt, ##__VA_ARGS__); \
    } while (false)
namespace android {
class TraceUtils {
public:
    class TraceEnder {
    public:
        ~TraceEnder() { ATRACE_END(); }
    };
    static void atraceFormatBegin(const char* fmt, ...) {
        const int BUFFER_SIZE = 256;
        va_list ap;
        char buf[BUFFER_SIZE];
        va_start(ap, fmt);
        vsnprintf(buf, BUFFER_SIZE, fmt, ap);
        va_end(ap);
        ATRACE_BEGIN(buf);
    }
    static void instantFormat(const char* fmt, ...) {
        const int BUFFER_SIZE = 256;
        va_list ap;
        char buf[BUFFER_SIZE];
        va_start(ap, fmt);
        vsnprintf(buf, BUFFER_SIZE, fmt, ap);
        va_end(ap);
        ATRACE_INSTANT(buf);
    }
};
}
