#ifndef _LOGD_LOG_WHITE_BLACK_LIST_H__
#define _LOGD_LOG_WHITE_BLACK_LIST_H__ 
#include <sys/types.h>
#include <list>
#include <string.h>
#include <LogBufferElement.h>
class Prune {
    friend class PruneList;
    const uid_t mUid;
    const pid_t mPid;
    int cmp(uid_t uid, pid_t pid) const;
public:
    static const uid_t uid_all = (uid_t) -1;
    static const pid_t pid_all = (pid_t) -1;
    Prune(uid_t uid, pid_t pid);
    uid_t getUid() const { return mUid; }
    pid_t getPid() const { return mPid; }
    int cmp(LogBufferElement *e) const { return cmp(e->getUid(), e->getPid()); }
    std::string format();
};
typedef std::list<Prune> PruneCollection;
class PruneList {
    PruneCollection mNaughty;
    PruneCollection mNice;
    bool mWorstUidEnabled;
public:
    PruneList();
    ~PruneList();
    int init(const char *str);
    bool naughty(LogBufferElement *element);
    bool naughty(void) { return !mNaughty.empty(); }
    bool nice(LogBufferElement *element);
    bool nice(void) { return !mNice.empty(); }
    bool worstUidEnabled() const { return mWorstUidEnabled; }
    std::string format();
};
#endif
