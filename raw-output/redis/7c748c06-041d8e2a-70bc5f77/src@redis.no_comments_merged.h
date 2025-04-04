#ifndef __REDIS_H
#define __REDIS_H 
#include "fmacros.h"
#include "config.h"
#if defined(__sun)
#include "solarisfixes.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <lua.h>
#include "ae.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"
#include "anet.h"
#include "zipmap.h"
#include "ziplist.h"
#include "intset.h"
#include "version.h"
#include "util.h"
#define REDIS_OK 0
#define REDIS_ERR -1
#define REDIS_SERVERPORT 6379
#define REDIS_MAXIDLETIME (60*5)
#define REDIS_IOBUF_LEN 1024
#define REDIS_LOADBUF_LEN 1024
#define REDIS_DEFAULT_DBNUM 16
#define REDIS_CONFIGLINE_MAX 1024
#define REDIS_MAX_SYNC_TIME 60
#define REDIS_EXPIRELOOKUPS_PER_CRON 10
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256)
#define REDIS_SHARED_INTEGERS 10000
#define REDIS_REPLY_CHUNK_BYTES (5*1500)
#define REDIS_MAX_LOGMSG_LEN 1024
#define REDIS_AUTO_AOFREWRITE_PERC 100
#define REDIS_AUTO_AOFREWRITE_MIN_SIZE (1024*1024)
#define REDIS_SLOWLOG_LOG_SLOWER_THAN 10000
#define REDIS_SLOWLOG_MAX_LEN 64
#define REDIS_HT_MINFILL 10
#define REDIS_CMD_DENYOOM 4
#define REDIS_CMD_FORCE_REPLICATION 8
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4
#define REDIS_VMPOINTER 8
#define REDIS_ENCODING_RAW 0
#define REDIS_ENCODING_INT 1
#define REDIS_ENCODING_HT 2
#define REDIS_ENCODING_ZIPMAP 3
#define REDIS_ENCODING_LINKEDLIST 4
#define REDIS_ENCODING_ZIPLIST 5
#define REDIS_ENCODING_INTSET 6
#define REDIS_ENCODING_SKIPLIST 7
<<<<<<< HEAD
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX
#define REDIS_RDB_ENC_INT8 0
#define REDIS_RDB_ENC_INT16 1
#define REDIS_RDB_ENC_INT32 2
#define REDIS_RDB_ENC_LZF 3
||||||| 70bc5f772
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX
#define REDIS_RDB_ENC_INT8 0
#define REDIS_RDB_ENC_INT16 1
#define REDIS_RDB_ENC_INT32 2
#define REDIS_RDB_ENC_LZF 3
#define REDIS_IO_LOAD 1
#define REDIS_IO_SAVE 2
#define REDIS_IO_LOADINPROG 4
#define REDIS_IO_SAVEINPROG 8
#define REDIS_IO_ONLYLOADS 1
#define REDIS_IO_ASAP 2
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
=======
#define REDIS_IO_LOAD 1
#define REDIS_IO_SAVE 2
#define REDIS_IO_LOADINPROG 4
#define REDIS_IO_SAVEINPROG 8
#define REDIS_IO_ONLYLOADS 1
#define REDIS_IO_ASAP 2
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
>>>>>>> 041d8e2a
#define REDIS_SLAVE 1
#define REDIS_MASTER 2
#define REDIS_MONITOR 4
#define REDIS_MULTI 8
#define REDIS_BLOCKED 16
#define REDIS_DIRTY_CAS 64
#define REDIS_CLOSE_AFTER_REPLY 128
#define REDIS_UNBLOCKED 256
#define REDIS_LUA_CLIENT 512
#define REDIS_REQ_INLINE 1
#define REDIS_REQ_MULTIBULK 2
#define REDIS_REPL_NONE 0
#define REDIS_REPL_CONNECT 1
#define REDIS_REPL_CONNECTING 2
#define REDIS_REPL_TRANSFER 3
#define REDIS_REPL_CONNECTED 4
#define REDIS_REPL_SYNCIO_TIMEOUT 5
#define REDIS_REPL_WAIT_BGSAVE_START 3
#define REDIS_REPL_WAIT_BGSAVE_END 4
#define REDIS_REPL_SEND_BULK 5
#define REDIS_REPL_ONLINE 6
#define REDIS_HEAD 0
#define REDIS_TAIL 1
#define REDIS_SORT_GET 0
#define REDIS_SORT_ASC 1
#define REDIS_SORT_DESC 2
#define REDIS_SORTKEY_MAX 1024
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3
#define REDIS_LOG_RAW (1<<10)
#define REDIS_NOTUSED(V) ((void) V)
#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25
#define APPENDFSYNC_NO 0
#define APPENDFSYNC_ALWAYS 1
#define APPENDFSYNC_EVERYSEC 2
#define REDIS_HASH_MAX_ZIPMAP_ENTRIES 512
#define REDIS_HASH_MAX_ZIPMAP_VALUE 64
#define REDIS_LIST_MAX_ZIPLIST_ENTRIES 512
#define REDIS_LIST_MAX_ZIPLIST_VALUE 64
#define REDIS_SET_MAX_INTSET_ENTRIES 512
#define REDIS_ZSET_MAX_ZIPLIST_ENTRIES 128
#define REDIS_ZSET_MAX_ZIPLIST_VALUE 64
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2
#define REDIS_MAXMEMORY_VOLATILE_LRU 0
#define REDIS_MAXMEMORY_VOLATILE_TTL 1
#define REDIS_MAXMEMORY_VOLATILE_RANDOM 2
#define REDIS_MAXMEMORY_ALLKEYS_LRU 3
#define REDIS_MAXMEMORY_ALLKEYS_RANDOM 4
#define REDIS_MAXMEMORY_NO_EVICTION 5
#define REDIS_LUA_TIME_LIMIT 60000
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define redisPanic(_e) _redisPanic(#_e,__FILE__,__LINE__),_exit(1)
void _redisAssert(char *estr, char *file, int line);
void _redisPanic(char *msg, char *file, int line);
#define REDIS_LRU_CLOCK_MAX ((1<<21)-1)
#define REDIS_LRU_CLOCK_RESOLUTION 10
typedef struct redisObject {
    unsigned type:4;
    unsigned notused:2;
    unsigned encoding:4;
    unsigned lru:22;
    int refcount;
    void *ptr;
} robj;
typedef struct vmPointer {
    unsigned type:4;
    unsigned storage:2;
    unsigned notused:26;
    unsigned int vtype;
    off_t page;
    off_t usedpages;
} vmpointer;
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0);
typedef struct redisDb {
    dict *dict;
    dict *expires;
    dict *blocking_keys;
    dict *watched_keys;
    int id;
} redisDb;
typedef struct multiCmd {
    robj **argv;
    int argc;
    struct redisCommand *cmd;
} multiCmd;
typedef struct multiState {
    multiCmd *commands;
    int count;
} multiState;
typedef struct blockingState {
    robj **keys;
    int count;
    time_t timeout;
    robj *target;
} blockingState;
typedef struct redisClient {
    int fd;
    redisDb *db;
    int dictid;
    sds querybuf;
    int argc;
    robj **argv;
    struct redisCommand *cmd;
    int reqtype;
    int multibulklen;
    long bulklen;
    list *reply;
    int sentlen;
    time_t lastinteraction;
    int flags;
    int slaveseldb;
    int authenticated;
    int replstate;
    int repldbfd;
    long repldboff;
    off_t repldbsize;
    multiState mstate;
    blockingState bpop;
    list *io_keys;
    list *watched_keys;
    dict *pubsub_channels;
    list *pubsub_patterns;
    int bufpos;
    char buf[REDIS_REPLY_CHUNK_BYTES];
} redisClient;
struct saveparam {
    time_t seconds;
    int changes;
};
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *cnegone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9,
    *messagebulk, *pmessagebulk, *subscribebulk, *unsubscribebulk, *mbulk3,
    *mbulk4, *psubscribebulk, *punsubscribebulk,
    *integers[REDIS_SHARED_INTEGERS];
};
typedef struct zskiplistNode {
    robj *obj;
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned int span;
    } level[];
} zskiplistNode;
typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
} zskiplist;
typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;
#define REDIS_CLUSTER_SLOTS 4096
#define REDIS_CLUSTER_OK 0
#define REDIS_CLUSTER_FAIL 1
#define REDIS_CLUSTER_NEEDHELP 2
#define REDIS_CLUSTER_NAMELEN 40
#define REDIS_CLUSTER_PORT_INCR 10000
struct clusterNode;
typedef struct clusterLink {
    int fd;
    sds sndbuf;
    sds rcvbuf;
    struct clusterNode *node;
} clusterLink;
#define REDIS_NODE_MASTER 1
#define REDIS_NODE_SLAVE 2
#define REDIS_NODE_PFAIL 4
#define REDIS_NODE_FAIL 8
#define REDIS_NODE_MYSELF 16
#define REDIS_NODE_HANDSHAKE 32
#define REDIS_NODE_NOADDR 64
#define REDIS_NODE_MEET 128
#define REDIS_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"
struct clusterNode {
    char name[REDIS_CLUSTER_NAMELEN];
    int flags;
    unsigned char slots[REDIS_CLUSTER_SLOTS/8];
    int numslaves;
    struct clusterNode **slaves;
    struct clusterNode *slaveof;
    time_t ping_sent;
    time_t pong_received;
    char *configdigest;
    time_t configdigest_ts;
    char ip[16];
    int port;
    clusterLink *link;
};
typedef struct clusterNode clusterNode;
typedef struct {
    char *configfile;
    clusterNode *myself;
    int state;
    int node_timeout;
    dict *nodes;
    clusterNode *migrating_slots_to[REDIS_CLUSTER_SLOTS];
    clusterNode *importing_slots_from[REDIS_CLUSTER_SLOTS];
    clusterNode *slots[REDIS_CLUSTER_SLOTS];
    zskiplist *slots_to_keys;
} clusterState;
#define CLUSTERMSG_TYPE_PING 0
#define CLUSTERMSG_TYPE_PONG 1
#define CLUSTERMSG_TYPE_MEET 2
#define CLUSTERMSG_TYPE_FAIL 3
typedef struct {
    char nodename[REDIS_CLUSTER_NAMELEN];
    uint32_t ping_sent;
    uint32_t pong_received;
    char ip[16];
    uint16_t port;
    uint16_t flags;
    uint32_t notused;
} clusterMsgDataGossip;
typedef struct {
    char nodename[REDIS_CLUSTER_NAMELEN];
} clusterMsgDataFail;
union clusterMsgData {
    struct {
        clusterMsgDataGossip gossip[1];
    } ping;
    struct {
        clusterMsgDataFail about;
    } fail;
};
typedef struct {
    uint32_t totlen;
    uint16_t type;
    uint16_t count;
    char sender[REDIS_CLUSTER_NAMELEN];
    unsigned char myslots[REDIS_CLUSTER_SLOTS/8];
    char slaveof[REDIS_CLUSTER_NAMELEN];
    char configdigest[32];
    uint16_t port;
    unsigned char state;
    unsigned char notused[5];
    union clusterMsgData data;
} clusterMsg;
struct redisServer {
    redisDb *db;
    dict *commands;
    aeEventLoop *el;
    int port;
    char *bindaddr;
    char *unixsocket;
    int ipfd;
    int sofd;
    int cfd;
    list *clients;
    list *slaves, *monitors;
    char neterr[ANET_ERR_LEN];
    int loading;
    off_t loading_total_bytes;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    struct redisCommand *delCommand, *multiCommand;
    int cronloops;
    time_t lastsave;
    time_t stat_starttime;
    long long stat_numcommands;
    long long stat_numconnections;
    long long stat_expiredkeys;
    long long stat_evictedkeys;
    long long stat_keyspace_hits;
    long long stat_keyspace_misses;
    size_t stat_peak_memory;
    long long stat_fork_time;
    list *slowlog;
    long long slowlog_entry_id;
    long long slowlog_log_slower_than;
    unsigned long slowlog_max_len;
    int verbosity;
    int maxidletime;
    int dbnum;
    int daemonize;
    int appendonly;
    int appendfsync;
    int no_appendfsync_on_rewrite;
    int auto_aofrewrite_perc;
    off_t auto_aofrewrite_min_size;
    off_t auto_aofrewrite_base_size;
    off_t appendonly_current_size;
    int aofrewrite_scheduled;
    int shutdown_asap;
    int activerehashing;
    char *requirepass;
    long long dirty;
    long long dirty_before_bgsave;
    time_t lastfsync;
    int appendfd;
    int appendseldb;
    time_t aof_flush_postponed_start;
    char *pidfile;
    pid_t bgsavechildpid;
    pid_t bgrewritechildpid;
    sds bgrewritebuf;
    sds aofbuf;
    struct saveparam *saveparams;
    int saveparamslen;
    char *dbfilename;
    int rdbcompression;
    char *appendfilename;
    char *logfile;
    int syslog_enabled;
    char *syslog_ident;
    int syslog_facility;
    int isslave;
    char *masterauth;
    char *masterhost;
    int masterport;
    redisClient *master;
    int repl_syncio_timeout;
    int replstate;
    off_t repl_transfer_left;
    int repl_transfer_s;
    int repl_transfer_fd;
    char *repl_transfer_tmpfile;
    time_t repl_transfer_lastio;
    int repl_serve_stale_data;
    time_t repl_down_since;
    unsigned int maxclients;
    unsigned long long maxmemory;
    int maxmemory_policy;
    int maxmemory_samples;
    unsigned int bpop_blocked_clients;
    list *unblocked_clients;
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    size_t hash_max_zipmap_entries;
    size_t hash_max_zipmap_value;
    size_t list_max_ziplist_entries;
    size_t list_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    time_t unixtime;
    dict *pubsub_channels;
    list *pubsub_patterns;
    unsigned lruclock:22;
    unsigned lruclock_padding:10;
    int cluster_enabled;
    clusterState cluster;
    lua_State *lua;
    redisClient *lua_client;
    dict *lua_scripts;
    long long lua_time_limit;
    long long lua_time_start;
};
typedef struct pubsubPattern {
    redisClient *client;
    robj *pattern;
} pubsubPattern;
typedef void redisCommandProc(redisClient *c);
typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, int *numkeys, int flags);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    int flags;
    redisGetKeysProc *getkeys_proc;
    int firstkey;
    int lastkey;
    int keystep;
    long long microseconds, calls;
};
struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};
typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;
typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction;
    unsigned char *zi;
    listNode *ln;
} listTypeIterator;
typedef struct {
    listTypeIterator *li;
    unsigned char *zi;
    listNode *ln;
} listTypeEntry;
typedef struct {
    robj *subject;
    int encoding;
    int ii;
    dictIterator *di;
} setTypeIterator;
typedef struct {
    int encoding;
    unsigned char *zi;
    unsigned char *zk, *zv;
    unsigned int zklen, zvlen;
    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;
#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2
extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType dbDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
dictType hashDictType;
long long ustime(void);
redisClient *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(redisClient *c);
void resetClient(redisClient *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void *addDeferredMultiBulkLength(redisClient *c);
void setDeferredMultiBulkLength(redisClient *c, void *node, long length);
void addReplySds(redisClient *c, sds s);
void processInputBuffer(redisClient *c);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReplyBulk(redisClient *c, robj *obj);
void addReplyBulkCString(redisClient *c, char *s);
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len);
void addReplyBulkLongLong(redisClient *c, long long ll);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void addReplySds(redisClient *c, sds s);
void addReplyError(redisClient *c, char *err);
void addReplyStatus(redisClient *c, char *status);
void addReplyDouble(redisClient *c, double d);
void addReplyLongLong(redisClient *c, long long ll);
void addReplyMultiBulkLen(redisClient *c, long length);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
void rewriteClientCommandVector(redisClient *c, int argc, ...);
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval);
#ifdef __GNUC__
void addReplyErrorFormat(redisClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(redisClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(redisClient *c, const char *fmt, ...);
void addReplyStatusFormat(redisClient *c, const char *fmt, ...);
#endif
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, int index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(redisClient *c);
int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele);
void popGenericCommand(redisClient *c, int where);
void unwatchAllKeys(redisClient *c);
void initClientMultiState(redisClient *c);
void freeClientMultiState(redisClient *c);
void queueMultiCommand(redisClient *c);
void touchWatchedKey(redisDb *db, robj *key);
void touchWatchedKeysOnFlush(int dbid);
void decrRefCount(void *o);
void incrRefCount(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(char *ptr, size_t len);
robj *dupStringObject(robj *o);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createListObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
int getLongFromObjectOrReply(redisClient *c, robj *o, long *target, const char *msg);
int checkType(redisClient *c, robj *o, int type);
int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg);
int getLongLongFromObject(robj *o, long long *target);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long estimateObjectIdleTime(robj *o);
int syncWrite(int fd, char *ptr, ssize_t size, int timeout);
int syncRead(int fd, char *ptr, ssize_t size, int timeout);
int syncReadLine(int fd, char *ptr, ssize_t size, int timeout);
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedMonitors(list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr);
void replicationCron(void);
void startLoading(FILE *fp);
void loadingProgress(off_t pos);
void stopLoading(void);
#include "rdb.h"
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
typedef struct {
    double min, max;
    int minex, maxex;
} zrangespec;
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score);
int zslDelete(zskiplist *zsl, double score, robj *obj);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned int zsetLength(robj *zobj);
void zsetConvert(robj *zobj, int encoding);
void freeMemoryIfNeeded(void);
int processCommand(redisClient *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(char *s);
void call(redisClient *c);
int prepareForShutdown();
void redisLog(int level, const char *fmt, ...);
void redisLogRaw(int level, const char *msg);
void usage();
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void oom(const char *msg);
void populateCommandTable(void);
void resetCommandTableStats(void);
robj *setTypeCreate(robj *value);
int setTypeAdd(robj *subject, robj *value);
int setTypeRemove(robj *subject, robj *value);
int setTypeIsMember(robj *subject, robj *value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele);
robj *setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele);
unsigned long setTypeSize(robj *subject);
void setTypeConvert(robj *subject, int enc);
void convertToRealHash(robj *o);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2);
int hashTypeGet(robj *o, robj *key, robj **objval, unsigned char **v, unsigned int *vlen);
robj *hashTypeGetObject(robj *o, robj *key);
int hashTypeExists(robj *o, robj *key);
int hashTypeSet(robj *o, robj *key, robj *value);
int hashTypeDelete(robj *o, robj *key);
unsigned long hashTypeLength(robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
int hashTypeCurrent(hashTypeIterator *hi, int what, robj **objval, unsigned char **v, unsigned int *vlen);
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key);
int pubsubUnsubscribeAllChannels(redisClient *c, int notify);
int pubsubUnsubscribeAllPatterns(redisClient *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
void loadServerConfig(char *filename);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams();
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
time_t getExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, time_t when);
robj *lookupKey(redisDb *db, robj *key);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply);
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
long long emptyDb();
int selectDb(redisClient *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int GetKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
#define REDIS_GETKEYS_ALL 0
#define REDIS_GETKEYS_PRELOAD 1
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys, int flags);
void getKeysFreeResult(int *result);
int *noPreloadGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags);
int *renameGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags);
int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags);
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterCron(void);
clusterNode *getNodeByQuery(redisClient *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
void scriptingInit(void);
char *redisGitSHA1(void);
char *redisGitDirty(void);
void authCommand(redisClient *c);
void pingCommand(redisClient *c);
void echoCommand(redisClient *c);
void setCommand(redisClient *c);
void setnxCommand(redisClient *c);
void setexCommand(redisClient *c);
void getCommand(redisClient *c);
void delCommand(redisClient *c);
void existsCommand(redisClient *c);
void setbitCommand(redisClient *c);
void getbitCommand(redisClient *c);
void setrangeCommand(redisClient *c);
void getrangeCommand(redisClient *c);
void incrCommand(redisClient *c);
void decrCommand(redisClient *c);
void incrbyCommand(redisClient *c);
void decrbyCommand(redisClient *c);
void selectCommand(redisClient *c);
void randomkeyCommand(redisClient *c);
void keysCommand(redisClient *c);
void dbsizeCommand(redisClient *c);
void lastsaveCommand(redisClient *c);
void saveCommand(redisClient *c);
void bgsaveCommand(redisClient *c);
void bgrewriteaofCommand(redisClient *c);
void shutdownCommand(redisClient *c);
void moveCommand(redisClient *c);
void renameCommand(redisClient *c);
void renamenxCommand(redisClient *c);
void lpushCommand(redisClient *c);
void rpushCommand(redisClient *c);
void lpushxCommand(redisClient *c);
void rpushxCommand(redisClient *c);
void linsertCommand(redisClient *c);
void lpopCommand(redisClient *c);
void rpopCommand(redisClient *c);
void llenCommand(redisClient *c);
void lindexCommand(redisClient *c);
void lrangeCommand(redisClient *c);
void ltrimCommand(redisClient *c);
void typeCommand(redisClient *c);
void lsetCommand(redisClient *c);
void saddCommand(redisClient *c);
void sremCommand(redisClient *c);
void smoveCommand(redisClient *c);
void sismemberCommand(redisClient *c);
void scardCommand(redisClient *c);
void spopCommand(redisClient *c);
void srandmemberCommand(redisClient *c);
void sinterCommand(redisClient *c);
void sinterstoreCommand(redisClient *c);
void sunionCommand(redisClient *c);
void sunionstoreCommand(redisClient *c);
void sdiffCommand(redisClient *c);
void sdiffstoreCommand(redisClient *c);
void syncCommand(redisClient *c);
void flushdbCommand(redisClient *c);
void flushallCommand(redisClient *c);
void sortCommand(redisClient *c);
void lremCommand(redisClient *c);
void rpoplpushCommand(redisClient *c);
void infoCommand(redisClient *c);
void mgetCommand(redisClient *c);
void monitorCommand(redisClient *c);
void expireCommand(redisClient *c);
void expireatCommand(redisClient *c);
void getsetCommand(redisClient *c);
void ttlCommand(redisClient *c);
void persistCommand(redisClient *c);
void slaveofCommand(redisClient *c);
void debugCommand(redisClient *c);
void msetCommand(redisClient *c);
void msetnxCommand(redisClient *c);
void zaddCommand(redisClient *c);
void zincrbyCommand(redisClient *c);
void zrangeCommand(redisClient *c);
void zrangebyscoreCommand(redisClient *c);
void zrevrangebyscoreCommand(redisClient *c);
void zcountCommand(redisClient *c);
void zrevrangeCommand(redisClient *c);
void zcardCommand(redisClient *c);
void zremCommand(redisClient *c);
void zscoreCommand(redisClient *c);
void zremrangebyscoreCommand(redisClient *c);
void multiCommand(redisClient *c);
void execCommand(redisClient *c);
void discardCommand(redisClient *c);
void blpopCommand(redisClient *c);
void brpopCommand(redisClient *c);
void brpoplpushCommand(redisClient *c);
void appendCommand(redisClient *c);
void strlenCommand(redisClient *c);
void zrankCommand(redisClient *c);
void zrevrankCommand(redisClient *c);
void hsetCommand(redisClient *c);
void hsetnxCommand(redisClient *c);
void hgetCommand(redisClient *c);
void hmsetCommand(redisClient *c);
void hmgetCommand(redisClient *c);
void hdelCommand(redisClient *c);
void hlenCommand(redisClient *c);
void zremrangebyrankCommand(redisClient *c);
void zunionstoreCommand(redisClient *c);
void zinterstoreCommand(redisClient *c);
void hkeysCommand(redisClient *c);
void hvalsCommand(redisClient *c);
void hgetallCommand(redisClient *c);
void hexistsCommand(redisClient *c);
void configCommand(redisClient *c);
void hincrbyCommand(redisClient *c);
void subscribeCommand(redisClient *c);
void unsubscribeCommand(redisClient *c);
void psubscribeCommand(redisClient *c);
void punsubscribeCommand(redisClient *c);
void publishCommand(redisClient *c);
void watchCommand(redisClient *c);
void unwatchCommand(redisClient *c);
void clusterCommand(redisClient *c);
void restoreCommand(redisClient *c);
void migrateCommand(redisClient *c);
void dumpCommand(redisClient *c);
void objectCommand(redisClient *c);
void clientCommand(redisClient *c);
void evalCommand(redisClient *c);
void evalShaCommand(redisClient *c);
#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif
#endif
