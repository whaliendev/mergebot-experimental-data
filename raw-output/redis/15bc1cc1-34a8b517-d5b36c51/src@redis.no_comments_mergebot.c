#include "redis.h"
#include "slowlog.h"
#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <float.h>
#include <math.h>
struct sharedObjectsStruct shared;
double R_Zero, R_PosInf, R_NegInf, R_Nan;
struct redisServer server;
struct redisCommand *commandTable;
struct redisCommand redisCommandTable[] = {
    {"get",getCommand,2,0,NULL,1,1,1,0,0},
    {"set",setCommand,3,REDIS_CMD_DENYOOM,noPreloadGetKeys,1,1,1,0,0},
    {"setnx",setnxCommand,3,REDIS_CMD_DENYOOM,noPreloadGetKeys,1,1,1,0,0},
    {"setex",setexCommand,4,REDIS_CMD_DENYOOM,noPreloadGetKeys,2,2,1,0,0},
    {"append",appendCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"strlen",strlenCommand,2,0,NULL,1,1,1,0,0},
    {"del",delCommand,-2,0,noPreloadGetKeys,1,-1,1,0,0},
    {"exists",existsCommand,2,0,NULL,1,1,1,0,0},
    {"setbit",setbitCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"getbit",getbitCommand,3,0,NULL,1,1,1,0,0},
    {"setrange",setrangeCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"getrange",getrangeCommand,4,0,NULL,1,1,1,0,0},
    {"substr",getrangeCommand,4,0,NULL,1,1,1,0,0},
    {"incr",incrCommand,2,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"decr",decrCommand,2,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"mget",mgetCommand,-2,0,NULL,1,-1,1,0,0},
    {"rpush",rpushCommand,-3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"lpush",lpushCommand,-3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"rpushx",rpushxCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"lpushx",lpushxCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"linsert",linsertCommand,5,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"rpop",rpopCommand,2,0,NULL,1,1,1,0,0},
    {"lpop",lpopCommand,2,0,NULL,1,1,1,0,0},
    {"brpop",brpopCommand,-3,0,NULL,1,1,1,0,0},
    {"brpoplpush",brpoplpushCommand,4,REDIS_CMD_DENYOOM,NULL,1,2,1,0,0},
    {"blpop",blpopCommand,-3,0,NULL,1,-2,1,0,0},
    {"llen",llenCommand,2,0,NULL,1,1,1,0,0},
    {"lindex",lindexCommand,3,0,NULL,1,1,1,0,0},
    {"lset",lsetCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"lrange",lrangeCommand,4,0,NULL,1,1,1,0,0},
    {"ltrim",ltrimCommand,4,0,NULL,1,1,1,0,0},
    {"lrem",lremCommand,4,0,NULL,1,1,1,0,0},
    {"rpoplpush",rpoplpushCommand,3,REDIS_CMD_DENYOOM,NULL,1,2,1,0,0},
    {"sadd",saddCommand,-3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"srem",sremCommand,-3,0,NULL,1,1,1,0,0},
    {"smove",smoveCommand,4,0,NULL,1,2,1,0,0},
    {"sismember",sismemberCommand,3,0,NULL,1,1,1,0,0},
    {"scard",scardCommand,2,0,NULL,1,1,1,0,0},
    {"spop",spopCommand,2,0,NULL,1,1,1,0,0},
    {"srandmember",srandmemberCommand,2,0,NULL,1,1,1,0,0},
    {"sinter",sinterCommand,-2,REDIS_CMD_DENYOOM,NULL,1,-1,1,0,0},
    {"sinterstore",sinterstoreCommand,-3,REDIS_CMD_DENYOOM,NULL,2,-1,1,0,0},
    {"sunion",sunionCommand,-2,REDIS_CMD_DENYOOM,NULL,1,-1,1,0,0},
    {"sunionstore",sunionstoreCommand,-3,REDIS_CMD_DENYOOM,NULL,2,-1,1,0,0},
    {"sdiff",sdiffCommand,-2,REDIS_CMD_DENYOOM,NULL,1,-1,1,0,0},
    {"sdiffstore",sdiffstoreCommand,-3,REDIS_CMD_DENYOOM,NULL,2,-1,1,0,0},
    {"smembers",sinterCommand,2,0,NULL,1,1,1,0,0},
    {"zadd",zaddCommand,-4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"zincrby",zincrbyCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"zrem",zremCommand,-3,0,NULL,1,1,1,0,0},
    {"zremrangebyscore",zremrangebyscoreCommand,4,0,NULL,1,1,1,0,0},
    {"zremrangebyrank",zremrangebyrankCommand,4,0,NULL,1,1,1,0,0},
    {"zunionstore",zunionstoreCommand,-4,REDIS_CMD_DENYOOM,zunionInterGetKeys,0,0,0,0,0},
    {"zinterstore",zinterstoreCommand,-4,REDIS_CMD_DENYOOM,zunionInterGetKeys,0,0,0,0,0},
    {"zrange",zrangeCommand,-4,0,NULL,1,1,1,0,0},
    {"zrangebyscore",zrangebyscoreCommand,-4,0,NULL,1,1,1,0,0},
    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,0,NULL,1,1,1,0,0},
    {"zcount",zcountCommand,4,0,NULL,1,1,1,0,0},
    {"zrevrange",zrevrangeCommand,-4,0,NULL,1,1,1,0,0},
    {"zcard",zcardCommand,2,0,NULL,1,1,1,0,0},
    {"zscore",zscoreCommand,3,0,NULL,1,1,1,0,0},
    {"zrank",zrankCommand,3,0,NULL,1,1,1,0,0},
    {"zrevrank",zrevrankCommand,3,0,NULL,1,1,1,0,0},
    {"hset",hsetCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"hsetnx",hsetnxCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"hget",hgetCommand,3,0,NULL,1,1,1,0,0},
    {"hmset",hmsetCommand,-4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"hmget",hmgetCommand,-3,0,NULL,1,1,1,0,0},
    {"hincrby",hincrbyCommand,4,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"hdel",hdelCommand,-3,0,NULL,1,1,1,0,0},
    {"hlen",hlenCommand,2,0,NULL,1,1,1,0,0},
    {"hkeys",hkeysCommand,2,0,NULL,1,1,1,0,0},
    {"hvals",hvalsCommand,2,0,NULL,1,1,1,0,0},
    {"hgetall",hgetallCommand,2,0,NULL,1,1,1,0,0},
    {"hexists",hexistsCommand,3,0,NULL,1,1,1,0,0},
    {"incrby",incrbyCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"decrby",decrbyCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"getset",getsetCommand,3,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"mset",msetCommand,-3,REDIS_CMD_DENYOOM,NULL,1,-1,2,0,0},
    {"msetnx",msetnxCommand,-3,REDIS_CMD_DENYOOM,NULL,1,-1,2,0,0},
    {"randomkey",randomkeyCommand,1,0,NULL,0,0,0,0,0},
    {"select",selectCommand,2,0,NULL,0,0,0,0,0},
    {"move",moveCommand,3,0,NULL,1,1,1,0,0},
    {"rename",renameCommand,3,0,renameGetKeys,1,2,1,0,0},
    {"renamenx",renamenxCommand,3,0,renameGetKeys,1,2,1,0,0},
    {"expire",expireCommand,3,0,NULL,1,1,1,0,0},
    {"expireat",expireatCommand,3,0,NULL,1,1,1,0,0},
    {"keys",keysCommand,2,0,NULL,0,0,0,0,0},
    {"dbsize",dbsizeCommand,1,0,NULL,0,0,0,0,0},
    {"auth",authCommand,2,0,NULL,0,0,0,0,0},
    {"ping",pingCommand,1,0,NULL,0,0,0,0,0},
    {"echo",echoCommand,2,0,NULL,0,0,0,0,0},
    {"save",saveCommand,1,0,NULL,0,0,0,0,0},
    {"bgsave",bgsaveCommand,1,0,NULL,0,0,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,0,NULL,0,0,0,0,0},
    {"shutdown",shutdownCommand,1,0,NULL,0,0,0,0,0},
    {"lastsave",lastsaveCommand,1,0,NULL,0,0,0,0,0},
    {"type",typeCommand,2,0,NULL,1,1,1,0,0},
    {"multi",multiCommand,1,0,NULL,0,0,0,0,0},
    {"exec",execCommand,1,REDIS_CMD_DENYOOM,NULL,0,0,0,0,0},
    {"discard",discardCommand,1,0,NULL,0,0,0,0,0},
    {"sync",syncCommand,1,0,NULL,0,0,0,0,0},
    {"flushdb",flushdbCommand,1,0,NULL,0,0,0,0,0},
    {"flushall",flushallCommand,1,0,NULL,0,0,0,0,0},
    {"sort",sortCommand,-2,REDIS_CMD_DENYOOM,NULL,1,1,1,0,0},
    {"info",infoCommand,-1,0,NULL,0,0,0,0,0},
    {"monitor",monitorCommand,1,0,NULL,0,0,0,0,0},
    {"ttl",ttlCommand,2,0,NULL,1,1,1,0,0},
    {"persist",persistCommand,2,0,NULL,1,1,1,0,0},
    {"slaveof",slaveofCommand,3,0,NULL,0,0,0,0,0},
    {"debug",debugCommand,-2,0,NULL,0,0,0,0,0},
    {"config",configCommand,-2,0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,0,NULL,0,0,0,0,0},
    {"publish",publishCommand,3,REDIS_CMD_FORCE_REPLICATION,NULL,0,0,0,0,0},
    {"watch",watchCommand,-2,0,noPreloadGetKeys,1,-1,1,0,0},
    {"unwatch",unwatchCommand,1,0,NULL,0,0,0,0,0},
    {"cluster",clusterCommand,-2,0,NULL,0,0,0,0,0},
    {"restore",restoreCommand,4,0,NULL,0,0,0,0,0},
    {"migrate",migrateCommand,6,0,NULL,0,0,0,0,0},
    {"dump",dumpCommand,2,0,NULL,0,0,0,0,0},
    {"object",objectCommand,-2,0,NULL,0,0,0,0,0},
<<<<<<< HEAD
    {"client",clientCommand,-2,0,NULL,0,0,0,0,0},
    {"eval",evalCommand,-3,REDIS_CMD_DENYOOM,zunionInterGetKeys,0,0,0,0,0},
    {"evalsha",evalShaCommand,-3,REDIS_CMD_DENYOOM,zunionInterGetKeys,0,0,0,0,0}
||||||| d5b36c511
    {"client",clientCommand,-2,0,NULL,0,0,0,0,0}
=======
    {"client",clientCommand,-2,0,NULL,0,0,0,0,0},
    {"slowlog",slowlogCommand,-2,0,NULL,0,0,0,0,0}
>>>>>>> 34a8b517
};
void redisLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    time_t now = time(NULL);
    FILE *fp;
    char buf[64];
    int rawmode = (level & REDIS_LOG_RAW);
    level &= 0xff;
    if (level < server.verbosity) return;
    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;
    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        strftime(buf,sizeof(buf),"%d %b %H:%M:%S",localtime(&now));
        fprintf(fp,"[%d] %s %c %s\n",(int)getpid(),buf,c[level],msg);
    }
    fflush(fp);
    if (server.logfile) fclose(fp);
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}
void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];
    if ((level&0xff) < server.verbosity) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    redisLogRaw(level,msg);
}
void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}
long long ustime(void) {
    struct timeval tv;
    long long ust;
    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}
void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}
void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}
int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);
    return strcasecmp(key1, key2) == 0;
}
void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    if (val == NULL) return;
    decrRefCount(val);
}
void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    sdsfree(val);
}
int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}
unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}
unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}
unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}
int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;
    if (o1->encoding == REDIS_ENCODING_INT &&
        o2->encoding == REDIS_ENCODING_INT)
            return o1->ptr == o2->ptr;
    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}
unsigned int dictEncObjHash(const void *key) {
    robj *o = (robj*) key;
    if (o->encoding == REDIS_ENCODING_RAW) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;
            len = ll2string(buf,32,(long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        } else {
            unsigned int hash;
            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}
dictType setDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};
dictType zsetDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};
dictType dbDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    dictRedisObjectDestructor
};
dictType keyptrDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    NULL,
    NULL
};
dictType commandTableDictType = {
    dictSdsCaseHash,
    NULL,
    NULL,
    dictSdsKeyCaseCompare,
    dictSdsDestructor,
    NULL
};
dictType hashDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    dictRedisObjectDestructor
};
dictType keylistDictType = {
    dictObjHash,
    NULL,
    NULL,
    dictObjKeyCompare,
    dictRedisObjectDestructor,
    dictListDestructor
};
dictType clusterNodesDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    NULL
};
int htNeedsResize(dict *dict) {
    long long size, used;
    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}
void tryResizeHashTables(void) {
    int j;
    for (j = 0; j < server.dbnum; j++) {
        if (htNeedsResize(server.db[j].dict))
            dictResize(server.db[j].dict);
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
    }
}
void incrementallyRehash(void) {
    int j;
    for (j = 0; j < server.dbnum; j++) {
        if (dictIsRehashing(server.db[j].dict)) {
            dictRehashMilliseconds(server.db[j].dict,1);
            break;
        }
    }
}
void updateDictResizePolicy(void) {
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}
void activeExpireCycle(void) {
    int j;
    for (j = 0; j < server.dbnum; j++) {
        int expired;
        redisDb *db = server.db+j;
        do {
            long num = dictSize(db->expires);
            time_t now = time(NULL);
            expired = 0;
            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                dictEntry *de;
                time_t t;
                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                t = (time_t) dictGetEntryVal(de);
                if (now > t) {
                    sds key = dictGetEntryKey(de);
                    robj *keyobj = createStringObject(key,sdslen(key));
                    propagateExpire(db,keyobj);
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    server.stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }
}
void updateLRUClock(void) {
    server.lruclock = (time(NULL)/REDIS_LRU_CLOCK_RESOLUTION) &
                                                REDIS_LRU_CLOCK_MAX;
}
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);
    server.unixtime = time(NULL);
    updateLRUClock();
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();
    if (server.shutdown_asap) {
        if (prepareForShutdown() == REDIS_OK) exit(0);
        redisLog(REDIS_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
    }
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;
        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 50) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
        }
    }
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1) {
        if (!(loops % 10)) tryResizeHashTables();
        if (server.activerehashing) incrementallyRehash();
    }
    if (!(loops % 50)) {
        redisLog(REDIS_VERBOSE,"%d clients connected (%d slaves), %zu bytes in use",
            listLength(server.clients)-listLength(server.slaves),
            listLength(server.slaves),
            zmalloc_used_memory());
    }
    if ((server.maxidletime && !(loops % 100)) || server.bpop_blocked_clients)
        closeTimedoutClients();
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1 &&
        server.aofrewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }
    if (server.bgsavechildpid != -1 || server.bgrewritechildpid != -1) {
        int statloc;
        pid_t pid;
        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;
            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);
            if (pid == server.bgsavechildpid) {
                backgroundSaveDoneHandler(exitcode,bysignal);
            } else {
                backgroundRewriteDoneHandler(exitcode,bysignal);
            }
            updateDictResizePolicy();
        }
    } else {
         time_t now = time(NULL);
         for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;
            if (server.dirty >= sp->changes &&
                now-server.lastsave > sp->seconds) {
                redisLog(REDIS_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, sp->seconds);
                rdbSaveBackground(server.dbfilename);
                break;
            }
         }
         if (server.bgsavechildpid == -1 &&
             server.bgrewritechildpid == -1 &&
             server.auto_aofrewrite_perc &&
             server.appendonly_current_size > server.auto_aofrewrite_min_size)
         {
            int base = server.auto_aofrewrite_base_size ?
                            server.auto_aofrewrite_base_size : 1;
            long long growth = (server.appendonly_current_size*100/base) - 100;
            if (growth >= server.auto_aofrewrite_perc) {
                redisLog(REDIS_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
         }
    }
    if (server.masterhost == NULL) activeExpireCycle();
    if (!(loops % 10)) replicationCron();
    if (server.cluster_enabled && !(loops % 10)) clusterCron();
    server.cronloops++;
    return 100;
}
void beforeSleep(struct aeEventLoop *eventLoop) {
    REDIS_NOTUSED(eventLoop);
    listNode *ln;
    redisClient *c;
    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        redisAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~REDIS_UNBLOCKED;
        if (c->querybuf && sdslen(c->querybuf) > 0)
            processInputBuffer(c);
    }
    flushAppendOnlyFile();
}
void createSharedObjects(void) {
    int j;
    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.cnegone = createObject(REDIS_STRING,sdsnew(":-1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(REDIS_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(REDIS_STRING,sdsnew("+QUEUED\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(REDIS_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(REDIS_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(REDIS_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(REDIS_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(REDIS_STRING,sdsnew(
        "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.space = createObject(REDIS_STRING,sdsnew(" "));
    shared.colon = createObject(REDIS_STRING,sdsnew(":"));
    shared.plus = createObject(REDIS_STRING,sdsnew("+"));
    shared.select0 = createStringObject("select 0\r\n",10);
    shared.select1 = createStringObject("select 1\r\n",10);
    shared.select2 = createStringObject("select 2\r\n",10);
    shared.select3 = createStringObject("select 3\r\n",10);
    shared.select4 = createStringObject("select 4\r\n",10);
    shared.select5 = createStringObject("select 5\r\n",10);
    shared.select6 = createStringObject("select 6\r\n",10);
    shared.select7 = createStringObject("select 7\r\n",10);
    shared.select8 = createStringObject("select 8\r\n",10);
    shared.select9 = createStringObject("select 9\r\n",10);
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
    shared.mbulk3 = createStringObject("*3\r\n",4);
    shared.mbulk4 = createStringObject("*4\r\n",4);
    for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(REDIS_STRING,(void*)(long)j);
        shared.integers[j]->encoding = REDIS_ENCODING_INT;
    }
}
void initServerConfig() {
    server.port = REDIS_SERVERPORT;
    server.bindaddr = NULL;
    server.unixsocket = NULL;
    server.ipfd = -1;
    server.sofd = -1;
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.verbosity = REDIS_VERBOSE;
    server.maxidletime = REDIS_MAXIDLETIME;
    server.saveparams = NULL;
    server.loading = 0;
    server.logfile = NULL;
    server.syslog_enabled = 0;
    server.syslog_ident = zstrdup("redis");
    server.syslog_facility = LOG_LOCAL0;
    server.daemonize = 0;
    server.appendonly = 0;
    server.appendfsync = APPENDFSYNC_EVERYSEC;
    server.no_appendfsync_on_rewrite = 0;
    server.auto_aofrewrite_perc = REDIS_AUTO_AOFREWRITE_PERC;
    server.auto_aofrewrite_min_size = REDIS_AUTO_AOFREWRITE_MIN_SIZE;
    server.auto_aofrewrite_base_size = 0;
    server.aofrewrite_scheduled = 0;
    server.lastfsync = time(NULL);
    server.appendfd = -1;
    server.appendseldb = -1;
    server.pidfile = zstrdup("/var/run/redis.pid");
    server.dbfilename = zstrdup("dump.rdb");
    server.appendfilename = zstrdup("appendonly.aof");
    server.requirepass = NULL;
    server.rdbcompression = 1;
    server.activerehashing = 1;
    server.maxclients = 0;
    server.bpop_blocked_clients = 0;
    server.maxmemory = 0;
    server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_LRU;
    server.maxmemory_samples = 3;
    server.hash_max_zipmap_entries = REDIS_HASH_MAX_ZIPMAP_ENTRIES;
    server.hash_max_zipmap_value = REDIS_HASH_MAX_ZIPMAP_VALUE;
    server.list_max_ziplist_entries = REDIS_LIST_MAX_ZIPLIST_ENTRIES;
    server.list_max_ziplist_value = REDIS_LIST_MAX_ZIPLIST_VALUE;
    server.set_max_intset_entries = REDIS_SET_MAX_INTSET_ENTRIES;
    server.zset_max_ziplist_entries = REDIS_ZSET_MAX_ZIPLIST_ENTRIES;
    server.zset_max_ziplist_value = REDIS_ZSET_MAX_ZIPLIST_VALUE;
    server.shutdown_asap = 0;
    server.cluster_enabled = 0;
    server.cluster.configfile = zstrdup("nodes.conf");
    server.lua_time_limit = REDIS_LUA_TIME_LIMIT;
    updateLRUClock();
    resetServerSaveParams();
    appendServerSaveParams(60*60,1);
    appendServerSaveParams(300,100);
    appendServerSaveParams(60,10000);
    server.isslave = 0;
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.replstate = REDIS_REPL_NONE;
    server.repl_syncio_timeout = REDIS_REPL_SYNCIO_TIMEOUT;
    server.repl_serve_stale_data = 1;
    server.repl_down_since = -1;
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;
    server.commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.slowlog_log_slower_than = REDIS_SLOWLOG_LOG_SLOWER_THAN;
    server.slowlog_max_len = REDIS_SLOWLOG_MAX_LEN;
}
void initServer() {
    int j;
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();
    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }
    server.clients = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.unblocked_clients = listCreate();
    createSharedObjects();
    server.el = aeCreateEventLoop();
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    if (server.port != 0) {
        server.ipfd = anetTcpServer(server.neterr,server.port,server.bindaddr);
        if (server.ipfd == ANET_ERR) {
            redisLog(REDIS_WARNING, "Opening port: %s", server.neterr);
            exit(1);
        }
    }
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket);
        server.sofd = anetUnixServer(server.neterr,server.unixsocket);
        if (server.sofd == ANET_ERR) {
            redisLog(REDIS_WARNING, "Opening socket: %s", server.neterr);
            exit(1);
        }
    }
    if (server.ipfd < 0 && server.sofd < 0) {
        redisLog(REDIS_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].id = j;
    }
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = listCreate();
    listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);
    server.cronloops = 0;
    server.bgsavechildpid = -1;
    server.bgrewritechildpid = -1;
    server.bgrewritebuf = sdsempty();
    server.aofbuf = sdsempty();
    server.lastsave = time(NULL);
    server.dirty = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_evictedkeys = 0;
    server.stat_starttime = time(NULL);
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_peak_memory = 0;
    server.stat_fork_time = 0;
    server.unixtime = time(NULL);
    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (server.ipfd > 0 && aeCreateFileEvent(server.el,server.ipfd,AE_READABLE,
        acceptTcpHandler,NULL) == AE_ERR) oom("creating file event");
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) oom("creating file event");
    if (server.appendonly) {
        server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.appendfd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }
    if (server.cluster_enabled) clusterInit();
<<<<<<< HEAD
    scriptingInit();
||||||| d5b36c511
=======
    slowlogInit();
>>>>>>> 34a8b517
    srand(time(NULL)^getpid());
}
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        int retval;
        retval = dictAdd(server.commands, sdsnew(c->name), c);
        assert(retval == DICT_OK);
    }
}
void resetCommandTableStats(void) {
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
    int j;
    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        c->microseconds = 0;
        c->calls = 0;
    }
}
struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}
struct redisCommand *lookupCommandByCString(char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);
    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}
void call(redisClient *c) {
    long long dirty, start = ustime(), duration;
    dirty = server.dirty;
    c->cmd->proc(c);
    dirty = server.dirty-dirty;
    duration = ustime()-start;
    c->cmd->microseconds += duration;
    slowlogPushEntryIfNeeded(c->argv,c->argc,duration);
    c->cmd->calls++;
    if (server.appendonly && dirty)
        feedAppendOnlyFile(c->cmd,c->db->id,c->argv,c->argc);
    if ((dirty || c->cmd->flags & REDIS_CMD_FORCE_REPLICATION) &&
        listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedMonitors(server.monitors,c->db->id,c->argv,c->argc);
    server.stat_numcommands++;
}
int processCommand(redisClient *c) {
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        addReply(c,shared.ok);
        c->flags |= REDIS_CLOSE_AFTER_REPLY;
        return REDIS_ERR;
    }
    c->cmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        addReplyErrorFormat(c,"unknown command '%s'",
            (char*)c->argv[0]->ptr);
        return REDIS_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return REDIS_OK;
    }
    if (server.requirepass && !c->authenticated && c->cmd->proc != authCommand)
    {
        addReplyError(c,"operation not permitted");
        return REDIS_OK;
    }
    if (server.cluster_enabled &&
                !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0)) {
        int hashslot;
        if (server.cluster.state != REDIS_CLUSTER_OK) {
            addReplyError(c,"The cluster is down. Check with CLUSTER INFO for more information");
            return REDIS_OK;
        } else {
            int ask;
            clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,&hashslot,&ask);
            if (n == NULL) {
                addReplyError(c,"Multi keys request invalid in cluster");
                return REDIS_OK;
            } else if (n != server.cluster.myself) {
                addReplySds(c,sdscatprintf(sdsempty(),
                    "-%s %d %s:%d\r\n", ask ? "ASK" : "MOVED",
                    hashslot,n->ip,n->port));
                return REDIS_OK;
            }
        }
    }
    if (server.maxmemory) freeMemoryIfNeeded();
    if (server.maxmemory && (c->cmd->flags & REDIS_CMD_DENYOOM) &&
        zmalloc_used_memory() > server.maxmemory)
    {
        addReplyError(c,"command not allowed when used memory > 'maxmemory'");
        return REDIS_OK;
    }
    if ((dictSize(c->pubsub_channels) > 0 || listLength(c->pubsub_patterns) > 0)
        &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand) {
        addReplyError(c,"only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context");
        return REDIS_OK;
    }
    if (server.masterhost && server.replstate != REDIS_REPL_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        c->cmd->proc != infoCommand && c->cmd->proc != slaveofCommand)
    {
        addReplyError(c,
            "link with MASTER is down and slave-serve-stale-data is set to no");
        return REDIS_OK;
    }
    if (server.loading && c->cmd->proc != infoCommand) {
        addReply(c, shared.loadingerr);
        return REDIS_OK;
    }
    if (c->flags & REDIS_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        call(c);
    }
    return REDIS_OK;
}
int prepareForShutdown() {
    redisLog(REDIS_WARNING,"User requested shutdown, saving DB...");
    if (server.bgsavechildpid != -1) {
        redisLog(REDIS_WARNING,"There is a live saving child. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    if (server.appendonly) {
        aof_fsync(server.appendfd);
    } else if (server.saveparamslen > 0) {
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
            return REDIS_ERR;
        }
    } else {
        redisLog(REDIS_WARNING,"Not saving DB.");
    }
    if (server.daemonize) unlink(server.pidfile);
    redisLog(REDIS_WARNING,"Server exit now, bye bye...");
    return REDIS_OK;
}
void authCommand(redisClient *c) {
    if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplyError(c,"invalid password");
    }
}
void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}
void echoCommand(redisClient *c) {
    addReplyBulk(c,c->argv[1]);
}
void bytesToHuman(char *s, unsigned long long n) {
    double d;
    if (n < 1024) {
        sprintf(s,"%lluB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}
sds genRedisInfoString(char *section) {
    sds info = sdsempty();
    time_t uptime = time(NULL)-server.stat_starttime;
    int j, numcommands;
    struct rusage self_ru, c_ru;
    unsigned long lol, bib;
    int allsections = 0, defsections = 0;
    int sections = 0;
    if (section) {
        allsections = strcasecmp(section,"all") == 0;
        defsections = strcasecmp(section,"default") == 0;
    }
    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &c_ru);
    getClientsMaxBuffers(&lol,&bib);
    if (allsections || defsections || !strcasecmp(section,"server")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%d\r\n"
            "arch_bits:%s\r\n"
            "multiplexing_api:%s\r\n"
            "process_id:%ld\r\n"
            "tcp_port:%d\r\n"
            "uptime_in_seconds:%ld\r\n"
            "uptime_in_days:%ld\r\n"
            "lru_clock:%ld\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            aeGetApiName(),
            (long) getpid(),
            server.port,
            uptime,
            uptime/(3600*24),
            (unsigned long) server.lruclock);
    }
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%d\r\n"
            "client_longest_output_list:%lu\r\n"
            "client_biggest_input_buf:%lu\r\n"
            "blocked_clients:%d\r\n",
            listLength(server.clients)-listLength(server.slaves),
            lol, bib,
            server.bpop_blocked_clients);
    }
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];
        bytesToHuman(hmem,zmalloc_used_memory());
        bytesToHuman(peak_hmem,server.stat_peak_memory);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Memory\r\n"
            "used_memory:%zu\r\n"
            "used_memory_human:%s\r\n"
            "used_memory_rss:%zu\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_allocator:%s\r\n",
            zmalloc_used_memory(),
            hmem,
            zmalloc_get_rss(),
            server.stat_peak_memory,
            peak_hmem,
            ((long long)lua_gc(server.lua,LUA_GCCOUNT,0))*1024LL,
            zmalloc_get_fragmentation_ratio(),
            ZMALLOC_LIB
            );
    }
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "aof_enabled:%d\r\n"
            "changes_since_last_save:%lld\r\n"
            "bgsave_in_progress:%d\r\n"
            "last_save_time:%ld\r\n"
            "bgrewriteaof_in_progress:%d\r\n",
            server.loading,
            server.appendonly,
            server.dirty,
            server.bgsavechildpid != -1,
            server.lastsave,
            server.bgrewritechildpid != -1);
        if (server.appendonly) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n",
                (long long) server.appendonly_current_size,
                (long long) server.auto_aofrewrite_base_size,
                server.aofrewrite_scheduled);
        }
        if (server.loading) {
            double perc;
            time_t eta, elapsed;
            off_t remaining_bytes = server.loading_total_bytes-
                                    server.loading_loaded_bytes;
            perc = ((double)server.loading_loaded_bytes /
                   server.loading_total_bytes) * 100;
            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1;
            } else {
                eta = (elapsed*remaining_bytes)/server.loading_loaded_bytes;
            }
            info = sdscatprintf(info,
                "loading_start_time:%ld\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%ld\r\n"
                ,(unsigned long) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                eta
            );
        }
    }
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "expired_keys:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%u\r\n"
            "latest_fork_usec:%lld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            server.stat_expiredkeys,
            server.stat_evictedkeys,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            listLength(server.pubsub_patterns),
            server.stat_fork_time);
    }
    if (allsections || defsections || !strcasecmp(section,"replication")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            info = sdscatprintf(info,
                "master_host:%s\r\n"
                "master_port:%d\r\n"
                "master_link_status:%s\r\n"
                "master_last_io_seconds_ago:%d\r\n"
                "master_sync_in_progress:%d\r\n"
                ,server.masterhost,
                server.masterport,
                (server.replstate == REDIS_REPL_CONNECTED) ?
                    "up" : "down",
                server.master ?
                ((int)(time(NULL)-server.master->lastinteraction)) : -1,
                server.replstate == REDIS_REPL_TRANSFER
            );
            if (server.replstate == REDIS_REPL_TRANSFER) {
                info = sdscatprintf(info,
                    "master_sync_left_bytes:%ld\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n"
                    ,(long)server.repl_transfer_left,
                    (int)(time(NULL)-server.repl_transfer_lastio)
                );
            }
            if (server.replstate != REDIS_REPL_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%ld\r\n",
                    (long)time(NULL)-server.repl_down_since);
            }
        }
        info = sdscatprintf(info,
            "connected_slaves:%d\r\n",
            listLength(server.slaves));
    }
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%.2f\r\n"
        "used_cpu_user:%.2f\r\n"
        "used_cpu_sys_children:%.2f\r\n"
        "used_cpu_user_children:%.2f\r\n",
        (float)self_ru.ru_utime.tv_sec+(float)self_ru.ru_utime.tv_usec/1000000,
        (float)self_ru.ru_stime.tv_sec+(float)self_ru.ru_stime.tv_usec/1000000,
        (float)c_ru.ru_utime.tv_sec+(float)c_ru.ru_utime.tv_usec/1000000,
        (float)c_ru.ru_stime.tv_sec+(float)c_ru.ru_stime.tv_usec/1000000);
    }
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
        for (j = 0; j < numcommands; j++) {
            struct redisCommand *c = redisCommandTable+j;
            if (!c->calls) continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                c->name, c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls));
        }
    }
    if (allsections || defsections || !strcasecmp(section,"cluster")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }
    if (allsections || defsections || !strcasecmp(section,"keyspace")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;
            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld\r\n",
                    j, keys, vkeys);
            }
        }
    }
    return info;
}
void infoCommand(redisClient *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";
    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}
void monitorCommand(redisClient *c) {
    if (c->flags & REDIS_SLAVE) return;
    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}
void freeMemoryIfNeeded(void) {
    if (server.maxmemory_policy == REDIS_MAXMEMORY_NO_EVICTION) return;
    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        int j, k, freed = 0;
        for (j = 0; j < server.dbnum; j++) {
            long bestval = 0;
            sds bestkey = NULL;
            struct dictEntry *de;
            redisDb *db = server.db+j;
            dict *dict;
            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM)
            {
                dict = server.db[j].dict;
            } else {
                dict = server.db[j].expires;
            }
            if (dictSize(dict) == 0) continue;
            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM ||
                server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_RANDOM)
            {
                de = dictGetRandomKey(dict);
                bestkey = dictGetEntryKey(de);
            }
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
            {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;
                    robj *o;
                    de = dictGetRandomKey(dict);
                    thiskey = dictGetEntryKey(de);
                    if (server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
                        de = dictFind(db->dict, thiskey);
                    o = dictGetEntryVal(de);
                    thisval = estimateObjectIdleTime(o);
                    if (bestkey == NULL || thisval > bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_TTL) {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;
                    de = dictGetRandomKey(dict);
                    thiskey = dictGetEntryKey(de);
                    thisval = (long) dictGetEntryVal(de);
                    if (bestkey == NULL || thisval < bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }
            if (bestkey) {
                robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
                propagateExpire(db,keyobj);
                dbDelete(db,keyobj);
                server.stat_evictedkeys++;
                decrRefCount(keyobj);
                freed++;
            }
        }
        if (!freed) return;
    }
}
#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];
    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return atoi(buf);
}
void linuxOvercommitMemoryWarning(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        redisLog(REDIS_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
}
#endif
void createPidFile(void) {
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}
void daemonize(void) {
    int fd;
    if (fork() != 0) exit(0);
    setsid();
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}
void version() {
    printf("Redis server version %s (%s:%d)\n", REDIS_VERSION,
        redisGitSHA1(), atoi(redisGitDirty()) > 0);
    exit(0);
}
void usage() {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    exit(1);
}
void redisAsciiArt(void) {
#include "asciilogo.h"
    char *buf = zmalloc(1024*16);
    snprintf(buf,1024*16,ascii_logo,
        REDIS_VERSION,
        redisGitSHA1(),
        strtol(redisGitDirty(),NULL,10) > 0,
        (sizeof(long) == 8) ? "64" : "32",
        server.cluster_enabled ? "cluster" : "stand alone",
        server.port,
        (long) getpid()
    );
    redisLogRaw(REDIS_NOTICE|REDIS_LOG_RAW,buf);
    zfree(buf);
}
int main(int argc, char **argv) {
    long long start;
    initServerConfig();
    if (argc == 2) {
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0) usage();
        resetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if ((argc > 2)) {
        usage();
    } else {
        redisLog(REDIS_WARNING,"Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    if (server.daemonize) daemonize();
    initServer();
    if (server.daemonize) createPidFile();
    redisAsciiArt();
    redisLog(REDIS_NOTICE,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    start = ustime();
    if (server.appendonly) {
        if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
        if (rdbLoad(server.dbfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from disk: %.3f seconds",(float)(ustime()-start)/1000000);
    }
    if (server.ipfd > 0)
        redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    if (server.sofd > 0)
        redisLog(REDIS_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
#ifdef HAVE_BACKTRACE
static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
    return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
    return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
  #if __x86_64__
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[14];
#elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[16];
#elif defined(__ia64__)
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}
static void sigsegvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    ucontext_t *uc = (ucontext_t*) secret;
    sds infostring;
    struct sigaction act;
    REDIS_NOTUSED(info);
    redisLog(REDIS_WARNING,
        "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
    infostring = genRedisInfoString("all");
    redisLogRaw(REDIS_WARNING, infostring);
    trace_size = backtrace(trace, 100);
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);
    for (i=1; i<trace_size; ++i)
        redisLog(REDIS_WARNING,"%s", messages[i]);
    if (server.daemonize) unlink(server.pidfile);
    sigemptyset (&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction (sig, &act, NULL);
    kill(getpid(),sig);
}
#endif
static void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);
    redisLog(REDIS_WARNING,"Received SIGTERM, scheduling shutdown...");
    server.shutdown_asap = 1;
}
void setupSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = sigtermHandler;
    sigaction(SIGTERM, &act, NULL);
#ifdef HAVE_BACKTRACE
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
#endif
    return;
}
