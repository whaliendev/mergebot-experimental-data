#define REDIS_VERSION "2.1.1"
#include "fmacros.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#if defined(__sun)
#include "solarisfixes.h"
#endif
#include "redis.h"
#include "ae.h"
#include "sds.h"
#include "anet.h"
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"
#include "lzf.h"
#include "pqsort.h"
#include "zipmap.h"
#include "ziplist.h"
#include "sha1.h"
#include "release.h"
#define REDIS_OK 0
#define REDIS_ERR -1
#define REDIS_SERVERPORT 6379
#define REDIS_MAXIDLETIME (60*5)
#define REDIS_IOBUF_LEN 1024
#define REDIS_LOADBUF_LEN 1024
#define REDIS_STATIC_ARGS 8
#define REDIS_DEFAULT_DBNUM 16
#define REDIS_CONFIGLINE_MAX 1024
#define REDIS_OBJFREELIST_MAX 1000000
#define REDIS_MAX_SYNC_TIME 60
#define REDIS_EXPIRELOOKUPS_PER_CRON 10
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256)
#define REDIS_WRITEV_THRESHOLD 3
#define REDIS_WRITEV_IOVEC_COUNT 256
#define REDIS_HT_MINFILL 10
#define REDIS_CMD_BULK 1
#define REDIS_CMD_INLINE 2
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
#define REDIS_ENCODING_LIST 4
#define REDIS_ENCODING_ZIPLIST 5
static char* strencoding[] = {
    "raw", "int", "zipmap", "hashtable"
};
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
#define REDIS_VM_MEMORY 0
#define REDIS_VM_SWAPPED 1
#define REDIS_VM_SWAPPING 2
#define REDIS_VM_LOADING 3
#define REDIS_VM_MAX_NEAR_PAGES 65536
#define REDIS_VM_MAX_RANDOM_JUMP 4096
#define REDIS_VM_MAX_THREADS 32
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1
#define REDIS_SLAVE 1
#define REDIS_MASTER 2
#define REDIS_MONITOR 4
#define REDIS_MULTI 8
#define REDIS_BLOCKED 16
#define REDIS_IO_WAIT 32
#define REDIS_DIRTY_CAS 64
#define REDIS_REPL_NONE 0
#define REDIS_REPL_CONNECT 1
#define REDIS_REPL_CONNECTED 2
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
#define REDIS_NOTUSED(V) ((void) V)
#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25
#define APPENDFSYNC_NO 0
#define APPENDFSYNC_ALWAYS 1
#define APPENDFSYNC_EVERYSEC 2
#define REDIS_HASH_MAX_ZIPMAP_ENTRIES 64
#define REDIS_HASH_MAX_ZIPMAP_VALUE 512
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define redisPanic(_e) _redisPanic(#_e,__FILE__,__LINE__),_exit(1)
static void _redisAssert(char *estr, char *file, int line);
static void _redisPanic(char *msg, char *file, int line);
typedef struct redisObject {
    unsigned type:4;
    unsigned storage:2;
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
    _var.storage = REDIS_VM_MEMORY; \
} while(0);
typedef struct redisDb {
    dict *dict;
    dict *expires;
    dict *blocking_keys;
    dict *io_keys;
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
typedef struct redisClient {
    int fd;
    redisDb *db;
    int dictid;
    sds querybuf;
    robj **argv, **mbargv;
    int argc, mbargc;
    int bulklen;
    int multibulk;
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
    robj **blocking_keys;
    int blocking_keys_num;
    time_t blockingto;
    list *io_keys;
    list *watched_keys;
    dict *pubsub_channels;
    list *pubsub_patterns;
} redisClient;
struct saveparam {
    time_t seconds;
    int changes;
};
struct redisServer {
    int port;
    int fd;
    redisDb *db;
    long long dirty;
    list *clients;
    list *slaves, *monitors;
    char neterr[ANET_ERR_LEN];
    aeEventLoop *el;
    int cronloops;
    list *objfreelist;
    time_t lastsave;
    time_t stat_starttime;
    long long stat_numcommands;
    long long stat_numconnections;
    long long stat_expiredkeys;
    int verbosity;
    int glueoutputbuf;
    int maxidletime;
    int dbnum;
    int daemonize;
    int appendonly;
    int appendfsync;
    int no_appendfsync_on_rewrite;
    int shutdown_asap;
    time_t lastfsync;
    int appendfd;
    int appendseldb;
    char *pidfile;
    pid_t bgsavechildpid;
    pid_t bgrewritechildpid;
    sds bgrewritebuf;
    sds aofbuf;
    struct saveparam *saveparams;
    int saveparamslen;
    char *logfile;
    char *bindaddr;
    char *dbfilename;
    char *appendfilename;
    char *requirepass;
    int rdbcompression;
    int activerehashing;
    int isslave;
    char *masterauth;
    char *masterhost;
    int masterport;
    redisClient *master;
    int replstate;
    unsigned int maxclients;
    unsigned long long maxmemory;
    unsigned int blpop_blocked_clients;
    unsigned int vm_blocked_clients;
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int vm_enabled;
    char *vm_swap_file;
    off_t vm_page_size;
    off_t vm_pages;
    unsigned long long vm_max_memory;
    size_t hash_max_zipmap_entries;
    size_t hash_max_zipmap_value;
    FILE *vm_fp;
    int vm_fd;
    off_t vm_next_page;
    off_t vm_near_pages;
    unsigned char *vm_bitmap;
    time_t unixtime;
    list *io_newjobs;
    list *io_processing;
    list *io_processed;
    list *io_ready_clients;
    pthread_mutex_t io_mutex;
    pthread_mutex_t obj_freelist_mutex;
    pthread_mutex_t io_swapfile_mutex;
    pthread_attr_t io_threads_attr;
    int io_active_threads;
    int vm_max_threads;
    int io_ready_pipe_read;
    int io_ready_pipe_write;
    unsigned long long vm_stats_used_pages;
    unsigned long long vm_stats_swapped_objects;
    unsigned long long vm_stats_swapouts;
    unsigned long long vm_stats_swapins;
    dict *pubsub_channels;
    list *pubsub_patterns;
    FILE *devnull;
    unsigned lruclock:22;
    unsigned lruclock_padding:10;
};
typedef struct pubsubPattern {
    redisClient *client;
    robj *pattern;
} pubsubPattern;
typedef void redisCommandProc(redisClient *c);
typedef void redisVmPreloadProc(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    int flags;
    redisVmPreloadProc *vm_preload_proc;
    int vm_firstkey;
    int vm_lastkey;
    int vm_keystep;
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
typedef struct zskiplistNode {
    struct zskiplistNode **forward;
    struct zskiplistNode *backward;
    unsigned int *span;
    double score;
    robj *obj;
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
#define REDIS_SHARED_INTEGERS 10000
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9,
    *messagebulk, *pmessagebulk, *subscribebulk, *unsubscribebulk, *mbulk3,
    *mbulk4, *psubscribebulk, *punsubscribebulk,
    *integers[REDIS_SHARED_INTEGERS];
} shared;
static double R_Zero, R_PosInf, R_NegInf, R_Nan;
#define REDIS_IOJOB_LOAD 0
#define REDIS_IOJOB_PREPARE_SWAP 1
#define REDIS_IOJOB_DO_SWAP 2
typedef struct iojob {
    int type;
    redisDb *db;
    robj *key;
    robj *id;
    robj *val;
    off_t page;
    off_t pages;
    int canceled;
    pthread_t thread;
} iojob;
static void freeStringObject(robj *o);
static void freeListObject(robj *o);
static void freeSetObject(robj *o);
static void decrRefCount(void *o);
static robj *createObject(int type, void *ptr);
static void freeClient(redisClient *c);
static int rdbLoad(char *filename);
static void addReply(redisClient *c, robj *obj);
static void addReplySds(redisClient *c, sds s);
static void incrRefCount(robj *o);
static int rdbSaveBackground(char *filename);
static robj *createStringObject(char *ptr, size_t len);
static robj *dupStringObject(robj *o);
static void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
static void replicationFeedMonitors(list *monitors, int dictid, robj **argv, int argc);
static void flushAppendOnlyFile(void);
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
static int syncWithMaster(void);
static robj *tryObjectEncoding(robj *o);
static robj *getDecodedObject(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteIfVolatile(redisDb *db, robj *key);
static int dbDelete(redisDb *db, robj *key);
static time_t getExpire(redisDb *db, robj *key);
static int setExpire(redisDb *db, robj *key, time_t when);
static void updateSlavesWaitingBgsave(int bgsaveerr);
static void freeMemoryIfNeeded(void);
static int processCommand(redisClient *c);
static void setupSigSegvAction(void);
static void rdbRemoveTempFile(pid_t childpid);
static void aofRemoveTempFile(pid_t childpid);
static size_t stringObjectLen(robj *o);
static void processInputBuffer(redisClient *c);
static zskiplist *zslCreate(void);
static void zslFree(zskiplist *zsl);
static void zslInsert(zskiplist *zsl, double score, robj *obj);
static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask);
static void initClientMultiState(redisClient *c);
static void freeClientMultiState(redisClient *c);
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd);
static void unblockClientWaitingData(redisClient *c);
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele);
static void vmInit(void);
static void vmMarkPagesFree(off_t page, off_t count);
static robj *vmLoadObject(robj *o);
static robj *vmPreviewObject(robj *o);
static int vmSwapOneObjectBlocking(void);
static int vmSwapOneObjectThreaded(void);
static int vmCanSwapOut(void);
static int tryFreeOneObjectFromFreelist(void);
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata, int mask);
static void vmCancelThreadedIOJob(robj *o);
static void lockThreadedIO(void);
static void unlockThreadedIO(void);
static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db);
static void freeIOJob(iojob *j);
static void queueIOJob(iojob *j);
static int vmWriteObjectOnSwap(robj *o, off_t page);
static robj *vmReadObjectFromSwap(off_t page, int type);
static void waitEmptyIOJobsQueue(void);
static void vmReopenSwapFile(void);
static int vmFreePage(off_t page);
static void zunionInterBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
static void execBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv);
static int blockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd);
static int dontWaitForSwappedKey(redisClient *c, robj *key);
static void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key);
static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
static struct redisCommand *lookupCommand(char *name);
static void call(redisClient *c, struct redisCommand *cmd);
static void resetClient(redisClient *c);
static void convertToRealHash(robj *o);
static int pubsubUnsubscribeAllChannels(redisClient *c, int notify);
static int pubsubUnsubscribeAllPatterns(redisClient *c, int notify);
static void freePubsubPattern(void *p);
static int listMatchPubsubPattern(void *a, void *b);
static int compareStringObjects(robj *a, robj *b);
static int equalStringObjects(robj *a, robj *b);
static void usage();
static int rewriteAppendOnlyFileBackground(void);
static vmpointer *vmSwapObjectBlocking(robj *val);
static int prepareForShutdown();
static void touchWatchedKey(redisDb *db, robj *key);
static void touchWatchedKeysOnFlush(int dbid);
static void unwatchAllKeys(redisClient *c);
static void authCommand(redisClient *c);
static void pingCommand(redisClient *c);
static void echoCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
static void setexCommand(redisClient *c);
static void getCommand(redisClient *c);
static void delCommand(redisClient *c);
static void existsCommand(redisClient *c);
static void incrCommand(redisClient *c);
static void decrCommand(redisClient *c);
static void incrbyCommand(redisClient *c);
static void decrbyCommand(redisClient *c);
static void selectCommand(redisClient *c);
static void randomkeyCommand(redisClient *c);
static void keysCommand(redisClient *c);
static void dbsizeCommand(redisClient *c);
static void lastsaveCommand(redisClient *c);
static void saveCommand(redisClient *c);
static void bgsaveCommand(redisClient *c);
static void bgrewriteaofCommand(redisClient *c);
static void shutdownCommand(redisClient *c);
static void moveCommand(redisClient *c);
static void renameCommand(redisClient *c);
static void renamenxCommand(redisClient *c);
static void lpushCommand(redisClient *c);
static void rpushCommand(redisClient *c);
static void lpopCommand(redisClient *c);
static void rpopCommand(redisClient *c);
static void llenCommand(redisClient *c);
static void lindexCommand(redisClient *c);
static void lrangeCommand(redisClient *c);
static void ltrimCommand(redisClient *c);
static void typeCommand(redisClient *c);
static void lsetCommand(redisClient *c);
static void saddCommand(redisClient *c);
static void sremCommand(redisClient *c);
static void smoveCommand(redisClient *c);
static void sismemberCommand(redisClient *c);
static void scardCommand(redisClient *c);
static void spopCommand(redisClient *c);
static void srandmemberCommand(redisClient *c);
static void sinterCommand(redisClient *c);
static void sinterstoreCommand(redisClient *c);
static void sunionCommand(redisClient *c);
static void sunionstoreCommand(redisClient *c);
static void sdiffCommand(redisClient *c);
static void sdiffstoreCommand(redisClient *c);
static void syncCommand(redisClient *c);
static void flushdbCommand(redisClient *c);
static void flushallCommand(redisClient *c);
static void sortCommand(redisClient *c);
static void lremCommand(redisClient *c);
static void rpoplpushcommand(redisClient *c);
static void infoCommand(redisClient *c);
static void mgetCommand(redisClient *c);
static void monitorCommand(redisClient *c);
static void expireCommand(redisClient *c);
static void expireatCommand(redisClient *c);
static void getsetCommand(redisClient *c);
static void ttlCommand(redisClient *c);
static void slaveofCommand(redisClient *c);
static void debugCommand(redisClient *c);
static void msetCommand(redisClient *c);
static void msetnxCommand(redisClient *c);
static void zaddCommand(redisClient *c);
static void zincrbyCommand(redisClient *c);
static void zrangeCommand(redisClient *c);
static void zrangebyscoreCommand(redisClient *c);
static void zcountCommand(redisClient *c);
static void zrevrangeCommand(redisClient *c);
static void zcardCommand(redisClient *c);
static void zremCommand(redisClient *c);
static void zscoreCommand(redisClient *c);
static void zremrangebyscoreCommand(redisClient *c);
static void multiCommand(redisClient *c);
static void execCommand(redisClient *c);
static void discardCommand(redisClient *c);
static void blpopCommand(redisClient *c);
static void brpopCommand(redisClient *c);
static void appendCommand(redisClient *c);
static void substrCommand(redisClient *c);
static void zrankCommand(redisClient *c);
static void zrevrankCommand(redisClient *c);
static void hsetCommand(redisClient *c);
static void hsetnxCommand(redisClient *c);
static void hgetCommand(redisClient *c);
static void hmsetCommand(redisClient *c);
static void hmgetCommand(redisClient *c);
static void hdelCommand(redisClient *c);
static void hlenCommand(redisClient *c);
static void zremrangebyrankCommand(redisClient *c);
static void zunionstoreCommand(redisClient *c);
static void zinterstoreCommand(redisClient *c);
static void hkeysCommand(redisClient *c);
static void hvalsCommand(redisClient *c);
static void hgetallCommand(redisClient *c);
static void hexistsCommand(redisClient *c);
static void configCommand(redisClient *c);
static void hincrbyCommand(redisClient *c);
static void subscribeCommand(redisClient *c);
static void unsubscribeCommand(redisClient *c);
static void psubscribeCommand(redisClient *c);
static void punsubscribeCommand(redisClient *c);
static void publishCommand(redisClient *c);
static void watchCommand(redisClient *c);
static void unwatchCommand(redisClient *c);
static struct redisServer server;
static struct redisCommand *commandTable;
static struct redisCommand readonlyCommandTable[] = {
    {"get",getCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"set",setCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setnx",setnxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"setex",setexCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,0,0,0},
    {"append",appendCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"substr",substrCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"del",delCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"exists",existsCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"incr",incrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"decr",decrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"mget",mgetCommand,-2,REDIS_CMD_INLINE,NULL,1,-1,1},
    {"rpush",rpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lpush",lpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"rpop",rpopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lpop",lpopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"brpop",brpopCommand,-3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"blpop",blpopCommand,-3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"llen",llenCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lindex",lindexCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lset",lsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"lrange",lrangeCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"ltrim",ltrimCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"lrem",lremCommand,4,REDIS_CMD_BULK,NULL,1,1,1},
    {"rpoplpush",rpoplpushcommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,2,1},
    {"sadd",saddCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"srem",sremCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"smove",smoveCommand,4,REDIS_CMD_BULK,NULL,1,2,1},
    {"sismember",sismemberCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"scard",scardCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"spop",spopCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"srandmember",srandmemberCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"sinter",sinterCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sinterstore",sinterstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"sunion",sunionCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sunionstore",sunionstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"sdiff",sdiffCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,-1,1},
    {"sdiffstore",sdiffstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,2,-1,1},
    {"smembers",sinterCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zadd",zaddCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zincrby",zincrbyCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zrem",zremCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"zremrangebyscore",zremrangebyscoreCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zremrangebyrank",zremrangebyrankCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zunionstore",zunionstoreCommand,-4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,zunionInterBlockClientOnSwappedKeys,0,0,0},
    {"zinterstore",zinterstoreCommand,-4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,zunionInterBlockClientOnSwappedKeys,0,0,0},
    {"zrange",zrangeCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zrangebyscore",zrangebyscoreCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zcount",zcountCommand,4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zrevrange",zrevrangeCommand,-4,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zcard",zcardCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"zscore",zscoreCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"zrank",zrankCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"zrevrank",zrevrankCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hset",hsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hsetnx",hsetnxCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hget",hgetCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hmset",hmsetCommand,-4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hmget",hmgetCommand,-3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hincrby",hincrbyCommand,4,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"hdel",hdelCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"hlen",hlenCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hkeys",hkeysCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hvals",hvalsCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hgetall",hgetallCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"hexists",hexistsCommand,3,REDIS_CMD_BULK,NULL,1,1,1},
    {"incrby",incrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"decrby",decrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"getset",getsetCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"mset",msetCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,-1,2},
    {"msetnx",msetnxCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,NULL,1,-1,2},
    {"randomkey",randomkeyCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"select",selectCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"move",moveCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"rename",renameCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"renamenx",renamenxCommand,3,REDIS_CMD_INLINE,NULL,1,1,1},
    {"expire",expireCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"expireat",expireatCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"keys",keysCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"dbsize",dbsizeCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"auth",authCommand,2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"ping",pingCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"echo",echoCommand,2,REDIS_CMD_BULK,NULL,0,0,0},
    {"save",saveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"bgsave",bgsaveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"shutdown",shutdownCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"lastsave",lastsaveCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"type",typeCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"multi",multiCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"exec",execCommand,1,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,execBlockClientOnSwappedKeys,0,0,0},
    {"discard",discardCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"sync",syncCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"flushdb",flushdbCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"flushall",flushallCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"sort",sortCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,NULL,1,1,1},
    {"info",infoCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"monitor",monitorCommand,1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"ttl",ttlCommand,2,REDIS_CMD_INLINE,NULL,1,1,1},
    {"slaveof",slaveofCommand,3,REDIS_CMD_INLINE,NULL,0,0,0},
    {"debug",debugCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"config",configCommand,-2,REDIS_CMD_BULK,NULL,0,0,0},
    {"subscribe",subscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"psubscribe",psubscribeCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,REDIS_CMD_INLINE,NULL,0,0,0},
    {"publish",publishCommand,3,REDIS_CMD_BULK|REDIS_CMD_FORCE_REPLICATION,NULL,0,0,0},
    {"watch",watchCommand,-2,REDIS_CMD_INLINE,NULL,0,0,0},
    {"unwatch",unwatchCommand,1,REDIS_CMD_INLINE,NULL,0,0,0}
};
static int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase)
{
    while(patternLen) {
        switch(pattern[0]) {
        case '*':
            while (pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1;
            while(stringLen) {
                if (stringmatchlen(pattern+1, patternLen-1,
                            string, stringLen, nocase))
                    return 1;
                string++;
                stringLen--;
            }
            return 0;
            break;
        case '?':
            if (stringLen == 0)
                return 0;
            string++;
            stringLen--;
            break;
        case '[':
        {
            int not, match;
            pattern++;
            patternLen--;
            not = pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            match = 0;
            while(1) {
                if (pattern[0] == '\\') {
                    pattern++;
                    patternLen--;
                    if (pattern[0] == string[0])
                        match = 1;
                } else if (pattern[0] == ']') {
                    break;
                } else if (patternLen == 0) {
                    pattern--;
                    patternLen++;
                    break;
                } else if (pattern[1] == '-' && patternLen >= 3) {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end) {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                } else {
                    if (!nocase) {
                        if (pattern[0] == string[0])
                            match = 1;
                    } else {
                        if (tolower((int)pattern[0]) == tolower((int)string[0]))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not)
                match = !match;
            if (!match)
                return 0;
            string++;
            stringLen--;
            break;
        }
        case '\\':
            if (patternLen >= 2) {
                pattern++;
                patternLen--;
            }
        default:
            if (!nocase) {
                if (pattern[0] != string[0])
                    return 0;
            } else {
                if (tolower((int)pattern[0]) != tolower((int)string[0]))
                    return 0;
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        if (stringLen == 0) {
            while(*pattern == '*') {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}
static int stringmatch(const char *pattern, const char *string, int nocase) {
    return stringmatchlen(pattern,strlen(pattern),string,strlen(string),nocase);
}
static long long memtoll(const char *p, int *err) {
    const char *u;
    char buf[128];
    long mul;
    long long val;
    unsigned int digits;
    if (err) *err = 0;
    u = p;
    if (*u == '-') u++;
    while(*u && isdigit(*u)) u++;
    if (*u == '\0' || !strcasecmp(u,"b")) {
        mul = 1;
    } else if (!strcasecmp(u,"k")) {
        mul = 1000;
    } else if (!strcasecmp(u,"kb")) {
        mul = 1024;
    } else if (!strcasecmp(u,"m")) {
        mul = 1000*1000;
    } else if (!strcasecmp(u,"mb")) {
        mul = 1024*1024;
    } else if (!strcasecmp(u,"g")) {
        mul = 1000L*1000*1000;
    } else if (!strcasecmp(u,"gb")) {
        mul = 1024L*1024*1024;
    } else {
        if (err) *err = 1;
        mul = 1;
    }
    digits = u-p;
    if (digits >= sizeof(buf)) {
        if (err) *err = 1;
        return LLONG_MAX;
    }
    memcpy(buf,p,digits);
    buf[digits] = '\0';
    val = strtoll(buf,NULL,10);
    return val*mul;
}
static int ll2string(char *s, size_t len, long long value) {
    char buf[32], *p;
    unsigned long long v;
    size_t l;
    if (len == 0) return 0;
    v = (value < 0) ? -value : value;
    p = buf+31;
    do {
        *p-- = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p-- = '-';
    p++;
    l = 32-(p-buf);
    if (l+1 > len) l = len-1;
    memcpy(s,p,l);
    s[l] = '\0';
    return l;
}
static void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;
    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;
    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*#";
        char buf[64];
        time_t now;
        now = time(NULL);
        strftime(buf,64,"%d %b %H:%M:%S",localtime(&now));
        fprintf(fp,"[%d] %s %c ",(int)getpid(),buf,c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp,"\n");
        fflush(fp);
    }
    va_end(ap);
    if (server.logfile) fclose(fp);
}
static void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}
static void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}
static int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}
static void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    if (val == NULL) return;
    decrRefCount(val);
}
static void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    sdsfree(val);
}
static int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}
static unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}
static unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}
static int dictEncObjKeyCompare(void *privdata, const void *key1,
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
static unsigned int dictEncObjHash(const void *key) {
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
static dictType setDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    NULL
};
static dictType zsetDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    dictVanillaFree
};
static dictType dbDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    dictRedisObjectDestructor
};
static dictType keyptrDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    NULL
};
static dictType hashDictType = {
    dictEncObjHash,
    NULL,
    NULL,
    dictEncObjKeyCompare,
    dictRedisObjectDestructor,
    dictRedisObjectDestructor
};
static dictType keylistDictType = {
    dictObjHash,
    NULL,
    NULL,
    dictObjKeyCompare,
    dictRedisObjectDestructor,
    dictListDestructor
};
static void version();
static void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}
static void closeTimedoutClients(void) {
    redisClient *c;
    listNode *ln;
    time_t now = time(NULL);
    listIter li;
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);
        if (server.maxidletime &&
            !(c->flags & REDIS_SLAVE) &&
            !(c->flags & REDIS_MASTER) &&
            dictSize(c->pubsub_channels) == 0 &&
            listLength(c->pubsub_patterns) == 0 &&
            (now - c->lastinteraction > server.maxidletime))
        {
            redisLog(REDIS_VERBOSE,"Closing idle client");
            freeClient(c);
        } else if (c->flags & REDIS_BLOCKED) {
            if (c->blockingto != 0 && c->blockingto < now) {
                addReply(c,shared.nullmultibulk);
                unblockClientWaitingData(c);
            }
        }
    }
}
static int htNeedsResize(dict *dict) {
    long long size, used;
    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}
static void tryResizeHashTables(void) {
    int j;
    for (j = 0; j < server.dbnum; j++) {
        if (htNeedsResize(server.db[j].dict))
            dictResize(server.db[j].dict);
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
    }
}
static void incrementallyRehash(void) {
    int j;
    for (j = 0; j < server.dbnum; j++) {
        if (dictIsRehashing(server.db[j].dict)) {
            dictRehashMilliseconds(server.db[j].dict,1);
            break;
        }
    }
}
void backgroundSaveDoneHandler(int statloc) {
    int exitcode = WEXITSTATUS(statloc);
    int bysignal = WIFSIGNALED(statloc);
    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background saving terminated with success");
        server.dirty = 0;
        server.lastsave = time(NULL);
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background saving error");
    } else {
        redisLog(REDIS_WARNING,
            "Background saving terminated by signal %d", WTERMSIG(statloc));
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    server.bgsavechildpid = -1;
    updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}
void backgroundRewriteDoneHandler(int statloc) {
    int exitcode = WEXITSTATUS(statloc);
    int bysignal = WIFSIGNALED(statloc);
    if (!bysignal && exitcode == 0) {
        int fd;
        char tmpfile[256];
        redisLog(REDIS_NOTICE,
            "Background append only file rewriting terminated with success");
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) server.bgrewritechildpid);
        fd = open(tmpfile,O_WRONLY|O_APPEND);
        if (fd == -1) {
            redisLog(REDIS_WARNING, "Not able to open the temp append only file produced by the child: %s", strerror(errno));
            goto cleanup;
        }
        if (write(fd,server.bgrewritebuf,sdslen(server.bgrewritebuf)) !=
                (signed) sdslen(server.bgrewritebuf)) {
            redisLog(REDIS_WARNING, "Error or short write trying to flush the parent diff of the append log file in the child temp file: %s", strerror(errno));
            close(fd);
            goto cleanup;
        }
        redisLog(REDIS_NOTICE,"Parent diff flushed into the new append log file with success (%lu bytes)",sdslen(server.bgrewritebuf));
        if (rename(tmpfile,server.appendfilename) == -1) {
            redisLog(REDIS_WARNING,"Can't rename the temp append only file into the stable one: %s", strerror(errno));
            close(fd);
            goto cleanup;
        }
        redisLog(REDIS_NOTICE,"Append only file successfully rewritten.");
        if (server.appendfd != -1) {
            close(server.appendfd);
            server.appendfd = fd;
            if (server.appendfsync != APPENDFSYNC_NO) aof_fsync(fd);
            server.appendseldb = -1;
            redisLog(REDIS_NOTICE,"The new append only file was selected for future appends.");
        } else {
            close(fd);
        }
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background append only file rewriting error");
    } else {
        redisLog(REDIS_WARNING,
            "Background append only file rewriting terminated by signal %d",
            WTERMSIG(statloc));
    }
cleanup:
    sdsfree(server.bgrewritebuf);
    server.bgrewritebuf = sdsempty();
    aofRemoveTempFile(server.bgrewritechildpid);
    server.bgrewritechildpid = -1;
}
static void updateDictResizePolicy(void) {
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}
static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);
    server.unixtime = time(NULL);
    server.lruclock = (time(NULL)/60)&((1<<21)-1);
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
    if ((server.maxidletime && !(loops % 100)) || server.blpop_blocked_clients)
        closeTimedoutClients();
    if (server.bgsavechildpid != -1 || server.bgrewritechildpid != -1) {
        int statloc;
        pid_t pid;
        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            if (pid == server.bgsavechildpid) {
                backgroundSaveDoneHandler(statloc);
            } else {
                backgroundRewriteDoneHandler(statloc);
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
    }
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
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    server.stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }
    if (vmCanSwapOut()) {
        while (server.vm_enabled && zmalloc_used_memory() >
                server.vm_max_memory)
        {
            int retval;
            if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
            retval = (server.vm_max_threads == 0) ?
                        vmSwapOneObjectBlocking() :
                        vmSwapOneObjectThreaded();
            if (retval == REDIS_ERR && !(loops % 300) &&
                zmalloc_used_memory() >
                (server.vm_max_memory+server.vm_max_memory/10))
            {
                redisLog(REDIS_WARNING,"WARNING: vm-max-memory limit exceeded by more than 10%% but unable to swap more objects out!");
            }
            if (retval == REDIS_ERR || server.vm_max_threads > 0) break;
        }
    }
    if (server.replstate == REDIS_REPL_CONNECT && !(loops % 10)) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER...");
        if (syncWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync succeeded");
            if (server.appendonly) rewriteAppendOnlyFileBackground();
        }
    }
    return 100;
}
static void beforeSleep(struct aeEventLoop *eventLoop) {
    REDIS_NOTUSED(eventLoop);
    if (server.vm_enabled && listLength(server.io_ready_clients)) {
        listIter li;
        listNode *ln;
        listRewind(server.io_ready_clients,&li);
        while((ln = listNext(&li))) {
            redisClient *c = ln->value;
            struct redisCommand *cmd;
            listDelNode(server.io_ready_clients,ln);
            c->flags &= (~REDIS_IO_WAIT);
            server.vm_blocked_clients--;
            aeCreateFileEvent(server.el, c->fd, AE_READABLE,
                readQueryFromClient, c);
            cmd = lookupCommand(c->argv[0]->ptr);
            assert(cmd != NULL);
            call(c,cmd);
            resetClient(c);
            if (c->querybuf && sdslen(c->querybuf) > 0)
                processInputBuffer(c);
        }
    }
    flushAppendOnlyFile();
}
static void createSharedObjects(void) {
    int j;
    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
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
static void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}
static void resetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}
static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_VERBOSE;
    server.maxidletime = REDIS_MAXIDLETIME;
    server.saveparams = NULL;
    server.logfile = NULL;
    server.bindaddr = NULL;
    server.glueoutputbuf = 1;
    server.daemonize = 0;
    server.appendonly = 0;
    server.appendfsync = APPENDFSYNC_EVERYSEC;
    server.no_appendfsync_on_rewrite = 0;
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
    server.blpop_blocked_clients = 0;
    server.maxmemory = 0;
    server.vm_enabled = 0;
    server.vm_swap_file = zstrdup("/tmp/redis-%p.vm");
    server.vm_page_size = 256;
    server.vm_pages = 1024*1024*100;
    server.vm_max_memory = 1024LL*1024*1024*1;
    server.vm_max_threads = 4;
    server.vm_blocked_clients = 0;
    server.hash_max_zipmap_entries = REDIS_HASH_MAX_ZIPMAP_ENTRIES;
    server.hash_max_zipmap_value = REDIS_HASH_MAX_ZIPMAP_VALUE;
    server.shutdown_asap = 0;
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
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;
}
static void initServer() {
    int j;
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSigSegvAction();
    server.devnull = fopen("/dev/null","w");
    if (server.devnull == NULL) {
        redisLog(REDIS_WARNING, "Can't open /dev/null: %s", server.neterr);
        exit(1);
    }
    server.clients = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.objfreelist = listCreate();
    createSharedObjects();
    server.el = aeCreateEventLoop();
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        if (server.vm_enabled)
            server.db[j].io_keys = dictCreate(&keylistDictType,NULL);
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
    server.stat_starttime = time(NULL);
    server.unixtime = time(NULL);
    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
        acceptHandler, NULL) == AE_ERR) oom("creating file event");
    if (server.appendonly) {
        server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.appendfd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }
    if (server.vm_enabled) vmInit();
}
static long long emptyDb() {
    int j;
    long long removed = 0;
    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict);
        dictEmpty(server.db[j].expires);
    }
    return removed;
}
static int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}
static void loadServerConfig(char *filename) {
    FILE *fp;
    char buf[REDIS_CONFIGLINE_MAX+1], *err = NULL;
    int linenum = 0;
    sds line = NULL;
    if (filename[0] == '-' && filename[1] == '\0')
        fp = stdin;
    else {
        if ((fp = fopen(filename,"r")) == NULL) {
            redisLog(REDIS_WARNING, "Fatal error, can't open config file '%s'", filename);
            exit(1);
        }
    }
    while(fgets(buf,REDIS_CONFIGLINE_MAX+1,fp) != NULL) {
        sds *argv;
        int argc, j;
        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line," \t\r\n");
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }
        argv = sdssplitlen(line,sdslen(line)," ",1,&argc);
        sdstolower(argv[0]);
        if (!strcasecmp(argv[0],"timeout") && argc == 2) {
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"port") && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"bind") && argc == 2) {
            server.bindaddr = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"save") && argc == 3) {
            int seconds = atoi(argv[1]);
            int changes = atoi(argv[2]);
            if (seconds < 1 || changes < 0) {
                err = "Invalid save parameters"; goto loaderr;
            }
            appendServerSaveParams(seconds,changes);
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"loglevel") && argc == 2) {
            if (!strcasecmp(argv[1],"debug")) server.verbosity = REDIS_DEBUG;
            else if (!strcasecmp(argv[1],"verbose")) server.verbosity = REDIS_VERBOSE;
            else if (!strcasecmp(argv[1],"notice")) server.verbosity = REDIS_NOTICE;
            else if (!strcasecmp(argv[1],"warning")) server.verbosity = REDIS_WARNING;
            else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;
            server.logfile = zstrdup(argv[1]);
            if (!strcasecmp(server.logfile,"stdout")) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile) {
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"databases") && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"include") && argc == 2) {
            loadServerConfig(argv[1]);
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"slaveof") && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.replstate = REDIS_REPL_CONNECT;
        } else if (!strcasecmp(argv[0],"masterauth") && argc == 2) {
         server.masterauth = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"glueoutputbuf") && argc == 2) {
            if ((server.glueoutputbuf = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"rdbcompression") && argc == 2) {
            if ((server.rdbcompression = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"activerehashing") && argc == 2) {
            if ((server.activerehashing = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendonly") && argc == 2) {
            if ((server.appendonly = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendfilename") && argc == 2) {
            zfree(server.appendfilename);
            server.appendfilename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"no-appendfsync-on-rewrite")
                   && argc == 2) {
            if ((server.no_appendfsync_on_rewrite= yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendfsync") && argc == 2) {
            if (!strcasecmp(argv[1],"no")) {
                server.appendfsync = APPENDFSYNC_NO;
            } else if (!strcasecmp(argv[1],"always")) {
                server.appendfsync = APPENDFSYNC_ALWAYS;
            } else if (!strcasecmp(argv[1],"everysec")) {
                server.appendfsync = APPENDFSYNC_EVERYSEC;
            } else {
                err = "argument must be 'no', 'always' or 'everysec'";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {
            server.requirepass = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"pidfile") && argc == 2) {
            zfree(server.pidfile);
            server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
            zfree(server.dbfilename);
            server.dbfilename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-enabled") && argc == 2) {
            if ((server.vm_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"vm-swap-file") && argc == 2) {
            zfree(server.vm_swap_file);
            server.vm_swap_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-max-memory") && argc == 2) {
            server.vm_max_memory = memtoll(argv[1],NULL);
        } else if (!strcasecmp(argv[0],"vm-page-size") && argc == 2) {
            server.vm_page_size = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"vm-pages") && argc == 2) {
            server.vm_pages = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"vm-max-threads") && argc == 2) {
            server.vm_max_threads = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-entries") && argc == 2){
            server.hash_max_zipmap_entries = memtoll(argv[1], NULL);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-value") && argc == 2){
            server.hash_max_zipmap_value = memtoll(argv[1], NULL);
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        for (j = 0; j < argc; j++)
            sdsfree(argv[j]);
        zfree(argv);
        sdsfree(line);
    }
    if (fp != stdin) fclose(fp);
    return;
loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", line);
    fprintf(stderr, "%s\n", err);
    exit(1);
}
static void freeClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    for (j = 0; j < c->mbargc; j++)
        decrRefCount(c->mbargv[j]);
    c->argc = 0;
    c->mbargc = 0;
}
static void freeClient(redisClient *c) {
    listNode *ln;
    sdsfree(c->querybuf);
    c->querybuf = NULL;
    if (c->flags & REDIS_BLOCKED)
        unblockClientWaitingData(c);
    unwatchAllKeys(c);
    listRelease(c->watched_keys);
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    listRelease(c->reply);
    freeClientArgv(c);
    close(c->fd);
    ln = listSearchKey(server.clients,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients,ln);
    if (c->flags & REDIS_IO_WAIT && listLength(c->io_keys) == 0) {
        ln = listSearchKey(server.io_ready_clients,c);
        if (ln) {
            listDelNode(server.io_ready_clients,ln);
            server.vm_blocked_clients--;
        }
    }
    while (server.vm_enabled && listLength(c->io_keys)) {
        ln = listFirst(c->io_keys);
        dontWaitForSwappedKey(c,ln->value);
    }
    listRelease(c->io_keys);
    if (c->flags & REDIS_SLAVE) {
        if (c->replstate == REDIS_REPL_SEND_BULK && c->repldbfd != -1)
            close(c->repldbfd);
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        redisAssert(ln != NULL);
        listDelNode(l,ln);
    }
    if (c->flags & REDIS_MASTER) {
        server.master = NULL;
        server.replstate = REDIS_REPL_CONNECT;
    }
    zfree(c->argv);
    zfree(c->mbargv);
    freeClientMultiState(c);
    zfree(c);
}
#define GLUEREPLY_UP_TO (1024)
static void glueReplyBuffersIfNeeded(redisClient *c) {
    int copylen = 0;
    char buf[GLUEREPLY_UP_TO];
    listNode *ln;
    listIter li;
    robj *o;
    listRewind(c->reply,&li);
    while((ln = listNext(&li))) {
        int objlen;
        o = ln->value;
        objlen = sdslen(o->ptr);
        if (copylen + objlen <= GLUEREPLY_UP_TO) {
            memcpy(buf+copylen,o->ptr,objlen);
            copylen += objlen;
            listDelNode(c->reply,ln);
        } else {
            if (copylen == 0) return;
            break;
        }
    }
    o = createObject(REDIS_STRING,sdsnewlen(buf,copylen));
    listAddNodeHead(c->reply,o);
}
static void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    if (!server.glueoutputbuf &&
        listLength(c->reply) > REDIS_WRITEV_THRESHOLD &&
        !(c->flags & REDIS_MASTER))
    {
        sendReplyToClientWritev(el, fd, privdata, mask);
        return;
    }
    while(listLength(c->reply)) {
        if (server.glueoutputbuf && listLength(c->reply) > 1)
            glueReplyBuffersIfNeeded(c);
        o = listNodeValue(listFirst(c->reply));
        objlen = sdslen(o->ptr);
        if (objlen == 0) {
            listDelNode(c->reply,listFirst(c->reply));
            continue;
        }
        if (c->flags & REDIS_MASTER) {
            nwritten = objlen - c->sentlen;
        } else {
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
        }
        c->sentlen += nwritten;
        totwritten += nwritten;
        if (c->sentlen == objlen) {
            listDelNode(c->reply,listFirst(c->reply));
            c->sentlen = 0;
        }
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    if (totwritten > 0) c->lastinteraction = time(NULL);
    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    }
}
static void sendReplyToClientWritev(aeEventLoop *el, int fd, void *privdata, int mask)
{
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen, willwrite;
    robj *o;
    struct iovec iov[REDIS_WRITEV_IOVEC_COUNT];
    int offset, ion = 0;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    listNode *node;
    while (listLength(c->reply)) {
        offset = c->sentlen;
        ion = 0;
        willwrite = 0;
        for(node = listFirst(c->reply); node; node = listNextNode(node)) {
            o = listNodeValue(node);
            objlen = sdslen(o->ptr);
            if (totwritten + objlen - offset > REDIS_MAX_WRITE_PER_EVENT)
                break;
            if(ion == REDIS_WRITEV_IOVEC_COUNT)
                break;
            iov[ion].iov_base = ((char*)o->ptr) + offset;
            iov[ion].iov_len = objlen - offset;
            willwrite += objlen - offset;
            offset = 0;
            ion++;
        }
        if(willwrite == 0)
            break;
        if((nwritten = writev(fd, iov, ion)) < 0) {
            if (errno != EAGAIN) {
                redisLog(REDIS_VERBOSE,
                         "Error writing to client: %s", strerror(errno));
                freeClient(c);
                return;
            }
            break;
        }
        totwritten += nwritten;
        offset = c->sentlen;
        while (nwritten && listLength(c->reply)) {
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            if(nwritten >= objlen - offset) {
                listDelNode(c->reply, listFirst(c->reply));
                nwritten -= objlen - offset;
                c->sentlen = 0;
            } else {
                c->sentlen += nwritten;
                break;
            }
            offset = 0;
        }
    }
    if (totwritten > 0)
        c->lastinteraction = time(NULL);
    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    }
}
static int qsortRedisCommands(const void *r1, const void *r2) {
    return strcasecmp(
        ((struct redisCommand*)r1)->name,
        ((struct redisCommand*)r2)->name);
}
static void sortCommandTable() {
    commandTable = (struct redisCommand*)malloc(sizeof(readonlyCommandTable));
    memcpy(commandTable,readonlyCommandTable,sizeof(readonlyCommandTable));
    qsort(commandTable,
        sizeof(readonlyCommandTable)/sizeof(struct redisCommand),
        sizeof(struct redisCommand),qsortRedisCommands);
}
static struct redisCommand *lookupCommand(char *name) {
    struct redisCommand tmp = {name,NULL,0,0,NULL,0,0,0};
    return bsearch(
        &tmp,
        commandTable,
        sizeof(readonlyCommandTable)/sizeof(struct redisCommand),
        sizeof(struct redisCommand),
        qsortRedisCommands);
}
static void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->bulklen = -1;
    c->multibulk = 0;
}
static void call(redisClient *c, struct redisCommand *cmd) {
    long long dirty;
    dirty = server.dirty;
    cmd->proc(c);
    dirty = server.dirty-dirty;
    if (server.appendonly && dirty)
        feedAppendOnlyFile(cmd,c->db->id,c->argv,c->argc);
    if ((dirty || cmd->flags & REDIS_CMD_FORCE_REPLICATION) &&
        listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedMonitors(server.monitors,c->db->id,c->argv,c->argc);
    server.stat_numcommands++;
}
static int processCommand(redisClient *c) {
    struct redisCommand *cmd;
    if (server.maxmemory) freeMemoryIfNeeded();
    if (c->multibulk == 0 && c->argc == 1 && ((char*)(c->argv[0]->ptr))[0] == '*') {
        c->multibulk = atoi(((char*)c->argv[0]->ptr)+1);
        if (c->multibulk <= 0) {
            resetClient(c);
            return 1;
        } else {
            decrRefCount(c->argv[c->argc-1]);
            c->argc--;
            return 1;
        }
    } else if (c->multibulk) {
        if (c->bulklen == -1) {
            if (((char*)c->argv[0]->ptr)[0] != '$') {
                addReplySds(c,sdsnew("-ERR multi bulk protocol error\r\n"));
                resetClient(c);
                return 1;
            } else {
                int bulklen = atoi(((char*)c->argv[0]->ptr)+1);
                decrRefCount(c->argv[0]);
                if (bulklen < 0 || bulklen > 1024*1024*1024) {
                    c->argc--;
                    addReplySds(c,sdsnew("-ERR invalid bulk write count\r\n"));
                    resetClient(c);
                    return 1;
                }
                c->argc--;
                c->bulklen = bulklen+2;
                return 1;
            }
        } else {
            c->mbargv = zrealloc(c->mbargv,(sizeof(robj*))*(c->mbargc+1));
            c->mbargv[c->mbargc] = c->argv[0];
            c->mbargc++;
            c->argc--;
            c->multibulk--;
            if (c->multibulk == 0) {
                robj **auxargv;
                int auxargc;
                auxargv = c->argv;
                c->argv = c->mbargv;
                c->mbargv = auxargv;
                auxargc = c->argc;
                c->argc = c->mbargc;
                c->mbargc = auxargc;
                c->bulklen = 0;
            } else {
                c->bulklen = -1;
                return 1;
            }
        }
    }
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        freeClient(c);
        return 0;
    }
    cmd = lookupCommand(c->argv[0]->ptr);
    if (!cmd) {
        addReplySds(c,
            sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n",
                (char*)c->argv[0]->ptr));
        resetClient(c);
        return 1;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc) ||
               (c->argc < -cmd->arity)) {
        addReplySds(c,
            sdscatprintf(sdsempty(),
                "-ERR wrong number of arguments for '%s' command\r\n",
                cmd->name));
        resetClient(c);
        return 1;
    } else if (cmd->flags & REDIS_CMD_BULK && c->bulklen == -1) {
        int bulklen = atoi(c->argv[c->argc-1]->ptr);
        decrRefCount(c->argv[c->argc-1]);
        if (bulklen < 0 || bulklen > 1024*1024*1024) {
            c->argc--;
            addReplySds(c,sdsnew("-ERR invalid bulk write count\r\n"));
            resetClient(c);
            return 1;
        }
        c->argc--;
        c->bulklen = bulklen+2;
        if ((signed)sdslen(c->querybuf) >= c->bulklen) {
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
        } else {
            return 1;
        }
    }
    if (cmd->flags & REDIS_CMD_BULK)
        c->argv[c->argc-1] = tryObjectEncoding(c->argv[c->argc-1]);
    if (server.requirepass && !c->authenticated && cmd->proc != authCommand) {
        addReplySds(c,sdsnew("-ERR operation not permitted\r\n"));
        resetClient(c);
        return 1;
    }
    if (server.maxmemory && (cmd->flags & REDIS_CMD_DENYOOM) &&
        zmalloc_used_memory() > server.maxmemory)
    {
        addReplySds(c,sdsnew("-ERR command not allowed when used memory > 'maxmemory'\r\n"));
        resetClient(c);
        return 1;
    }
    if ((dictSize(c->pubsub_channels) > 0 || listLength(c->pubsub_patterns) > 0)
        &&
        cmd->proc != subscribeCommand && cmd->proc != unsubscribeCommand &&
        cmd->proc != psubscribeCommand && cmd->proc != punsubscribeCommand) {
        addReplySds(c,sdsnew("-ERR only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context\r\n"));
        resetClient(c);
        return 1;
    }
    if (c->flags & REDIS_MULTI &&
        cmd->proc != execCommand && cmd->proc != discardCommand &&
        cmd->proc != multiCommand && cmd->proc != watchCommand)
    {
        queueMultiCommand(c,cmd);
        addReply(c,shared.queued);
    } else {
        if (server.vm_enabled && server.vm_max_threads > 0 &&
            blockClientOnSwappedKeys(c,cmd)) return 1;
        call(c,cmd);
    }
    resetClient(c);
    return 1;
}
static void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int outc = 0, j;
    robj **outv;
    robj *static_outv[REDIS_STATIC_ARGS*3+1];
    robj *lenobj;
    if (argc <= REDIS_STATIC_ARGS) {
        outv = static_outv;
    } else {
        outv = zmalloc(sizeof(robj*)*(argc*3+1));
    }
    lenobj = createObject(REDIS_STRING,
            sdscatprintf(sdsempty(), "*%d\r\n", argc));
    lenobj->refcount = 0;
    outv[outc++] = lenobj;
    for (j = 0; j < argc; j++) {
        lenobj = createObject(REDIS_STRING,
            sdscatprintf(sdsempty(),"$%lu\r\n",
                (unsigned long) stringObjectLen(argv[j])));
        lenobj->refcount = 0;
        outv[outc++] = lenobj;
        outv[outc++] = argv[j];
        outv[outc++] = shared.crlf;
    }
    for (j = 0; j < outc; j++) incrRefCount(outv[j]);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;
        if (slave->slaveseldb != dictid) {
            robj *selectcmd;
            switch(dictid) {
            case 0: selectcmd = shared.select0; break;
            case 1: selectcmd = shared.select1; break;
            case 2: selectcmd = shared.select2; break;
            case 3: selectcmd = shared.select3; break;
            case 4: selectcmd = shared.select4; break;
            case 5: selectcmd = shared.select5; break;
            case 6: selectcmd = shared.select6; break;
            case 7: selectcmd = shared.select7; break;
            case 8: selectcmd = shared.select8; break;
            case 9: selectcmd = shared.select9; break;
            default:
                selectcmd = createObject(REDIS_STRING,
                    sdscatprintf(sdsempty(),"select %d\r\n",dictid));
                selectcmd->refcount = 0;
                break;
            }
            addReply(slave,selectcmd);
            slave->slaveseldb = dictid;
        }
        for (j = 0; j < outc; j++) addReply(slave,outv[j]);
    }
    for (j = 0; j < outc; j++) decrRefCount(outv[j]);
    if (outv != static_outv) zfree(outv);
}
static sds sdscatrepr(sds s, char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",1); break;
        case '\r': s = sdscatlen(s,"\\r",1); break;
        case '\t': s = sdscatlen(s,"\\t",1); break;
        case '\a': s = sdscatlen(s,"\\a",1); break;
        case '\b': s = sdscatlen(s,"\\b",1); break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}
static void replicationFeedMonitors(list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;
    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (dictid != 0) cmdrepr = sdscatprintf(cmdrepr,"(db %d) ", dictid);
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == REDIS_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "%ld", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(REDIS_STRING,cmdrepr);
    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        redisClient *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}
static void processInputBuffer(redisClient *c) {
again:
    if (c->flags & REDIS_BLOCKED || c->flags & REDIS_IO_WAIT) return;
    if (c->bulklen == -1) {
        char *p = strchr(c->querybuf,'\n');
        size_t querylen;
        if (p) {
            sds query, *argv;
            int argc, j;
            query = c->querybuf;
            c->querybuf = sdsempty();
            querylen = 1+(p-(query));
            if (sdslen(query) > querylen) {
                c->querybuf = sdscatlen(c->querybuf,query+querylen,sdslen(query)-querylen);
            }
            *p = '\0';
            if (*(p-1) == '\r') *(p-1) = '\0';
            sdsupdatelen(query);
            argv = sdssplitlen(query,sdslen(query)," ",1,&argc);
            sdsfree(query);
            if (c->argv) zfree(c->argv);
            c->argv = zmalloc(sizeof(robj*)*argc);
            for (j = 0; j < argc; j++) {
                if (sdslen(argv[j])) {
                    c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
                    c->argc++;
                } else {
                    sdsfree(argv[j]);
                }
            }
            zfree(argv);
            if (c->argc) {
                if (processCommand(c) && sdslen(c->querybuf)) goto again;
            } else {
                if (sdslen(c->querybuf)) goto again;
            }
            return;
        } else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
            redisLog(REDIS_VERBOSE, "Client protocol error");
            freeClient(c);
            return;
        }
    } else {
        int qbl = sdslen(c->querybuf);
        if (c->bulklen <= qbl) {
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
            if (processCommand(c) && sdslen(c->querybuf)) goto again;
            return;
        }
    }
}
static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    char buf[REDIS_IOBUF_LEN];
    int nread;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    nread = read(fd, buf, REDIS_IOBUF_LEN);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    if (nread) {
        c->querybuf = sdscatlen(c->querybuf, buf, nread);
        c->lastinteraction = time(NULL);
    } else {
        return;
    }
    processInputBuffer(c);
}
static int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}
static void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}
static int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}
static redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(*c));
    anetNonBlock(NULL,fd);
    anetTcpNoDelay(NULL,fd);
    if (!c) return NULL;
    selectDb(c,0);
    c->fd = fd;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bulklen = -1;
    c->multibulk = 0;
    c->mbargc = 0;
    c->mbargv = NULL;
    c->sentlen = 0;
    c->flags = 0;
    c->lastinteraction = time(NULL);
    c->authenticated = 0;
    c->replstate = REDIS_REPL_NONE;
    c->reply = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->blocking_keys = NULL;
    c->blocking_keys_num = 0;
    c->io_keys = listCreate();
    c->watched_keys = listCreate();
    listSetFreeMethod(c->io_keys,decrRefCount);
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    c->pubsub_patterns = listCreate();
    listSetFreeMethod(c->pubsub_patterns,decrRefCount);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
        readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    listAddNodeTail(server.clients,c);
    initClientMultiState(c);
    return c;
}
static void addReply(redisClient *c, robj *obj) {
    if (listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         c->replstate == REDIS_REPL_ONLINE) &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return;
    if (server.vm_enabled && obj->storage != REDIS_VM_MEMORY) {
        obj = dupStringObject(obj);
        obj->refcount = 0;
    }
    listAddNodeTail(c->reply,getDecodedObject(obj));
}
static void addReplySds(redisClient *c, sds s) {
    robj *o = createObject(REDIS_STRING,s);
    addReply(c,o);
    decrRefCount(o);
}
static void addReplyDouble(redisClient *c, double d) {
    char buf[128];
    snprintf(buf,sizeof(buf),"%.17g",d);
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n%s\r\n",
        (unsigned long) strlen(buf),buf));
}
static void addReplyLongLong(redisClient *c, long long ll) {
    char buf[128];
    size_t len;
    if (ll == 0) {
        addReply(c,shared.czero);
        return;
    } else if (ll == 1) {
        addReply(c,shared.cone);
        return;
    }
    buf[0] = ':';
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplySds(c,sdsnewlen(buf,len+3));
}
static void addReplyUlong(redisClient *c, unsigned long ul) {
    char buf[128];
    size_t len;
    if (ul == 0) {
        addReply(c,shared.czero);
        return;
    } else if (ul == 1) {
        addReply(c,shared.cone);
        return;
    }
    len = snprintf(buf,sizeof(buf),":%lu\r\n",ul);
    addReplySds(c,sdsnewlen(buf,len));
}
static void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len, intlen;
    char buf[128];
    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    } else {
        long n = (long)obj->ptr;
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }
    buf[0] = '$';
    intlen = ll2string(buf+1,sizeof(buf)-1,(long long)len);
    buf[intlen+1] = '\r';
    buf[intlen+2] = '\n';
    addReplySds(c,sdsnewlen(buf,intlen+3));
}
static void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}
static void addReplyBulkSds(redisClient *c, sds s) {
    robj *o = createStringObject(s, sdslen(s));
    addReplyBulk(c,o);
    decrRefCount(o);
}
static void addReplyBulkCString(redisClient *c, char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *o = createStringObject(s,strlen(s));
        addReplyBulk(c,o);
        decrRefCount(o);
    }
}
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    redisClient *c;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);
    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE,"Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING,"Error allocating resoures for the client");
        close(cfd);
        return;
    }
    if (server.maxclients && listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";
        if (write(c->fd,err,strlen(err)) == -1) {
        }
        freeClient(c);
        return;
    }
    server.stat_numconnections++;
}
static robj *createObject(int type, void *ptr) {
    robj *o;
    if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
    if (listLength(server.objfreelist)) {
        listNode *head = listFirst(server.objfreelist);
        o = listNodeValue(head);
        listDelNode(server.objfreelist,head);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
    } else {
        if (server.vm_enabled)
            pthread_mutex_unlock(&server.obj_freelist_mutex);
        o = zmalloc(sizeof(*o));
    }
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    if (server.vm_enabled) {
        o->lru = server.lruclock;
        o->storage = REDIS_VM_MEMORY;
    }
    return o;
}
static robj *createStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}
static robj *createStringObjectFromLongLong(long long value) {
    robj *o;
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        incrRefCount(shared.integers[value]);
        o = shared.integers[value];
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {
            o = createObject(REDIS_STRING, NULL);
            o->encoding = REDIS_ENCODING_INT;
            o->ptr = (void*)((long)value);
        } else {
            o = createObject(REDIS_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}
static robj *dupStringObject(robj *o) {
    assert(o->encoding == REDIS_ENCODING_RAW);
    return createStringObject(o->ptr,sdslen(o->ptr));
}
static robj *createListObject(void) {
    list *l = listCreate();
    robj *o = createObject(REDIS_LIST,l);
    listSetFreeMethod(l,decrRefCount);
    o->encoding = REDIS_ENCODING_LIST;
    return o;
}
static robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();
    robj *o = createObject(REDIS_LIST,zl);
    o->encoding = REDIS_ENCODING_ZIPLIST;
    return o;
}
static robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    return createObject(REDIS_SET,d);
}
static robj *createHashObject(void) {
    unsigned char *zm = zipmapNew();
    robj *o = createObject(REDIS_HASH,zm);
    o->encoding = REDIS_ENCODING_ZIPMAP;
    return o;
}
static robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    zs->dict = dictCreate(&zsetDictType,NULL);
    zs->zsl = zslCreate();
    return createObject(REDIS_ZSET,zs);
}
static void freeStringObject(robj *o) {
    if (o->encoding == REDIS_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}
static void freeListObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_LIST:
        listRelease((list*) o->ptr);
        break;
    case REDIS_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown list encoding type");
    }
}
static void freeSetObject(robj *o) {
    dictRelease((dict*) o->ptr);
}
static void freeZsetObject(robj *o) {
    zset *zs = o->ptr;
    dictRelease(zs->dict);
    zslFree(zs->zsl);
    zfree(zs);
}
static void freeHashObject(robj *o) {
    switch (o->encoding) {
    case REDIS_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case REDIS_ENCODING_ZIPMAP:
        zfree(o->ptr);
        break;
    default:
        redisPanic("Unknown hash encoding type");
        break;
    }
}
static void incrRefCount(robj *o) {
    o->refcount++;
}
static void decrRefCount(void *obj) {
    robj *o = obj;
    if (server.vm_enabled &&
        (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING))
    {
        vmpointer *vp = obj;
        if (o->storage == REDIS_VM_LOADING) vmCancelThreadedIOJob(o);
        vmMarkPagesFree(vp->page,vp->usedpages);
        server.vm_stats_swapped_objects--;
        zfree(vp);
        return;
    }
    if (o->refcount <= 0) redisPanic("decrRefCount against refcount <= 0");
    if (server.vm_enabled && o->storage == REDIS_VM_SWAPPING)
        vmCancelThreadedIOJob(o);
    if (--(o->refcount) == 0) {
        switch(o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        case REDIS_LIST: freeListObject(o); break;
        case REDIS_SET: freeSetObject(o); break;
        case REDIS_ZSET: freeZsetObject(o); break;
        case REDIS_HASH: freeHashObject(o); break;
        default: redisPanic("Unknown object type"); break;
        }
        if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
    }
}
static int checkType(redisClient *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}
static int isStringRepresentableAsLong(sds s, long *longval) {
    char buf[32], *endptr;
    long value;
    int slen;
    value = strtol(s, &endptr, 10);
    if (endptr[0] != '\0') return REDIS_ERR;
    slen = ll2string(buf,32,value);
    if (sdslen(s) != (unsigned)slen || memcmp(buf,s,slen)) return REDIS_ERR;
    if (longval) *longval = value;
    return REDIS_OK;
}
static robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;
    if (o->encoding != REDIS_ENCODING_RAW)
        return o;
     if (o->refcount > 1) return o;
    redisAssert(o->type == REDIS_STRING);
    if (isStringRepresentableAsLong(s,&value) == REDIS_ERR) return o;
    if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
        decrRefCount(o);
        incrRefCount(shared.integers[value]);
        return shared.integers[value];
    } else {
        o->encoding = REDIS_ENCODING_INT;
        sdsfree(o->ptr);
        o->ptr = (void*) value;
        return o;
    }
}
static robj *getDecodedObject(robj *o) {
    robj *dec;
    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
        char buf[32];
        ll2string(buf,32,(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        redisPanic("Unknown encoding type");
    }
}
static int compareStringObjects(robj *a, robj *b) {
    redisAssert(a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    int bothsds = 1;
    if (a == b) return 0;
    if (a->encoding != REDIS_ENCODING_RAW) {
        ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
        bothsds = 0;
    } else {
        astr = a->ptr;
    }
    if (b->encoding != REDIS_ENCODING_RAW) {
        ll2string(bufb,sizeof(bufb),(long) b->ptr);
        bstr = bufb;
        bothsds = 0;
    } else {
        bstr = b->ptr;
    }
    return bothsds ? sdscmp(astr,bstr) : strcmp(astr,bstr);
}
static int equalStringObjects(robj *a, robj *b) {
    if (a->encoding != REDIS_ENCODING_RAW && b->encoding != REDIS_ENCODING_RAW){
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a,b) == 0;
    }
}
static size_t stringObjectLen(robj *o) {
    redisAssert(o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_RAW) {
        return sdslen(o->ptr);
    } else {
        char buf[32];
        return ll2string(buf,32,(long)o->ptr);
    }
}
static int getDoubleFromObject(robj *o, double *target) {
    double value;
    char *eptr;
    if (o == NULL) {
        value = 0;
    } else {
        redisAssert(o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW) {
            value = strtod(o->ptr, &eptr);
            if (eptr[0] != '\0') return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}
static int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is not a double\r\n"));
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
static int getLongLongFromObject(robj *o, long long *target) {
    long long value;
    char *eptr;
    if (o == NULL) {
        value = 0;
    } else {
        redisAssert(o->type == REDIS_STRING);
        if (o->encoding == REDIS_ENCODING_RAW) {
            value = strtoll(o->ptr, &eptr, 10);
            if (eptr[0] != '\0') return REDIS_ERR;
        } else if (o->encoding == REDIS_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            redisPanic("Unknown string encoding");
        }
    }
    *target = value;
    return REDIS_OK;
}
static int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != REDIS_OK) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is not an integer\r\n"));
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
static int getLongFromObjectOrReply(redisClient *c, robj *o, long *target, const char *msg) {
    long long value;
    if (getLongLongFromObjectOrReply(c, o, &value, msg) != REDIS_OK) return REDIS_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplySds(c, sdscatprintf(sdsempty(), "-ERR %s\r\n", msg));
        } else {
            addReplySds(c, sdsnew("-ERR value is out of range\r\n"));
        }
        return REDIS_ERR;
    }
    *target = value;
    return REDIS_OK;
}
static robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetEntryVal(de);
        if (server.vm_enabled) {
            if (val->storage == REDIS_VM_MEMORY ||
                val->storage == REDIS_VM_SWAPPING)
            {
                if (val->storage == REDIS_VM_SWAPPING)
                    vmCancelThreadedIOJob(val);
                val->lru = server.lruclock;
            } else {
                int notify = (val->storage == REDIS_VM_LOADING);
                redisAssert(val->type == REDIS_VMPOINTER);
                val = vmLoadObject(val);
                dictGetEntryVal(de) = val;
                if (notify) handleClientsBlockedOnSwappedKey(db,key);
            }
        }
        return val;
    } else {
        return NULL;
    }
}
static robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}
static robj *lookupKeyWrite(redisDb *db, robj *key) {
    deleteIfVolatile(db,key);
    touchWatchedKey(db,key);
    return lookupKey(db,key);
}
static robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}
static robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}
static int dbAdd(redisDb *db, robj *key, robj *val) {
    if (dictFind(db->dict, key->ptr) != NULL) {
        return REDIS_ERR;
    } else {
        sds copy = sdsdup(key->ptr);
        dictAdd(db->dict, copy, val);
        return REDIS_OK;
    }
}
static int dbReplace(redisDb *db, robj *key, robj *val) {
    if (dictFind(db->dict,key->ptr) == NULL) {
        sds copy = sdsdup(key->ptr);
        dictAdd(db->dict, copy, val);
        return 1;
    } else {
        dictReplace(db->dict, key->ptr, val);
        return 0;
    }
}
static int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}
static robj *dbRandomKey(redisDb *db) {
    struct dictEntry *de;
    while(1) {
        sds key;
        robj *keyobj;
        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;
        key = dictGetEntryKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue;
            }
        }
        return keyobj;
    }
}
static int dbDelete(redisDb *db, robj *key) {
    int retval;
    if (dictSize(db->expires)) dictDelete(db->expires,key->ptr);
    retval = dictDelete(db->dict,key->ptr);
    return retval == DICT_OK;
}
static int rdbSaveType(FILE *fp, unsigned char type) {
    if (fwrite(&type,1,1,fp) == 0) return -1;
    return 0;
}
static int rdbSaveTime(FILE *fp, time_t t) {
    int32_t t32 = (int32_t) t;
    if (fwrite(&t32,4,1,fp) == 0) return -1;
    return 0;
}
static int rdbSaveLen(FILE *fp, uint32_t len) {
    unsigned char buf[2];
    if (len < (1<<6)) {
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);
        if (fwrite(buf,1,1,fp) == 0) return -1;
    } else if (len < (1<<14)) {
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (fwrite(buf,2,1,fp) == 0) return -1;
    } else {
        buf[0] = (REDIS_RDB_32BITLEN<<6);
        if (fwrite(buf,1,1,fp) == 0) return -1;
        len = htonl(len);
        if (fwrite(&len,4,1,fp) == 0) return -1;
    }
    return 0;
}
static int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}
static int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;
    return rdbEncodeInteger(value,enc);
}
static int rdbSaveLzfStringObject(FILE *fp, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    unsigned char byte;
    void *out;
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;
    if (fwrite(&byte,1,1,fp) == 0) goto writeerr;
    if (rdbSaveLen(fp,comprlen) == -1) goto writeerr;
    if (rdbSaveLen(fp,len) == -1) goto writeerr;
    if (fwrite(out,comprlen,1,fp) == 0) goto writeerr;
    zfree(out);
    return comprlen;
writeerr:
    zfree(out);
    return -1;
}
static int rdbSaveRawString(FILE *fp, unsigned char *s, size_t len) {
    int enclen;
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (fwrite(buf,enclen,1,fp) == 0) return -1;
            return 0;
        }
    }
    if (server.rdbcompression && len > 20) {
        int retval;
        retval = rdbSaveLzfStringObject(fp,s,len);
        if (retval == -1) return -1;
        if (retval > 0) return 0;
    }
    if (rdbSaveLen(fp,len) == -1) return -1;
    if (len && fwrite(s,len,1,fp) == 0) return -1;
    return 0;
}
static int rdbSaveLongLongAsStringObject(FILE *fp, long long value) {
    unsigned char buf[32];
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        if (fwrite(buf,enclen,1,fp) == 0) return -1;
    } else {
        enclen = ll2string((char*)buf,32,value);
        redisAssert(enclen < 32);
        if (rdbSaveLen(fp,enclen) == -1) return -1;
        if (fwrite(buf,enclen,1,fp) == 0) return -1;
    }
    return 0;
}
static int rdbSaveStringObject(FILE *fp, robj *obj) {
    if (obj->encoding == REDIS_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(fp,(long)obj->ptr);
    } else {
        redisAssert(obj->encoding == REDIS_ENCODING_RAW);
        return rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
    }
}
static int rdbSaveDoubleValue(FILE *fp, double val) {
    unsigned char buf[128];
    int len;
    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        double min = -4503599627370495;
        double max = 4503599627370496;
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf),(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    if (fwrite(buf,len,1,fp) == 0) return -1;
    return 0;
}
static int rdbSaveObject(FILE *fp, robj *o) {
    if (o->type == REDIS_STRING) {
        if (rdbSaveStringObject(fp,o) == -1) return -1;
    } else if (o->type == REDIS_LIST) {
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            unsigned char *p;
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;
            if (rdbSaveLen(fp,ziplistLen(o->ptr)) == -1) return -1;
            p = ziplistIndex(o->ptr,0);
            while(ziplistGet(p,&vstr,&vlen,&vlong)) {
                if (vstr) {
                    if (rdbSaveRawString(fp,vstr,vlen) == -1)
                        return -1;
                } else {
                    if (rdbSaveLongLongAsStringObject(fp,vlong) == -1)
                        return -1;
                }
                p = ziplistNext(o->ptr,p);
            }
        } else if (o->encoding == REDIS_ENCODING_LIST) {
            list *list = o->ptr;
            listIter li;
            listNode *ln;
            if (rdbSaveLen(fp,listLength(list)) == -1) return -1;
            listRewind(list,&li);
            while((ln = listNext(&li))) {
                robj *eleobj = listNodeValue(ln);
                if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
            }
        } else {
            redisPanic("Unknown list encoding");
        }
    } else if (o->type == REDIS_SET) {
        dict *set = o->ptr;
        dictIterator *di = dictGetIterator(set);
        dictEntry *de;
        if (rdbSaveLen(fp,dictSize(set)) == -1) return -1;
        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetEntryKey(de);
            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
        }
        dictReleaseIterator(di);
    } else if (o->type == REDIS_ZSET) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;
        if (rdbSaveLen(fp,dictSize(zs->dict)) == -1) return -1;
        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetEntryKey(de);
            double *score = dictGetEntryVal(de);
            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
            if (rdbSaveDoubleValue(fp,*score) == -1) return -1;
        }
        dictReleaseIterator(di);
    } else if (o->type == REDIS_HASH) {
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            unsigned char *p = zipmapRewind(o->ptr);
            unsigned int count = zipmapLen(o->ptr);
            unsigned char *key, *val;
            unsigned int klen, vlen;
            if (rdbSaveLen(fp,count) == -1) return -1;
            while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
                if (rdbSaveRawString(fp,key,klen) == -1) return -1;
                if (rdbSaveRawString(fp,val,vlen) == -1) return -1;
            }
        } else {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;
            if (rdbSaveLen(fp,dictSize((dict*)o->ptr)) == -1) return -1;
            while((de = dictNext(di)) != NULL) {
                robj *key = dictGetEntryKey(de);
                robj *val = dictGetEntryVal(de);
                if (rdbSaveStringObject(fp,key) == -1) return -1;
                if (rdbSaveStringObject(fp,val) == -1) return -1;
            }
            dictReleaseIterator(di);
        }
    } else {
        redisPanic("Unknown object type");
    }
    return 0;
}
static off_t rdbSavedObjectLen(robj *o, FILE *fp) {
    if (fp == NULL) fp = server.devnull;
    rewind(fp);
    assert(rdbSaveObject(fp,o) != 1);
    return ftello(fp);
}
static off_t rdbSavedObjectPages(robj *o, FILE *fp) {
    off_t bytes = rdbSavedObjectLen(o,fp);
    return (bytes+(server.vm_page_size-1))/server.vm_page_size;
}
static int rdbSave(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);
    if (server.vm_enabled)
        waitEmptyIOJobsQueue();
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed saving the DB: %s", strerror(errno));
        return REDIS_ERR;
    }
    if (fwrite("REDIS0001",9,1,fp) == 0) goto werr;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }
        if (rdbSaveType(fp,REDIS_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(fp,j) == -1) goto werr;
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetEntryKey(de);
            robj key, *o = dictGetEntryVal(de);
            time_t expiretime;
            initStaticStringObject(key,keystr);
            expiretime = getExpire(db,&key);
            if (expiretime != -1) {
                if (expiretime < now) continue;
                if (rdbSaveType(fp,REDIS_EXPIRETIME) == -1) goto werr;
                if (rdbSaveTime(fp,expiretime) == -1) goto werr;
            }
            if (!server.vm_enabled || o->storage == REDIS_VM_MEMORY ||
                                      o->storage == REDIS_VM_SWAPPING) {
                if (rdbSaveType(fp,o->type) == -1) goto werr;
                if (rdbSaveStringObject(fp,&key) == -1) goto werr;
                if (rdbSaveObject(fp,o) == -1) goto werr;
            } else {
                robj *po;
                po = vmPreviewObject(o);
                if (rdbSaveType(fp,po->type) == -1) goto werr;
                if (rdbSaveStringObject(fp,&key) == -1) goto werr;
                if (rdbSaveObject(fp,po) == -1) goto werr;
                decrRefCount(po);
            }
        }
        dictReleaseIterator(di);
    }
    if (rdbSaveType(fp,REDIS_EOF) == -1) goto werr;
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    return REDIS_OK;
werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}
static int rdbSaveBackground(char *filename) {
    pid_t childpid;
    if (server.bgsavechildpid != -1) return REDIS_ERR;
    if (server.vm_enabled) waitEmptyIOJobsQueue();
    if ((childpid = fork()) == 0) {
        if (server.vm_enabled) vmReopenSwapFile();
        close(server.fd);
        if (rdbSave(filename) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);
        server.bgsavechildpid = childpid;
        updateDictResizePolicy();
        return REDIS_OK;
    }
    return REDIS_OK;
}
static void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];
    snprintf(tmpfile,256,"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}
static int rdbLoadType(FILE *fp) {
    unsigned char type;
    if (fread(&type,1,1,fp) == 0) return -1;
    return type;
}
static time_t rdbLoadTime(FILE *fp) {
    int32_t t32;
    if (fread(&t32,4,1,fp) == 0) return -1;
    return (time_t) t32;
}
static uint32_t rdbLoadLen(FILE *fp, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;
    if (isencoded) *isencoded = 0;
    if (fread(buf,1,1,fp) == 0) return REDIS_RDB_LENERR;
    type = (buf[0]&0xC0)>>6;
    if (type == REDIS_RDB_6BITLEN) {
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_ENCVAL) {
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        if (fread(buf+1,1,1,fp) == 0) return REDIS_RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];
    } else {
        if (fread(&len,4,1,fp) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    }
}
static robj *rdbLoadIntegerObject(FILE *fp, int enctype, int encode) {
    unsigned char enc[4];
    long long val;
    if (enctype == REDIS_RDB_ENC_INT8) {
        if (fread(enc,1,1,fp) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (fread(enc,2,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (fread(enc,4,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0;
        redisPanic("Unknown RDB integer encoding type");
    }
    if (encode)
        return createStringObjectFromLongLong(val);
    else
        return createObject(REDIS_STRING,sdsfromlonglong(val));
}
static robj *rdbLoadLzfStringObject(FILE*fp) {
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;
    if ((clen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (fread(c,clen,1,fp) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}
static robj *rdbGenericLoadStringObject(FILE*fp, int encode) {
    int isencoded;
    uint32_t len;
    sds val;
    len = rdbLoadLen(fp,&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return rdbLoadIntegerObject(fp,len,encode);
        case REDIS_RDB_ENC_LZF:
            return rdbLoadLzfStringObject(fp);
        default:
            redisPanic("Unknown RDB encoding type");
        }
    }
    if (len == REDIS_RDB_LENERR) return NULL;
    val = sdsnewlen(NULL,len);
    if (len && fread(val,len,1,fp) == 0) {
        sdsfree(val);
        return NULL;
    }
    return createObject(REDIS_STRING,val);
}
static robj *rdbLoadStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,0);
}
static robj *rdbLoadEncodedStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,1);
}
static int rdbLoadDoubleValue(FILE *fp, double *val) {
    char buf[128];
    unsigned char len;
    if (fread(&len,1,1,fp) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (fread(buf,len,1,fp) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}
static robj *rdbLoadObject(int type, FILE *fp) {
    robj *o, *ele, *dec;
    size_t len;
    redisLog(REDIS_DEBUG,"LOADING OBJECT %d (at %d)\n",type,ftell(fp));
    if (type == REDIS_STRING) {
        if ((o = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (type == REDIS_LIST) {
        if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZiplistObject();
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                dec = getDecodedObject(ele);
                o->ptr = ziplistPush(o->ptr,dec->ptr,sdslen(dec->ptr),REDIS_TAIL);
                decrRefCount(dec);
                decrRefCount(ele);
            } else {
                ele = tryObjectEncoding(ele);
                listAddNodeTail(o->ptr,ele);
                incrRefCount(ele);
            }
        }
    } else if (type == REDIS_SET) {
        if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createSetObject();
        if (len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            dictAdd((dict*)o->ptr,ele,NULL);
        }
    } else if (type == REDIS_ZSET) {
        size_t zsetlen;
        zset *zs;
        if ((zsetlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;
        while(zsetlen--) {
            robj *ele;
            double *score = zmalloc(sizeof(double));
            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (rdbLoadDoubleValue(fp,score) == -1) return NULL;
            dictAdd(zs->dict,ele,score);
            zslInsert(zs->zsl,*score,ele);
            incrRefCount(ele);
        }
    } else if (type == REDIS_HASH) {
        size_t hashlen;
        if ((hashlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createHashObject();
        if (hashlen > server.hash_max_zipmap_entries)
            convertToRealHash(o);
        while(hashlen--) {
            robj *key, *val;
            if ((key = rdbLoadStringObject(fp)) == NULL) return NULL;
            if ((val = rdbLoadStringObject(fp)) == NULL) return NULL;
            if (o->encoding != REDIS_ENCODING_HT &&
               (sdslen(key->ptr) > server.hash_max_zipmap_value ||
                sdslen(val->ptr) > server.hash_max_zipmap_value))
            {
                    convertToRealHash(o);
            }
            if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                unsigned char *zm = o->ptr;
                zm = zipmapSet(zm,key->ptr,sdslen(key->ptr),
                                  val->ptr,sdslen(val->ptr),NULL);
                o->ptr = zm;
                decrRefCount(key);
                decrRefCount(val);
            } else {
                key = tryObjectEncoding(key);
                val = tryObjectEncoding(val);
                dictAdd((dict*)o->ptr,key,val);
            }
        }
    } else {
        redisPanic("Unknown object type");
    }
    return o;
}
static int rdbLoad(char *filename) {
    FILE *fp;
    uint32_t dbid;
    int type, retval, rdbver;
    int swap_all_values = 0;
    redisDb *db = server.db+0;
    char buf[1024];
    time_t expiretime, now = time(NULL);
    fp = fopen(filename,"r");
    if (!fp) return REDIS_ERR;
    if (fread(buf,9,1,fp) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        return REDIS_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver != 1) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        return REDIS_ERR;
    }
    while(1) {
        robj *key, *val;
        int force_swapout;
        expiretime = -1;
        if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        if (type == REDIS_EXPIRETIME) {
            if ((expiretime = rdbLoadTime(fp)) == -1) goto eoferr;
            if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        }
        if (type == REDIS_EOF) break;
        if (type == REDIS_SELECTDB) {
            if ((dbid = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue;
        }
        if ((key = rdbLoadStringObject(fp)) == NULL) goto eoferr;
        if ((val = rdbLoadObject(type,fp)) == NULL) goto eoferr;
        if (expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
            continue;
        }
        retval = dbAdd(db,key,val);
        if (retval == REDIS_ERR) {
            redisLog(REDIS_WARNING,"Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", key->ptr);
            exit(1);
        }
        if (expiretime != -1) setExpire(db,key,expiretime);
        if (swap_all_values) {
            dictEntry *de = dictFind(db->dict,key->ptr);
            if (de) {
                vmpointer *vp;
                val = dictGetEntryVal(de);
                if (val->refcount == 1 &&
                    (vp = vmSwapObjectBlocking(val)) != NULL)
                    dictGetEntryVal(de) = vp;
            }
            decrRefCount(key);
            continue;
        }
        decrRefCount(key);
        force_swapout = 0;
        if ((zmalloc_used_memory() - server.vm_max_memory) > 1024*1024*32)
            force_swapout = 1;
        if (!swap_all_values && server.vm_enabled && force_swapout) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
            if (zmalloc_used_memory() > server.vm_max_memory)
                swap_all_values = 1;
        }
    }
    fclose(fp);
    return REDIS_OK;
eoferr:
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR;
}
static int prepareForShutdown() {
    redisLog(REDIS_WARNING,"User requested shutdown, saving DB...");
    if (server.bgsavechildpid != -1) {
        redisLog(REDIS_WARNING,"There is a live saving child. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    if (server.appendonly) {
        aof_fsync(server.appendfd);
        if (server.vm_enabled) unlink(server.vm_swap_file);
    } else {
        if (rdbSave(server.dbfilename) == REDIS_OK) {
            if (server.daemonize)
                unlink(server.pidfile);
            redisLog(REDIS_WARNING,"%zu bytes used at exit",zmalloc_used_memory());
        } else {
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
            return REDIS_ERR;
        }
    }
    redisLog(REDIS_WARNING,"Server exit now, bye bye...");
    return REDIS_OK;
}
static void authCommand(redisClient *c) {
    if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplySds(c,sdscatprintf(sdsempty(),"-ERR invalid password\r\n"));
    }
}
static void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}
static void echoCommand(redisClient *c) {
    addReplyBulk(c,c->argv[1]);
}
static void setGenericCommand(redisClient *c, int nx, robj *key, robj *val, robj *expire) {
    int retval;
    long seconds = 0;
    if (expire) {
        if (getLongFromObjectOrReply(c, expire, &seconds, NULL) != REDIS_OK)
            return;
        if (seconds <= 0) {
            addReplySds(c,sdsnew("-ERR invalid expire time in SETEX\r\n"));
            return;
        }
    }
    touchWatchedKey(c->db,key);
    if (nx) deleteIfVolatile(c->db,key);
    retval = dbAdd(c->db,key,val);
    if (retval == REDIS_ERR) {
        if (!nx) {
            dbReplace(c->db,key,val);
            incrRefCount(val);
        } else {
            addReply(c,shared.czero);
            return;
        }
    } else {
        incrRefCount(val);
    }
    server.dirty++;
    removeExpire(c->db,key);
    if (expire) setExpire(c->db,key,time(NULL)+seconds);
    addReply(c, nx ? shared.cone : shared.ok);
}
static void setCommand(redisClient *c) {
    setGenericCommand(c,0,c->argv[1],c->argv[2],NULL);
}
static void setnxCommand(redisClient *c) {
    setGenericCommand(c,1,c->argv[1],c->argv[2],NULL);
}
static void setexCommand(redisClient *c) {
    setGenericCommand(c,0,c->argv[1],c->argv[3],c->argv[2]);
}
static int getGenericCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;
    if (o->type != REDIS_STRING) {
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}
static void getCommand(redisClient *c) {
    getGenericCommand(c);
}
static void getsetCommand(redisClient *c) {
    if (getGenericCommand(c) == REDIS_ERR) return;
    dbReplace(c->db,c->argv[1],c->argv[2]);
    incrRefCount(c->argv[2]);
    server.dirty++;
    removeExpire(c->db,c->argv[1]);
}
static void mgetCommand(redisClient *c) {
    int j;
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->argc-1));
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}
static void msetGenericCommand(redisClient *c, int nx) {
    int j, busykeys = 0;
    if ((c->argc % 2) == 0) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments for MSET\r\n"));
        return;
    }
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
    }
    if (busykeys) {
        addReply(c, shared.czero);
        return;
    }
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        dbReplace(c->db,c->argv[j],c->argv[j+1]);
        incrRefCount(c->argv[j+1]);
        removeExpire(c->db,c->argv[j]);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}
static void msetCommand(redisClient *c) {
    msetGenericCommand(c,0);
}
static void msetnxCommand(redisClient *c) {
    msetGenericCommand(c,1);
}
static void incrDecrCommand(redisClient *c, long long incr) {
    long long value;
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,REDIS_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != REDIS_OK) return;
    value += incr;
    o = createStringObjectFromLongLong(value);
    dbReplace(c->db,c->argv[1],o);
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,o);
    addReply(c,shared.crlf);
}
static void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}
static void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}
static void incrbyCommand(redisClient *c) {
    long long incr;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,incr);
}
static void decrbyCommand(redisClient *c) {
    long long incr;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != REDIS_OK) return;
    incrDecrCommand(c,-incr);
}
static void appendCommand(redisClient *c) {
    int retval;
    size_t totlen;
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        retval = dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dbReplace(c->db,c->argv[1],o);
        }
        if (c->argv[2]->encoding == REDIS_ENCODING_RAW) {
            o->ptr = sdscatlen(o->ptr,
                c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        } else {
            o->ptr = sdscatprintf(o->ptr, "%ld",
                (unsigned long) c->argv[2]->ptr);
        }
        totlen = sdslen(o->ptr);
    }
    server.dirty++;
    addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",(unsigned long)totlen));
}
static void substrCommand(redisClient *c) {
    robj *o;
    long start = atoi(c->argv[2]->ptr);
    long end = atoi(c->argv[3]->ptr);
    size_t rangelen, strlen;
    sds range;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_STRING)) return;
    o = getDecodedObject(o);
    strlen = sdslen(o->ptr);
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > end || (size_t)start >= strlen) {
        addReply(c,shared.nullbulk);
        decrRefCount(o);
        return;
    }
    if ((size_t)end >= strlen) end = strlen-1;
    rangelen = (end-start)+1;
    addReplySds(c,sdscatprintf(sdsempty(),"$%zu\r\n",rangelen));
    range = sdsnewlen((char*)o->ptr+start,rangelen);
    addReplySds(c,range);
    addReply(c,shared.crlf);
    decrRefCount(o);
}
static void delCommand(redisClient *c) {
    int deleted = 0, j;
    for (j = 1; j < c->argc; j++) {
        if (dbDelete(c->db,c->argv[j])) {
            touchWatchedKey(c->db,c->argv[j]);
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);
}
static void existsCommand(redisClient *c) {
    expireIfNeeded(c->db,c->argv[1]);
    if (dbExists(c->db,c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}
static void selectCommand(redisClient *c) {
    int id = atoi(c->argv[1]->ptr);
    if (selectDb(c,id) == REDIS_ERR) {
        addReplySds(c,sdsnew("-ERR invalid DB index\r\n"));
    } else {
        addReply(c,shared.ok);
    }
}
static void randomkeyCommand(redisClient *c) {
    robj *key;
    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }
    addReplyBulk(c,key);
    decrRefCount(key);
}
static void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern);
    unsigned long numkeys = 0;
    robj *lenobj = createObject(REDIS_STRING,NULL);
    di = dictGetIterator(c->db->dict);
    addReply(c,lenobj);
    decrRefCount(lenobj);
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetEntryKey(de);
        robj *keyobj;
        if ((pattern[0] == '*' && pattern[1] == '\0') ||
            stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",numkeys);
}
static void dbsizeCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",dictSize(c->db->dict)));
}
static void lastsaveCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",server.lastsave));
}
static void typeCommand(redisClient *c) {
    robj *o;
    char *type;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "+none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "+string"; break;
        case REDIS_LIST: type = "+list"; break;
        case REDIS_SET: type = "+set"; break;
        case REDIS_ZSET: type = "+zset"; break;
        case REDIS_HASH: type = "+hash"; break;
        default: type = "+unknown"; break;
        }
    }
    addReplySds(c,sdsnew(type));
    addReply(c,shared.crlf);
}
static void saveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background save in progress\r\n"));
        return;
    }
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
static void bgsaveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background save already in progress\r\n"));
        return;
    }
    if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
        char *status = "+Background saving started\r\n";
        addReplySds(c,sdsnew(status));
    } else {
        addReply(c,shared.err);
    }
}
static void shutdownCommand(redisClient *c) {
    if (prepareForShutdown() == REDIS_OK)
        exit(0);
    addReplySds(c, sdsnew("-ERR Errors trying to SHUTDOWN. Check logs.\r\n"));
}
static void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
    incrRefCount(o);
    deleteIfVolatile(c->db,c->argv[2]);
    if (dbAdd(c->db,c->argv[2],o) == REDIS_ERR) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dbReplace(c->db,c->argv[2],o);
    }
    dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[2]);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}
static void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}
static void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}
static void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    src = c->db;
    srcid = c->db->id;
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid);
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    deleteIfVolatile(dst,c->argv[1]);
    if (dbAdd(dst,c->argv[1],o) == REDIS_ERR) {
        addReply(c,shared.czero);
        return;
    }
    incrRefCount(o);
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}
static void lPush(robj *subject, robj *value, int where) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),pos);
        decrRefCount(value);
    } else if (subject->encoding == REDIS_ENCODING_LIST) {
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr,value);
        } else {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}
static robj *lPop(robj *subject, int where) {
    robj *value = NULL;
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        int pos = (where == REDIS_HEAD) ? 0 : -1;
        p = ziplistIndex(subject->ptr,pos);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
    } else if (subject->encoding == REDIS_ENCODING_LIST) {
        list *list = subject->ptr;
        listNode *ln;
        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        } else {
            ln = listLast(list);
        }
        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}
static unsigned long lLength(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LIST) {
        return listLength((list*)subject->ptr);
    } else {
        redisPanic("Unknown list encoding");
    }
}
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction;
    unsigned char *zi;
    listNode *ln;
} lIterator;
typedef struct {
    lIterator *li;
    unsigned char *zi;
    listNode *ln;
} lEntry;
static lIterator *lInitIterator(robj *subject, int index, unsigned char direction) {
    lIterator *li = zmalloc(sizeof(lIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        li->zi = ziplistIndex(subject->ptr,index);
    } else if (li->encoding == REDIS_ENCODING_LIST) {
        li->ln = listIndex(subject->ptr,index);
    } else {
        redisPanic("Unknown list encoding");
    }
    return li;
}
static void lReleaseIterator(lIterator *li) {
    zfree(li);
}
static int lNext(lIterator *li, lEntry *entry) {
    entry->li = li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        entry->zi = li->zi;
        if (entry->zi != NULL) {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    } else if (li->encoding == REDIS_ENCODING_LIST) {
        entry->ln = li->ln;
        if (entry->ln != NULL) {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return 0;
}
static robj *lGet(lEntry *entry) {
    lIterator *li = entry->li;
    robj *value = NULL;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        redisAssert(entry->zi != NULL);
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LIST) {
        redisAssert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}
static int lEqual(lEntry *entry, robj *o) {
    lIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        redisAssert(o->encoding == REDIS_ENCODING_RAW);
        return ziplistCompare(entry->zi,o->ptr,sdslen(o->ptr));
    } else if (li->encoding == REDIS_ENCODING_LIST) {
        return equalStringObjects(o,listNodeValue(entry->ln));
    } else {
        redisPanic("Unknown list encoding");
    }
}
static void lDelete(lEntry *entry) {
    lIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = entry->zi;
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    } else if (entry->li->encoding == REDIS_ENCODING_LIST) {
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    } else {
        redisPanic("Unknown list encoding");
    }
}
static void pushGenericCommand(redisClient *c, int where) {
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);
    if (lobj == NULL) {
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
<<<<<<< HEAD
        lobj = createZiplistObject();
        dictAdd(c->db->dict,c->argv[1],lobj);
        incrRefCount(c->argv[1]);
||||||| d55d5c5dd
        lobj = createListObject();
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            listAddNodeHead(list,c->argv[2]);
        } else {
            listAddNodeTail(list,c->argv[2]);
        }
        dictAdd(c->db->dict,c->argv[1],lobj);
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
=======
        lobj = createListObject();
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            listAddNodeHead(list,c->argv[2]);
        } else {
            listAddNodeTail(list,c->argv[2]);
        }
        incrRefCount(c->argv[2]);
        dbAdd(c->db,c->argv[1],lobj);
>>>>>>> 22194a7f
    } else {
        if (lobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
    }
    lPush(lobj,c->argv[2],where);
    addReplyLongLong(c,lLength(lobj));
    server.dirty++;
}
static void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}
static void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}
static void llenCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    addReplyUlong(c,lLength(o));
}
static void lindexCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    int index = atoi(c->argv[2]->ptr);
    robj *value = NULL;
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        p = ziplistIndex(o->ptr,index);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else if (o->encoding == REDIS_ENCODING_LIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL) {
            value = listNodeValue(ln);
            addReplyBulk(c,value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}
static void lsetCommand(redisClient *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    int index = atoi(c->argv[2]->ptr);
    robj *value = c->argv[3];
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl,index);
        if (p == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            o->ptr = ziplistDelete(o->ptr,&p);
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,sdslen(value->ptr));
            decrRefCount(value);
            addReply(c,shared.ok);
            server.dirty++;
        }
    } else if (o->encoding == REDIS_ENCODING_LIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            decrRefCount((robj*)listNodeValue(ln));
            listNodeValue(ln) = value;
            incrRefCount(value);
            addReply(c,shared.ok);
            server.dirty++;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}
static void popGenericCommand(redisClient *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    robj *value = lPop(o,where);
    if (value == NULL) {
        addReply(c,shared.nullbulk);
    } else {
<<<<<<< HEAD
        addReplyBulk(c,value);
        decrRefCount(value);
        if (lLength(o) == 0) deleteKey(c->db,c->argv[1]);
||||||| d55d5c5dd
        robj *ele = listNodeValue(ln);
        addReplyBulk(c,ele);
        listDelNode(list,ln);
        if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
=======
        robj *ele = listNodeValue(ln);
        addReplyBulk(c,ele);
        listDelNode(list,ln);
        if (listLength(list) == 0) dbDelete(c->db,c->argv[1]);
>>>>>>> 22194a7f
        server.dirty++;
    }
}
static void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}
static void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}
static void lrangeCommand(redisClient *c) {
    robj *o, *value;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int rangelen, j;
    lEntry entry;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_LIST)) return;
    llen = lLength(o);
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",rangelen));
    lIterator *li = lInitIterator(o,start,REDIS_TAIL);
    for (j = 0; j < rangelen; j++) {
        redisAssert(lNext(li,&entry));
        value = lGet(&entry);
        addReplyBulk(c,value);
        decrRefCount(value);
    }
    lReleaseIterator(li);
}
static void ltrimCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int j, ltrim, rtrim;
    list *list;
    listNode *ln;
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    llen = lLength(o);
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > end || start >= llen) {
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        o->ptr = ziplistDeleteRange(o->ptr,0,ltrim);
        o->ptr = ziplistDeleteRange(o->ptr,-rtrim,rtrim);
    } else if (o->encoding == REDIS_ENCODING_LIST) {
        list = o->ptr;
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
<<<<<<< HEAD
    if (lLength(o) == 0) deleteKey(c->db,c->argv[1]);
||||||| d55d5c5dd
    for (j = 0; j < rtrim; j++) {
        ln = listLast(list);
        listDelNode(list,ln);
    }
    if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
=======
    for (j = 0; j < rtrim; j++) {
        ln = listLast(list);
        listDelNode(list,ln);
    }
    if (listLength(list) == 0) dbDelete(c->db,c->argv[1]);
>>>>>>> 22194a7f
    server.dirty++;
    addReply(c,shared.ok);
}
static void lremCommand(redisClient *c) {
    robj *subject, *obj = c->argv[3];
    int toremove = atoi(c->argv[2]->ptr);
    int removed = 0;
    lEntry entry;
    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,REDIS_LIST)) return;
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);
    lIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = lInitIterator(subject,-1,REDIS_HEAD);
    } else {
        li = lInitIterator(subject,0,REDIS_TAIL);
    }
    while (lNext(li,&entry)) {
        if (lEqual(&entry,obj)) {
            lDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
<<<<<<< HEAD
    lReleaseIterator(li);
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);
    if (lLength(subject) == 0) deleteKey(c->db,c->argv[1]);
||||||| d55d5c5dd
    if (listLength(list) == 0) deleteKey(c->db,c->argv[1]);
=======
    if (listLength(list) == 0) dbDelete(c->db,c->argv[1]);
>>>>>>> 22194a7f
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",removed));
}
static void rpoplpushcommand(redisClient *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,REDIS_LIST)) return;
    if (lLength(sobj) == 0) {
        addReply(c,shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        if (dobj && checkType(c,dobj,REDIS_LIST)) return;
        value = lPop(sobj,REDIS_TAIL);
<<<<<<< HEAD
        if (!handleClientsWaitingListPush(c,c->argv[2],value)) {
            if (!dobj) {
                dobj = createZiplistObject();
                dictAdd(c->db->dict,c->argv[2],dobj);
                incrRefCount(c->argv[2]);
||||||| d55d5c5dd
        if (!handleClientsWaitingListPush(c,c->argv[2],ele)) {
            if (dobj == NULL) {
                dobj = createListObject();
                dictAdd(c->db->dict,c->argv[2],dobj);
                incrRefCount(c->argv[2]);
=======
        if (!handleClientsWaitingListPush(c,c->argv[2],ele)) {
            if (dobj == NULL) {
                dobj = createListObject();
                dbAdd(c->db,c->argv[2],dobj);
>>>>>>> 22194a7f
            }
            lPush(dobj,value,REDIS_HEAD);
        }
        addReplyBulk(c,value);
<<<<<<< HEAD
        decrRefCount(value);
        if (lLength(sobj) == 0) deleteKey(c->db,c->argv[1]);
||||||| d55d5c5dd
        listDelNode(srclist,ln);
        if (listLength(srclist) == 0) deleteKey(c->db,c->argv[1]);
=======
        listDelNode(srclist,ln);
        if (listLength(srclist) == 0) dbDelete(c->db,c->argv[1]);
>>>>>>> 22194a7f
        server.dirty++;
    }
}
static void saddCommand(redisClient *c) {
    robj *set;
    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = createSetObject();
        dbAdd(c->db,c->argv[1],set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (dictAdd(set->ptr,c->argv[2],NULL) == DICT_OK) {
        incrRefCount(c->argv[2]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}
static void sremCommand(redisClient *c) {
    robj *set;
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    if (dictDelete(set->ptr,c->argv[2]) == DICT_OK) {
        server.dirty++;
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) dbDelete(c->db,c->argv[1]);
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}
static void smoveCommand(redisClient *c) {
    robj *srcset, *dstset;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    if (srcset == NULL || srcset->type != REDIS_SET) {
        addReply(c, srcset ? shared.wrongtypeerr : shared.czero);
        return;
    }
    if (dstset && dstset->type != REDIS_SET) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    if (dictDelete(srcset->ptr,c->argv[3]) == DICT_ERR) {
        addReply(c,shared.czero);
        return;
    }
    if (dictSize((dict*)srcset->ptr) == 0 && srcset != dstset)
        dbDelete(c->db,c->argv[1]);
    server.dirty++;
    if (!dstset) {
        dstset = createSetObject();
        dbAdd(c->db,c->argv[2],dstset);
    }
    if (dictAdd(dstset->ptr,c->argv[3],NULL) == DICT_OK)
        incrRefCount(c->argv[3]);
    addReply(c,shared.cone);
}
static void sismemberCommand(redisClient *c) {
    robj *set;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    if (dictFind(set->ptr,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}
static void scardCommand(redisClient *c) {
    robj *o;
    dict *s;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;
    s = o->ptr;
    addReplyUlong(c,dictSize(s));
}
static void spopCommand(redisClient *c) {
    robj *set;
    dictEntry *de;
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);
        addReplyBulk(c,ele);
        dictDelete(set->ptr,ele);
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) dbDelete(c->db,c->argv[1]);
        server.dirty++;
    }
}
static void srandmemberCommand(redisClient *c) {
    robj *set;
    dictEntry *de;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);
        addReplyBulk(c,ele);
    }
}
static int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    dict **d1 = (void*) s1, **d2 = (void*) s2;
    return dictSize(*d1)-dictSize(*d2);
}
static void sinterGenericCommand(redisClient *c, robj **setskeys, unsigned long setsnum, robj *dstkey) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *lenobj = NULL, *dstset = NULL;
    unsigned long j, cardinality = 0;
    for (j = 0; j < setsnum; j++) {
        robj *setobj;
        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            zfree(dv);
            if (dstkey) {
                if (dbDelete(c->db,dstkey))
                    server.dirty++;
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }
    qsort(dv,setsnum,sizeof(dict*),qsortCompareSetsByCardinality);
    if (!dstkey) {
        lenobj = createObject(REDIS_STRING,NULL);
        addReply(c,lenobj);
        decrRefCount(lenobj);
    } else {
        dstset = createSetObject();
    }
    di = dictGetIterator(dv[0]);
    while((de = dictNext(di)) != NULL) {
        robj *ele;
        for (j = 1; j < setsnum; j++)
            if (dictFind(dv[j],dictGetEntryKey(de)) == NULL) break;
        if (j != setsnum)
            continue;
        ele = dictGetEntryKey(de);
        if (!dstkey) {
            addReplyBulk(c,ele);
            cardinality++;
        } else {
            dictAdd(dstset->ptr,ele,NULL);
            incrRefCount(ele);
        }
    }
    dictReleaseIterator(di);
    if (dstkey) {
        dbDelete(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    } else {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    }
    zfree(dv);
}
static void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}
static void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2
static void sunionDiffGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey, int op) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *dstset = NULL;
    int j, cardinality = 0;
    for (j = 0; j < setsnum; j++) {
        robj *setobj;
        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            dv[j] = NULL;
            continue;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }
    dstset = createSetObject();
    for (j = 0; j < setsnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !dv[j]) break;
        if (!dv[j]) continue;
        di = dictGetIterator(dv[j]);
        while((de = dictNext(di)) != NULL) {
            robj *ele;
            ele = dictGetEntryKey(de);
            if (op == REDIS_OP_UNION || j == 0) {
                if (dictAdd(dstset->ptr,ele,NULL) == DICT_OK) {
                    incrRefCount(ele);
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (dictDelete(dstset->ptr,ele) == DICT_OK) {
                    cardinality--;
                }
            }
        }
        dictReleaseIterator(di);
        if (op == REDIS_OP_DIFF && cardinality == 0) break;
    }
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        di = dictGetIterator(dstset->ptr);
        while((de = dictNext(di)) != NULL) {
            robj *ele;
            ele = dictGetEntryKey(de);
            addReplyBulk(c,ele);
        }
        dictReleaseIterator(di);
        decrRefCount(dstset);
    } else {
        dbDelete(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    }
    zfree(dv);
}
static void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}
static void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}
static void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}
static void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}
static zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
    zskiplistNode *zn = zmalloc(sizeof(*zn));
    zn->forward = zmalloc(sizeof(zskiplistNode*) * level);
    if (level > 1)
        zn->span = zmalloc(sizeof(unsigned int) * (level - 1));
    else
        zn->span = NULL;
    zn->score = score;
    zn->obj = obj;
    return zn;
}
static zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;
    zsl = zmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->forward[j] = NULL;
        if (j < ZSKIPLIST_MAXLEVEL-1)
            zsl->header->span[j] = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}
static void zslFreeNode(zskiplistNode *node) {
    decrRefCount(node->obj);
    zfree(node->forward);
    zfree(node->span);
    zfree(node);
}
static void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->forward[0], *next;
    zfree(zsl->header->forward);
    zfree(zsl->header->span);
    zfree(zsl->header);
    while(node) {
        next = node->forward[0];
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}
static int zslRandomLevel(void) {
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}
static void zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0))) {
            rank[i] += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }
    level = zslRandomLevel();
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->span[i-1] = zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level,score,obj);
    for (i = 0; i < level; i++) {
        x->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = x;
        if (i > 0) {
            x->span[i-1] = update[i]->span[i-1] - (rank[0] - rank[i]);
            update[i]->span[i-1] = (rank[0] - rank[i]) + 1;
        }
    }
    for (i = level; i < zsl->level; i++) {
        update[i]->span[i-1]++;
    }
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->forward[0])
        x->forward[0]->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
}
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->forward[i] == x) {
            if (i > 0) {
                update[i]->span[i-1] += x->span[i-1] - 1;
            }
            update[i]->forward[i] = x->forward[i];
        } else {
            update[i]->span[i-1] -= 1;
        }
    }
    if (x->forward[0]) {
        x->forward[0]->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while(zsl->level > 1 && zsl->header->forward[zsl->level-1] == NULL)
        zsl->level--;
    zsl->length--;
}
static int zslDelete(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0)))
            x = x->forward[i];
        update[i] = x;
    }
    x = x->forward[0];
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0;
    }
    return 0;
}
static unsigned long zslDeleteRangeByScore(zskiplist *zsl, double min, double max, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < min)
            x = x->forward[i];
        update[i] = x;
    }
    x = x->forward[0];
    while (x && x->score <= max) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}
static unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i > 0 ? x->span[i-1] : 1)) < start) {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }
    traversed++;
    x = x->forward[0];
    while (x && traversed <= end) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}
static zskiplistNode *zslFirstWithScore(zskiplist *zsl, double score) {
    zskiplistNode *x;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < score)
            x = x->forward[i];
    }
    return x->forward[0];
}
static unsigned long zslGetRank(zskiplist *zsl, double score, robj *o) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,o) <= 0))) {
            rank += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        if (x->obj && equalStringObjects(x->obj,o)) {
            return rank;
        }
    }
    return 0;
}
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i>0 ? x->span[i-1] : 1)) <= rank)
        {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}
static void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double scoreval, int doincrement) {
    robj *zsetobj;
    zset *zs;
    double *score;
    if (isnan(scoreval)) {
        addReplySds(c,sdsnew("-ERR provide score is Not A Number (nan)\r\n"));
        return;
    }
    zsetobj = lookupKeyWrite(c->db,key);
    if (zsetobj == NULL) {
        zsetobj = createZsetObject();
        dbAdd(c->db,key,zsetobj);
    } else {
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    zs = zsetobj->ptr;
    score = zmalloc(sizeof(double));
    if (doincrement) {
        dictEntry *de;
        de = dictFind(zs->dict,ele);
        if (de) {
            double *oldscore = dictGetEntryVal(de);
            *score = *oldscore + scoreval;
        } else {
            *score = scoreval;
        }
        if (isnan(*score)) {
            addReplySds(c,
                sdsnew("-ERR resulting score is Not A Number (nan)\r\n"));
            zfree(score);
            return;
        }
    } else {
        *score = scoreval;
    }
    if (dictAdd(zs->dict,ele,score) == DICT_OK) {
        incrRefCount(ele);
        zslInsert(zs->zsl,*score,ele);
        incrRefCount(ele);
        server.dirty++;
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.cone);
    } else {
        dictEntry *de;
        double *oldscore;
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        oldscore = dictGetEntryVal(de);
        if (*score != *oldscore) {
            int deleted;
            deleted = zslDelete(zs->zsl,*oldscore,ele);
            redisAssert(deleted != 0);
            zslInsert(zs->zsl,*score,ele);
            incrRefCount(ele);
            dictReplace(zs->dict,ele,score);
            server.dirty++;
        } else {
            zfree(score);
        }
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.czero);
    }
}
static void zaddCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c, c->argv[2], &scoreval, NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}
static void zincrbyCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c, c->argv[2], &scoreval, NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}
static void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    dictEntry *de;
    double *oldscore;
    int deleted;
    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    oldscore = dictGetEntryVal(de);
    deleted = zslDelete(zs->zsl,*oldscore,c->argv[2]);
    redisAssert(deleted != 0);
    dictDelete(zs->dict,c->argv[2]);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}
static void zremrangebyscoreCommand(redisClient *c) {
    double min;
    double max;
    long deleted;
    robj *zsetobj;
    zset *zs;
    if ((getDoubleFromObjectOrReply(c, c->argv[2], &min, NULL) != REDIS_OK) ||
        (getDoubleFromObjectOrReply(c, c->argv[3], &max, NULL) != REDIS_OK)) return;
    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    deleted = zslDeleteRangeByScore(zs->zsl,min,max,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}
static void zremrangebyrankCommand(redisClient *c) {
    long start;
    long end;
    int llen;
    long deleted;
    robj *zsetobj;
    zset *zs;
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;
    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    llen = zs->zsl->length;
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > end || start >= llen) {
        addReply(c,shared.czero);
        return;
    }
    if (end >= llen) end = llen-1;
    deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c, deleted);
}
typedef struct {
    dict *dict;
    double weight;
} zsetopsrc;
static int qsortCompareZsetopsrcByCardinality(const void *s1, const void *s2) {
    zsetopsrc *d1 = (void*) s1, *d2 = (void*) s2;
    unsigned long size1, size2;
    size1 = d1->dict ? dictSize(d1->dict) : 0;
    size2 = d2->dict ? dictSize(d2->dict) : 0;
    return size1 - size2;
}
#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetEntryVal(_e) == NULL ? 1.0 : *(double*)dictGetEntryVal(_e))
inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        redisPanic("Unknown ZUNION/INTER aggregate type");
    }
}
static void zunionInterGenericCommand(redisClient *c, robj *dstkey, int op) {
    int i, j, setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    robj *dstobj;
    zset *dstzset;
    dictIterator *di;
    dictEntry *de;
    setnum = atoi(c->argv[2]->ptr);
    if (setnum < 1) {
        addReplySds(c,sdsnew("-ERR at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE\r\n"));
        return;
    }
    if (3+setnum > c->argc) {
        addReply(c,shared.syntaxerr);
        return;
    }
    src = zmalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++) {
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (!obj) {
            src[i].dict = NULL;
        } else {
            if (obj->type == REDIS_ZSET) {
                src[i].dict = ((zset*)obj->ptr)->dict;
            } else if (obj->type == REDIS_SET) {
                src[i].dict = (obj->ptr);
            } else {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }
        }
        src[i].weight = 1.0;
    }
    if (j < c->argc) {
        int remaining = c->argc - j;
        while (remaining) {
            if (remaining >= (setnum + 1) && !strcasecmp(c->argv[j]->ptr,"weights")) {
                j++; remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c, c->argv[j], &src[i].weight, NULL) != REDIS_OK)
                        return;
                }
            } else if (remaining >= 2 && !strcasecmp(c->argv[j]->ptr,"aggregate")) {
                j++; remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }
    qsort(src,setnum,sizeof(zsetopsrc),qsortCompareZsetopsrcByCardinality);
    dstobj = createZsetObject();
    dstzset = dstobj->ptr;
    if (op == REDIS_OP_INTER) {
        if (src[0].dict && dictSize(src[0].dict) > 0) {
            di = dictGetIterator(src[0].dict);
            while((de = dictNext(di)) != NULL) {
                double *score = zmalloc(sizeof(double)), value;
                *score = src[0].weight * zunionInterDictValue(de);
                for (j = 1; j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    } else {
                        break;
                    }
                }
                if (j != setnum) {
                    zfree(score);
                } else {
                    robj *o = dictGetEntryKey(de);
                    dictAdd(dstzset->dict,o,score);
                    incrRefCount(o);
                    zslInsert(dstzset->zsl,*score,o);
                    incrRefCount(o);
                }
            }
            dictReleaseIterator(di);
        }
    } else if (op == REDIS_OP_UNION) {
        for (i = 0; i < setnum; i++) {
            if (!src[i].dict) continue;
            di = dictGetIterator(src[i].dict);
            while((de = dictNext(di)) != NULL) {
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL) continue;
                double *score = zmalloc(sizeof(double)), value;
                *score = src[i].weight * zunionInterDictValue(de);
                for (j = (i+1); j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    }
                }
                robj *o = dictGetEntryKey(de);
                dictAdd(dstzset->dict,o,score);
                incrRefCount(o);
                zslInsert(dstzset->zsl,*score,o);
                incrRefCount(o);
            }
            dictReleaseIterator(di);
        }
    } else {
        redisAssert(op == REDIS_OP_INTER || op == REDIS_OP_UNION);
    }
    dbDelete(c->db,dstkey);
    if (dstzset->zsl->length) {
        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c, dstzset->zsl->length);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c, shared.czero);
    }
    zfree(src);
}
static void zunionstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_UNION);
}
static void zinterstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_INTER);
}
static void zrangeGenericCommand(redisClient *c, int reverse) {
    robj *o;
    long start;
    long end;
    int withscores = 0;
    int llen;
    int rangelen, j;
    zset *zsetobj;
    zskiplist *zsl;
    zskiplistNode *ln;
    robj *ele;
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;
    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_ZSET)) return;
    zsetobj = o->ptr;
    zsl = zsetobj->zsl;
    llen = zsl->length;
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;
    if (reverse) {
        ln = start == 0 ? zsl->tail : zslGetElementByRank(zsl, llen-start);
    } else {
        ln = start == 0 ?
            zsl->header->forward[0] : zslGetElementByRank(zsl, start+1);
    }
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",
        withscores ? (rangelen*2) : rangelen));
    for (j = 0; j < rangelen; j++) {
        ele = ln->obj;
        addReplyBulk(c,ele);
        if (withscores)
            addReplyDouble(c,ln->score);
        ln = reverse ? ln->backward : ln->forward[0];
    }
}
static void zrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,0);
}
static void zrevrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,1);
}
static void genericZrangebyscoreCommand(redisClient *c, int justcount) {
    robj *o;
    double min, max;
    int minex = 0, maxex = 0;
    int offset = 0, limit = -1;
    int withscores = 0;
    int badsyntax = 0;
    if (((char*)c->argv[2]->ptr)[0] == '(') {
        min = strtod((char*)c->argv[2]->ptr+1,NULL);
        minex = 1;
    } else {
        min = strtod(c->argv[2]->ptr,NULL);
    }
    if (((char*)c->argv[3]->ptr)[0] == '(') {
        max = strtod((char*)c->argv[3]->ptr+1,NULL);
        maxex = 1;
    } else {
        max = strtod(c->argv[3]->ptr,NULL);
    }
    if (c->argc == 5 || c->argc == 8) {
        if (strcasecmp(c->argv[c->argc-1]->ptr,"withscores") == 0)
            withscores = 1;
        else
            badsyntax = 1;
    }
    if (c->argc != (4 + withscores) && c->argc != (7 + withscores))
        badsyntax = 1;
    if (badsyntax) {
        addReplySds(c,
            sdsnew("-ERR wrong number of arguments for ZRANGEBYSCORE\r\n"));
        return;
    }
    if (c->argc == (7 + withscores) && strcasecmp(c->argv[4]->ptr,"limit")) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == (7 + withscores)) {
        offset = atoi(c->argv[5]->ptr);
        limit = atoi(c->argv[6]->ptr);
        if (offset < 0) offset = 0;
    }
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,justcount ? shared.czero : shared.emptymultibulk);
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            robj *ele, *lenobj = NULL;
            unsigned long rangelen = 0;
            ln = zslFirstWithScore(zsl,min);
            while (minex && ln && ln->score == min) ln = ln->forward[0];
            if (ln == NULL) {
                addReply(c,justcount ? shared.czero : shared.emptymultibulk);
                return;
            }
            if (!justcount) {
                lenobj = createObject(REDIS_STRING,NULL);
                addReply(c,lenobj);
                decrRefCount(lenobj);
            }
            while(ln && (maxex ? (ln->score < max) : (ln->score <= max))) {
                if (offset) {
                    offset--;
                    ln = ln->forward[0];
                    continue;
                }
                if (limit == 0) break;
                if (!justcount) {
                    ele = ln->obj;
                    addReplyBulk(c,ele);
                    if (withscores)
                        addReplyDouble(c,ln->score);
                }
                ln = ln->forward[0];
                rangelen++;
                if (limit > 0) limit--;
            }
            if (justcount) {
                addReplyLongLong(c,(long)rangelen);
            } else {
                lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",
                     withscores ? (rangelen*2) : rangelen);
            }
        }
    }
}
static void zrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0);
}
static void zcountCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,1);
}
static void zcardCommand(redisClient *c) {
    robj *o;
    zset *zs;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;
    zs = o->ptr;
    addReplyUlong(c,zs->zsl->length);
}
static void zscoreCommand(redisClient *c) {
    robj *o;
    zset *zs;
    dictEntry *de;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;
    zs = o->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
    } else {
        double *score = dictGetEntryVal(de);
        addReplyDouble(c,*score);
    }
}
static void zrankGenericCommand(redisClient *c, int reverse) {
    robj *o;
    zset *zs;
    zskiplist *zsl;
    dictEntry *de;
    unsigned long rank;
    double *score;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;
    zs = o->ptr;
    zsl = zs->zsl;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
        return;
    }
    score = dictGetEntryVal(de);
    rank = zslGetRank(zsl, *score, c->argv[2]);
    if (rank) {
        if (reverse) {
            addReplyLongLong(c, zsl->length - rank);
        } else {
            addReplyLongLong(c, rank-1);
        }
    } else {
        addReply(c,shared.nullbulk);
    }
}
static void zrankCommand(redisClient *c) {
    zrankGenericCommand(c, 0);
}
static void zrevrankCommand(redisClient *c) {
    zrankGenericCommand(c, 1);
}
#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2
static void hashTryConversion(robj *subject, robj **argv, int start, int end) {
    int i;
    if (subject->encoding != REDIS_ENCODING_ZIPMAP) return;
    for (i = start; i <= end; i++) {
        if (argv[i]->encoding == REDIS_ENCODING_RAW &&
            sdslen(argv[i]->ptr) > server.hash_max_zipmap_value)
        {
            convertToRealHash(subject);
            return;
        }
    }
}
static void hashTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}
static robj *hashGet(robj *o, robj *key) {
    robj *value = NULL;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        unsigned char *v;
        unsigned int vlen;
        key = getDecodedObject(key);
        if (zipmapGet(o->ptr,key->ptr,sdslen(key->ptr),&v,&vlen)) {
            value = createStringObject((char*)v,vlen);
        }
        decrRefCount(key);
    } else {
        dictEntry *de = dictFind(o->ptr,key);
        if (de != NULL) {
            value = dictGetEntryVal(de);
            incrRefCount(value);
        }
    }
    return value;
}
static int hashExists(robj *o, robj *key) {
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        if (zipmapExists(o->ptr,key->ptr,sdslen(key->ptr))) {
            decrRefCount(key);
            return 1;
        }
        decrRefCount(key);
    } else {
        if (dictFind(o->ptr,key) != NULL) {
            return 1;
        }
    }
    return 0;
}
static int hashSet(robj *o, robj *key, robj *value) {
    int update = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        value = getDecodedObject(value);
        o->ptr = zipmapSet(o->ptr,
            key->ptr,sdslen(key->ptr),
            value->ptr,sdslen(value->ptr), &update);
        decrRefCount(key);
        decrRefCount(value);
        if (zipmapLen(o->ptr) > server.hash_max_zipmap_entries)
            convertToRealHash(o);
    } else {
        if (dictReplace(o->ptr,key,value)) {
            incrRefCount(key);
        } else {
            update = 1;
        }
        incrRefCount(value);
    }
    return update;
}
static int hashDelete(robj *o, robj *key) {
    int deleted = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        o->ptr = zipmapDel(o->ptr,key->ptr,sdslen(key->ptr), &deleted);
        decrRefCount(key);
    } else {
        deleted = dictDelete((dict*)o->ptr,key) == DICT_OK;
        if (deleted && htNeedsResize(o->ptr)) dictResize(o->ptr);
    }
    return deleted;
}
static unsigned long hashLength(robj *o) {
    return (o->encoding == REDIS_ENCODING_ZIPMAP) ?
        zipmapLen((unsigned char*)o->ptr) : dictSize((dict*)o->ptr);
}
typedef struct {
    int encoding;
    unsigned char *zi;
    unsigned char *zk, *zv;
    unsigned int zklen, zvlen;
    dictIterator *di;
    dictEntry *de;
} hashIterator;
static hashIterator *hashInitIterator(robj *subject) {
    hashIterator *hi = zmalloc(sizeof(hashIterator));
    hi->encoding = subject->encoding;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        hi->zi = zipmapRewind(subject->ptr);
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        redisAssert(NULL);
    }
    return hi;
}
static void hashReleaseIterator(hashIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }
    zfree(hi);
}
static int hashNext(hashIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if ((hi->zi = zipmapNext(hi->zi, &hi->zk, &hi->zklen,
            &hi->zv, &hi->zvlen)) == NULL) return REDIS_ERR;
    } else {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;
    }
    return REDIS_OK;
}
static robj *hashCurrent(hashIterator *hi, int what) {
    robj *o;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if (what & REDIS_HASH_KEY) {
            o = createStringObject((char*)hi->zk,hi->zklen);
        } else {
            o = createStringObject((char*)hi->zv,hi->zvlen);
        }
    } else {
        if (what & REDIS_HASH_KEY) {
            o = dictGetEntryKey(hi->de);
        } else {
            o = dictGetEntryVal(hi->de);
        }
        incrRefCount(o);
    }
    return o;
}
static robj *hashLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != REDIS_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}
static void hsetCommand(redisClient *c) {
    int update;
    robj *o;
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,3);
    hashTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
    update = hashSet(o,c->argv[2],c->argv[3]);
    addReply(c, update ? shared.czero : shared.cone);
    server.dirty++;
}
static void hsetnxCommand(redisClient *c) {
    robj *o;
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,3);
    if (hashExists(o, c->argv[2])) {
        addReply(c, shared.czero);
    } else {
        hashTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        hashSet(o,c->argv[2],c->argv[3]);
        addReply(c, shared.cone);
        server.dirty++;
    }
}
static void hmsetCommand(redisClient *c) {
    int i;
    robj *o;
    if ((c->argc % 2) == 1) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments for HMSET\r\n"));
        return;
    }
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTryConversion(o,c->argv,2,c->argc-1);
    for (i = 2; i < c->argc; i += 2) {
        hashTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        hashSet(o,c->argv[i],c->argv[i+1]);
    }
    addReply(c, shared.ok);
    server.dirty++;
}
static void hincrbyCommand(redisClient *c) {
    long long value, incr;
    robj *o, *current, *new;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != REDIS_OK) return;
    if ((o = hashLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    if ((current = hashGet(o,c->argv[2])) != NULL) {
        if (getLongLongFromObjectOrReply(c,current,&value,
            "hash value is not an integer") != REDIS_OK) {
            decrRefCount(current);
            return;
        }
        decrRefCount(current);
    } else {
        value = 0;
    }
    value += incr;
    new = createStringObjectFromLongLong(value);
    hashTryObjectEncoding(o,&c->argv[2],NULL);
    hashSet(o,c->argv[2],new);
    decrRefCount(new);
    addReplyLongLong(c,value);
    server.dirty++;
}
static void hgetCommand(redisClient *c) {
    robj *o, *value;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    if ((value = hashGet(o,c->argv[2])) != NULL) {
        addReplyBulk(c,value);
        decrRefCount(value);
    } else {
        addReply(c,shared.nullbulk);
    }
}
static void hmgetCommand(redisClient *c) {
    int i;
    robj *o, *value;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o != NULL && o->type != REDIS_HASH) {
        addReply(c,shared.wrongtypeerr);
    }
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->argc-2));
    for (i = 2; i < c->argc; i++) {
        if (o != NULL && (value = hashGet(o,c->argv[i])) != NULL) {
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    }
}
static void hdelCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    if (hashDelete(o,c->argv[2])) {
        if (hashLength(o) == 0) dbDelete(c->db,c->argv[1]);
        addReply(c,shared.cone);
        server.dirty++;
    } else {
        addReply(c,shared.czero);
    }
}
static void hlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    addReplyUlong(c,hashLength(o));
}
static void genericHgetallCommand(redisClient *c, int flags) {
    robj *o, *lenobj, *obj;
    unsigned long count = 0;
    hashIterator *hi;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,REDIS_HASH)) return;
    lenobj = createObject(REDIS_STRING,NULL);
    addReply(c,lenobj);
    decrRefCount(lenobj);
    hi = hashInitIterator(o);
    while (hashNext(hi) != REDIS_ERR) {
        if (flags & REDIS_HASH_KEY) {
            obj = hashCurrent(hi,REDIS_HASH_KEY);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {
            obj = hashCurrent(hi,REDIS_HASH_VALUE);
            addReplyBulk(c,obj);
            decrRefCount(obj);
            count++;
        }
    }
    hashReleaseIterator(hi);
    lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",count);
}
static void hkeysCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY);
}
static void hvalsCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}
static void hgetallCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}
static void hexistsCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_HASH)) return;
    addReply(c, hashExists(o,c->argv[2]) ? shared.cone : shared.czero);
}
static void convertToRealHash(robj *o) {
    unsigned char *key, *val, *p, *zm = o->ptr;
    unsigned int klen, vlen;
    dict *dict = dictCreate(&hashDictType,NULL);
    assert(o->type == REDIS_HASH && o->encoding != REDIS_ENCODING_HT);
    p = zipmapRewind(zm);
    while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
        robj *keyobj, *valobj;
        keyobj = createStringObject((char*)key,klen);
        valobj = createStringObject((char*)val,vlen);
        keyobj = tryObjectEncoding(keyobj);
        valobj = tryObjectEncoding(valobj);
        dictAdd(dict,keyobj,valobj);
    }
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
    zfree(zm);
}
static void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    touchWatchedKeysOnFlush(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}
static void flushallCommand(redisClient *c) {
    touchWatchedKeysOnFlush(-1);
    server.dirty += emptyDb();
    addReply(c,shared.ok);
    if (server.bgsavechildpid != -1) {
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    rdbSave(server.dbfilename);
    server.dirty++;
}
static redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    so->type = type;
    so->pattern = pattern;
    return so;
}
static robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p, *f;
    sds spat, ssub;
    robj keyobj, fieldobj, *o;
    int prefixlen, sublen, postfixlen, fieldlen;
    struct {
        long len;
        long free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname, fieldname;
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
        incrRefCount(subst);
        return subst;
    }
    subst = getDecodedObject(subst);
    ssub = subst->ptr;
    if (sdslen(spat)+sdslen(ssub)-1 > REDIS_SORTKEY_MAX) return NULL;
    p = strchr(spat,'*');
    if (!p) {
        decrRefCount(subst);
        return NULL;
    }
    if ((f = strstr(p+1, "->")) != NULL) {
        fieldlen = sdslen(spat)-(f-spat);
        memcpy(fieldname.buf,f+2,fieldlen-1);
        fieldname.len = fieldlen-2;
    } else {
        fieldlen = 0;
    }
    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1)-fieldlen;
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;
    decrRefCount(subst);
    initStaticStringObject(keyobj,((char*)&keyname)+(sizeof(long)*2));
    o = lookupKeyRead(db,&keyobj);
    if (o == NULL) return NULL;
    if (fieldlen > 0) {
        if (o->type != REDIS_HASH || fieldname.len < 1) return NULL;
        initStaticStringObject(fieldobj,((char*)&fieldname)+(sizeof(long)*2));
        o = hashGet(o, &fieldobj);
    } else {
        if (o->type != REDIS_STRING) return NULL;
        incrRefCount(o);
    }
    return o;
}
static int sortCompare(const void *s1, const void *s2) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;
    if (!server.sort_alpha) {
        if (so1->u.score > so2->u.score) {
            cmp = 1;
        } else if (so1->u.score < so2->u.score) {
            cmp = -1;
        } else {
            cmp = 0;
        }
    } else {
        if (server.sort_bypattern) {
            if (!so1->u.cmpobj || !so2->u.cmpobj) {
                if (so1->u.cmpobj == so2->u.cmpobj)
                    cmp = 0;
                else if (so1->u.cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                cmp = strcoll(so1->u.cmpobj->ptr,so2->u.cmpobj->ptr);
            }
        } else {
            cmp = compareStringObjects(so1->obj,so2->obj);
        }
    }
    return server.sort_desc ? -cmp : cmp;
}
static void sortCommand(redisClient *c) {
    list *operations;
    unsigned int outputlen = 0;
    int desc = 0, alpha = 0;
    int limit_start = 0, limit_count = -1, start, end;
    int j, dontsort = 0, vectorlen;
    int getop = 0;
    robj *sortval, *sortby = NULL, *storekey = NULL;
    redisSortObject *vector;
    sortval = lookupKeyRead(c->db,c->argv[1]);
    if (sortval == NULL) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (sortval->type != REDIS_SET && sortval->type != REDIS_LIST &&
        sortval->type != REDIS_ZSET)
    {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    operations = listCreate();
    listSetFreeMethod(operations,zfree);
    j = 2;
    incrRefCount(sortval);
    while(j < c->argc) {
        int leftargs = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"asc")) {
            desc = 0;
        } else if (!strcasecmp(c->argv[j]->ptr,"desc")) {
            desc = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"alpha")) {
            alpha = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"limit") && leftargs >= 2) {
            limit_start = atoi(c->argv[j+1]->ptr);
            limit_count = atoi(c->argv[j+2]->ptr);
            j+=2;
        } else if (!strcasecmp(c->argv[j]->ptr,"store") && leftargs >= 1) {
            storekey = c->argv[j+1];
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"by") && leftargs >= 1) {
            sortby = c->argv[j+1];
            if (strchr(c->argv[j+1]->ptr,'*') == NULL) dontsort = 1;
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_GET,c->argv[j+1]));
            getop++;
            j++;
        } else {
            decrRefCount(sortval);
            listRelease(operations);
            addReply(c,shared.syntaxerr);
            return;
        }
        j++;
    }
    switch(sortval->type) {
    case REDIS_LIST: vectorlen = lLength(sortval); break;
    case REDIS_SET: vectorlen = dictSize((dict*)sortval->ptr); break;
    case REDIS_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
    default: vectorlen = 0; redisPanic("Bad SORT type");
    }
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    j = 0;
    if (sortval->type == REDIS_LIST) {
        lIterator *li = lInitIterator(sortval,0,REDIS_TAIL);
        lEntry entry;
        while(lNext(li,&entry)) {
            vector[j].obj = lGet(&entry);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        lReleaseIterator(li);
    } else {
        dict *set;
        dictIterator *di;
        dictEntry *setele;
        if (sortval->type == REDIS_SET) {
            set = sortval->ptr;
        } else {
            zset *zs = sortval->ptr;
            set = zs->dict;
        }
        di = dictGetIterator(set);
        while((setele = dictNext(di)) != NULL) {
            vector[j].obj = dictGetEntryKey(setele);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        dictReleaseIterator(di);
    }
    redisAssert(j == vectorlen);
    if (dontsort == 0) {
        for (j = 0; j < vectorlen; j++) {
            robj *byval;
            if (sortby) {
                byval = lookupKeyByPattern(c->db,sortby,vector[j].obj);
                if (!byval) continue;
            } else {
                byval = vector[j].obj;
            }
            if (alpha) {
                if (sortby) vector[j].u.cmpobj = getDecodedObject(byval);
            } else {
                if (byval->encoding == REDIS_ENCODING_RAW) {
                    vector[j].u.score = strtod(byval->ptr,NULL);
                } else if (byval->encoding == REDIS_ENCODING_INT) {
                    vector[j].u.score = (long)byval->ptr;
                } else {
                    redisAssert(1 != 1);
                }
            }
            if (sortby) {
                decrRefCount(byval);
            }
        }
    }
    start = (limit_start < 0) ? 0 : limit_start;
    end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1;
    if (start >= vectorlen) {
        start = vectorlen-1;
        end = vectorlen-2;
    }
    if (end >= vectorlen) end = vectorlen-1;
    if (dontsort == 0) {
        server.sort_desc = desc;
        server.sort_alpha = alpha;
        server.sort_bypattern = sortby ? 1 : 0;
        if (sortby && (start != 0 || end != vectorlen-1))
            pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare, start,end);
        else
            qsort(vector,vectorlen,sizeof(redisSortObject),sortCompare);
    }
    outputlen = getop ? getop*(end-start+1) : end-start+1;
    if (storekey == NULL) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",outputlen));
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;
            if (!getop) addReplyBulk(c,vector[j].obj);
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);
                if (sop->type == REDIS_SORT_GET) {
                    if (!val) {
                        addReply(c,shared.nullbulk);
                    } else {
                        addReplyBulk(c,val);
                        decrRefCount(val);
                    }
                } else {
                    redisAssert(sop->type == REDIS_SORT_GET);
                }
            }
        }
    } else {
        robj *sobj = createZiplistObject();
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;
            if (!getop) {
                lPush(sobj,vector[j].obj,REDIS_TAIL);
            } else {
                listRewind(operations,&li);
                while((ln = listNext(&li))) {
                    redisSortOperation *sop = ln->value;
                    robj *val = lookupKeyByPattern(c->db,sop->pattern,
                        vector[j].obj);
                    if (sop->type == REDIS_SORT_GET) {
                        if (!val) val = createStringObject("",0);
                        lPush(sobj,val,REDIS_TAIL);
                        decrRefCount(val);
                    } else {
                        redisAssert(sop->type == REDIS_SORT_GET);
                    }
                }
            }
        }
<<<<<<< HEAD
        if (dictReplace(c->db->dict,storekey,sobj)) {
            incrRefCount(storekey);
        }
||||||| d55d5c5dd
        if (dictReplace(c->db->dict,storekey,listObject)) {
            incrRefCount(storekey);
        }
=======
        dbReplace(c->db,storekey,listObject);
>>>>>>> 22194a7f
        server.dirty += 1+outputlen;
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",outputlen));
    }
    if (sortval->type == REDIS_LIST)
        for (j = 0; j < vectorlen; j++)
            decrRefCount(vector[j].obj);
    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        if (alpha && vector[j].u.cmpobj)
            decrRefCount(vector[j].u.cmpobj);
    }
    zfree(vector);
}
static void bytesToHuman(char *s, unsigned long long n) {
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
static sds genRedisInfoString(void) {
    sds info;
    time_t uptime = time(NULL)-server.stat_starttime;
    int j;
    char hmem[64];
    bytesToHuman(hmem,zmalloc_used_memory());
    info = sdscatprintf(sdsempty(),
        "redis_version:%s\r\n"
        "redis_git_sha1:%s\r\n"
        "redis_git_dirty:%d\r\n"
        "arch_bits:%s\r\n"
        "multiplexing_api:%s\r\n"
        "process_id:%ld\r\n"
        "uptime_in_seconds:%ld\r\n"
        "uptime_in_days:%ld\r\n"
        "connected_clients:%d\r\n"
        "connected_slaves:%d\r\n"
        "blocked_clients:%d\r\n"
        "used_memory:%zu\r\n"
        "used_memory_human:%s\r\n"
        "changes_since_last_save:%lld\r\n"
        "bgsave_in_progress:%d\r\n"
        "last_save_time:%ld\r\n"
        "bgrewriteaof_in_progress:%d\r\n"
        "total_connections_received:%lld\r\n"
        "total_commands_processed:%lld\r\n"
        "expired_keys:%lld\r\n"
        "hash_max_zipmap_entries:%zu\r\n"
        "hash_max_zipmap_value:%zu\r\n"
        "pubsub_channels:%ld\r\n"
        "pubsub_patterns:%u\r\n"
        "vm_enabled:%d\r\n"
        "role:%s\r\n"
        ,REDIS_VERSION,
        REDIS_GIT_SHA1,
        strtol(REDIS_GIT_DIRTY,NULL,10) > 0,
        (sizeof(long) == 8) ? "64" : "32",
        aeGetApiName(),
        (long) getpid(),
        uptime,
        uptime/(3600*24),
        listLength(server.clients)-listLength(server.slaves),
        listLength(server.slaves),
        server.blpop_blocked_clients,
        zmalloc_used_memory(),
        hmem,
        server.dirty,
        server.bgsavechildpid != -1,
        server.lastsave,
        server.bgrewritechildpid != -1,
        server.stat_numconnections,
        server.stat_numcommands,
        server.stat_expiredkeys,
        server.hash_max_zipmap_entries,
        server.hash_max_zipmap_value,
        dictSize(server.pubsub_channels),
        listLength(server.pubsub_patterns),
        server.vm_enabled != 0,
        server.masterhost == NULL ? "master" : "slave"
    );
    if (server.masterhost) {
        info = sdscatprintf(info,
            "master_host:%s\r\n"
            "master_port:%d\r\n"
            "master_link_status:%s\r\n"
            "master_last_io_seconds_ago:%d\r\n"
            ,server.masterhost,
            server.masterport,
            (server.replstate == REDIS_REPL_CONNECTED) ?
                "up" : "down",
            server.master ? ((int)(time(NULL)-server.master->lastinteraction)) : -1
        );
    }
    if (server.vm_enabled) {
        lockThreadedIO();
        info = sdscatprintf(info,
            "vm_conf_max_memory:%llu\r\n"
            "vm_conf_page_size:%llu\r\n"
            "vm_conf_pages:%llu\r\n"
            "vm_stats_used_pages:%llu\r\n"
            "vm_stats_swapped_objects:%llu\r\n"
            "vm_stats_swappin_count:%llu\r\n"
            "vm_stats_swappout_count:%llu\r\n"
            "vm_stats_io_newjobs_len:%lu\r\n"
            "vm_stats_io_processing_len:%lu\r\n"
            "vm_stats_io_processed_len:%lu\r\n"
            "vm_stats_io_active_threads:%lu\r\n"
            "vm_stats_blocked_clients:%lu\r\n"
            ,(unsigned long long) server.vm_max_memory,
            (unsigned long long) server.vm_page_size,
            (unsigned long long) server.vm_pages,
            (unsigned long long) server.vm_stats_used_pages,
            (unsigned long long) server.vm_stats_swapped_objects,
            (unsigned long long) server.vm_stats_swapins,
            (unsigned long long) server.vm_stats_swapouts,
            (unsigned long) listLength(server.io_newjobs),
            (unsigned long) listLength(server.io_processing),
            (unsigned long) listLength(server.io_processed),
            (unsigned long) server.io_active_threads,
            (unsigned long) server.vm_blocked_clients
        );
        unlockThreadedIO();
    }
    for (j = 0; j < server.dbnum; j++) {
        long long keys, vkeys;
        keys = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (keys || vkeys) {
            info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld\r\n",
                j, keys, vkeys);
        }
    }
    return info;
}
static void infoCommand(redisClient *c) {
    sds info = genRedisInfoString();
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}
static void monitorCommand(redisClient *c) {
    if (c->flags & REDIS_SLAVE) return;
    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}
static int removeExpire(redisDb *db, robj *key) {
    if (dictDelete(db->expires,key->ptr) == DICT_OK) {
        return 1;
    } else {
        return 0;
    }
}
static int setExpire(redisDb *db, robj *key, time_t when) {
    sds copy = sdsdup(key->ptr);
    if (dictAdd(db->expires,copy,(void*)when) == DICT_ERR) {
        sdsfree(copy);
        return 0;
    } else {
        return 1;
    }
}
static time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;
    return (time_t) dictGetEntryVal(de);
}
static int expireIfNeeded(redisDb *db, robj *key) {
    time_t when;
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return 0;
    when = (time_t) dictGetEntryVal(de);
    if (time(NULL) <= when) return 0;
    dbDelete(db,key);
    server.stat_expiredkeys++;
    return 1;
}
static int deleteIfVolatile(redisDb *db, robj *key) {
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return 0;
    server.dirty++;
    server.stat_expiredkeys++;
    dictDelete(db->expires,key->ptr);
    return dictDelete(db->dict,key->ptr) == DICT_OK;
}
static void expireGenericCommand(redisClient *c, robj *key, robj *param, long offset) {
    dictEntry *de;
    time_t seconds;
    if (getLongFromObjectOrReply(c, param, &seconds, NULL) != REDIS_OK) return;
    seconds -= offset;
    de = dictFind(c->db->dict,key->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    if (seconds <= 0) {
        if (dbDelete(c->db,key)) server.dirty++;
        addReply(c, shared.cone);
        return;
    } else {
        time_t when = time(NULL)+seconds;
        if (setExpire(c->db,key,when)) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
        return;
    }
}
static void expireCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],0);
}
static void expireatCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],time(NULL));
}
static void ttlCommand(redisClient *c) {
    time_t expire;
    int ttl = -1;
    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = (int) (expire-time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",ttl));
}
static void initClientMultiState(redisClient *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
}
static void freeClientMultiState(redisClient *c) {
    int j;
    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}
static void queueMultiCommand(redisClient *c, struct redisCommand *cmd) {
    multiCmd *mc;
    int j;
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    mc = c->mstate.commands+c->mstate.count;
    mc->cmd = cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    c->mstate.count++;
}
static void multiCommand(redisClient *c) {
    if (c->flags & REDIS_MULTI) {
        addReplySds(c,sdsnew("-ERR MULTI calls can not be nested\r\n"));
        return;
    }
    c->flags |= REDIS_MULTI;
    addReply(c,shared.ok);
}
static void discardCommand(redisClient *c) {
    if (!(c->flags & REDIS_MULTI)) {
        addReplySds(c,sdsnew("-ERR DISCARD without MULTI\r\n"));
        return;
    }
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= (~REDIS_MULTI);
    addReply(c,shared.ok);
}
static void execCommandReplicateMulti(redisClient *c) {
    struct redisCommand *cmd;
    robj *multistring = createStringObject("MULTI",5);
    cmd = lookupCommand("multi");
    if (server.appendonly)
        feedAppendOnlyFile(cmd,c->db->id,&multistring,1);
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,&multistring,1);
    decrRefCount(multistring);
}
static void execCommand(redisClient *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    if (!(c->flags & REDIS_MULTI)) {
        addReplySds(c,sdsnew("-ERR EXEC without MULTI\r\n"));
        return;
    }
    if (c->flags & REDIS_DIRTY_CAS) {
        freeClientMultiState(c);
        initClientMultiState(c);
        c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);
        unwatchAllKeys(c);
        addReply(c,shared.nullmultibulk);
        return;
    }
    execCommandReplicateMulti(c);
    unwatchAllKeys(c);
    orig_argv = c->argv;
    orig_argc = c->argc;
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->mstate.count));
    for (j = 0; j < c->mstate.count; j++) {
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        call(c,c->mstate.commands[j].cmd);
    }
    c->argv = orig_argv;
    c->argc = orig_argc;
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);
    server.dirty++;
}
static void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout) {
    dictEntry *de;
    list *l;
    int j;
    c->blocking_keys = zmalloc(sizeof(robj*)*numkeys);
    c->blocking_keys_num = numkeys;
    c->blockingto = timeout;
    for (j = 0; j < numkeys; j++) {
        c->blocking_keys[j] = keys[j];
        incrRefCount(keys[j]);
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            assert(retval == DICT_OK);
        } else {
            l = dictGetEntryVal(de);
        }
        listAddNodeTail(l,c);
    }
    c->flags |= REDIS_BLOCKED;
    server.blpop_blocked_clients++;
}
static void unblockClientWaitingData(redisClient *c) {
    dictEntry *de;
    list *l;
    int j;
    assert(c->blocking_keys != NULL);
    for (j = 0; j < c->blocking_keys_num; j++) {
        de = dictFind(c->db->blocking_keys,c->blocking_keys[j]);
        assert(de != NULL);
        l = dictGetEntryVal(de);
        listDelNode(l,listSearchKey(l,c));
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,c->blocking_keys[j]);
        decrRefCount(c->blocking_keys[j]);
    }
    zfree(c->blocking_keys);
    c->blocking_keys = NULL;
    c->flags &= (~REDIS_BLOCKED);
    server.blpop_blocked_clients--;
    if (c->querybuf && sdslen(c->querybuf) > 0) processInputBuffer(c);
}
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele) {
    struct dictEntry *de;
    redisClient *receiver;
    list *l;
    listNode *ln;
    de = dictFind(c->db->blocking_keys,key);
    if (de == NULL) return 0;
    l = dictGetEntryVal(de);
    ln = listFirst(l);
    assert(ln != NULL);
    receiver = ln->value;
    addReplySds(receiver,sdsnew("*2\r\n"));
    addReplyBulk(receiver,key);
    addReplyBulk(receiver,ele);
    unblockClientWaitingData(receiver);
    return 1;
}
static void blockingPopGenericCommand(redisClient *c, int where) {
    robj *o;
    time_t timeout;
    int j;
    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != REDIS_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                list *list = o->ptr;
                if (listLength(list) != 0) {
                    robj *argv[2], **orig_argv;
                    int orig_argc;
                    orig_argv = c->argv;
                    orig_argc = c->argc;
                    argv[1] = c->argv[j];
                    c->argv = argv;
                    c->argc = 2;
                    addReplySds(c,sdsnew("*2\r\n"));
                    addReplyBulk(c,argv[1]);
                    popGenericCommand(c,where);
                    c->argv = orig_argv;
                    c->argc = orig_argc;
                    return;
                }
            }
        }
    }
    timeout = strtol(c->argv[c->argc-1]->ptr,NULL,10);
    if (timeout > 0) timeout += time(NULL);
    blockForKeys(c,c->argv+1,c->argc-2,timeout);
}
static void blpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_HEAD);
}
static void brpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_TAIL);
}
static int syncWrite(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nwritten, ret = size;
    time_t start = time(NULL);
    timeout++;
    while(size) {
        if (aeWait(fd,AE_WRITABLE,1000) & AE_WRITABLE) {
            nwritten = write(fd,ptr,size);
            if (nwritten == -1) return -1;
            ptr += nwritten;
            size -= nwritten;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return ret;
}
static int syncRead(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread, totread = 0;
    time_t start = time(NULL);
    timeout++;
    while(size) {
        if (aeWait(fd,AE_READABLE,1000) & AE_READABLE) {
            nread = read(fd,ptr,size);
            if (nread == -1) return -1;
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return totread;
}
static int syncReadLine(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread = 0;
    size--;
    while(size) {
        char c;
        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
    }
    return nread;
}
static void syncCommand(redisClient *c) {
    if (c->flags & REDIS_SLAVE) return;
    if (listLength(c->reply) != 0) {
        addReplySds(c,sdsnew("-ERR SYNC is invalid with pending input\r\n"));
        return;
    }
    redisLog(REDIS_NOTICE,"Slave ask for synchronization");
    if (server.bgsavechildpid != -1) {
        redisClient *slave;
        listNode *ln;
        listIter li;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }
        if (ln) {
            listRelease(c->reply);
            c->reply = listDup(slave->reply);
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else {
        redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC");
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
            addReplySds(c,sdsnew("-ERR Unalbe to perform background save\r\n"));
            return;
        }
        c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
    }
    c->repldbfd = -1;
    c->flags |= REDIS_SLAVE;
    c->slaveseldb = 0;
    listAddNodeTail(server.slaves,c);
    return;
}
static void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;
    if (slave->repldboff == 0) {
        sds bulkcount;
        bulkcount = sdscatprintf(sdsempty(),"$%lld\r\n",(unsigned long long)
            slave->repldbsize);
        if (write(fd,bulkcount,sdslen(bulkcount)) != (signed)sdslen(bulkcount))
        {
            sdsfree(bulkcount);
            freeClient(slave);
            return;
        }
        sdsfree(bulkcount);
    }
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        redisLog(REDIS_VERBOSE,"Write error sending DB to slave: %s",
            strerror(errno));
        freeClient(slave);
        return;
    }
    slave->repldboff += nwritten;
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        slave->replstate = REDIS_REPL_ONLINE;
        if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
            sendReplyToClient, slave) == AE_ERR) {
            freeClient(slave);
            return;
        }
        addReplySds(slave,sdsempty());
        redisLog(REDIS_NOTICE,"Synchronization with slave succeeded");
    }
}
static void updateSlavesWaitingBgsave(int bgsaveerr) {
    listNode *ln;
    int startbgsave = 0;
    listIter li;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            struct redis_stat buf;
            if (bgsaveerr != REDIS_OK) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }
            if ((slave->repldbfd = open(server.dbfilename,O_RDONLY)) == -1 ||
                redis_fstat(slave->repldbfd,&buf) == -1) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                continue;
            }
            slave->repldboff = 0;
            slave->repldbsize = buf.st_size;
            slave->replstate = REDIS_REPL_SEND_BULK;
            aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
            if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                freeClient(slave);
                continue;
            }
        }
    }
    if (startbgsave) {
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            listIter li;
            listRewind(server.slaves,&li);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;
                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}
static int syncWithMaster(void) {
    char buf[1024], tmpfile[256], authcmd[1024];
    long dumpsize;
    int fd = anetTcpConnect(NULL,server.masterhost,server.masterport);
    int dfd, maxtries = 5;
    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    if(server.masterauth) {
     snprintf(authcmd, 1024, "AUTH %s\r\n", server.masterauth);
     if (syncWrite(fd, authcmd, strlen(server.masterauth)+7, 5) == -1) {
            close(fd);
            redisLog(REDIS_WARNING,"Unable to AUTH to MASTER: %s",
                strerror(errno));
            return REDIS_ERR;
     }
        if (syncReadLine(fd,buf,1024,3600) == -1) {
            close(fd);
            redisLog(REDIS_WARNING,"I/O error reading auth result from MASTER: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        if (buf[0] != '+') {
            close(fd);
            redisLog(REDIS_WARNING,"Cannot AUTH to MASTER, is the masterauth password correct?");
            return REDIS_ERR;
        }
    }
    if (syncWrite(fd,"SYNC \r\n",7,5) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    if (syncReadLine(fd,buf,1024,3600) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error reading bulk count from MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    if (buf[0] != '$') {
        close(fd);
        redisLog(REDIS_WARNING,"Bad protocol from MASTER, the first byte is not '$', are you sure the host and port are right?");
        return REDIS_ERR;
    }
    dumpsize = strtol(buf+1,NULL,10);
    redisLog(REDIS_NOTICE,"Receiving %ld bytes data dump from MASTER",dumpsize);
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)time(NULL),(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        return REDIS_ERR;
    }
    while(dumpsize) {
        int nread, nwritten;
        nread = read(fd,buf,(dumpsize < 1024)?dumpsize:1024);
        if (nread == -1) {
            redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
                strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        nwritten = write(dfd,buf,nread);
        if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Write error writing to the DB dump file needed for MASTER <-> SLAVE synchrnonization: %s", strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        dumpsize -= nread;
    }
    close(dfd);
    if (rename(tmpfile,server.dbfilename) == -1) {
        redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
        unlink(tmpfile);
        close(fd);
        return REDIS_ERR;
    }
    emptyDb();
    if (rdbLoad(server.dbfilename) != REDIS_OK) {
        redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
        close(fd);
        return REDIS_ERR;
    }
    server.master = createClient(fd);
    server.master->flags |= REDIS_MASTER;
    server.master->authenticated = 1;
    server.replstate = REDIS_REPL_CONNECTED;
    return REDIS_OK;
}
static void slaveofCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            sdsfree(server.masterhost);
            server.masterhost = NULL;
            if (server.master) freeClient(server.master);
            server.replstate = REDIS_REPL_NONE;
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        sdsfree(server.masterhost);
        server.masterhost = sdsdup(c->argv[1]->ptr);
        server.masterport = atoi(c->argv[2]->ptr);
        if (server.master) freeClient(server.master);
        server.replstate = REDIS_REPL_CONNECT;
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}
static int tryFreeOneObjectFromFreelist(void) {
    robj *o;
    if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
    if (listLength(server.objfreelist)) {
        listNode *head = listFirst(server.objfreelist);
        o = listNodeValue(head);
        listDelNode(server.objfreelist,head);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
        zfree(o);
        return REDIS_OK;
    } else {
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
        return REDIS_ERR;
    }
}
static void freeMemoryIfNeeded(void) {
    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        int j, k, freed = 0;
        if (tryFreeOneObjectFromFreelist() == REDIS_OK) continue;
        for (j = 0; j < server.dbnum; j++) {
            int minttl = -1;
            robj *minkey = NULL;
            struct dictEntry *de;
            if (dictSize(server.db[j].expires)) {
                freed = 1;
                for (k = 0; k < 3; k++) {
                    time_t t;
                    de = dictGetRandomKey(server.db[j].expires);
                    t = (time_t) dictGetEntryVal(de);
                    if (minttl == -1 || t < minttl) {
                        minkey = dictGetEntryKey(de);
                        minttl = t;
                    }
                }
                dbDelete(server.db+j,minkey);
            }
        }
        if (!freed) return;
    }
}
static void stopAppendOnly(void) {
    flushAppendOnlyFile();
    aof_fsync(server.appendfd);
    close(server.appendfd);
    server.appendfd = -1;
    server.appendseldb = -1;
    server.appendonly = 0;
    if (server.bgsavechildpid != -1) {
        int statloc;
        if (kill(server.bgsavechildpid,SIGKILL) != -1)
            wait3(&statloc,0,NULL);
        sdsfree(server.bgrewritebuf);
        server.bgrewritebuf = sdsempty();
        server.bgsavechildpid = -1;
    }
}
static int startAppendOnly(void) {
    server.appendonly = 1;
    server.lastfsync = time(NULL);
    server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (server.appendfd == -1) {
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, but I can't open the AOF file: %s",strerror(errno));
        return REDIS_ERR;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_ERR) {
        server.appendonly = 0;
        close(server.appendfd);
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, I can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.",strerror(errno));
        return REDIS_ERR;
    }
    return REDIS_OK;
}
static void flushAppendOnlyFile(void) {
    time_t now;
    ssize_t nwritten;
    if (sdslen(server.aofbuf) == 0) return;
     nwritten = write(server.appendfd,server.aofbuf,sdslen(server.aofbuf));
     if (nwritten != (signed)sdslen(server.aofbuf)) {
         if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
         } else {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to the append-only file: %s",strerror(errno));
         }
         exit(1);
    }
    sdsfree(server.aofbuf);
    server.aofbuf = sdsempty();
    if (server.no_appendfsync_on_rewrite &&
        (server.bgrewritechildpid != -1 || server.bgsavechildpid != -1))
            return;
    now = time(NULL);
    if (server.appendfsync == APPENDFSYNC_ALWAYS ||
        (server.appendfsync == APPENDFSYNC_EVERYSEC &&
         now-server.lastfsync > 1))
    {
        aof_fsync(server.appendfd);
        server.lastfsync = now;
    }
}
static sds catAppendOnlyGenericCommand(sds buf, int argc, robj **argv) {
    int j;
    buf = sdscatprintf(buf,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        robj *o = getDecodedObject(argv[j]);
        buf = sdscatprintf(buf,"$%lu\r\n",(unsigned long)sdslen(o->ptr));
        buf = sdscatlen(buf,o->ptr,sdslen(o->ptr));
        buf = sdscatlen(buf,"\r\n",2);
        decrRefCount(o);
    }
    return buf;
}
static sds catAppendOnlyExpireAtCommand(sds buf, robj *key, robj *seconds) {
    int argc = 3;
    long when;
    robj *argv[3];
    seconds = getDecodedObject(seconds);
    when = time(NULL)+strtol(seconds->ptr,NULL,10);
    decrRefCount(seconds);
    argv[0] = createStringObject("EXPIREAT",8);
    argv[1] = key;
    argv[2] = createObject(REDIS_STRING,
        sdscatprintf(sdsempty(),"%ld",when));
    buf = catAppendOnlyGenericCommand(buf, argc, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    return buf;
}
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    robj *tmpargv[3];
    if (dictid != server.appendseldb) {
        char seldb[64];
        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.appendseldb = dictid;
    }
    if (cmd->proc == expireCommand) {
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else if (cmd->proc == setexCommand) {
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else {
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }
    server.aofbuf = sdscatlen(server.aofbuf,buf,sdslen(buf));
    if (server.bgrewritechildpid != -1)
        server.bgrewritebuf = sdscatlen(server.bgrewritebuf,buf,sdslen(buf));
    sdsfree(buf);
}
static struct redisClient *createFakeClient(void) {
    struct redisClient *c = zmalloc(sizeof(*c));
    selectDb(c,0);
    c->fd = -1;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->flags = 0;
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}
static void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    freeClientMultiState(c);
    zfree(c);
}
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int appendonly = server.appendonly;
    if (redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0)
        return REDIS_ERR;
    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }
    server.appendonly = 0;
    fakeClient = createFakeClient();
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;
        int force_swapout;
        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        argv = zmalloc(sizeof(robj*)*argc);
        for (j = 0; j < argc; j++) {
            if (fgets(buf,sizeof(buf),fp) == NULL) goto readerr;
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) goto fmterr;
            argv[j] = createObject(REDIS_STRING,argsds);
            if (fread(buf,2,1,fp) == 0) goto fmterr;
        }
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", argv[0]->ptr);
            exit(1);
        }
        if (cmd->flags & REDIS_CMD_BULK)
            argv[argc-1] = tryObjectEncoding(argv[argc-1]);
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);
        while(listLength(fakeClient->reply))
            listDelNode(fakeClient->reply,listFirst(fakeClient->reply));
        for (j = 0; j < argc; j++) decrRefCount(argv[j]);
        zfree(argv);
        force_swapout = 0;
        if ((zmalloc_used_memory() - server.vm_max_memory) > 1024*1024*32)
            force_swapout = 1;
        if (server.vm_enabled && force_swapout) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
        }
    }
    if (fakeClient->flags & REDIS_MULTI) goto readerr;
    fclose(fp);
    freeFakeClient(fakeClient);
    server.appendonly = appendonly;
    return REDIS_OK;
readerr:
    if (feof(fp)) {
        redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file");
    } else {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
    }
    exit(1);
fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file");
    exit(1);
}
static int fwriteBulkString(FILE *fp, char *s, unsigned long len) {
    char cbuf[128];
    int clen;
    cbuf[0] = '$';
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,len);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (fwrite(cbuf,clen,1,fp) == 0) return 0;
    if (len > 0 && fwrite(s,len,1,fp) == 0) return 0;
    if (fwrite("\r\n",2,1,fp) == 0) return 0;
    return 1;
}
static int fwriteBulkDouble(FILE *fp, double d) {
    char buf[128], dbuf[128];
    snprintf(dbuf,sizeof(dbuf),"%.17g\r\n",d);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(dbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(dbuf,strlen(dbuf),1,fp) == 0) return 0;
    return 1;
}
static int fwriteBulkLongLong(FILE *fp, long long l) {
    char bbuf[128], lbuf[128];
    unsigned int blen, llen;
    llen = ll2string(lbuf,32,l);
    blen = snprintf(bbuf,sizeof(bbuf),"$%u\r\n%s\r\n",llen,lbuf);
    if (fwrite(bbuf,blen,1,fp) == 0) return 0;
    return 1;
}
static int fwriteBulkObject(FILE *fp, robj *obj) {
    if (obj->encoding == REDIS_ENCODING_INT) {
        return fwriteBulkLongLong(fp,(long)obj->ptr);
    } else if (obj->encoding == REDIS_ENCODING_RAW) {
        return fwriteBulkString(fp,obj->ptr,sdslen(obj->ptr));
    } else {
        redisPanic("Unknown string encoding");
    }
}
static int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed rewriting the append only file: %s", strerror(errno));
        return REDIS_ERR;
    }
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }
        if (fwrite(selectcmd,sizeof(selectcmd)-1,1,fp) == 0) goto werr;
        if (fwriteBulkLongLong(fp,j) == 0) goto werr;
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetEntryKey(de);
            robj key, *o;
            time_t expiretime;
            int swapped;
            keystr = dictGetEntryKey(de);
            o = dictGetEntryVal(de);
            initStaticStringObject(key,keystr);
            if (!server.vm_enabled || o->storage == REDIS_VM_MEMORY ||
                o->storage == REDIS_VM_SWAPPING) {
                swapped = 0;
            } else {
                o = vmPreviewObject(o);
                swapped = 1;
            }
            expiretime = getExpire(db,&key);
            if (o->type == REDIS_STRING) {
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                if (fwriteBulkObject(fp,&key) == 0) goto werr;
                if (fwriteBulkObject(fp,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                char cmd[]="*3\r\n$5\r\nRPUSH\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *p = ziplistIndex(zl,0);
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vlong;
                    while(ziplistGet(p,&vstr,&vlen,&vlong)) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,key) == 0) goto werr;
                        if (vstr) {
                            if (fwriteBulkString(fp,(char*)vstr,vlen) == 0)
                                goto werr;
                        } else {
                            if (fwriteBulkLongLong(fp,vlong) == 0)
                                goto werr;
                        }
                        p = ziplistNext(zl,p);
                    }
                } else if (o->encoding == REDIS_ENCODING_LIST) {
                    list *list = o->ptr;
                    listNode *ln;
                    listIter li;
<<<<<<< HEAD
                    listRewind(list,&li);
                    while((ln = listNext(&li))) {
                        robj *eleobj = listNodeValue(ln);
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,key) == 0) goto werr;
                        if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                    }
                } else {
                    redisPanic("Unknown list encoding");
||||||| d55d5c5dd
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,key) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
=======
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,&key) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
>>>>>>> 22194a7f
                }
            } else if (o->type == REDIS_SET) {
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;
                while((de = dictNext(di)) != NULL) {
                    char cmd[]="*3\r\n$4\r\nSADD\r\n";
                    robj *eleobj = dictGetEntryKey(de);
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,&key) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_ZSET) {
                zset *zs = o->ptr;
                dictIterator *di = dictGetIterator(zs->dict);
                dictEntry *de;
                while((de = dictNext(di)) != NULL) {
                    char cmd[]="*4\r\n$4\r\nZADD\r\n";
                    robj *eleobj = dictGetEntryKey(de);
                    double *score = dictGetEntryVal(de);
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulkObject(fp,&key) == 0) goto werr;
                    if (fwriteBulkDouble(fp,*score) == 0) goto werr;
                    if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_HASH) {
                char cmd[]="*4\r\n$4\r\nHSET\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                    unsigned char *p = zipmapRewind(o->ptr);
                    unsigned char *field, *val;
                    unsigned int flen, vlen;
                    while((p = zipmapNext(p,&field,&flen,&val,&vlen)) != NULL) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkString(fp,(char*)field,flen) == -1)
                            return -1;
                        if (fwriteBulkString(fp,(char*)val,vlen) == -1)
                            return -1;
                    }
                } else {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;
                    while((de = dictNext(di)) != NULL) {
                        robj *field = dictGetEntryKey(de);
                        robj *val = dictGetEntryVal(de);
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkObject(fp,field) == -1) return -1;
                        if (fwriteBulkObject(fp,val) == -1) return -1;
                    }
                    dictReleaseIterator(di);
                }
            } else {
                redisPanic("Unknown object type");
            }
            if (expiretime != -1) {
                char cmd[]="*3\r\n$8\r\nEXPIREAT\r\n";
                if (expiretime < now) continue;
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
<<<<<<< HEAD
                if (fwriteBulkObject(fp,key) == 0) goto werr;
                if (fwriteBulkLongLong(fp,expiretime) == 0) goto werr;
||||||| d55d5c5dd
                if (fwriteBulkObject(fp,key) == 0) goto werr;
                if (fwriteBulkLong(fp,expiretime) == 0) goto werr;
=======
                if (fwriteBulkObject(fp,&key) == 0) goto werr;
                if (fwriteBulkLong(fp,expiretime) == 0) goto werr;
>>>>>>> 22194a7f
            }
            if (swapped) decrRefCount(o);
        }
        dictReleaseIterator(di);
    }
    fflush(fp);
    aof_fsync(fileno(fp));
    fclose(fp);
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"SYNC append only file rewrite performed");
    return REDIS_OK;
werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}
static int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    if (server.bgrewritechildpid != -1) return REDIS_ERR;
    if (server.vm_enabled) waitEmptyIOJobsQueue();
    if ((childpid = fork()) == 0) {
        char tmpfile[256];
        if (server.vm_enabled) vmReopenSwapFile();
        close(server.fd);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        if (childpid == -1) {
            redisLog(REDIS_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        server.bgrewritechildpid = childpid;
        updateDictResizePolicy();
        server.appendseldb = -1;
        return REDIS_OK;
    }
    return REDIS_OK;
}
static void bgrewriteaofCommand(redisClient *c) {
    if (server.bgrewritechildpid != -1) {
        addReplySds(c,sdsnew("-ERR background append only file rewriting already in progress\r\n"));
        return;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
        char *status = "+Background append only file rewriting started\r\n";
        addReplySds(c,sdsnew(status));
    } else {
        addReply(c,shared.err);
    }
}
static void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];
    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}
static vmpointer *createVmPointer(int vtype) {
    vmpointer *vp = zmalloc(sizeof(vmpointer));
    vp->type = REDIS_VMPOINTER;
    vp->storage = REDIS_VM_SWAPPED;
    vp->vtype = vtype;
    return vp;
}
static void vmInit(void) {
    off_t totsize;
    int pipefds[2];
    size_t stacksize;
    struct flock fl;
    if (server.vm_max_threads != 0)
        zmalloc_enable_thread_safeness();
    redisLog(REDIS_NOTICE,"Using '%s' as swap file",server.vm_swap_file);
    if ((server.vm_fp = fopen(server.vm_swap_file,"r+b")) == NULL) {
        server.vm_fp = fopen(server.vm_swap_file,"w+b");
    }
    if (server.vm_fp == NULL) {
        redisLog(REDIS_WARNING,
            "Can't open the swap file: %s. Exiting.",
            strerror(errno));
        exit(1);
    }
    server.vm_fd = fileno(server.vm_fp);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = fl.l_len = 0;
    if (fcntl(server.vm_fd,F_SETLK,&fl) == -1) {
        redisLog(REDIS_WARNING,
            "Can't lock the swap file at '%s': %s. Make sure it is not used by another Redis instance.", server.vm_swap_file, strerror(errno));
        exit(1);
    }
    server.vm_next_page = 0;
    server.vm_near_pages = 0;
    server.vm_stats_used_pages = 0;
    server.vm_stats_swapped_objects = 0;
    server.vm_stats_swapouts = 0;
    server.vm_stats_swapins = 0;
    totsize = server.vm_pages*server.vm_page_size;
    redisLog(REDIS_NOTICE,"Allocating %lld bytes of swap file",totsize);
    if (ftruncate(server.vm_fd,totsize) == -1) {
        redisLog(REDIS_WARNING,"Can't ftruncate swap file: %s. Exiting.",
            strerror(errno));
        exit(1);
    } else {
        redisLog(REDIS_NOTICE,"Swap file allocated with success");
    }
    server.vm_bitmap = zmalloc((server.vm_pages+7)/8);
    redisLog(REDIS_VERBOSE,"Allocated %lld bytes page table for %lld pages",
        (long long) (server.vm_pages+7)/8, server.vm_pages);
    memset(server.vm_bitmap,0,(server.vm_pages+7)/8);
    server.io_newjobs = listCreate();
    server.io_processing = listCreate();
    server.io_processed = listCreate();
    server.io_ready_clients = listCreate();
    pthread_mutex_init(&server.io_mutex,NULL);
    pthread_mutex_init(&server.obj_freelist_mutex,NULL);
    pthread_mutex_init(&server.io_swapfile_mutex,NULL);
    server.io_active_threads = 0;
    if (pipe(pipefds) == -1) {
        redisLog(REDIS_WARNING,"Unable to intialized VM: pipe(2): %s. Exiting."
            ,strerror(errno));
        exit(1);
    }
    server.io_ready_pipe_read = pipefds[0];
    server.io_ready_pipe_write = pipefds[1];
    redisAssert(anetNonBlock(NULL,server.io_ready_pipe_read) != ANET_ERR);
    pthread_attr_init(&server.io_threads_attr);
    pthread_attr_getstacksize(&server.io_threads_attr, &stacksize);
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&server.io_threads_attr, stacksize);
    if (aeCreateFileEvent(server.el, server.io_ready_pipe_read, AE_READABLE,
        vmThreadedIOCompletedJob, NULL) == AE_ERR)
        oom("creating file event");
}
static void vmMarkPageUsed(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    redisAssert(vmFreePage(page) == 1);
    server.vm_bitmap[byte] |= 1<<bit;
}
static void vmMarkPagesUsed(off_t page, off_t count) {
    off_t j;
    for (j = 0; j < count; j++)
        vmMarkPageUsed(page+j);
    server.vm_stats_used_pages += count;
    redisLog(REDIS_DEBUG,"Mark USED pages: %lld pages at %lld\n",
        (long long)count, (long long)page);
}
static void vmMarkPageFree(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    redisAssert(vmFreePage(page) == 0);
    server.vm_bitmap[byte] &= ~(1<<bit);
}
static void vmMarkPagesFree(off_t page, off_t count) {
    off_t j;
    for (j = 0; j < count; j++)
        vmMarkPageFree(page+j);
    server.vm_stats_used_pages -= count;
    redisLog(REDIS_DEBUG,"Mark FREE pages: %lld pages at %lld\n",
        (long long)count, (long long)page);
}
static int vmFreePage(off_t page) {
    off_t byte = page/8;
    int bit = page&7;
    return (server.vm_bitmap[byte] & (1<<bit)) == 0;
}
static int vmFindContiguousPages(off_t *first, off_t n) {
    off_t base, offset = 0, since_jump = 0, numfree = 0;
    if (server.vm_near_pages == REDIS_VM_MAX_NEAR_PAGES) {
        server.vm_near_pages = 0;
        server.vm_next_page = 0;
    }
    server.vm_near_pages++;
    base = server.vm_next_page;
    while(offset < server.vm_pages) {
        off_t this = base+offset;
        if (this >= server.vm_pages) {
            this -= server.vm_pages;
            if (this == 0) {
                numfree = 0;
            }
        }
        if (vmFreePage(this)) {
            numfree++;
            if (numfree == n) {
                *first = this-(n-1);
                server.vm_next_page = this+1;
                redisLog(REDIS_DEBUG, "FOUND CONTIGUOUS PAGES: %lld pages at %lld\n", (long long) n, (long long) *first);
                return REDIS_OK;
            }
        } else {
            numfree = 0;
        }
        since_jump++;
        if (!numfree && since_jump >= REDIS_VM_MAX_RANDOM_JUMP/4) {
            offset += random() % REDIS_VM_MAX_RANDOM_JUMP;
            since_jump = 0;
        } else {
            offset++;
        }
    }
    return REDIS_ERR;
}
static int vmWriteObjectOnSwap(robj *o, off_t page) {
    if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
    if (fseeko(server.vm_fp,page*server.vm_page_size,SEEK_SET) == -1) {
        if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
        redisLog(REDIS_WARNING,
            "Critical VM problem in vmWriteObjectOnSwap(): can't seek: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    rdbSaveObject(server.vm_fp,o);
    fflush(server.vm_fp);
    if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
    return REDIS_OK;
}
static vmpointer *vmSwapObjectBlocking(robj *val) {
    off_t pages = rdbSavedObjectPages(val,NULL);
    off_t page;
    vmpointer *vp;
    assert(val->storage == REDIS_VM_MEMORY);
    assert(val->refcount == 1);
    if (vmFindContiguousPages(&page,pages) == REDIS_ERR) return NULL;
    if (vmWriteObjectOnSwap(val,page) == REDIS_ERR) return NULL;
    vp = createVmPointer(val->type);
    vp->page = page;
    vp->usedpages = pages;
    decrRefCount(val);
    vmMarkPagesUsed(page,pages);
    redisLog(REDIS_DEBUG,"VM: object %p swapped out at %lld (%lld pages)",
        (void*) val,
        (unsigned long long) page, (unsigned long long) pages);
    server.vm_stats_swapped_objects++;
    server.vm_stats_swapouts++;
    return vp;
}
static robj *vmReadObjectFromSwap(off_t page, int type) {
    robj *o;
    if (server.vm_enabled) pthread_mutex_lock(&server.io_swapfile_mutex);
    if (fseeko(server.vm_fp,page*server.vm_page_size,SEEK_SET) == -1) {
        redisLog(REDIS_WARNING,
            "Unrecoverable VM problem in vmReadObjectFromSwap(): can't seek: %s",
            strerror(errno));
        _exit(1);
    }
    o = rdbLoadObject(type,server.vm_fp);
    if (o == NULL) {
        redisLog(REDIS_WARNING, "Unrecoverable VM problem in vmReadObjectFromSwap(): can't load object from swap file: %s", strerror(errno));
        _exit(1);
    }
    if (server.vm_enabled) pthread_mutex_unlock(&server.io_swapfile_mutex);
    return o;
}
static robj *vmGenericLoadObject(vmpointer *vp, int preview) {
    robj *val;
    redisAssert(vp->type == REDIS_VMPOINTER &&
        (vp->storage == REDIS_VM_SWAPPED || vp->storage == REDIS_VM_LOADING));
    val = vmReadObjectFromSwap(vp->page,vp->vtype);
    if (!preview) {
        redisLog(REDIS_DEBUG, "VM: object %p loaded from disk", (void*)vp);
        vmMarkPagesFree(vp->page,vp->usedpages);
        zfree(vp);
        server.vm_stats_swapped_objects--;
    } else {
        redisLog(REDIS_DEBUG, "VM: object %p previewed from disk", (void*)vp);
    }
    server.vm_stats_swapins++;
    return val;
}
static robj *vmLoadObject(robj *o) {
    if (o->storage == REDIS_VM_LOADING)
        vmCancelThreadedIOJob(o);
    return vmGenericLoadObject((vmpointer*)o,0);
}
static robj *vmPreviewObject(robj *o) {
    return vmGenericLoadObject((vmpointer*)o,1);
}
static double computeObjectSwappability(robj *o) {
    time_t minage = abs(server.lruclock - o->lru);
    long asize = 0;
    list *l;
    dict *d;
    struct dictEntry *de;
    int z;
    if (minage <= 0) return 0;
    switch(o->type) {
    case REDIS_STRING:
        if (o->encoding != REDIS_ENCODING_RAW) {
            asize = sizeof(*o);
        } else {
            asize = sdslen(o->ptr)+sizeof(*o)+sizeof(long)*2;
        }
        break;
    case REDIS_LIST:
        l = o->ptr;
        listNode *ln = listFirst(l);
        asize = sizeof(list);
        if (ln) {
            robj *ele = ln->value;
            long elesize;
            elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                            (sizeof(*o)+sdslen(ele->ptr)) : sizeof(*o);
            asize += (sizeof(listNode)+elesize)*listLength(l);
        }
        break;
    case REDIS_SET:
    case REDIS_ZSET:
        z = (o->type == REDIS_ZSET);
        d = z ? ((zset*)o->ptr)->dict : o->ptr;
        asize = sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
        if (z) asize += sizeof(zset)-sizeof(dict);
        if (dictSize(d)) {
            long elesize;
            robj *ele;
            de = dictGetRandomKey(d);
            ele = dictGetEntryKey(de);
            elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                            (sizeof(*o)+sdslen(ele->ptr)) : sizeof(*o);
            asize += (sizeof(struct dictEntry)+elesize)*dictSize(d);
            if (z) asize += sizeof(zskiplistNode)*dictSize(d);
        }
        break;
    case REDIS_HASH:
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            unsigned char *p = zipmapRewind((unsigned char*)o->ptr);
            unsigned int len = zipmapLen((unsigned char*)o->ptr);
            unsigned int klen, vlen;
            unsigned char *key, *val;
            if ((p = zipmapNext(p,&key,&klen,&val,&vlen)) == NULL) {
                klen = 0;
                vlen = 0;
            }
            asize = len*(klen+vlen+3);
        } else if (o->encoding == REDIS_ENCODING_HT) {
            d = o->ptr;
            asize = sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            if (dictSize(d)) {
                long elesize;
                robj *ele;
                de = dictGetRandomKey(d);
                ele = dictGetEntryKey(de);
                elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                                (sizeof(*o)+sdslen(ele->ptr)) : sizeof(*o);
                ele = dictGetEntryVal(de);
                elesize = (ele->encoding == REDIS_ENCODING_RAW) ?
                                (sizeof(*o)+sdslen(ele->ptr)) : sizeof(*o);
                asize += (sizeof(struct dictEntry)+elesize)*dictSize(d);
            }
        }
        break;
    }
    return (double)minage*log(1+asize);
}
static int vmSwapOneObject(int usethreads) {
    int j, i;
    struct dictEntry *best = NULL;
    double best_swappability = 0;
    redisDb *best_db = NULL;
    robj *val;
    sds key;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        int maxtries = 100;
        if (dictSize(db->dict) == 0) continue;
        for (i = 0; i < 5; i++) {
            dictEntry *de;
            double swappability;
            if (maxtries) maxtries--;
            de = dictGetRandomKey(db->dict);
            val = dictGetEntryVal(de);
            if (val->storage != REDIS_VM_MEMORY || val->refcount != 1) {
                if (maxtries) i--;
                continue;
            }
            swappability = computeObjectSwappability(val);
            if (!best || swappability > best_swappability) {
                best = de;
                best_swappability = swappability;
                best_db = db;
            }
        }
    }
    if (best == NULL) return REDIS_ERR;
    key = dictGetEntryKey(best);
    val = dictGetEntryVal(best);
    redisLog(REDIS_DEBUG,"Key with best swappability: %s, %f",
        key, best_swappability);
    if (usethreads) {
        robj *keyobj = createStringObject(key,sdslen(key));
        vmSwapObjectThreaded(keyobj,val,best_db);
        decrRefCount(keyobj);
        return REDIS_OK;
    } else {
        vmpointer *vp;
        if ((vp = vmSwapObjectBlocking(val)) != NULL) {
            dictGetEntryVal(best) = vp;
            return REDIS_OK;
        } else {
            return REDIS_ERR;
        }
    }
}
static int vmSwapOneObjectBlocking() {
    return vmSwapOneObject(0);
}
static int vmSwapOneObjectThreaded() {
    return vmSwapOneObject(1);
}
static int vmCanSwapOut(void) {
    return (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1);
}
static void freeIOJob(iojob *j) {
    if ((j->type == REDIS_IOJOB_PREPARE_SWAP ||
        j->type == REDIS_IOJOB_DO_SWAP ||
        j->type == REDIS_IOJOB_LOAD) && j->val != NULL)
    {
        if (j->val->storage == REDIS_VM_SWAPPING)
            j->val->storage = REDIS_VM_MEMORY;
        decrRefCount(j->val);
    }
    decrRefCount(j->key);
    zfree(j);
}
static void vmThreadedIOCompletedJob(aeEventLoop *el, int fd, void *privdata,
            int mask)
{
    char buf[1];
    int retval, processed = 0, toprocess = -1, trytoswap = 1;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);
    while((retval = read(fd,buf,1)) == 1) {
        iojob *j;
        listNode *ln;
        struct dictEntry *de;
        redisLog(REDIS_DEBUG,"Processing I/O completed job");
        lockThreadedIO();
        assert(listLength(server.io_processed) != 0);
        if (toprocess == -1) {
            toprocess = (listLength(server.io_processed)*REDIS_MAX_COMPLETED_JOBS_PROCESSED)/100;
            if (toprocess <= 0) toprocess = 1;
        }
        ln = listFirst(server.io_processed);
        j = ln->value;
        listDelNode(server.io_processed,ln);
        unlockThreadedIO();
        if (j->canceled) {
            freeIOJob(j);
            continue;
        }
        redisLog(REDIS_DEBUG,"COMPLETED Job type: %d, ID %p, key: %s", j->type, (void*)j->id, (unsigned char*)j->key->ptr);
        de = dictFind(j->db->dict,j->key->ptr);
        redisAssert(de != NULL);
        if (j->type == REDIS_IOJOB_LOAD) {
            redisDb *db;
            vmpointer *vp = dictGetEntryVal(de);
            vmMarkPagesFree(vp->page,vp->usedpages);
            redisLog(REDIS_DEBUG, "VM: object %s loaded from disk (threaded)",
                (unsigned char*) j->key->ptr);
            server.vm_stats_swapped_objects--;
            server.vm_stats_swapins++;
            dictGetEntryVal(de) = j->val;
            incrRefCount(j->val);
            db = j->db;
            handleClientsBlockedOnSwappedKey(db,j->key);
            freeIOJob(j);
            zfree(vp);
        } else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
            if (!vmCanSwapOut() ||
                vmFindContiguousPages(&j->page,j->pages) == REDIS_ERR)
            {
                j->val->storage = REDIS_VM_MEMORY;
                freeIOJob(j);
            } else {
                vmMarkPagesUsed(j->page,j->pages);
                j->type = REDIS_IOJOB_DO_SWAP;
                lockThreadedIO();
                queueIOJob(j);
                unlockThreadedIO();
            }
        } else if (j->type == REDIS_IOJOB_DO_SWAP) {
            vmpointer *vp;
            if (j->val->storage != REDIS_VM_SWAPPING) {
                vmpointer *vp = (vmpointer*) j->id;
                printf("storage: %d\n",vp->storage);
                printf("key->name: %s\n",(char*)j->key->ptr);
                printf("val: %p\n",(void*)j->val);
                printf("val->type: %d\n",j->val->type);
                printf("val->ptr: %s\n",(char*)j->val->ptr);
            }
            redisAssert(j->val->storage == REDIS_VM_SWAPPING);
            vp = createVmPointer(j->val->type);
            vp->page = j->page;
            vp->usedpages = j->pages;
            dictGetEntryVal(de) = vp;
            j->val->storage = REDIS_VM_MEMORY;
            decrRefCount(j->val);
            redisLog(REDIS_DEBUG,
                "VM: object %s swapped out at %lld (%lld pages) (threaded)",
                (unsigned char*) j->key->ptr,
                (unsigned long long) j->page, (unsigned long long) j->pages);
            server.vm_stats_swapped_objects++;
            server.vm_stats_swapouts++;
            freeIOJob(j);
            if (trytoswap && vmCanSwapOut() &&
                zmalloc_used_memory() > server.vm_max_memory)
            {
                int more = 1;
                while(more) {
                    lockThreadedIO();
                    more = listLength(server.io_newjobs) <
                            (unsigned) server.vm_max_threads;
                    unlockThreadedIO();
                    if (vmSwapOneObjectThreaded() == REDIS_ERR) {
                        trytoswap = 0;
                        break;
                    }
                }
            }
        }
        processed++;
        if (processed == toprocess) return;
    }
    if (retval < 0 && errno != EAGAIN) {
        redisLog(REDIS_WARNING,
            "WARNING: read(2) error in vmThreadedIOCompletedJob() %s",
            strerror(errno));
    }
}
static void lockThreadedIO(void) {
    pthread_mutex_lock(&server.io_mutex);
}
static void unlockThreadedIO(void) {
    pthread_mutex_unlock(&server.io_mutex);
}
static void vmCancelThreadedIOJob(robj *o) {
    list *lists[3] = {
        server.io_newjobs,
        server.io_processing,
        server.io_processed
    };
    int i;
    assert(o->storage == REDIS_VM_LOADING || o->storage == REDIS_VM_SWAPPING);
again:
    lockThreadedIO();
    for (i = 0; i < 3; i++) {
        listNode *ln;
        listIter li;
        listRewind(lists[i],&li);
        while ((ln = listNext(&li)) != NULL) {
            iojob *job = ln->value;
            if (job->canceled) continue;
            if (job->id == o) {
                redisLog(REDIS_DEBUG,"*** CANCELED %p (key %s) (type %d) (LIST ID %d)\n",
                    (void*)job, (char*)job->key->ptr, job->type, i);
                if (i != 1 && job->type == REDIS_IOJOB_DO_SWAP)
                    vmMarkPagesFree(job->page,job->pages);
                switch(i) {
                case 0:
                    freeIOJob(job);
                    listDelNode(lists[i],ln);
                    break;
                case 1:
                    unlockThreadedIO();
                    usleep(1);
                    goto again;
                case 2:
                    job->canceled = 1;
                    break;
                }
                if (o->storage == REDIS_VM_LOADING)
                    o->storage = REDIS_VM_SWAPPED;
                else if (o->storage == REDIS_VM_SWAPPING)
                    o->storage = REDIS_VM_MEMORY;
                unlockThreadedIO();
                redisLog(REDIS_DEBUG,"*** DONE");
                return;
            }
        }
    }
    unlockThreadedIO();
    printf("Not found: %p\n", (void*)o);
    redisAssert(1 != 1);
}
static void *IOThreadEntryPoint(void *arg) {
    iojob *j;
    listNode *ln;
    REDIS_NOTUSED(arg);
    pthread_detach(pthread_self());
    while(1) {
        lockThreadedIO();
        if (listLength(server.io_newjobs) == 0) {
            redisLog(REDIS_DEBUG,"Thread %ld exiting, nothing to do",
                (long) pthread_self());
            server.io_active_threads--;
            unlockThreadedIO();
            return NULL;
        }
        ln = listFirst(server.io_newjobs);
        j = ln->value;
        listDelNode(server.io_newjobs,ln);
        j->thread = pthread_self();
        listAddNodeTail(server.io_processing,j);
        ln = listLast(server.io_processing);
        unlockThreadedIO();
        redisLog(REDIS_DEBUG,"Thread %ld got a new job (type %d): %p about key '%s'",
            (long) pthread_self(), j->type, (void*)j, (char*)j->key->ptr);
        if (j->type == REDIS_IOJOB_LOAD) {
            vmpointer *vp = (vmpointer*)j->id;
            j->val = vmReadObjectFromSwap(j->page,vp->vtype);
        } else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
            FILE *fp = fopen("/dev/null","w+");
            j->pages = rdbSavedObjectPages(j->val,fp);
            fclose(fp);
        } else if (j->type == REDIS_IOJOB_DO_SWAP) {
            if (vmWriteObjectOnSwap(j->val,j->page) == REDIS_ERR)
                j->canceled = 1;
        }
        redisLog(REDIS_DEBUG,"Thread %ld completed the job: %p (key %s)",
            (long) pthread_self(), (void*)j, (char*)j->key->ptr);
        lockThreadedIO();
        listDelNode(server.io_processing,ln);
        listAddNodeTail(server.io_processed,j);
        unlockThreadedIO();
        assert(write(server.io_ready_pipe_write,"x",1) == 1);
    }
    return NULL;
}
static void spawnIOThread(void) {
    pthread_t thread;
    sigset_t mask, omask;
    int err;
    sigemptyset(&mask);
    sigaddset(&mask,SIGCHLD);
    sigaddset(&mask,SIGHUP);
    sigaddset(&mask,SIGPIPE);
    pthread_sigmask(SIG_SETMASK, &mask, &omask);
    while ((err = pthread_create(&thread,&server.io_threads_attr,IOThreadEntryPoint,NULL)) != 0) {
        redisLog(REDIS_WARNING,"Unable to spawn an I/O thread: %s",
            strerror(err));
        usleep(1000000);
    }
    pthread_sigmask(SIG_SETMASK, &omask, NULL);
    server.io_active_threads++;
}
static void waitEmptyIOJobsQueue(void) {
    while(1) {
        int io_processed_len;
        lockThreadedIO();
        if (listLength(server.io_newjobs) == 0 &&
            listLength(server.io_processing) == 0 &&
            server.io_active_threads == 0)
        {
            unlockThreadedIO();
            return;
        }
        io_processed_len = listLength(server.io_processed);
        unlockThreadedIO();
        if (io_processed_len) {
            vmThreadedIOCompletedJob(NULL,server.io_ready_pipe_read,NULL,0);
            usleep(1000);
        } else {
            usleep(10000);
        }
    }
}
static void vmReopenSwapFile(void) {
    server.vm_fp = fopen(server.vm_swap_file,"r+b");
    if (server.vm_fp == NULL) {
        redisLog(REDIS_WARNING,"Can't re-open the VM swap file: %s. Exiting.",
            server.vm_swap_file);
        _exit(1);
    }
    server.vm_fd = fileno(server.vm_fp);
}
static void queueIOJob(iojob *j) {
    redisLog(REDIS_DEBUG,"Queued IO Job %p type %d about key '%s'\n",
        (void*)j, j->type, (char*)j->key->ptr);
    listAddNodeTail(server.io_newjobs,j);
    if (server.io_active_threads < server.vm_max_threads)
        spawnIOThread();
}
static int vmSwapObjectThreaded(robj *key, robj *val, redisDb *db) {
    iojob *j;
    j = zmalloc(sizeof(*j));
    j->type = REDIS_IOJOB_PREPARE_SWAP;
    j->db = db;
    j->key = key;
    incrRefCount(key);
    j->id = j->val = val;
    incrRefCount(val);
    j->canceled = 0;
    j->thread = (pthread_t) -1;
    val->storage = REDIS_VM_SWAPPING;
    lockThreadedIO();
    queueIOJob(j);
    unlockThreadedIO();
    return REDIS_OK;
}
static int waitForSwappedKey(redisClient *c, robj *key) {
    struct dictEntry *de;
    robj *o;
    list *l;
    de = dictFind(c->db->dict,key->ptr);
    if (de == NULL) return 0;
    o = dictGetEntryVal(de);
    if (o->storage == REDIS_VM_MEMORY) {
        return 0;
    } else if (o->storage == REDIS_VM_SWAPPING) {
        vmCancelThreadedIOJob(o);
        return 0;
    }
    listAddNodeTail(c->io_keys,key);
    incrRefCount(key);
    de = dictFind(c->db->io_keys,key);
    if (de == NULL) {
        int retval;
        l = listCreate();
        retval = dictAdd(c->db->io_keys,key,l);
        incrRefCount(key);
        assert(retval == DICT_OK);
    } else {
        l = dictGetEntryVal(de);
    }
    listAddNodeTail(l,c);
    if (o->storage == REDIS_VM_SWAPPED) {
        iojob *j;
        vmpointer *vp = (vmpointer*)o;
        o->storage = REDIS_VM_LOADING;
        j = zmalloc(sizeof(*j));
        j->type = REDIS_IOJOB_LOAD;
        j->db = c->db;
        j->id = (robj*)vp;
        j->key = key;
        incrRefCount(key);
        j->page = vp->page;
        j->val = NULL;
        j->canceled = 0;
        j->thread = (pthread_t) -1;
        lockThreadedIO();
        queueIOJob(j);
        unlockThreadedIO();
    }
    return 1;
}
static void waitForMultipleSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int j, last;
    if (cmd->vm_firstkey == 0) return;
    last = cmd->vm_lastkey;
    if (last < 0) last = argc+last;
    for (j = cmd->vm_firstkey; j <= last; j += cmd->vm_keystep) {
        redisAssert(j < argc);
        waitForSwappedKey(c,argv[j]);
    }
}
static void zunionInterBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, num;
    REDIS_NOTUSED(cmd);
    num = atoi(argv[2]->ptr);
    if (num > (argc-3)) return;
    for (i = 0; i < num; i++) {
        waitForSwappedKey(c,argv[3+i]);
    }
}
static void execBlockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd, int argc, robj **argv) {
    int i, margc;
    struct redisCommand *mcmd;
    robj **margv;
    REDIS_NOTUSED(cmd);
    REDIS_NOTUSED(argc);
    REDIS_NOTUSED(argv);
    if (!(c->flags & REDIS_MULTI)) return;
    for (i = 0; i < c->mstate.count; i++) {
        mcmd = c->mstate.commands[i].cmd;
        margc = c->mstate.commands[i].argc;
        margv = c->mstate.commands[i].argv;
        if (mcmd->vm_preload_proc != NULL) {
            mcmd->vm_preload_proc(c,mcmd,margc,margv);
        } else {
            waitForMultipleSwappedKeys(c,mcmd,margc,margv);
        }
    }
}
static int blockClientOnSwappedKeys(redisClient *c, struct redisCommand *cmd) {
    if (cmd->vm_preload_proc != NULL) {
        cmd->vm_preload_proc(c,cmd,c->argc,c->argv);
    } else {
        waitForMultipleSwappedKeys(c,cmd,c->argc,c->argv);
    }
    if (listLength(c->io_keys)) {
        c->flags |= REDIS_IO_WAIT;
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        server.vm_blocked_clients++;
        return 1;
    } else {
        return 0;
    }
}
static int dontWaitForSwappedKey(redisClient *c, robj *key) {
    list *l;
    listNode *ln;
    listIter li;
    struct dictEntry *de;
    listRewind(c->io_keys,&li);
    while ((ln = listNext(&li)) != NULL) {
        if (equalStringObjects(ln->value,key)) {
            listDelNode(c->io_keys,ln);
            break;
        }
    }
    assert(ln != NULL);
    de = dictFind(c->db->io_keys,key);
    assert(de != NULL);
    l = dictGetEntryVal(de);
    ln = listSearchKey(l,c);
    assert(ln != NULL);
    listDelNode(l,ln);
    if (listLength(l) == 0)
        dictDelete(c->db->io_keys,key);
    return listLength(c->io_keys) == 0;
}
static void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key) {
    struct dictEntry *de;
    list *l;
    listNode *ln;
    int len;
    de = dictFind(db->io_keys,key);
    if (!de) return;
    l = dictGetEntryVal(de);
    len = listLength(l);
    while (len--) {
        ln = listFirst(l);
        redisClient *c = ln->value;
        if (dontWaitForSwappedKey(c,key)) {
            listAddNodeTail(server.io_ready_clients,c);
        }
    }
}
static void configSetCommand(redisClient *c) {
    robj *o = getDecodedObject(c->argv[3]);
    long long ll;
    if (!strcasecmp(c->argv[2]->ptr,"dbfilename")) {
        zfree(server.dbfilename);
        server.dbfilename = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"requirepass")) {
        zfree(server.requirepass);
        server.requirepass = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"masterauth")) {
        zfree(server.masterauth);
        server.masterauth = zstrdup(o->ptr);
    } else if (!strcasecmp(c->argv[2]->ptr,"maxmemory")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0) goto badfmt;
        server.maxmemory = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"timeout")) {
        if (getLongLongFromObject(o,&ll) == REDIS_ERR ||
            ll < 0 || ll > LONG_MAX) goto badfmt;
        server.maxidletime = ll;
    } else if (!strcasecmp(c->argv[2]->ptr,"appendfsync")) {
        if (!strcasecmp(o->ptr,"no")) {
            server.appendfsync = APPENDFSYNC_NO;
        } else if (!strcasecmp(o->ptr,"everysec")) {
            server.appendfsync = APPENDFSYNC_EVERYSEC;
        } else if (!strcasecmp(o->ptr,"always")) {
            server.appendfsync = APPENDFSYNC_ALWAYS;
        } else {
            goto badfmt;
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"no-appendfsync-on-rewrite")) {
        int yn = yesnotoi(o->ptr);
        if (yn == -1) goto badfmt;
        server.no_appendfsync_on_rewrite = yn;
    } else if (!strcasecmp(c->argv[2]->ptr,"appendonly")) {
        int old = server.appendonly;
        int new = yesnotoi(o->ptr);
        if (new == -1) goto badfmt;
        if (old != new) {
            if (new == 0) {
                stopAppendOnly();
            } else {
                if (startAppendOnly() == REDIS_ERR) {
                    addReplySds(c,sdscatprintf(sdsempty(),
                        "-ERR Unable to turn on AOF. Check server logs.\r\n"));
                    decrRefCount(o);
                    return;
                }
            }
        }
    } else if (!strcasecmp(c->argv[2]->ptr,"save")) {
        int vlen, j;
        sds *v = sdssplitlen(o->ptr,sdslen(o->ptr)," ",1,&vlen);
        if (vlen & 1) {
            sdsfreesplitres(v,vlen);
            goto badfmt;
        }
        for (j = 0; j < vlen; j++) {
            char *eptr;
            long val;
            val = strtoll(v[j], &eptr, 10);
            if (eptr[0] != '\0' ||
                ((j & 1) == 0 && val < 1) ||
                ((j & 1) == 1 && val < 0)) {
                sdsfreesplitres(v,vlen);
                goto badfmt;
            }
        }
        resetServerSaveParams();
        for (j = 0; j < vlen; j += 2) {
            time_t seconds;
            int changes;
            seconds = strtoll(v[j],NULL,10);
            changes = strtoll(v[j+1],NULL,10);
            appendServerSaveParams(seconds, changes);
        }
        sdsfreesplitres(v,vlen);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-ERR not supported CONFIG parameter %s\r\n",
            (char*)c->argv[2]->ptr));
        decrRefCount(o);
        return;
    }
    decrRefCount(o);
    addReply(c,shared.ok);
    return;
badfmt:
    addReplySds(c,sdscatprintf(sdsempty(),
        "-ERR invalid argument '%s' for CONFIG SET '%s'\r\n",
            (char*)o->ptr,
            (char*)c->argv[2]->ptr));
    decrRefCount(o);
}
static void configGetCommand(redisClient *c) {
    robj *o = getDecodedObject(c->argv[2]);
    robj *lenobj = createObject(REDIS_STRING,NULL);
    char *pattern = o->ptr;
    int matches = 0;
    addReply(c,lenobj);
    decrRefCount(lenobj);
    if (stringmatch(pattern,"dbfilename",0)) {
        addReplyBulkCString(c,"dbfilename");
        addReplyBulkCString(c,server.dbfilename);
        matches++;
    }
    if (stringmatch(pattern,"requirepass",0)) {
        addReplyBulkCString(c,"requirepass");
        addReplyBulkCString(c,server.requirepass);
        matches++;
    }
    if (stringmatch(pattern,"masterauth",0)) {
        addReplyBulkCString(c,"masterauth");
        addReplyBulkCString(c,server.masterauth);
        matches++;
    }
    if (stringmatch(pattern,"maxmemory",0)) {
        char buf[128];
        ll2string(buf,128,server.maxmemory);
        addReplyBulkCString(c,"maxmemory");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"timeout",0)) {
        char buf[128];
        ll2string(buf,128,server.maxidletime);
        addReplyBulkCString(c,"timeout");
        addReplyBulkCString(c,buf);
        matches++;
    }
    if (stringmatch(pattern,"appendonly",0)) {
        addReplyBulkCString(c,"appendonly");
        addReplyBulkCString(c,server.appendonly ? "yes" : "no");
        matches++;
    }
    if (stringmatch(pattern,"no-appendfsync-on-rewrite",0)) {
        addReplyBulkCString(c,"no-appendfsync-on-rewrite");
        addReplyBulkCString(c,server.no_appendfsync_on_rewrite ? "yes" : "no");
        matches++;
    }
    if (stringmatch(pattern,"appendfsync",0)) {
        char *policy;
        switch(server.appendfsync) {
        case APPENDFSYNC_NO: policy = "no"; break;
        case APPENDFSYNC_EVERYSEC: policy = "everysec"; break;
        case APPENDFSYNC_ALWAYS: policy = "always"; break;
        default: policy = "unknown"; break;
        }
        addReplyBulkCString(c,"appendfsync");
        addReplyBulkCString(c,policy);
        matches++;
    }
    if (stringmatch(pattern,"save",0)) {
        sds buf = sdsempty();
        int j;
        for (j = 0; j < server.saveparamslen; j++) {
            buf = sdscatprintf(buf,"%ld %d",
                    server.saveparams[j].seconds,
                    server.saveparams[j].changes);
            if (j != server.saveparamslen-1)
                buf = sdscatlen(buf," ",1);
        }
        addReplyBulkCString(c,"save");
        addReplyBulkCString(c,buf);
        sdsfree(buf);
        matches++;
    }
    decrRefCount(o);
    lenobj->ptr = sdscatprintf(sdsempty(),"*%d\r\n",matches*2);
}
static void configCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"set")) {
        if (c->argc != 4) goto badarity;
        configSetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"get")) {
        if (c->argc != 3) goto badarity;
        configGetCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"resetstat")) {
        if (c->argc != 2) goto badarity;
        server.stat_numcommands = 0;
        server.stat_numconnections = 0;
        server.stat_expiredkeys = 0;
        server.stat_starttime = time(NULL);
        addReply(c,shared.ok);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-ERR CONFIG subcommand must be one of GET, SET, RESETSTAT\r\n"));
    }
    return;
badarity:
    addReplySds(c,sdscatprintf(sdsempty(),
        "-ERR Wrong number of arguments for CONFIG %s\r\n",
        (char*) c->argv[1]->ptr));
}
static void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;
    decrRefCount(pat->pattern);
    zfree(pat);
}
static int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;
    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}
static int pubsubSubscribeChannel(redisClient *c, robj *channel) {
    struct dictEntry *de;
    list *clients = NULL;
    int retval = 0;
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = dictGetEntryVal(de);
        }
        listAddNodeTail(clients,c);
    }
    addReply(c,shared.mbulk3);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    return retval;
}
static int pubsubUnsubscribeChannel(redisClient *c, robj *channel, int notify) {
    struct dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;
    incrRefCount(channel);
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {
        retval = 1;
        de = dictFind(server.pubsub_channels,channel);
        assert(de != NULL);
        clients = dictGetEntryVal(de);
        ln = listSearchKey(clients,c);
        assert(ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            dictDelete(server.pubsub_channels,channel);
        }
    }
    if (notify) {
        addReply(c,shared.mbulk3);
        addReply(c,shared.unsubscribebulk);
        addReplyBulk(c,channel);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    decrRefCount(channel);
    return retval;
}
static int pubsubSubscribePattern(redisClient *c, robj *pattern) {
    int retval = 0;
    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        listAddNodeTail(server.pubsub_patterns,pat);
    }
    addReply(c,shared.mbulk3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    return retval;
}
static int pubsubUnsubscribePattern(redisClient *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;
    incrRefCount(pattern);
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        listDelNode(c->pubsub_patterns,ln);
        pat.client = c;
        pat.pattern = pattern;
        ln = listSearchKey(server.pubsub_patterns,&pat);
        listDelNode(server.pubsub_patterns,ln);
    }
    if (notify) {
        addReply(c,shared.mbulk3);
        addReply(c,shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+
                       listLength(c->pubsub_patterns));
    }
    decrRefCount(pattern);
    return retval;
}
static int pubsubUnsubscribeAllChannels(redisClient *c, int notify) {
    dictIterator *di = dictGetIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;
    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetEntryKey(de);
        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    dictReleaseIterator(di);
    return count;
}
static int pubsubUnsubscribeAllPatterns(redisClient *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;
    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;
        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    return count;
}
static int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    struct dictEntry *de;
    listNode *ln;
    listIter li;
    de = dictFind(server.pubsub_channels,channel);
    if (de) {
        list *list = dictGetEntryVal(de);
        listNode *ln;
        listIter li;
        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            redisClient *c = ln->value;
            addReply(c,shared.mbulk3);
            addReply(c,shared.messagebulk);
            addReplyBulk(c,channel);
            addReplyBulk(c,message);
            receivers++;
        }
    }
    if (listLength(server.pubsub_patterns)) {
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {
            pubsubPattern *pat = ln->value;
            if (stringmatchlen((char*)pat->pattern->ptr,
                                sdslen(pat->pattern->ptr),
                                (char*)channel->ptr,
                                sdslen(channel->ptr),0)) {
                addReply(pat->client,shared.mbulk4);
                addReply(pat->client,shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);
                addReplyBulk(pat->client,channel);
                addReplyBulk(pat->client,message);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    return receivers;
}
static void subscribeCommand(redisClient *c) {
    int j;
    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
}
static void unsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
        return;
    } else {
        int j;
        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
}
static void psubscribeCommand(redisClient *c) {
    int j;
    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
}
static void punsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
        return;
    } else {
        int j;
        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
}
static void publishCommand(redisClient *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    addReplyLongLong(c,receivers);
}
typedef struct watchedKey {
    robj *key;
    redisDb *db;
} watchedKey;
static void watchForKey(redisClient *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return;
    }
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) {
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    listAddNodeTail(clients,c);
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}
static void unwatchAllKeys(redisClient *c) {
    listIter li;
    listNode *ln;
    if (listLength(c->watched_keys) == 0) return;
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;
        wk = listNodeValue(ln);
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        assert(clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}
static void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;
    if (dictSize(db->watched_keys) == 0) return;
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        redisClient *c = listNodeValue(ln);
        c->flags |= REDIS_DIRTY_CAS;
    }
}
static void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        redisClient *c = listNodeValue(ln);
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);
            if (dbid == -1 || wk->db->id == dbid) {
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= REDIS_DIRTY_CAS;
            }
        }
    }
}
static void watchCommand(redisClient *c) {
    int j;
    if (c->flags & REDIS_MULTI) {
        addReplySds(c,sdsnew("-ERR WATCH inside MULTI is not allowed\r\n"));
        return;
    }
    for (j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}
static void unwatchCommand(redisClient *c) {
    unwatchAllKeys(c);
    c->flags &= (~REDIS_DIRTY_CAS);
    addReply(c,shared.ok);
}
static void xorDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20], *s = ptr;
    int j;
    SHA1Init(&ctx);
    SHA1Update(&ctx,s,len);
    SHA1Final(hash,&ctx);
    for (j = 0; j < 20; j++)
        digest[j] ^= hash[j];
}
static void xorObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    xorDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}
static void mixDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    char *s = ptr;
    xorDigest(digest,s,len);
    SHA1Init(&ctx);
    SHA1Update(&ctx,digest,20);
    SHA1Final(digest,&ctx);
}
static void mixObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    mixDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}
static void computeDatasetDigest(unsigned char *final) {
    unsigned char digest[20];
    char buf[128];
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    uint32_t aux;
    memset(final,0,20);
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        if (dictSize(db->dict) == 0) continue;
        di = dictGetIterator(db->dict);
        aux = htonl(j);
        mixDigest(final,&aux,sizeof(aux));
        while((de = dictNext(di)) != NULL) {
            sds key;
            robj *keyobj, *o;
            time_t expiretime;
            memset(digest,0,20);
            key = dictGetEntryKey(de);
            keyobj = createStringObject(key,sdslen(key));
            mixDigest(digest,key,sdslen(key));
            o = lookupKeyRead(db,keyobj);
            aux = htonl(o->type);
            mixDigest(digest,&aux,sizeof(aux));
            expiretime = getExpire(db,keyobj);
            if (o->type == REDIS_STRING) {
                mixObjectDigest(digest,o);
            } else if (o->type == REDIS_LIST) {
                lIterator *li = lInitIterator(o,0,REDIS_TAIL);
                lEntry entry;
                while(lNext(li,&entry)) {
                    robj *eleobj = lGet(&entry);
                    mixObjectDigest(digest,eleobj);
                    decrRefCount(eleobj);
                }
                lReleaseIterator(li);
            } else if (o->type == REDIS_SET) {
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;
                while((de = dictNext(di)) != NULL) {
                    robj *eleobj = dictGetEntryKey(de);
                    xorObjectDigest(digest,eleobj);
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_ZSET) {
                zset *zs = o->ptr;
                dictIterator *di = dictGetIterator(zs->dict);
                dictEntry *de;
                while((de = dictNext(di)) != NULL) {
                    robj *eleobj = dictGetEntryKey(de);
                    double *score = dictGetEntryVal(de);
                    unsigned char eledigest[20];
                    snprintf(buf,sizeof(buf),"%.17g",*score);
                    memset(eledigest,0,20);
                    mixObjectDigest(eledigest,eleobj);
                    mixDigest(eledigest,buf,strlen(buf));
                    xorDigest(digest,eledigest,20);
                }
                dictReleaseIterator(di);
            } else if (o->type == REDIS_HASH) {
                hashIterator *hi;
                robj *obj;
                hi = hashInitIterator(o);
                while (hashNext(hi) != REDIS_ERR) {
                    unsigned char eledigest[20];
                    memset(eledigest,0,20);
                    obj = hashCurrent(hi,REDIS_HASH_KEY);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    obj = hashCurrent(hi,REDIS_HASH_VALUE);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    xorDigest(digest,eledigest,20);
                }
                hashReleaseIterator(hi);
            } else {
                redisPanic("Unknown object type");
            }
            if (expiretime != -1) xorDigest(digest,"!!expire!!",10);
            xorDigest(final,digest,20);
            decrRefCount(keyobj);
        }
        dictReleaseIterator(di);
    }
}
static void debugCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"segfault")) {
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"reload")) {
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        emptyDb();
        if (rdbLoad(server.dbfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        redisLog(REDIS_WARNING,"DB reloaded by DEBUG RELOAD");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"loadaof")) {
        emptyDb();
        if (loadAppendOnlyFile(server.appendfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        redisLog(REDIS_WARNING,"Append Only File loaded by DEBUG LOADAOF");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]->ptr);
        robj *val;
        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        val = dictGetEntryVal(de);
        if (!server.vm_enabled || (val->storage == REDIS_VM_MEMORY ||
                                   val->storage == REDIS_VM_SWAPPING)) {
            char *strenc;
            char buf[128];
            if (val->encoding < (sizeof(strencoding)/sizeof(char*))) {
                strenc = strencoding[val->encoding];
            } else {
                snprintf(buf,64,"unknown encoding %d\n", val->encoding);
                strenc = buf;
            }
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Value at:%p refcount:%d "
                "encoding:%s serializedlength:%lld\r\n",
                (void*)val, val->refcount,
                strenc, (long long) rdbSavedObjectLen(val,NULL)));
        } else {
            vmpointer *vp = (vmpointer*) val;
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Value swapped at: page %llu "
                "using %llu pages\r\n",
                (unsigned long long) vp->page,
                (unsigned long long) vp->usedpages));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"swapin") && c->argc == 3) {
        lookupKeyRead(c->db,c->argv[2]);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"swapout") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]->ptr);
        robj *val;
        vmpointer *vp;
        if (!server.vm_enabled) {
            addReplySds(c,sdsnew("-ERR Virtual Memory is disabled\r\n"));
            return;
        }
        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        val = dictGetEntryVal(de);
        if (val->storage != REDIS_VM_MEMORY) {
            addReplySds(c,sdsnew("-ERR This key is not in memory\r\n"));
        } else if (val->refcount != 1) {
            addReplySds(c,sdsnew("-ERR Object is shared\r\n"));
        } else if ((vp = vmSwapObjectBlocking(val)) != NULL) {
            dictGetEntryVal(de) = vp;
            addReply(c,shared.ok);
        } else {
            addReply(c,shared.err);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"populate") && c->argc == 3) {
        long keys, j;
        robj *key, *val;
        char buf[128];
        if (getLongFromObjectOrReply(c, c->argv[2], &keys, NULL) != REDIS_OK)
            return;
        for (j = 0; j < keys; j++) {
            snprintf(buf,sizeof(buf),"key:%lu",j);
            key = createStringObject(buf,strlen(buf));
            if (lookupKeyRead(c->db,key) != NULL) {
                decrRefCount(key);
                continue;
            }
            snprintf(buf,sizeof(buf),"value:%lu",j);
            val = createStringObject(buf,strlen(buf));
            dbAdd(c->db,key,val);
            decrRefCount(key);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"digest") && c->argc == 2) {
        unsigned char digest[20];
        sds d = sdsnew("+");
        int j;
        computeDatasetDigest(digest);
        for (j = 0; j < 20; j++)
            d = sdscatprintf(d, "%02x",digest[j]);
        d = sdscatlen(d,"\r\n",2);
        addReplySds(c,d);
    } else {
        addReplySds(c,sdsnew(
            "-ERR Syntax error, try DEBUG [SEGFAULT|OBJECT <key>|SWAPIN <key>|SWAPOUT <key>|RELOAD]\r\n"));
    }
}
static void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}
static void _redisPanic(char *msg, char *file, int line) {
    redisLog(REDIS_WARNING,"!!! Software Failure. Press left mouse button to continue");
    redisLog(REDIS_WARNING,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
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
static void daemonize(void) {
    int fd;
    FILE *fp;
    if (fork() != 0) exit(0);
    setsid();
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",getpid());
        fclose(fp);
    }
}
static void version() {
    printf("Redis server version %s (%s:%d)\n", REDIS_VERSION,
        REDIS_GIT_SHA1, atoi(REDIS_GIT_DIRTY) > 0);
    exit(0);
}
static void usage() {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    exit(1);
}
int main(int argc, char **argv) {
    time_t start;
    initServerConfig();
    sortCommandTable();
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
    redisLog(REDIS_NOTICE,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    start = time(NULL);
    if (server.appendonly) {
        if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from append only file: %ld seconds",time(NULL)-start);
    } else {
        if (rdbLoad(server.dbfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from disk: %ld seconds",time(NULL)-start);
    }
    redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
#ifdef HAVE_BACKTRACE
static char *findFuncName(void *pointer, unsigned long *offset);
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
#elif defined(__i386__) || defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[REG_EIP];
#elif defined(__ia64__)
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}
static void segvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    unsigned long offset=0;
    ucontext_t *uc = (ucontext_t*) secret;
    sds infostring;
    REDIS_NOTUSED(info);
    redisLog(REDIS_WARNING,
        "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
    infostring = genRedisInfoString();
    redisLog(REDIS_WARNING, "%s",infostring);
    trace_size = backtrace(trace, 100);
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);
    for (i=1; i<trace_size; ++i) {
        char *fn = findFuncName(trace[i], &offset), *p;
        p = strchr(messages[i],'+');
        if (!fn || (p && ((unsigned long)strtol(p+1,NULL,10)) < offset)) {
            redisLog(REDIS_WARNING,"%s", messages[i]);
        } else {
            redisLog(REDIS_WARNING,"%d redis-server %p %s + %d", i, trace[i], fn, (unsigned int)offset);
        }
    }
    _exit(0);
}
static void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);
    redisLog(REDIS_WARNING,"SIGTERM received, scheduling shutting down...");
    server.shutdown_asap = 1;
}
static void setupSigSegvAction(void) {
    struct sigaction act;
    sigemptyset (&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = segvHandler;
    sigaction (SIGSEGV, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    sigaction (SIGFPE, &act, NULL);
    sigaction (SIGILL, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = sigtermHandler;
    sigaction (SIGTERM, &act, NULL);
    return;
}
#include "staticsymbols.h"
static char *findFuncName(void *pointer, unsigned long *offset){
    int i, ret = -1;
    unsigned long off, minoff = 0;
    for (i=0; symsTable[i].pointer; i++) {
        unsigned long lp = (unsigned long) pointer;
        if (lp != (unsigned long)-1 && lp >= symsTable[i].pointer) {
            off=lp-symsTable[i].pointer;
            if (ret < 0 || off < minoff) {
                minoff=off;
                ret=i;
            }
        }
    }
    if (ret == -1) return NULL;
    *offset = minoff;
    return symsTable[ret].name;
}
#else
static void setupSigSegvAction(void) {
}
#endif
