#ifndef _LOGD_LOG_BUFFER_H__
#define _LOGD_LOG_BUFFER_H__ 
#include <sys/types.h>
#include <list>
#include <string>
#include <log/log.h>
#include <sysutils/SocketClient.h>
#include <private/android_filesystem_config.h>
#include "LogBufferElement.h"
#include "LogTimes.h"
#include "LogStatistics.h"
#include "LogWhiteBlackList.h"
typedef std::list<LogBufferElement *> LogBufferElementCollection;
class LogBuffer {
    LogBufferElementCollection mLogElements;
    pthread_mutex_t mLogElementsLock;
    LogStatistics stats;
    PruneList mPrune;
    typedef std::unordered_map<uid_t,
                               LogBufferElementCollection::iterator>
                LogBufferIteratorMap;
    LogBufferIteratorMap mLastWorstUid[LOG_ID_MAX];
    unsigned long mMaxSize[LOG_ID_MAX];
public:
    LastLogTimes &mTimes;
    LogBuffer(LastLogTimes *times);
    void init();
    int log(log_id_t log_id, log_time realtime,
            uid_t uid, pid_t pid, pid_t tid,
            const char *msg, unsigned short len);
    uint64_t flushTo(SocketClient *writer, const uint64_t start,
                     bool privileged,
                     int (*filter)(const LogBufferElement *element, void *arg) = NULL,
                     void *arg = NULL);
    void clear(log_id_t id, uid_t uid = AID_ROOT);
    unsigned long getSize(log_id_t id);
    int setSize(log_id_t id, unsigned long size);
    unsigned long getSizeUsed(log_id_t id);
    std::string formatStatistics(uid_t uid, unsigned int logMask);
    void enableStatistics() {
        stats.enableStatistics();
    }
    int initPrune(const char *cp) { return mPrune.init(cp); }
    std::string formatPrune() { return mPrune.format(); }
    char *pidToName(pid_t pid) { return stats.pidToName(pid); }
    uid_t pidToUid(pid_t pid) { return stats.pidToUid(pid); }
    char *uidToName(uid_t uid) { return stats.uidToName(uid); }
    void lock() { pthread_mutex_lock(&mLogElementsLock); }
    void unlock() { pthread_mutex_unlock(&mLogElementsLock); }
private:
    void maybePrune(log_id_t id);
    void prune(log_id_t id, unsigned long pruneRows, uid_t uid = AID_ROOT);
    LogBufferElementCollection::iterator erase(LogBufferElementCollection::iterator it);
};
#endif
