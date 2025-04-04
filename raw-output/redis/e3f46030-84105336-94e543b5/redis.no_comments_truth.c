#define REDIS_VERSION "1.3.4"
#include "fmacros.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __USE_POSIX199309 
#define __USE_UNIX98 
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
#define REDIS_OK 0
#define REDIS_ERR -1
#define REDIS_SERVERPORT 6379
#define REDIS_MAXIDLETIME (60*5)
#define REDIS_IOBUF_LEN 1024
#define REDIS_LOADBUF_LEN 1024
#define REDIS_STATIC_ARGS 4
#define REDIS_DEFAULT_DBNUM 16
#define REDIS_CONFIGLINE_MAX 1024
#define REDIS_OBJFREELIST_MAX 1000000
#define REDIS_MAX_SYNC_TIME 60
#define REDIS_EXPIRELOOKUPS_PER_CRON 100
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256)
#define REDIS_WRITEV_THRESHOLD 3
#define REDIS_WRITEV_IOVEC_COUNT 256
#define REDIS_HT_MINFILL 10
#define REDIS_CMD_BULK 1
#define REDIS_CMD_INLINE 2
#define REDIS_CMD_DENYOOM 4
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4
#define REDIS_ENCODING_RAW 0
#define REDIS_ENCODING_INT 1
#define REDIS_ENCODING_ZIPMAP 2
#define REDIS_ENCODING_HT 3
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
static void _redisAssert(char *estr, char *file, int line);
struct redisObjectVM {
    off_t page;
    off_t usedpages;
    time_t atime;
} vm;
typedef struct redisObject {
    void *ptr;
    unsigned char type;
    unsigned char encoding;
    unsigned char storage;
    unsigned char vtype;
    int refcount;
    struct redisObjectVM vm;
} robj;
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
    if (server.vm_enabled) _var.storage = REDIS_VM_MEMORY; \
} while(0);
typedef struct redisDb {
    dict *dict;
    dict *expires;
    dict *blockingkeys;
    dict *io_keys;
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
    robj **blockingkeys;
    int blockingkeysnum;
    time_t blockingto;
    list *io_keys;
} redisClient;
struct saveparam {
    time_t seconds;
    int changes;
};
struct redisServer {
    int port;
    int fd;
    redisDb *db;
    dict *sharingpool;
    unsigned int sharingpoolsize;
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
    int verbosity;
    int glueoutputbuf;
    int maxidletime;
    int dbnum;
    int daemonize;
    int appendonly;
    int appendfsync;
    time_t lastfsync;
    int appendfd;
    int appendseldb;
    char *pidfile;
    pid_t bgsavechildpid;
    pid_t bgrewritechildpid;
    sds bgrewritebuf;
    struct saveparam *saveparams;
    int saveparamslen;
    char *logfile;
    char *bindaddr;
    char *dbfilename;
    char *appendfilename;
    char *requirepass;
    int shareobjects;
    int rdbcompression;
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
    FILE *devnull;
};
typedef void redisCommandProc(redisClient *c);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    int flags;
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
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9;
} shared;
static double R_Zero, R_PosInf, R_NegInf, R_Nan;
#define REDIS_IOJOB_LOAD 0
#define REDIS_IOJOB_PREPARE_SWAP 1
#define REDIS_IOJOB_DO_SWAP 2
typedef struct iojob {
    int type;
    redisDb *db;
    robj *key;
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
static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc);
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
static int syncWithMaster(void);
static robj *tryObjectSharing(robj *o);
static int tryObjectEncoding(robj *o);
static robj *getDecodedObject(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteIfVolatile(redisDb *db, robj *key);
static int deleteIfSwapped(redisDb *db, robj *key);
static int deleteKey(redisDb *db, robj *key);
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
static robj *vmLoadObject(robj *key);
static robj *vmPreviewObject(robj *key);
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
static int blockClientOnSwappedKeys(struct redisCommand *cmd, redisClient *c);
static int dontWaitForSwappedKey(redisClient *c, robj *key);
static void handleClientsBlockedOnSwappedKey(redisDb *db, robj *key);
static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
static struct redisCommand *lookupCommand(char *name);
static void call(redisClient *c, struct redisCommand *cmd);
static void resetClient(redisClient *c);
static void convertToRealHash(robj *o);
static void authCommand(redisClient *c);
static void pingCommand(redisClient *c);
static void echoCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
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
static void hgetCommand(redisClient *c);
static void zremrangebyrankCommand(redisClient *c);
static struct redisServer server;
static struct redisCommand cmdTable[] = {
    {"get",getCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"set",setCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,0,0,0},
    {"setnx",setnxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,0,0,0},
    {"append",appendCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"substr",substrCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"del",delCommand,-2,REDIS_CMD_INLINE,0,0,0},
    {"exists",existsCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"incr",incrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,1,1},
    {"decr",decrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,1,1},
    {"mget",mgetCommand,-2,REDIS_CMD_INLINE,1,-1,1},
    {"rpush",rpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"lpush",lpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"rpop",rpopCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"lpop",lpopCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"brpop",brpopCommand,-3,REDIS_CMD_INLINE,1,1,1},
    {"blpop",blpopCommand,-3,REDIS_CMD_INLINE,1,1,1},
    {"llen",llenCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"lindex",lindexCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"lset",lsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"lrange",lrangeCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"ltrim",ltrimCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"lrem",lremCommand,4,REDIS_CMD_BULK,1,1,1},
    {"rpoplpush",rpoplpushcommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,2,1},
    {"sadd",saddCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"srem",sremCommand,3,REDIS_CMD_BULK,1,1,1},
    {"smove",smoveCommand,4,REDIS_CMD_BULK,1,2,1},
    {"sismember",sismemberCommand,3,REDIS_CMD_BULK,1,1,1},
    {"scard",scardCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"spop",spopCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"srandmember",srandmemberCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"sinter",sinterCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,-1,1},
    {"sinterstore",sinterstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,2,-1,1},
    {"sunion",sunionCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,-1,1},
    {"sunionstore",sunionstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,2,-1,1},
    {"sdiff",sdiffCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,-1,1},
    {"sdiffstore",sdiffstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,2,-1,1},
    {"smembers",sinterCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"zadd",zaddCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"zincrby",zincrbyCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"zrem",zremCommand,3,REDIS_CMD_BULK,1,1,1},
    {"zremrangebyscore",zremrangebyscoreCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"zremrangebyrank",zremrangebyrankCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"zrange",zrangeCommand,-4,REDIS_CMD_INLINE,1,1,1},
    {"zrangebyscore",zrangebyscoreCommand,-4,REDIS_CMD_INLINE,1,1,1},
    {"zcount",zcountCommand,4,REDIS_CMD_INLINE,1,1,1},
    {"zrevrange",zrevrangeCommand,-4,REDIS_CMD_INLINE,1,1,1},
    {"zcard",zcardCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"zscore",zscoreCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"zrank",zrankCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"zrevrank",zrevrankCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"hset",hsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"hget",hgetCommand,3,REDIS_CMD_BULK,1,1,1},
    {"incrby",incrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,1,1},
    {"decrby",decrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,1,1},
    {"getset",getsetCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,1,1},
    {"mset",msetCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,-1,2},
    {"msetnx",msetnxCommand,-3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM,1,-1,2},
    {"randomkey",randomkeyCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"select",selectCommand,2,REDIS_CMD_INLINE,0,0,0},
    {"move",moveCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"rename",renameCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"renamenx",renamenxCommand,3,REDIS_CMD_INLINE,1,1,1},
    {"expire",expireCommand,3,REDIS_CMD_INLINE,0,0,0},
    {"expireat",expireatCommand,3,REDIS_CMD_INLINE,0,0,0},
    {"keys",keysCommand,2,REDIS_CMD_INLINE,0,0,0},
    {"dbsize",dbsizeCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"auth",authCommand,2,REDIS_CMD_INLINE,0,0,0},
    {"ping",pingCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"echo",echoCommand,2,REDIS_CMD_BULK,0,0,0},
    {"save",saveCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"bgsave",bgsaveCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"shutdown",shutdownCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"lastsave",lastsaveCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"type",typeCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"multi",multiCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"exec",execCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"discard",discardCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"sync",syncCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"flushdb",flushdbCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"flushall",flushallCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"sort",sortCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM,1,1,1},
    {"info",infoCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"monitor",monitorCommand,1,REDIS_CMD_INLINE,0,0,0},
    {"ttl",ttlCommand,2,REDIS_CMD_INLINE,1,1,1},
    {"slaveof",slaveofCommand,3,REDIS_CMD_INLINE,0,0,0},
    {"debug",debugCommand,-2,REDIS_CMD_INLINE,0,0,0},
    {NULL,NULL,0,0,0,0,0}
};
int stringmatchlen(const char *pattern, int patternLen,
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
static int sdsDictKeyCompare(void *privdata, const void *key1,
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
static int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata,o1->ptr,o2->ptr);
}
static unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}
static int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;
    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = sdsDictKeyCompare(privdata,o1->ptr,o2->ptr);
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
            len = snprintf(buf,32,"%ld",(long)o->ptr);
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
    dictObjHash,
    NULL,
    NULL,
    dictObjKeyCompare,
    dictRedisObjectDestructor,
    dictRedisObjectDestructor
};
static dictType keyptrDictType = {
    dictObjHash,
    NULL,
    NULL,
    dictObjKeyCompare,
    dictRedisObjectDestructor,
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
        if (htNeedsResize(server.db[j].dict)) {
            redisLog(REDIS_VERBOSE,"The hash table %d is too sparse, resize it...",j);
            dictResize(server.db[j].dict);
            redisLog(REDIS_VERBOSE,"Hash table %d resized.",j);
        }
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
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
            "Background saving terminated by signal");
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
            fsync(fd);
            server.appendseldb = -1;
            redisLog(REDIS_NOTICE,"The new append only file was selected for future appends.");
        } else {
            close(fd);
        }
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background append only file rewriting error");
    } else {
        redisLog(REDIS_WARNING,
            "Background append only file rewriting terminated by signal");
    }
cleanup:
    sdsfree(server.bgrewritebuf);
    server.bgrewritebuf = sdsempty();
    aofRemoveTempFile(server.bgrewritechildpid);
    server.bgrewritechildpid = -1;
}
static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);
    server.unixtime = time(NULL);
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;
        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 5) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
        }
    }
    if (server.bgsavechildpid == -1) tryResizeHashTables();
    if (!(loops % 5)) {
        redisLog(REDIS_VERBOSE,"%d clients connected (%d slaves), %zu bytes in use, %d shared objects",
            listLength(server.clients)-listLength(server.slaves),
            listLength(server.slaves),
            zmalloc_used_memory(),
            dictSize(server.sharingpool));
    }
    if ((server.maxidletime && !(loops % 10)) || server.blpop_blocked_clients)
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
                    deleteKey(db,dictGetEntryKey(de));
                    expired++;
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
            if (retval == REDIS_ERR && (loops % 30) == 0 &&
                zmalloc_used_memory() >
                (server.vm_max_memory+server.vm_max_memory/10))
            {
                redisLog(REDIS_WARNING,"WARNING: vm-max-memory limit exceeded by more than 10%% but unable to swap more objects out!");
            }
            if (retval == REDIS_ERR || server.vm_max_threads > 0) break;
        }
    }
    if (server.replstate == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER...");
        if (syncWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync succeeded");
        }
    }
    return 1000;
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
}
static void createSharedObjects(void) {
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
    server.appendfsync = APPENDFSYNC_ALWAYS;
    server.lastfsync = time(NULL);
    server.appendfd = -1;
    server.appendseldb = -1;
    server.pidfile = "/var/run/redis.pid";
    server.dbfilename = "dump.rdb";
    server.appendfilename = "appendonly.aof";
    server.requirepass = NULL;
    server.shareobjects = 0;
    server.rdbcompression = 1;
    server.sharingpoolsize = 1024;
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
    server.sharingpool = dictCreate(&setDictType,NULL);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blockingkeys = dictCreate(&keylistDictType,NULL);
        if (server.vm_enabled)
            server.db[j].io_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].id = j;
    }
    server.cronloops = 0;
    server.bgsavechildpid = -1;
    server.bgrewritechildpid = -1;
    server.bgrewritebuf = sdsempty();
    server.lastsave = time(NULL);
    server.dirty = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
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
            redisLog(REDIS_WARNING,"Fatal error, can't open config file");
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
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = strtoll(argv[1], NULL, 10);
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
        } else if (!strcasecmp(argv[0],"shareobjects") && argc == 2) {
            if ((server.shareobjects = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"rdbcompression") && argc == 2) {
            if ((server.rdbcompression = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"shareobjectspoolsize") && argc == 2) {
            server.sharingpoolsize = atoi(argv[1]);
            if (server.sharingpoolsize < 1) {
                err = "invalid object sharing pool size"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"appendonly") && argc == 2) {
            if ((server.appendonly = yesnotoi(argv[1])) == -1) {
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
            server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
            server.dbfilename = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-enabled") && argc == 2) {
            if ((server.vm_enabled = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"vm-swap-file") && argc == 2) {
            zfree(server.vm_swap_file);
            server.vm_swap_file = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"vm-max-memory") && argc == 2) {
            server.vm_max_memory = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"vm-page-size") && argc == 2) {
            server.vm_page_size = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"vm-pages") && argc == 2) {
            server.vm_pages = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"vm-max-threads") && argc == 2) {
            server.vm_max_threads = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-entries") && argc == 2){
            server.hash_max_zipmap_entries = strtol(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"hash-max-zipmap-value") && argc == 2){
            server.hash_max_zipmap_value = strtol(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"vm-max-threads") && argc == 2) {
            server.vm_max_threads = strtoll(argv[1], NULL, 10);
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
static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while(cmdTable[j].name != NULL) {
        if (!strcasecmp(name,cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
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
    if (server.appendonly && server.dirty-dirty)
        feedAppendOnlyFile(cmd,c->db->id,c->argv,c->argc);
    if (server.dirty-dirty && listLength(server.slaves))
        replicationFeedSlaves(server.slaves,cmd,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedSlaves(server.monitors,cmd,c->db->id,c->argv,c->argc);
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
    } else if (server.maxmemory && cmd->flags & REDIS_CMD_DENYOOM && zmalloc_used_memory() > server.maxmemory) {
        addReplySds(c,sdsnew("-ERR command not allowed when used memory > 'maxmemory'\r\n"));
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
    if (server.shareobjects) {
        int j;
        for(j = 1; j < c->argc; j++)
            c->argv[j] = tryObjectSharing(c->argv[j]);
    }
    if (cmd->flags & REDIS_CMD_BULK)
        tryObjectEncoding(c->argv[c->argc-1]);
    if (server.requirepass && !c->authenticated && cmd->proc != authCommand) {
        addReplySds(c,sdsnew("-ERR operation not permitted\r\n"));
        resetClient(c);
        return 1;
    }
    if (c->flags & REDIS_MULTI && cmd->proc != execCommand && cmd->proc != discardCommand) {
        queueMultiCommand(c,cmd);
        addReply(c,shared.queued);
    } else {
        if (server.vm_enabled && server.vm_max_threads > 0 &&
            blockClientOnSwappedKeys(cmd,c)) return 1;
        call(c,cmd);
    }
    resetClient(c);
    return 1;
}
static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int outc = 0, j;
    robj **outv;
    robj *static_outv[REDIS_STATIC_ARGS*2+1];
    if (argc <= REDIS_STATIC_ARGS) {
        outv = static_outv;
    } else {
        outv = zmalloc(sizeof(robj*)*(argc*2+1));
    }
    for (j = 0; j < argc; j++) {
        if (j != 0) outv[outc++] = shared.space;
        if ((cmd->flags & REDIS_CMD_BULK) && j == argc-1) {
            robj *lenobj;
            lenobj = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),"%lu\r\n",
                    (unsigned long) stringObjectLen(argv[j])));
            lenobj->refcount = 0;
            outv[outc++] = lenobj;
        }
        outv[outc++] = argv[j];
    }
    outv[outc++] = shared.crlf;
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
    if (!(c->flags & REDIS_BLOCKED))
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
    c->blockingkeys = NULL;
    c->blockingkeysnum = 0;
    c->io_keys = listCreate();
    listSetFreeMethod(c->io_keys,decrRefCount);
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
static void addReplyLong(redisClient *c, long l) {
    char buf[128];
    size_t len;
    len = snprintf(buf,sizeof(buf),":%ld\r\n",l);
    addReplySds(c,sdsnewlen(buf,len));
}
static void addReplyBulkLen(redisClient *c, robj *obj) {
    size_t len;
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
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",(unsigned long)len));
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
        if (server.vm_enabled) {
            pthread_mutex_unlock(&server.obj_freelist_mutex);
            o = zmalloc(sizeof(*o));
        } else {
            o = zmalloc(sizeof(*o)-sizeof(struct redisObjectVM));
        }
    }
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    if (server.vm_enabled) {
        o->vm.atime = server.unixtime;
        o->storage = REDIS_VM_MEMORY;
    }
    return o;
}
static robj *createStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}
static robj *dupStringObject(robj *o) {
    assert(o->encoding == REDIS_ENCODING_RAW);
    return createStringObject(o->ptr,sdslen(o->ptr));
}
static robj *createListObject(void) {
    list *l = listCreate();
    listSetFreeMethod(l,decrRefCount);
    return createObject(REDIS_LIST,l);
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
    listRelease((list*) o->ptr);
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
        redisAssert(0);
        break;
    }
}
static void incrRefCount(robj *o) {
    redisAssert(!server.vm_enabled || o->storage == REDIS_VM_MEMORY);
    o->refcount++;
}
static void decrRefCount(void *obj) {
    robj *o = obj;
    if (server.vm_enabled &&
        (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING))
    {
        if (o->storage == REDIS_VM_SWAPPED || o->storage == REDIS_VM_LOADING) {
            redisAssert(o->refcount == 1);
        }
        if (o->storage == REDIS_VM_LOADING) vmCancelThreadedIOJob(obj);
        redisAssert(o->type == REDIS_STRING);
        freeStringObject(o);
        vmMarkPagesFree(o->vm.page,o->vm.usedpages);
        pthread_mutex_lock(&server.obj_freelist_mutex);
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
        pthread_mutex_unlock(&server.obj_freelist_mutex);
        server.vm_stats_swapped_objects--;
        return;
    }
    if (--(o->refcount) == 0) {
        if (server.vm_enabled && o->storage == REDIS_VM_SWAPPING)
            vmCancelThreadedIOJob(obj);
        switch(o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        case REDIS_LIST: freeListObject(o); break;
        case REDIS_SET: freeSetObject(o); break;
        case REDIS_ZSET: freeZsetObject(o); break;
        case REDIS_HASH: freeHashObject(o); break;
        default: redisAssert(0 != 0); break;
        }
        if (server.vm_enabled) pthread_mutex_lock(&server.obj_freelist_mutex);
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
        if (server.vm_enabled) pthread_mutex_unlock(&server.obj_freelist_mutex);
    }
}
static robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key);
    if (de) {
        robj *key = dictGetEntryKey(de);
        robj *val = dictGetEntryVal(de);
        if (server.vm_enabled) {
            if (key->storage == REDIS_VM_MEMORY ||
                key->storage == REDIS_VM_SWAPPING)
            {
                if (key->storage == REDIS_VM_SWAPPING)
                    vmCancelThreadedIOJob(key);
                key->vm.atime = server.unixtime;
            } else {
                int notify = (key->storage == REDIS_VM_LOADING);
                redisAssert(val == NULL);
                val = vmLoadObject(key);
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
    return lookupKey(db,key);
}
static int deleteKey(redisDb *db, robj *key) {
    int retval;
    incrRefCount(key);
    if (dictSize(db->expires)) dictDelete(db->expires,key);
    retval = dictDelete(db->dict,key);
    decrRefCount(key);
    return retval == DICT_OK;
}
static robj *tryObjectSharing(robj *o) {
    struct dictEntry *de;
    unsigned long c;
    if (o == NULL || server.shareobjects == 0) return o;
    redisAssert(o->type == REDIS_STRING);
    de = dictFind(server.sharingpool,o);
    if (de) {
        robj *shared = dictGetEntryKey(de);
        c = ((unsigned long) dictGetEntryVal(de))+1;
        dictGetEntryVal(de) = (void*) c;
        incrRefCount(shared);
        decrRefCount(o);
        return shared;
    } else {
        if (dictSize(server.sharingpool) >=
                server.sharingpoolsize) {
            de = dictGetRandomKey(server.sharingpool);
            redisAssert(de != NULL);
            c = ((unsigned long) dictGetEntryVal(de))-1;
            dictGetEntryVal(de) = (void*) c;
            if (c == 0) {
                dictDelete(server.sharingpool,de->key);
            }
        } else {
            c = 0;
        }
        if (c == 0) {
            int retval;
            retval = dictAdd(server.sharingpool,o,(void*)1);
            redisAssert(retval == DICT_OK);
            incrRefCount(o);
        }
        return o;
    }
}
static int isStringRepresentableAsLong(sds s, long *longval) {
    char buf[32], *endptr;
    long value;
    int slen;
    value = strtol(s, &endptr, 10);
    if (endptr[0] != '\0') return REDIS_ERR;
    slen = snprintf(buf,32,"%ld",value);
    if (sdslen(s) != (unsigned)slen || memcmp(buf,s,slen)) return REDIS_ERR;
    if (longval) *longval = value;
    return REDIS_OK;
}
static int tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;
    if (o->encoding != REDIS_ENCODING_RAW)
        return REDIS_ERR;
     if (o->refcount > 1) return REDIS_ERR;
    redisAssert(o->type == REDIS_STRING);
    if (isStringRepresentableAsLong(s,&value) == REDIS_ERR) return REDIS_ERR;
    o->encoding = REDIS_ENCODING_INT;
    sdsfree(o->ptr);
    o->ptr = (void*) value;
    return REDIS_OK;
}
static robj *getDecodedObject(robj *o) {
    robj *dec;
    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
        char buf[32];
        snprintf(buf,32,"%ld",(long)o->ptr);
        dec = createStringObject(buf,strlen(buf));
        return dec;
    } else {
        redisAssert(1 != 1);
    }
}
static int compareStringObjects(robj *a, robj *b) {
    redisAssert(a->type == REDIS_STRING && b->type == REDIS_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    int bothsds = 1;
    if (a == b) return 0;
    if (a->encoding != REDIS_ENCODING_RAW) {
        snprintf(bufa,sizeof(bufa),"%ld",(long) a->ptr);
        astr = bufa;
        bothsds = 0;
    } else {
        astr = a->ptr;
    }
    if (b->encoding != REDIS_ENCODING_RAW) {
        snprintf(bufb,sizeof(bufb),"%ld",(long) b->ptr);
        bstr = bufb;
        bothsds = 0;
    } else {
        bstr = b->ptr;
    }
    return bothsds ? sdscmp(astr,bstr) : strcmp(astr,bstr);
}
static size_t stringObjectLen(robj *o) {
    redisAssert(o->type == REDIS_STRING);
    if (o->encoding == REDIS_ENCODING_RAW) {
        return sdslen(o->ptr);
    } else {
        char buf[32];
        return snprintf(buf,32,"%ld",(long)o->ptr);
    }
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
static int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    snprintf(buf,32,"%lld",value);
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;
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
static int rdbSaveStringObject(FILE *fp, robj *obj) {
    int retval;
    if (obj->encoding != REDIS_ENCODING_RAW) {
        obj = getDecodedObject(obj);
        retval = rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
        decrRefCount(obj);
    } else {
        retval = rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
    }
    return retval;
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
        list *list = o->ptr;
        listIter li;
        listNode *ln;
        if (rdbSaveLen(fp,listLength(list)) == -1) return -1;
        listRewind(list,&li);
        while((ln = listNext(&li))) {
            robj *eleobj = listNodeValue(ln);
            if (rdbSaveStringObject(fp,eleobj) == -1) return -1;
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
        redisAssert(0 != 0);
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
            robj *key = dictGetEntryKey(de);
            robj *o = dictGetEntryVal(de);
            time_t expiretime = getExpire(db,key);
            if (expiretime != -1) {
                if (expiretime < now) continue;
                if (rdbSaveType(fp,REDIS_EXPIRETIME) == -1) goto werr;
                if (rdbSaveTime(fp,expiretime) == -1) goto werr;
            }
            if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
                                      key->storage == REDIS_VM_SWAPPING) {
                if (rdbSaveType(fp,o->type) == -1) goto werr;
                if (rdbSaveStringObject(fp,key) == -1) goto werr;
                if (rdbSaveObject(fp,o) == -1) goto werr;
            } else {
                robj *po;
                po = vmPreviewObject(key);
                if (rdbSaveType(fp,key->vtype) == -1) goto werr;
                if (rdbSaveStringObject(fp,key) == -1) goto werr;
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
static robj *rdbLoadIntegerObject(FILE *fp, int enctype) {
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
        redisAssert(0!=0);
    }
    return createObject(REDIS_STRING,sdscatprintf(sdsempty(),"%lld",val));
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
static robj *rdbLoadStringObject(FILE*fp) {
    int isencoded;
    uint32_t len;
    sds val;
    len = rdbLoadLen(fp,&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return tryObjectSharing(rdbLoadIntegerObject(fp,len));
        case REDIS_RDB_ENC_LZF:
            return tryObjectSharing(rdbLoadLzfStringObject(fp));
        default:
            redisAssert(0!=0);
        }
    }
    if (len == REDIS_RDB_LENERR) return NULL;
    val = sdsnewlen(NULL,len);
    if (len && fread(val,len,1,fp) == 0) {
        sdsfree(val);
        return NULL;
    }
    return tryObjectSharing(createObject(REDIS_STRING,val));
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
    robj *o;
    if (type == REDIS_STRING) {
        if ((o = rdbLoadStringObject(fp)) == NULL) return NULL;
        tryObjectEncoding(o);
    } else if (type == REDIS_LIST || type == REDIS_SET) {
        uint32_t listlen;
        if ((listlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = (type == REDIS_LIST) ? createListObject() : createSetObject();
        if (type == REDIS_SET && listlen > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,listlen);
        while(listlen--) {
            robj *ele;
            if ((ele = rdbLoadStringObject(fp)) == NULL) return NULL;
            tryObjectEncoding(ele);
            if (type == REDIS_LIST) {
                listAddNodeTail((list*)o->ptr,ele);
            } else {
                dictAdd((dict*)o->ptr,ele,NULL);
            }
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
            if ((ele = rdbLoadStringObject(fp)) == NULL) return NULL;
            tryObjectEncoding(ele);
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
                tryObjectEncoding(key);
                tryObjectEncoding(val);
                dictAdd((dict*)o->ptr,key,val);
                incrRefCount(key);
                incrRefCount(val);
            }
        }
    } else {
        redisAssert(0 != 0);
    }
    return o;
}
static int rdbLoad(char *filename) {
    FILE *fp;
    robj *keyobj = NULL;
    uint32_t dbid;
    int type, retval, rdbver;
    dict *d = server.db[0].dict;
    redisDb *db = server.db+0;
    char buf[1024];
    time_t expiretime = -1, now = time(NULL);
    long long loadedkeys = 0;
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
        robj *o;
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
            d = db->dict;
            continue;
        }
        if ((keyobj = rdbLoadStringObject(fp)) == NULL) goto eoferr;
        if ((o = rdbLoadObject(type,fp)) == NULL) goto eoferr;
        retval = dictAdd(d,keyobj,o);
        if (retval == DICT_ERR) {
            redisLog(REDIS_WARNING,"Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", keyobj->ptr);
            exit(1);
        }
        if (expiretime != -1) {
            setExpire(db,keyobj,expiretime);
            if (expiretime < now) deleteKey(db,keyobj);
            expiretime = -1;
        }
        keyobj = o = NULL;
        loadedkeys++;
        if (server.vm_enabled && (loadedkeys % 5000) == 0) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
        }
    }
    fclose(fp);
    return REDIS_OK;
eoferr:
    if (keyobj) decrRefCount(keyobj);
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR;
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
    addReplyBulkLen(c,c->argv[1]);
    addReply(c,c->argv[1]);
    addReply(c,shared.crlf);
}
static void setGenericCommand(redisClient *c, int nx) {
    int retval;
    if (nx) deleteIfVolatile(c->db,c->argv[1]);
    retval = dictAdd(c->db->dict,c->argv[1],c->argv[2]);
    if (retval == DICT_ERR) {
        if (!nx) {
            if (server.vm_enabled && deleteIfSwapped(c->db,c->argv[1]))
                incrRefCount(c->argv[1]);
            dictReplace(c->db->dict,c->argv[1],c->argv[2]);
            incrRefCount(c->argv[2]);
        } else {
            addReply(c,shared.czero);
            return;
        }
    } else {
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
    }
    server.dirty++;
    removeExpire(c->db,c->argv[1]);
    addReply(c, nx ? shared.cone : shared.ok);
}
static void setCommand(redisClient *c) {
    setGenericCommand(c,0);
}
static void setnxCommand(redisClient *c) {
    setGenericCommand(c,1);
}
static int getGenericCommand(redisClient *c) {
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
        return REDIS_OK;
    } else {
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
            return REDIS_ERR;
        } else {
            addReplyBulkLen(c,o);
            addReply(c,o);
            addReply(c,shared.crlf);
            return REDIS_OK;
        }
    }
}
static void getCommand(redisClient *c) {
    getGenericCommand(c);
}
static void getsetCommand(redisClient *c) {
    if (getGenericCommand(c) == REDIS_ERR) return;
    if (dictAdd(c->db->dict,c->argv[1],c->argv[2]) == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],c->argv[2]);
    } else {
        incrRefCount(c->argv[1]);
    }
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
                addReplyBulkLen(c,o);
                addReply(c,o);
                addReply(c,shared.crlf);
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
        int retval;
        tryObjectEncoding(c->argv[j+1]);
        retval = dictAdd(c->db->dict,c->argv[j],c->argv[j+1]);
        if (retval == DICT_ERR) {
            dictReplace(c->db->dict,c->argv[j],c->argv[j+1]);
            incrRefCount(c->argv[j+1]);
        } else {
            incrRefCount(c->argv[j]);
            incrRefCount(c->argv[j+1]);
        }
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
    int retval;
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        value = 0;
    } else {
        if (o->type != REDIS_STRING) {
            value = 0;
        } else {
            char *eptr;
            if (o->encoding == REDIS_ENCODING_RAW)
                value = strtoll(o->ptr, &eptr, 10);
            else if (o->encoding == REDIS_ENCODING_INT)
                value = (long)o->ptr;
            else
                redisAssert(1 != 1);
        }
    }
    value += incr;
    o = createObject(REDIS_STRING,sdscatprintf(sdsempty(),"%lld",value));
    tryObjectEncoding(o);
    retval = dictAdd(c->db->dict,c->argv[1],o);
    if (retval == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],o);
        removeExpire(c->db,c->argv[1]);
    } else {
        incrRefCount(c->argv[1]);
    }
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
    long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
    incrDecrCommand(c,incr);
}
static void decrbyCommand(redisClient *c) {
    long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
    incrDecrCommand(c,-incr);
}
static void appendCommand(redisClient *c) {
    int retval;
    size_t totlen;
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        retval = dictAdd(c->db->dict,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        dictEntry *de;
        de = dictFind(c->db->dict,c->argv[1]);
        assert(de != NULL);
        o = dictGetEntryVal(de);
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (o->refcount != 1 || o->encoding != REDIS_ENCODING_RAW) {
            robj *decoded = getDecodedObject(o);
            o = createStringObject(decoded->ptr, sdslen(decoded->ptr));
            decrRefCount(decoded);
            dictReplace(c->db->dict,c->argv[1],o);
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
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
        } else {
            size_t rangelen, strlen;
            sds range;
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
            addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",rangelen));
            range = sdsnewlen((char*)o->ptr+start,rangelen);
            addReplySds(c,range);
            addReply(c,shared.crlf);
            decrRefCount(o);
        }
    }
}
static void delCommand(redisClient *c) {
    int deleted = 0, j;
    for (j = 1; j < c->argc; j++) {
        if (deleteKey(c->db,c->argv[j])) {
            server.dirty++;
            deleted++;
        }
    }
    switch(deleted) {
    case 0:
        addReply(c,shared.czero);
        break;
    case 1:
        addReply(c,shared.cone);
        break;
    default:
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",deleted));
        break;
    }
}
static void existsCommand(redisClient *c) {
    addReply(c,lookupKeyRead(c->db,c->argv[1]) ? shared.cone : shared.czero);
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
    dictEntry *de;
    while(1) {
        de = dictGetRandomKey(c->db->dict);
        if (!de || expireIfNeeded(c->db,dictGetEntryKey(de)) == 0) break;
    }
    if (de == NULL) {
        addReply(c,shared.plus);
        addReply(c,shared.crlf);
    } else {
        addReply(c,shared.plus);
        addReply(c,dictGetEntryKey(de));
        addReply(c,shared.crlf);
    }
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
        robj *keyobj = dictGetEntryKey(de);
        sds key = keyobj->ptr;
        if ((pattern[0] == '*' && pattern[1] == '\0') ||
            stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulkLen(c,keyobj);
                addReply(c,keyobj);
                addReply(c,shared.crlf);
                numkeys++;
            }
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
    redisLog(REDIS_WARNING,"User requested shutdown, saving DB...");
    if (server.bgsavechildpid != -1) {
        redisLog(REDIS_WARNING,"There is a live saving child. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    if (server.appendonly) {
        fsync(server.appendfd);
        if (server.vm_enabled) unlink(server.vm_swap_file);
        exit(0);
    } else {
        if (rdbSave(server.dbfilename) == REDIS_OK) {
            if (server.daemonize)
                unlink(server.pidfile);
            redisLog(REDIS_WARNING,"%zu bytes used at exit",zmalloc_used_memory());
            redisLog(REDIS_WARNING,"Server exit now, bye bye...");
            if (server.vm_enabled) unlink(server.vm_swap_file);
            exit(0);
        } else {
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
            addReplySds(c,sdsnew("-ERR can't quit, problems saving the DB\r\n"));
        }
    }
}
static void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nokeyerr);
        return;
    }
    incrRefCount(o);
    deleteIfVolatile(c->db,c->argv[2]);
    if (dictAdd(c->db->dict,c->argv[2],o) == DICT_ERR) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dictReplace(c->db->dict,c->argv[2],o);
    } else {
        incrRefCount(c->argv[2]);
    }
    deleteKey(c->db,c->argv[1]);
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
    if (dictAdd(dst->dict,c->argv[1],o) == DICT_ERR) {
        addReply(c,shared.czero);
        return;
    }
    incrRefCount(c->argv[1]);
    incrRefCount(o);
    deleteKey(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}
static void pushGenericCommand(redisClient *c, int where) {
    robj *lobj;
    list *list;
    lobj = lookupKeyWrite(c->db,c->argv[1]);
    if (lobj == NULL) {
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
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
    } else {
        if (lobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (handleClientsWaitingListPush(c,c->argv[1],c->argv[2])) {
            addReply(c,shared.cone);
            return;
        }
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            listAddNodeHead(list,c->argv[2]);
        } else {
            listAddNodeTail(list,c->argv[2]);
        }
        incrRefCount(c->argv[2]);
    }
    server.dirty++;
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",listLength(list)));
}
static void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}
static void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}
static void llenCommand(redisClient *c) {
    robj *o;
    list *l;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
        return;
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            l = o->ptr;
            addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",listLength(l)));
        }
    }
}
static void lindexCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            ln = listIndex(list, index);
            if (ln == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *ele = listNodeValue(ln);
                addReplyBulkLen(c,ele);
                addReply(c,ele);
                addReply(c,shared.crlf);
            }
        }
    }
}
static void lsetCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nokeyerr);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            ln = listIndex(list, index);
            if (ln == NULL) {
                addReply(c,shared.outofrangeerr);
            } else {
                robj *ele = listNodeValue(ln);
                decrRefCount(ele);
                listNodeValue(ln) = c->argv[3];
                incrRefCount(c->argv[3]);
                addReply(c,shared.ok);
                server.dirty++;
            }
        }
    }
}
static void popGenericCommand(redisClient *c, int where) {
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            if (where == REDIS_HEAD)
                ln = listFirst(list);
            else
                ln = listLast(list);
            if (ln == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *ele = listNodeValue(ln);
                addReplyBulkLen(c,ele);
                addReply(c,ele);
                addReply(c,shared.crlf);
                listDelNode(list,ln);
                server.dirty++;
            }
        }
    }
}
static void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}
static void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}
static void lrangeCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullmultibulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            int llen = listLength(list);
            int rangelen, j;
            robj *ele;
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
            ln = listIndex(list, start);
            addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",rangelen));
            for (j = 0; j < rangelen; j++) {
                ele = listNodeValue(ln);
                addReplyBulkLen(c,ele);
                addReply(c,ele);
                addReply(c,shared.crlf);
                ln = ln->next;
            }
        }
    }
}
static void ltrimCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.ok);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            int llen = listLength(list);
            int j, ltrim, rtrim;
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
            for (j = 0; j < ltrim; j++) {
                ln = listFirst(list);
                listDelNode(list,ln);
            }
            for (j = 0; j < rtrim; j++) {
                ln = listLast(list);
                listDelNode(list,ln);
            }
            server.dirty++;
            addReply(c,shared.ok);
        }
    }
}
static void lremCommand(redisClient *c) {
    robj *o;
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln, *next;
            int toremove = atoi(c->argv[2]->ptr);
            int removed = 0;
            int fromtail = 0;
            if (toremove < 0) {
                toremove = -toremove;
                fromtail = 1;
            }
            ln = fromtail ? list->tail : list->head;
            while (ln) {
                robj *ele = listNodeValue(ln);
                next = fromtail ? ln->prev : ln->next;
                if (compareStringObjects(ele,c->argv[3]) == 0) {
                    listDelNode(list,ln);
                    server.dirty++;
                    removed++;
                    if (toremove && removed == toremove) break;
                }
                ln = next;
            }
            addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",removed));
        }
    }
}
static void rpoplpushcommand(redisClient *c) {
    robj *sobj;
    sobj = lookupKeyWrite(c->db,c->argv[1]);
    if (sobj == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (sobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *srclist = sobj->ptr;
            listNode *ln = listLast(srclist);
            if (ln == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
                robj *ele = listNodeValue(ln);
                list *dstlist;
                if (dobj && dobj->type != REDIS_LIST) {
                    addReply(c,shared.wrongtypeerr);
                    return;
                }
                if (!handleClientsWaitingListPush(c,c->argv[2],ele)) {
                    if (dobj == NULL) {
                        dobj = createListObject();
                        dictAdd(c->db->dict,c->argv[2],dobj);
                        incrRefCount(c->argv[2]);
                    }
                    dstlist = dobj->ptr;
                    listAddNodeHead(dstlist,ele);
                    incrRefCount(ele);
                }
                addReplyBulkLen(c,ele);
                addReply(c,ele);
                addReply(c,shared.crlf);
                listDelNode(srclist,ln);
                server.dirty++;
            }
        }
    }
}
static void saddCommand(redisClient *c) {
    robj *set;
    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = createSetObject();
        dictAdd(c->db->dict,c->argv[1],set);
        incrRefCount(c->argv[1]);
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
    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.czero);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (dictDelete(set->ptr,c->argv[2]) == DICT_OK) {
            server.dirty++;
            if (htNeedsResize(set->ptr)) dictResize(set->ptr);
            addReply(c,shared.cone);
        } else {
            addReply(c,shared.czero);
        }
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
    server.dirty++;
    if (!dstset) {
        dstset = createSetObject();
        dictAdd(c->db->dict,c->argv[2],dstset);
        incrRefCount(c->argv[2]);
    }
    if (dictAdd(dstset->ptr,c->argv[3],NULL) == DICT_OK)
        incrRefCount(c->argv[3]);
    addReply(c,shared.cone);
}
static void sismemberCommand(redisClient *c) {
    robj *set;
    set = lookupKeyRead(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.czero);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (dictFind(set->ptr,c->argv[2]))
            addReply(c,shared.cone);
        else
            addReply(c,shared.czero);
    }
}
static void scardCommand(redisClient *c) {
    robj *o;
    dict *s;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
        return;
    } else {
        if (o->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            s = o->ptr;
            addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",
                dictSize(s)));
        }
    }
}
static void spopCommand(redisClient *c) {
    robj *set;
    dictEntry *de;
    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        de = dictGetRandomKey(set->ptr);
        if (de == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            robj *ele = dictGetEntryKey(de);
            addReplyBulkLen(c,ele);
            addReply(c,ele);
            addReply(c,shared.crlf);
            dictDelete(set->ptr,ele);
            if (htNeedsResize(set->ptr)) dictResize(set->ptr);
            server.dirty++;
        }
    }
}
static void srandmemberCommand(redisClient *c) {
    robj *set;
    dictEntry *de;
    set = lookupKeyRead(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        de = dictGetRandomKey(set->ptr);
        if (de == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            robj *ele = dictGetEntryKey(de);
            addReplyBulkLen(c,ele);
            addReply(c,ele);
            addReply(c,shared.crlf);
        }
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
                if (deleteKey(c->db,dstkey))
                    server.dirty++;
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.nullmultibulk);
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
            addReplyBulkLen(c,ele);
            addReply(c,ele);
            addReply(c,shared.crlf);
            cardinality++;
        } else {
            dictAdd(dstset->ptr,ele,NULL);
            incrRefCount(ele);
        }
    }
    dictReleaseIterator(di);
    if (dstkey) {
        deleteKey(c->db,dstkey);
        dictAdd(c->db->dict,dstkey,dstset);
        incrRefCount(dstkey);
    }
    if (!dstkey) {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",
            dictSize((dict*)dstset->ptr)));
        server.dirty++;
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
            addReplyBulkLen(c,ele);
            addReply(c,ele);
            addReply(c,shared.crlf);
        }
        dictReleaseIterator(di);
    } else {
        deleteKey(c->db,dstkey);
        dictAdd(c->db->dict,dstkey,dstset);
        incrRefCount(dstkey);
    }
    if (!dstkey) {
        decrRefCount(dstset);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",
            dictSize((dict*)dstset->ptr)));
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
    if (level > 0)
        zn->span = zmalloc(sizeof(unsigned int) * (level - 1));
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
    return level;
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
    if (x && score == x->score && compareStringObjects(x->obj,obj) == 0) {
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
        if (x->obj && compareStringObjects(x->obj,o) == 0) {
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
        while (x->forward[i] && (traversed + (i > 0 ? x->span[i-1] : 1)) <= rank) {
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
    zsetobj = lookupKeyWrite(c->db,key);
    if (zsetobj == NULL) {
        zsetobj = createZsetObject();
        dictAdd(c->db->dict,key,zsetobj);
        incrRefCount(key);
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
    scoreval = strtod(c->argv[2]->ptr,NULL);
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}
static void zincrbyCommand(redisClient *c) {
    double scoreval;
    scoreval = strtod(c->argv[2]->ptr,NULL);
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}
static void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    zsetobj = lookupKeyWrite(c->db,c->argv[1]);
    if (zsetobj == NULL) {
        addReply(c,shared.czero);
    } else {
        dictEntry *de;
        double *oldscore;
        int deleted;
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
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
        server.dirty++;
        addReply(c,shared.cone);
    }
}
static void zremrangebyscoreCommand(redisClient *c) {
    double min = strtod(c->argv[2]->ptr,NULL);
    double max = strtod(c->argv[3]->ptr,NULL);
    robj *zsetobj;
    zset *zs;
    zsetobj = lookupKeyWrite(c->db,c->argv[1]);
    if (zsetobj == NULL) {
        addReply(c,shared.czero);
    } else {
        long deleted;
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        zs = zsetobj->ptr;
        deleted = zslDeleteRangeByScore(zs->zsl,min,max,zs->dict);
        if (htNeedsResize(zs->dict)) dictResize(zs->dict);
        server.dirty += deleted;
        addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",deleted));
    }
}
static void zremrangebyrankCommand(redisClient *c) {
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    robj *zsetobj;
    zset *zs;
    zsetobj = lookupKeyWrite(c->db,c->argv[1]);
    if (zsetobj == NULL) {
        addReply(c,shared.czero);
    } else {
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        zs = zsetobj->ptr;
        int llen = zs->zsl->length;
        long deleted;
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
        server.dirty += deleted;
        addReplyLong(c, deleted);
    }
}
static void zrangeGenericCommand(redisClient *c, int reverse) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int withscores = 0;
    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullmultibulk);
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            int llen = zsl->length;
            int rangelen, j;
            robj *ele;
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
                ln = start == 0 ? zsl->header->forward[0] : zslGetElementByRank(zsl, start+1);
            }
            addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",
                withscores ? (rangelen*2) : rangelen));
            for (j = 0; j < rangelen; j++) {
                ele = ln->obj;
                addReplyBulkLen(c,ele);
                addReply(c,ele);
                addReply(c,shared.crlf);
                if (withscores)
                    addReplyDouble(c,ln->score);
                ln = reverse ? ln->backward : ln->forward[0];
            }
        }
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
        addReply(c,justcount ? shared.czero : shared.nullmultibulk);
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
                    addReplyBulkLen(c,ele);
                    addReply(c,ele);
                    addReply(c,shared.crlf);
                    if (withscores)
                        addReplyDouble(c,ln->score);
                }
                ln = ln->forward[0];
                rangelen++;
                if (limit > 0) limit--;
            }
            if (justcount) {
                addReplyLong(c,(long)rangelen);
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
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
        return;
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            zs = o->ptr;
            addReplySds(c,sdscatprintf(sdsempty(),":%lu\r\n",zs->zsl->length));
        }
    }
}
static void zscoreCommand(redisClient *c) {
    robj *o;
    zset *zs;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
        return;
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            dictEntry *de;
            zs = o->ptr;
            de = dictFind(zs->dict,c->argv[2]);
            if (!de) {
                addReply(c,shared.nullbulk);
            } else {
                double *score = dictGetEntryVal(de);
                addReplyDouble(c,*score);
            }
        }
    }
}
static void zrankGenericCommand(redisClient *c, int reverse) {
    robj *o;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }
    if (o->type != REDIS_ZSET) {
        addReply(c,shared.wrongtypeerr);
    } else {
        zset *zs = o->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        unsigned long rank;
        de = dictFind(zs->dict,c->argv[2]);
        if (!de) {
            addReply(c,shared.nullbulk);
            return;
        }
        double *score = dictGetEntryVal(de);
        rank = zslGetRank(zsl, *score, c->argv[2]);
        if (rank) {
            if (reverse) {
                addReplyLong(c, zsl->length - rank);
            } else {
                addReplyLong(c, rank-1);
            }
        } else {
            addReply(c,shared.nullbulk);
        }
    }
}
static void zrankCommand(redisClient *c) {
    zrankGenericCommand(c, 0);
}
static void zrevrankCommand(redisClient *c) {
    zrankGenericCommand(c, 1);
}
static void hsetCommand(redisClient *c) {
    int update = 0;
    robj *o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        o = createHashObject();
        dictAdd(c->db->dict,c->argv[1],o);
        incrRefCount(c->argv[1]);
    } else {
        if (o->type != REDIS_HASH) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        unsigned char *zm = o->ptr;
        robj *valobj = getDecodedObject(c->argv[3]);
        zm = zipmapSet(zm,c->argv[2]->ptr,sdslen(c->argv[2]->ptr),
            valobj->ptr,sdslen(valobj->ptr),&update);
        decrRefCount(valobj);
        o->ptr = zm;
    } else {
        if (dictAdd(o->ptr,c->argv[2],c->argv[3]) == DICT_OK) {
            incrRefCount(c->argv[2]);
        } else {
            update = 1;
        }
        incrRefCount(c->argv[3]);
    }
    server.dirty++;
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",update == 0));
}
static void hgetCommand(redisClient *c) {
    robj *o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
        return;
    } else {
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            unsigned char *zm = o->ptr;
            unsigned char *val;
            unsigned int vlen;
            if (zipmapGet(zm,c->argv[2]->ptr,sdslen(c->argv[2]->ptr), &val,&vlen)) {
                addReplySds(c,sdscatprintf(sdsempty(),"$%u\r\n", vlen));
                addReplySds(c,sdsnewlen(val,vlen));
                addReply(c,shared.crlf);
                return;
            } else {
                addReply(c,shared.nullbulk);
                return;
            }
        } else {
            struct dictEntry *de;
            de = dictFind(o->ptr,c->argv[2]);
            if (de == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *e = dictGetEntryVal(de);
                addReplyBulkLen(c,e);
                addReply(c,e);
                addReply(c,shared.crlf);
            }
        }
    }
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
        tryObjectEncoding(keyobj);
        tryObjectEncoding(valobj);
        dictAdd(dict,keyobj,valobj);
    }
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
    zfree(zm);
}
static void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}
static void flushallCommand(redisClient *c) {
    server.dirty += emptyDb();
    addReply(c,shared.ok);
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
    char *p;
    sds spat, ssub;
    robj keyobj;
    int prefixlen, sublen, postfixlen;
    struct {
        long len;
        long free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname;
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
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
    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1);
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;
    initStaticStringObject(keyobj,((char*)&keyname)+(sizeof(long)*2))
    decrRefCount(subst);
    return lookupKeyRead(db,&keyobj);
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
            robj *dec1, *dec2;
            dec1 = getDecodedObject(so1->obj);
            dec2 = getDecodedObject(so2->obj);
            cmp = strcoll(dec1->ptr,dec2->ptr);
            decrRefCount(dec1);
            decrRefCount(dec2);
        }
    }
    return server.sort_desc ? -cmp : cmp;
}
static void sortCommand(redisClient *c) {
    list *operations;
    int outputlen = 0;
    int desc = 0, alpha = 0;
    int limit_start = 0, limit_count = -1, start, end;
    int j, dontsort = 0, vectorlen;
    int getop = 0;
    robj *sortval, *sortby = NULL, *storekey = NULL;
    redisSortObject *vector;
    sortval = lookupKeyRead(c->db,c->argv[1]);
    if (sortval == NULL) {
        addReply(c,shared.nullmultibulk);
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
    case REDIS_LIST: vectorlen = listLength((list*)sortval->ptr); break;
    case REDIS_SET: vectorlen = dictSize((dict*)sortval->ptr); break;
    case REDIS_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
    default: vectorlen = 0; redisAssert(0);
    }
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    j = 0;
    if (sortval->type == REDIS_LIST) {
        list *list = sortval->ptr;
        listNode *ln;
        listIter li;
        listRewind(list,&li);
        while((ln = listNext(&li))) {
            robj *ele = ln->value;
            vector[j].obj = ele;
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
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
            if (sortby) {
                robj *byval;
                byval = lookupKeyByPattern(c->db,sortby,vector[j].obj);
                if (!byval || byval->type != REDIS_STRING) continue;
                if (alpha) {
                    vector[j].u.cmpobj = getDecodedObject(byval);
                } else {
                    if (byval->encoding == REDIS_ENCODING_RAW) {
                        vector[j].u.score = strtod(byval->ptr,NULL);
                    } else {
                        if (byval->encoding == REDIS_ENCODING_INT) {
                            vector[j].u.score = (long)byval->ptr;
                        } else
                            redisAssert(1 != 1);
                    }
                }
            } else {
                if (!alpha) {
                    if (vector[j].obj->encoding == REDIS_ENCODING_RAW)
                        vector[j].u.score = strtod(vector[j].obj->ptr,NULL);
                    else {
                        if (vector[j].obj->encoding == REDIS_ENCODING_INT)
                            vector[j].u.score = (long) vector[j].obj->ptr;
                        else
                            redisAssert(1 != 1);
                    }
                }
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
            if (!getop) {
                addReplyBulkLen(c,vector[j].obj);
                addReply(c,vector[j].obj);
                addReply(c,shared.crlf);
            }
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);
                if (sop->type == REDIS_SORT_GET) {
                    if (!val || val->type != REDIS_STRING) {
                        addReply(c,shared.nullbulk);
                    } else {
                        addReplyBulkLen(c,val);
                        addReply(c,val);
                        addReply(c,shared.crlf);
                    }
                } else {
                    redisAssert(sop->type == REDIS_SORT_GET);
                }
            }
        }
    } else {
        robj *listObject = createListObject();
        list *listPtr = (list*) listObject->ptr;
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;
            if (!getop) {
                listAddNodeTail(listPtr,vector[j].obj);
                incrRefCount(vector[j].obj);
            }
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                    vector[j].obj);
                if (sop->type == REDIS_SORT_GET) {
                    if (!val || val->type != REDIS_STRING) {
                        listAddNodeTail(listPtr,createStringObject("",0));
                    } else {
                        listAddNodeTail(listPtr,val);
                        incrRefCount(val);
                    }
                } else {
                    redisAssert(sop->type == REDIS_SORT_GET);
                }
            }
        }
        if (dictReplace(c->db->dict,storekey,listObject)) {
            incrRefCount(storekey);
        }
        server.dirty += 1+outputlen;
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",outputlen));
    }
    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        if (sortby && alpha && vector[j].u.cmpobj)
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
        "vm_enabled:%d\r\n"
        "role:%s\r\n"
        ,REDIS_VERSION,
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
    if (dictDelete(db->expires,key) == DICT_OK) {
        return 1;
    } else {
        return 0;
    }
}
static int setExpire(redisDb *db, robj *key, time_t when) {
    if (dictAdd(db->expires,key,(void*)when) == DICT_ERR) {
        return 0;
    } else {
        incrRefCount(key);
        return 1;
    }
}
static time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return -1;
    return (time_t) dictGetEntryVal(de);
}
static int expireIfNeeded(redisDb *db, robj *key) {
    time_t when;
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;
    when = (time_t) dictGetEntryVal(de);
    if (time(NULL) <= when) return 0;
    dictDelete(db->expires,key);
    return dictDelete(db->dict,key) == DICT_OK;
}
static int deleteIfVolatile(redisDb *db, robj *key) {
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;
    server.dirty++;
    dictDelete(db->expires,key);
    return dictDelete(db->dict,key) == DICT_OK;
}
static void expireGenericCommand(redisClient *c, robj *key, time_t seconds) {
    dictEntry *de;
    de = dictFind(c->db->dict,key);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    if (seconds < 0) {
        if (deleteKey(c->db,key)) server.dirty++;
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
    expireGenericCommand(c,c->argv[1],strtol(c->argv[2]->ptr,NULL,10));
}
static void expireatCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],strtol(c->argv[2]->ptr,NULL,10)-time(NULL));
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
static void execCommand(redisClient *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    if (!(c->flags & REDIS_MULTI)) {
        addReplySds(c,sdsnew("-ERR EXEC without MULTI\r\n"));
        return;
    }
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
    c->flags &= (~REDIS_MULTI);
}
static void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout) {
    dictEntry *de;
    list *l;
    int j;
    c->blockingkeys = zmalloc(sizeof(robj*)*numkeys);
    c->blockingkeysnum = numkeys;
    c->blockingto = timeout;
    for (j = 0; j < numkeys; j++) {
        c->blockingkeys[j] = keys[j];
        incrRefCount(keys[j]);
        de = dictFind(c->db->blockingkeys,keys[j]);
        if (de == NULL) {
            int retval;
            l = listCreate();
            retval = dictAdd(c->db->blockingkeys,keys[j],l);
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
    assert(c->blockingkeys != NULL);
    for (j = 0; j < c->blockingkeysnum; j++) {
        de = dictFind(c->db->blockingkeys,c->blockingkeys[j]);
        assert(de != NULL);
        l = dictGetEntryVal(de);
        listDelNode(l,listSearchKey(l,c));
        if (listLength(l) == 0)
            dictDelete(c->db->blockingkeys,c->blockingkeys[j]);
        decrRefCount(c->blockingkeys[j]);
    }
    zfree(c->blockingkeys);
    c->blockingkeys = NULL;
    c->flags &= (~REDIS_BLOCKED);
    server.blpop_blocked_clients--;
    if (c->querybuf && sdslen(c->querybuf) > 0) processInputBuffer(c);
}
static int handleClientsWaitingListPush(redisClient *c, robj *key, robj *ele) {
    struct dictEntry *de;
    redisClient *receiver;
    list *l;
    listNode *ln;
    de = dictFind(c->db->blockingkeys,key);
    if (de == NULL) return 0;
    l = dictGetEntryVal(de);
    ln = listFirst(l);
    assert(ln != NULL);
    receiver = ln->value;
    addReplySds(receiver,sdsnew("*2\r\n"));
    addReplyBulkLen(receiver,key);
    addReply(receiver,key);
    addReply(receiver,shared.crlf);
    addReplyBulkLen(receiver,ele);
    addReply(receiver,ele);
    addReply(receiver,shared.crlf);
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
                    addReplyBulkLen(c,argv[1]);
                    addReply(c,argv[1]);
                    addReply(c,shared.crlf);
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
    int dfd;
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
    snprintf(tmpfile,256,"temp-%d.%ld.rdb",(int)time(NULL),(long int)random());
    dfd = open(tmpfile,O_CREAT|O_WRONLY,0644);
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
                deleteKey(server.db+j,minkey);
            }
        }
        if (!freed) return;
    }
}
static void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    int j;
    ssize_t nwritten;
    time_t now;
    robj *tmpargv[3];
    if (dictid != server.appendseldb) {
        char seldb[64];
        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.appendseldb = dictid;
    }
    if (cmd->proc == expireCommand) {
        long when;
        tmpargv[0] = createStringObject("EXPIREAT",8);
        tmpargv[1] = argv[1];
        incrRefCount(argv[1]);
        when = time(NULL)+strtol(argv[2]->ptr,NULL,10);
        tmpargv[2] = createObject(REDIS_STRING,
            sdscatprintf(sdsempty(),"%ld",when));
        argv = tmpargv;
    }
    buf = sdscatprintf(buf,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        robj *o = argv[j];
        o = getDecodedObject(o);
        buf = sdscatprintf(buf,"$%lu\r\n",(unsigned long)sdslen(o->ptr));
        buf = sdscatlen(buf,o->ptr,sdslen(o->ptr));
        buf = sdscatlen(buf,"\r\n",2);
        decrRefCount(o);
    }
    if (cmd->proc == expireCommand) {
        for (j = 0; j < 3; j++)
            decrRefCount(argv[j]);
    }
     nwritten = write(server.appendfd,buf,sdslen(buf));
     if (nwritten != (signed)sdslen(buf)) {
         if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
         } else {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to the append-only file: %s",strerror(errno));
         }
         exit(1);
    }
    if (server.bgrewritechildpid != -1)
        server.bgrewritebuf = sdscatlen(server.bgrewritebuf,buf,sdslen(buf));
    sdsfree(buf);
    now = time(NULL);
    if (server.appendfsync == APPENDFSYNC_ALWAYS ||
        (server.appendfsync == APPENDFSYNC_EVERYSEC &&
         now-server.lastfsync > 1))
    {
        fsync(server.appendfd);
        server.lastfsync = now;
    }
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
    return c;
}
static void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    zfree(c);
}
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    unsigned long long loadedkeys = 0;
    if (redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0)
        return REDIS_ERR;
    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }
    fakeClient = createFakeClient();
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;
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
        if (server.shareobjects) {
            int j;
            for(j = 1; j < argc; j++)
                argv[j] = tryObjectSharing(argv[j]);
        }
        if (cmd->flags & REDIS_CMD_BULK)
            tryObjectEncoding(argv[argc-1]);
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);
        while(listLength(fakeClient->reply))
            listDelNode(fakeClient->reply,listFirst(fakeClient->reply));
        for (j = 0; j < argc; j++) decrRefCount(argv[j]);
        zfree(argv);
        loadedkeys++;
        if (server.vm_enabled && (loadedkeys % 5000) == 0) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
        }
    }
    fclose(fp);
    freeFakeClient(fakeClient);
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
static int fwriteBulk(FILE *fp, robj *obj) {
    char buf[128];
    int decrrc = 0;
    if (obj->encoding != REDIS_ENCODING_RAW) {
        obj = getDecodedObject(obj);
        decrrc = 1;
    }
    snprintf(buf,sizeof(buf),"$%ld\r\n",(long)sdslen(obj->ptr));
    if (fwrite(buf,strlen(buf),1,fp) == 0) goto err;
    if (sdslen(obj->ptr) && fwrite(obj->ptr,sdslen(obj->ptr),1,fp) == 0)
        goto err;
    if (fwrite("\r\n",2,1,fp) == 0) goto err;
    if (decrrc) decrRefCount(obj);
    return 1;
err:
    if (decrrc) decrRefCount(obj);
    return 0;
}
static int fwriteBulkDouble(FILE *fp, double d) {
    char buf[128], dbuf[128];
    snprintf(dbuf,sizeof(dbuf),"%.17g\r\n",d);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(dbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(dbuf,strlen(dbuf),1,fp) == 0) return 0;
    return 1;
}
static int fwriteBulkLong(FILE *fp, long l) {
    char buf[128], lbuf[128];
    snprintf(lbuf,sizeof(lbuf),"%ld\r\n",l);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(lbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(lbuf,strlen(lbuf),1,fp) == 0) return 0;
    return 1;
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
        if (fwriteBulkLong(fp,j) == 0) goto werr;
        while((de = dictNext(di)) != NULL) {
            robj *key, *o;
            time_t expiretime;
            int swapped;
            key = dictGetEntryKey(de);
            if (!server.vm_enabled || key->storage == REDIS_VM_MEMORY ||
                key->storage == REDIS_VM_SWAPPING) {
                o = dictGetEntryVal(de);
                swapped = 0;
            } else {
                o = vmPreviewObject(key);
                swapped = 1;
            }
            expiretime = getExpire(db,key);
            if (o->type == REDIS_STRING) {
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                if (fwriteBulk(fp,key) == 0) goto werr;
                if (fwriteBulk(fp,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                list *list = o->ptr;
                listNode *ln;
                listIter li;
                listRewind(list,&li);
                while((ln = listNext(&li))) {
                    char cmd[]="*3\r\n$5\r\nRPUSH\r\n";
                    robj *eleobj = listNodeValue(ln);
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulk(fp,key) == 0) goto werr;
                    if (fwriteBulk(fp,eleobj) == 0) goto werr;
                }
            } else if (o->type == REDIS_SET) {
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;
                while((de = dictNext(di)) != NULL) {
                    char cmd[]="*3\r\n$4\r\nSADD\r\n";
                    robj *eleobj = dictGetEntryKey(de);
                    if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                    if (fwriteBulk(fp,key) == 0) goto werr;
                    if (fwriteBulk(fp,eleobj) == 0) goto werr;
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
                    if (fwriteBulk(fp,key) == 0) goto werr;
                    if (fwriteBulkDouble(fp,*score) == 0) goto werr;
                    if (fwriteBulk(fp,eleobj) == 0) goto werr;
                }
                dictReleaseIterator(di);
            } else {
                redisAssert(0 != 0);
            }
            if (expiretime != -1) {
                char cmd[]="*3\r\n$8\r\nEXPIREAT\r\n";
                if (expiretime < now) continue;
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                if (fwriteBulk(fp,key) == 0) goto werr;
                if (fwriteBulkLong(fp,expiretime) == 0) goto werr;
            }
            if (swapped) decrRefCount(o);
        }
        dictReleaseIterator(di);
    }
    fflush(fp);
    fsync(fileno(fp));
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
static void expandVmSwapFilename(void) {
    char *p = strstr(server.vm_swap_file,"%p");
    sds new;
    if (!p) return;
    new = sdsempty();
    *p = '\0';
    new = sdscat(new,server.vm_swap_file);
    new = sdscatprintf(new,"%ld",(long) getpid());
    new = sdscat(new,p+2);
    zfree(server.vm_swap_file);
    server.vm_swap_file = new;
}
static void vmInit(void) {
    off_t totsize;
    int pipefds[2];
    size_t stacksize;
    if (server.vm_max_threads != 0)
        zmalloc_enable_thread_safeness();
    expandVmSwapFilename();
    redisLog(REDIS_NOTICE,"Using '%s' as swap file",server.vm_swap_file);
    if ((server.vm_fp = fopen(server.vm_swap_file,"r+b")) == NULL) {
        server.vm_fp = fopen(server.vm_swap_file,"w+b");
    }
    if (server.vm_fp == NULL) {
        redisLog(REDIS_WARNING,
            "Impossible to open the swap file: %s. Exiting.",
            strerror(errno));
        exit(1);
    }
    server.vm_fd = fileno(server.vm_fp);
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
static int vmSwapObjectBlocking(robj *key, robj *val) {
    off_t pages = rdbSavedObjectPages(val,NULL);
    off_t page;
    assert(key->storage == REDIS_VM_MEMORY);
    assert(key->refcount == 1);
    if (vmFindContiguousPages(&page,pages) == REDIS_ERR) return REDIS_ERR;
    if (vmWriteObjectOnSwap(val,page) == REDIS_ERR) return REDIS_ERR;
    key->vm.page = page;
    key->vm.usedpages = pages;
    key->storage = REDIS_VM_SWAPPED;
    key->vtype = val->type;
    decrRefCount(val);
    vmMarkPagesUsed(page,pages);
    redisLog(REDIS_DEBUG,"VM: object %s swapped out at %lld (%lld pages)",
        (unsigned char*) key->ptr,
        (unsigned long long) page, (unsigned long long) pages);
    server.vm_stats_swapped_objects++;
    server.vm_stats_swapouts++;
    return REDIS_OK;
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
static robj *vmGenericLoadObject(robj *key, int preview) {
    robj *val;
    redisAssert(key->storage == REDIS_VM_SWAPPED || key->storage == REDIS_VM_LOADING);
    val = vmReadObjectFromSwap(key->vm.page,key->vtype);
    if (!preview) {
        key->storage = REDIS_VM_MEMORY;
        key->vm.atime = server.unixtime;
        vmMarkPagesFree(key->vm.page,key->vm.usedpages);
        redisLog(REDIS_DEBUG, "VM: object %s loaded from disk",
            (unsigned char*) key->ptr);
        server.vm_stats_swapped_objects--;
    } else {
        redisLog(REDIS_DEBUG, "VM: object %s previewed from disk",
            (unsigned char*) key->ptr);
    }
    server.vm_stats_swapins++;
    return val;
}
static robj *vmLoadObject(robj *key) {
    if (key->storage == REDIS_VM_LOADING)
        vmCancelThreadedIOJob(key);
    return vmGenericLoadObject(key,0);
}
static robj *vmPreviewObject(robj *key) {
    return vmGenericLoadObject(key,1);
}
static double computeObjectSwappability(robj *o) {
    time_t age = server.unixtime - o->vm.atime;
    long asize = 0;
    list *l;
    dict *d;
    struct dictEntry *de;
    int z;
    if (age <= 0) return 0;
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
                            (sizeof(*o)+sdslen(ele->ptr)) :
                            sizeof(*o);
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
                            (sizeof(*o)+sdslen(ele->ptr)) :
                            sizeof(*o);
            asize += (sizeof(struct dictEntry)+elesize)*dictSize(d);
            if (z) asize += sizeof(zskiplistNode)*dictSize(d);
        }
        break;
    }
    return (double)age*log(1+asize);
}
static int vmSwapOneObject(int usethreads) {
    int j, i;
    struct dictEntry *best = NULL;
    double best_swappability = 0;
    redisDb *best_db = NULL;
    robj *key, *val;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        int maxtries = 100;
        if (dictSize(db->dict) == 0) continue;
        for (i = 0; i < 5; i++) {
            dictEntry *de;
            double swappability;
            if (maxtries) maxtries--;
            de = dictGetRandomKey(db->dict);
            key = dictGetEntryKey(de);
            val = dictGetEntryVal(de);
            if (key->storage != REDIS_VM_MEMORY ||
                (server.vm_max_threads != 0 && val->refcount != 1)) {
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
        key->ptr, best_swappability);
    if (key->refcount > 1) {
        robj *newkey = dupStringObject(key);
        decrRefCount(key);
        key = dictGetEntryKey(best) = newkey;
    }
    if (usethreads) {
        vmSwapObjectThreaded(key,val,best_db);
        return REDIS_OK;
    } else {
        if (vmSwapObjectBlocking(key,val) == REDIS_OK) {
            dictGetEntryVal(best) = NULL;
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
static int deleteIfSwapped(redisDb *db, robj *key) {
    dictEntry *de;
    robj *foundkey;
    if ((de = dictFind(db->dict,key)) == NULL) return 0;
    foundkey = dictGetEntryKey(de);
    if (foundkey->storage == REDIS_VM_MEMORY) return 0;
    deleteKey(db,key);
    return 1;
}
static void freeIOJob(iojob *j) {
    if ((j->type == REDIS_IOJOB_PREPARE_SWAP ||
        j->type == REDIS_IOJOB_DO_SWAP ||
        j->type == REDIS_IOJOB_LOAD) && j->val != NULL)
        decrRefCount(j->val);
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
        robj *key;
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
        redisLog(REDIS_DEBUG,"Job %p type: %d, key at %p (%s) refcount: %d\n", (void*) j, j->type, (void*)j->key, (char*)j->key->ptr, j->key->refcount);
        de = dictFind(j->db->dict,j->key);
        assert(de != NULL);
        key = dictGetEntryKey(de);
        if (j->type == REDIS_IOJOB_LOAD) {
            redisDb *db;
            key->storage = REDIS_VM_MEMORY;
            key->vm.atime = server.unixtime;
            vmMarkPagesFree(key->vm.page,key->vm.usedpages);
            redisLog(REDIS_DEBUG, "VM: object %s loaded from disk (threaded)",
                (unsigned char*) key->ptr);
            server.vm_stats_swapped_objects--;
            server.vm_stats_swapins++;
            dictGetEntryVal(de) = j->val;
            incrRefCount(j->val);
            db = j->db;
            freeIOJob(j);
            handleClientsBlockedOnSwappedKey(db,key);
        } else if (j->type == REDIS_IOJOB_PREPARE_SWAP) {
            if (!vmCanSwapOut() ||
                vmFindContiguousPages(&j->page,j->pages) == REDIS_ERR)
            {
                freeIOJob(j);
                key->storage = REDIS_VM_MEMORY;
            } else {
                vmMarkPagesUsed(j->page,j->pages);
                j->type = REDIS_IOJOB_DO_SWAP;
                lockThreadedIO();
                queueIOJob(j);
                unlockThreadedIO();
            }
        } else if (j->type == REDIS_IOJOB_DO_SWAP) {
            robj *val;
            if (key->storage != REDIS_VM_SWAPPING) {
                printf("key->storage: %d\n",key->storage);
                printf("key->name: %s\n",(char*)key->ptr);
                printf("key->refcount: %d\n",key->refcount);
                printf("val: %p\n",(void*)j->val);
                printf("val->type: %d\n",j->val->type);
                printf("val->ptr: %s\n",(char*)j->val->ptr);
            }
            redisAssert(key->storage == REDIS_VM_SWAPPING);
            val = dictGetEntryVal(de);
            key->vm.page = j->page;
            key->vm.usedpages = j->pages;
            key->storage = REDIS_VM_SWAPPED;
            key->vtype = j->val->type;
            decrRefCount(val);
            dictGetEntryVal(de) = NULL;
            redisLog(REDIS_DEBUG,
                "VM: object %s swapped out at %lld (%lld pages) (threaded)",
                (unsigned char*) key->ptr,
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
            if (compareStringObjects(job->key,o) == 0) {
                redisLog(REDIS_DEBUG,"*** CANCELED %p (%s) (type %d) (LIST ID %d)\n",
                    (void*)job, (char*)o->ptr, job->type, i);
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
                return;
            }
        }
    }
    unlockThreadedIO();
    assert(1 != 1);
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
            j->val = vmReadObjectFromSwap(j->page,j->key->vtype);
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
    sigemptyset(&mask);
    sigaddset(&mask,SIGCHLD);
    sigaddset(&mask,SIGHUP);
    sigaddset(&mask,SIGPIPE);
    pthread_sigmask(SIG_SETMASK, &mask, &omask);
    pthread_create(&thread,&server.io_threads_attr,IOThreadEntryPoint,NULL);
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
    assert(key->storage == REDIS_VM_MEMORY);
    assert(key->refcount == 1);
    j = zmalloc(sizeof(*j));
    j->type = REDIS_IOJOB_PREPARE_SWAP;
    j->db = db;
    j->key = dupStringObject(key);
    j->val = val;
    incrRefCount(val);
    j->canceled = 0;
    j->thread = (pthread_t) -1;
    key->storage = REDIS_VM_SWAPPING;
    lockThreadedIO();
    queueIOJob(j);
    unlockThreadedIO();
    return REDIS_OK;
}
static int waitForSwappedKey(redisClient *c, robj *key) {
    struct dictEntry *de;
    robj *o;
    list *l;
    de = dictFind(c->db->dict,key);
    if (de == NULL) return 0;
    o = dictGetEntryKey(de);
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
        o->storage = REDIS_VM_LOADING;
        j = zmalloc(sizeof(*j));
        j->type = REDIS_IOJOB_LOAD;
        j->db = c->db;
        j->key = dupStringObject(key);
        j->key->vtype = o->vtype;
        j->page = o->vm.page;
        j->val = NULL;
        j->canceled = 0;
        j->thread = (pthread_t) -1;
        lockThreadedIO();
        queueIOJob(j);
        unlockThreadedIO();
    }
    return 1;
}
static int blockClientOnSwappedKeys(struct redisCommand *cmd, redisClient *c) {
    int j, last;
    if (cmd->vm_firstkey == 0) return 0;
    last = cmd->vm_lastkey;
    if (last < 0) last = c->argc+last;
    for (j = cmd->vm_firstkey; j <= last; j += cmd->vm_keystep)
        waitForSwappedKey(c,c->argv[j]);
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
        if (compareStringObjects(ln->value,key) == 0) {
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
        dictEntry *de = dictFind(c->db->dict,c->argv[2]);
        robj *key, *val;
        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        key = dictGetEntryKey(de);
        val = dictGetEntryVal(de);
        if (!server.vm_enabled || (key->storage == REDIS_VM_MEMORY ||
                                   key->storage == REDIS_VM_SWAPPING)) {
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Key at:%p refcount:%d, value at:%p refcount:%d "
                "encoding:%d serializedlength:%lld\r\n",
                (void*)key, key->refcount, (void*)val, val->refcount,
                val->encoding, (long long) rdbSavedObjectLen(val,NULL)));
        } else {
            addReplySds(c,sdscatprintf(sdsempty(),
                "+Key at:%p refcount:%d, value swapped at: page %llu "
                "using %llu pages\r\n",
                (void*)key, key->refcount, (unsigned long long) key->vm.page,
                (unsigned long long) key->vm.usedpages));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"swapout") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]);
        robj *key, *val;
        if (!server.vm_enabled) {
            addReplySds(c,sdsnew("-ERR Virtual Memory is disabled\r\n"));
            return;
        }
        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        key = dictGetEntryKey(de);
        val = dictGetEntryVal(de);
        if (key->refcount > 1) {
            robj *newkey = dupStringObject(key);
            decrRefCount(key);
            key = dictGetEntryKey(de) = newkey;
        }
        if (key->storage != REDIS_VM_MEMORY) {
            addReplySds(c,sdsnew("-ERR This key is not in memory\r\n"));
        } else if (vmSwapObjectBlocking(key,val) == REDIS_OK) {
            dictGetEntryVal(de) = NULL;
            addReply(c,shared.ok);
        } else {
            addReply(c,shared.err);
        }
    } else {
        addReplySds(c,sdsnew(
            "-ERR Syntax error, try DEBUG [SEGFAULT|OBJECT <key>|SWAPOUT <key>|RELOAD]\r\n"));
    }
}
static void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING,"==> %s:%d '%s' is not true\n",file,line,estr);
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
        redisLog(REDIS_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low condition memory. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
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
int main(int argc, char **argv) {
    time_t start;
    initServerConfig();
    if (argc == 2) {
        resetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
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
