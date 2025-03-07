#ifndef __REDIS_H
#define __REDIS_H 
#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"
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
#include <signal.h>
typedef long long mstime_t;
#include "ae.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"
#include "anet.h"
#include "ziplist.h"
#include "intset.h"
#include "version.h"
#include "util.h"
#include "latency.h"
#include "sparkline.h"
#include "quicklist.h"
#include "rax.h"
#include "connection.h"
#define REDISMODULE_CORE 1
#include "redismodule.h"
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"
#define C_OK 0
#define C_ERR -1
#define CONFIG_DEFAULT_DYNAMIC_HZ 1
#define CONFIG_DEFAULT_HZ 10
#define CONFIG_MIN_HZ 1
#define CONFIG_MAX_HZ 500
#define MAX_CLIENTS_PER_CLOCK_TICK 200
#define CONFIG_DEFAULT_SERVER_PORT 6379
#define CONFIG_DEFAULT_SERVER_TLS_PORT 0
#define CONFIG_DEFAULT_TCP_BACKLOG 511
#define CONFIG_DEFAULT_CLIENT_TIMEOUT 0
#define CONFIG_DEFAULT_DBNUM 16
#define CONFIG_DEFAULT_IO_THREADS_NUM 1
#define CONFIG_DEFAULT_IO_THREADS_DO_READS 0
#define CONFIG_MAX_LINE 1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024 * 64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define LOG_MAX_LEN 1024
#define AOF_REWRITE_PERC 100
#define AOF_REWRITE_MIN_SIZE (64 * 1024 * 1024)
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_READ_DIFF_INTERVAL_BYTES (1024 * 10)
#define CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN 10000
#define CONFIG_DEFAULT_SLOWLOG_MAX_LEN 128
#define CONFIG_DEFAULT_MAX_CLIENTS 10000
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_DEFAULT_SLAVE_PRIORITY 100
#define CONFIG_DEFAULT_REPL_TIMEOUT 60
#define CONFIG_DEFAULT_REPL_PING_SLAVE_PERIOD 10
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_DEFAULT_REPL_BACKLOG_SIZE (1024 * 1024)
#define CONFIG_DEFAULT_REPL_BACKLOG_TIME_LIMIT (60 * 60)
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024 * 16)
#define CONFIG_BGSAVE_RETRY_DELAY 5
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_SYSLOG_IDENT "redis"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_IP NULL
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_PORT 0
#define CONFIG_DEFAULT_CLUSTER_ANNOUNCE_BUS_PORT 0
#define CONFIG_DEFAULT_DAEMONIZE 0
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_TCP_KEEPALIVE 300
#define CONFIG_DEFAULT_PROTECTED_MODE 1
#define CONFIG_DEFAULT_GOPHER_ENABLED 0
#define CONFIG_DEFAULT_LOGFILE ""
#define CONFIG_DEFAULT_SYSLOG_ENABLED 0
#define CONFIG_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR 1
#define CONFIG_DEFAULT_RDB_COMPRESSION 1
#define CONFIG_DEFAULT_RDB_CHECKSUM 1
#define CONFIG_DEFAULT_RDB_FILENAME "dump.rdb"
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC 0
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC_DELAY 5
#define CONFIG_DEFAULT_RDB_KEY_SAVE_DELAY 0
#define CONFIG_DEFAULT_KEY_LOAD_DELAY 0
#define CONFIG_DEFAULT_SLAVE_SERVE_STALE_DATA 1
#define CONFIG_DEFAULT_SLAVE_READ_ONLY 1
#define CONFIG_DEFAULT_SLAVE_IGNORE_MAXMEMORY 1
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_IP NULL
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_PORT 0
#define CONFIG_DEFAULT_REPL_DISABLE_TCP_NODELAY 0
#define CONFIG_DEFAULT_MAXMEMORY 0
#define CONFIG_DEFAULT_MAXMEMORY_SAMPLES 5
#define CONFIG_DEFAULT_LFU_LOG_FACTOR 10
#define CONFIG_DEFAULT_LFU_DECAY_TIME 1
#define CONFIG_DEFAULT_AOF_FILENAME "appendonly.aof"
#define CONFIG_DEFAULT_AOF_NO_FSYNC_ON_REWRITE 0
#define CONFIG_DEFAULT_AOF_LOAD_TRUNCATED 1
#define CONFIG_DEFAULT_AOF_USE_RDB_PREAMBLE 1
#define CONFIG_DEFAULT_ACTIVE_REHASHING 1
#define CONFIG_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC 1
#define CONFIG_DEFAULT_RDB_SAVE_INCREMENTAL_FSYNC 1
#define CONFIG_DEFAULT_MIN_SLAVES_TO_WRITE 0
#define CONFIG_DEFAULT_MIN_SLAVES_MAX_LAG 10
#define CONFIG_DEFAULT_ACL_FILENAME ""
#define NET_IP_STR_LEN 46
#define NET_PEER_ID_LEN (NET_IP_STR_LEN + 32)
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define CONFIG_DEFAULT_LATENCY_MONITOR_THRESHOLD 0
#define CONFIG_DEFAULT_SLAVE_LAZY_FLUSH 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_EVICTION 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_EXPIRE 0
#define CONFIG_DEFAULT_LAZYFREE_LAZY_SERVER_DEL 0
#define CONFIG_DEFAULT_ALWAYS_SHOW_LOGO 0
#define CONFIG_DEFAULT_ACTIVE_DEFRAG 0
#define CONFIG_DEFAULT_DEFRAG_THRESHOLD_LOWER \
  10
#define CONFIG_DEFAULT_DEFRAG_THRESHOLD_UPPER \
  100
#define CONFIG_DEFAULT_DEFRAG_IGNORE_BYTES \
  (100 << 20)
#define CONFIG_DEFAULT_DEFRAG_CYCLE_MIN 5
#define CONFIG_DEFAULT_DEFRAG_CYCLE_MAX \
  75
#define CONFIG_DEFAULT_DEFRAG_MAX_SCAN_FIELDS \
  1000
#define CONFIG_DEFAULT_PROTO_MAX_BULK_LEN \
  (512ll * 1024 * 1024)
#define CONFIG_DEFAULT_TRACKING_TABLE_MAX_FILL \
  10
#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 20
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC \
  25
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1
#define SERVER_CHILD_NOERROR_RETVAL 255
#define STATS_METRIC_SAMPLES 16
#define STATS_METRIC_COMMAND 0
#define STATS_METRIC_NET_INPUT 1
#define STATS_METRIC_NET_OUTPUT 2
#define STATS_METRIC_COUNT 3
#define PROTO_MAX_QUERYBUF_LEN (1024 * 1024 * 1024)
#define PROTO_IOBUF_LEN (1024 * 16)
#define PROTO_REPLY_CHUNK_BYTES (16 * 1024)
#define PROTO_INLINE_MAX_SIZE (1024 * 64)
#define PROTO_MBULK_BIG_ARG (1024 * 32)
#define LONG_STR_SIZE 21
#define REDIS_AUTOSYNC_BYTES (1024 * 1024 * 32)
#define LIMIT_PENDING_QUERYBUF (4 * 1024 * 1024)
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS + 96)
#define HASHTABLE_MIN_FILL 10
#define CMD_WRITE (1ULL << 0)
#define CMD_READONLY (1ULL << 1)
#define CMD_DENYOOM (1ULL << 2)
#define CMD_MODULE (1ULL << 3)
#define CMD_ADMIN (1ULL << 4)
#define CMD_PUBSUB (1ULL << 5)
#define CMD_NOSCRIPT (1ULL << 6)
#define CMD_RANDOM (1ULL << 7)
#define CMD_SORT_FOR_SCRIPT (1ULL << 8)
#define CMD_LOADING (1ULL << 9)
#define CMD_STALE (1ULL << 10)
#define CMD_SKIP_MONITOR (1ULL << 11)
#define CMD_SKIP_SLOWLOG (1ULL << 12)
#define CMD_ASKING (1ULL << 13)
#define CMD_FAST (1ULL << 14)
#define CMD_MODULE_GETKEYS (1ULL << 15)
#define CMD_MODULE_NO_CLUSTER (1ULL << 16)
#define CMD_CATEGORY_KEYSPACE (1ULL << 17)
#define CMD_CATEGORY_READ (1ULL << 18)
#define CMD_CATEGORY_WRITE (1ULL << 19)
#define CMD_CATEGORY_SET (1ULL << 20)
#define CMD_CATEGORY_SORTEDSET (1ULL << 21)
#define CMD_CATEGORY_LIST (1ULL << 22)
#define CMD_CATEGORY_HASH (1ULL << 23)
#define CMD_CATEGORY_STRING (1ULL << 24)
#define CMD_CATEGORY_BITMAP (1ULL << 25)
#define CMD_CATEGORY_HYPERLOGLOG (1ULL << 26)
#define CMD_CATEGORY_GEO (1ULL << 27)
#define CMD_CATEGORY_STREAM (1ULL << 28)
#define CMD_CATEGORY_PUBSUB (1ULL << 29)
#define CMD_CATEGORY_ADMIN (1ULL << 30)
#define CMD_CATEGORY_FAST (1ULL << 31)
#define CMD_CATEGORY_SLOW (1ULL << 32)
#define CMD_CATEGORY_BLOCKING (1ULL << 33)
#define CMD_CATEGORY_DANGEROUS (1ULL << 34)
#define CMD_CATEGORY_CONNECTION (1ULL << 35)
#define CMD_CATEGORY_TRANSACTION (1ULL << 36)
#define CMD_CATEGORY_SCRIPTING (1ULL << 37)
#define AOF_OFF 0
#define AOF_ON 1
#define AOF_WAIT_REWRITE 2
#define CLIENT_SLAVE (1 << 0)
#define CLIENT_MASTER (1 << 1)
#define CLIENT_MONITOR \
  (1 << 2)
#define CLIENT_MULTI (1 << 3)
#define CLIENT_BLOCKED \
  (1 << 4)
#define CLIENT_DIRTY_CAS (1 << 5)
#define CLIENT_CLOSE_AFTER_REPLY \
  (1 << 6)
#define CLIENT_UNBLOCKED \
  (1 << 7)
#define CLIENT_ASKING (1 << 9)
#define CLIENT_CLOSE_ASAP (1 << 10)
#define CLIENT_UNIX_SOCKET \
  (1 << 11)
#define CLIENT_DIRTY_EXEC \
  (1 << 12)
#define CLIENT_MASTER_FORCE_REPLY \
  (1 << 13)
#define CLIENT_FORCE_AOF (1 << 14)
#define CLIENT_FORCE_REPL (1 << 15)
#define CLIENT_PRE_PSYNC (1 << 16)
#define CLIENT_READONLY (1 << 17)
#define CLIENT_PUBSUB (1 << 18)
#define CLIENT_PREVENT_AOF_PROP (1 << 19)
#define CLIENT_PREVENT_REPL_PROP (1 << 20)
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP | CLIENT_PREVENT_REPL_PROP)
#define CLIENT_PENDING_WRITE \
  (1 << 21)
#define CLIENT_REPLY_SKIP_NEXT \
  (1 << 23)
#define CLIENT_REPLY_SKIP (1 << 24)
#define CLIENT_LUA_DEBUG (1 << 25)
#define CLIENT_LUA_DEBUG_SYNC (1 << 26)
#define CLIENT_MODULE (1 << 27)
#define CLIENT_PROTECTED (1 << 28)
#define CLIENT_PENDING_READ \
  (1 << 29)
#define CLIENT_PENDING_COMMAND \
  (1 << 30)
#define CLIENT_TRACKING \
  (1 << 31)
#define BLOCKED_NONE 0
#define BLOCKED_LIST 1
#define BLOCKED_WAIT 2
#define BLOCKED_MODULE 3
#define BLOCKED_STREAM 4
#define BLOCKED_ZSET 5
#define BLOCKED_NUM 6
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2
#define CLIENT_TYPE_NORMAL 0
#define CLIENT_TYPE_SLAVE 1
#define CLIENT_TYPE_PUBSUB 2
#define CLIENT_TYPE_MASTER 3
#define CLIENT_TYPE_OBUF_COUNT \
  3
#define REPL_STATE_NONE 0
#define REPL_STATE_CONNECT 1
#define REPL_STATE_CONNECTING 2
#define REPL_STATE_RECEIVE_PONG 3
#define REPL_STATE_SEND_AUTH 4
#define REPL_STATE_RECEIVE_AUTH 5
#define REPL_STATE_SEND_PORT 6
#define REPL_STATE_RECEIVE_PORT 7
#define REPL_STATE_SEND_IP 8
#define REPL_STATE_RECEIVE_IP 9
#define REPL_STATE_SEND_CAPA 10
#define REPL_STATE_RECEIVE_CAPA 11
#define REPL_STATE_SEND_PSYNC 12
#define REPL_STATE_RECEIVE_PSYNC 13
#define REPL_STATE_TRANSFER 14
#define REPL_STATE_CONNECTED 15
#define SLAVE_STATE_WAIT_BGSAVE_START 6
#define SLAVE_STATE_WAIT_BGSAVE_END 7
#define SLAVE_STATE_SEND_BULK 8
#define SLAVE_STATE_ONLINE 9
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1 << 0)
#define SLAVE_CAPA_PSYNC2 (1 << 1)
#define CONFIG_REPL_SYNCIO_TIMEOUT 5
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1
#define SORT_OP_GET 0
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1 << 10)
#define CONFIG_DEFAULT_VERBOSITY LL_NOTICE
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3
#define ZSKIPLIST_MAXLEVEL 64
#define ZSKIPLIST_P 0.25
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2
#define CONFIG_DEFAULT_AOF_FSYNC AOF_FSYNC_EVERYSEC
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2
#define CONFIG_DEFAULT_REPL_DISKLESS_LOAD REPL_DISKLESS_LOAD_DISABLED
#define OBJ_HASH_MAX_ZIPLIST_ENTRIES 512
#define OBJ_HASH_MAX_ZIPLIST_VALUE 64
#define OBJ_SET_MAX_INTSET_ENTRIES 512
#define OBJ_ZSET_MAX_ZIPLIST_ENTRIES 128
#define OBJ_ZSET_MAX_ZIPLIST_VALUE 64
#define OBJ_STREAM_NODE_MAX_BYTES 4096
#define OBJ_STREAM_NODE_MAX_ENTRIES 100
#define OBJ_LIST_MAX_ZIPLIST_SIZE -2
#define OBJ_LIST_COMPRESS_DEPTH 0
#define CONFIG_DEFAULT_HLL_SPARSE_MAX_BYTES 3000
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2
#define MAXMEMORY_FLAG_LRU (1 << 0)
#define MAXMEMORY_FLAG_LFU (1 << 1)
#define MAXMEMORY_FLAG_ALLKEYS (1 << 2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
  (MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_LRU ((0 << 8) | MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1 << 8) | MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2 << 8)
#define MAXMEMORY_VOLATILE_RANDOM (3 << 8)
#define MAXMEMORY_ALLKEYS_LRU \
  ((4 << 8) | MAXMEMORY_FLAG_LRU | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU \
  ((5 << 8) | MAXMEMORY_FLAG_LFU | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6 << 8) | MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7 << 8)
#define CONFIG_DEFAULT_MAXMEMORY_POLICY MAXMEMORY_NO_EVICTION
#define LUA_SCRIPT_TIME_LIMIT 5000
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1
#define SHUTDOWN_NOFLAGS 0
#define SHUTDOWN_SAVE \
  1
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1 << 0)
#define CMD_CALL_STATS (1 << 1)
#define CMD_CALL_PROPAGATE_AOF (1 << 2)
#define CMD_CALL_PROPAGATE_REPL (1 << 3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF | CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1
#define RDB_CHILD_TYPE_SOCKET 2
#define NOTIFY_KEYSPACE (1 << 0)
#define NOTIFY_KEYEVENT (1 << 1)
#define NOTIFY_GENERIC (1 << 2)
#define NOTIFY_STRING (1 << 3)
#define NOTIFY_LIST (1 << 4)
#define NOTIFY_SET (1 << 5)
#define NOTIFY_HASH (1 << 6)
#define NOTIFY_ZSET (1 << 7)
#define NOTIFY_EXPIRED (1 << 8)
#define NOTIFY_EVICTED (1 << 9)
#define NOTIFY_STREAM (1 << 10)
#define NOTIFY_KEY_MISS (1 << 11)
#define NOTIFY_ALL \
  (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | \
   NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | \
   NOTIFY_KEY_MISS)
#define NET_FIRST_BIND_ADDR (server.bindaddr_count ? server.bindaddr[0] : NULL)
#define OBJ_STRING 0
#define OBJ_LIST 1
#define OBJ_SET 2
#define OBJ_ZSET 3
#define OBJ_HASH 4
#define OBJ_MODULE 5
#define OBJ_STREAM 6
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1 << REDISMODULE_TYPE_ENCVER_BITS) - 1)
#define REDISMODULE_AUX_BEFORE_RDB (1 << 0)
#define REDISMODULE_AUX_AFTER_RDB (1 << 1)
struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct redisObject;
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver,
                                     int when);
typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io,
                                      struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest,
                                     void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);
typedef struct RedisModuleType {
  uint64_t id;
  struct RedisModule *module;
  moduleTypeLoadFunc rdb_load;
  moduleTypeSaveFunc rdb_save;
  moduleTypeRewriteFunc aof_rewrite;
  moduleTypeMemUsageFunc mem_usage;
  moduleTypeDigestFunc digest;
  moduleTypeFreeFunc free;
  moduleTypeAuxLoadFunc aux_load;
  moduleTypeAuxSaveFunc aux_save;
  int aux_save_triggers;
  char name[10];
} moduleType;
typedef struct moduleValue {
  moduleType *type;
  void *value;
} moduleValue;
typedef struct RedisModuleIO {
  size_t bytes;
  rio *rio;
  moduleType *type;
  int error;
  int ver;
  struct RedisModuleCtx *ctx;
  struct redisObject *key;
} RedisModuleIO;
typedef struct RedisModuleDigest {
  unsigned char o[20];
  unsigned char x[20];
} RedisModuleDigest;
#define OBJ_ENCODING_RAW 0
#define OBJ_ENCODING_INT 1
#define OBJ_ENCODING_HT 2
#define OBJ_ENCODING_ZIPMAP 3
#define OBJ_ENCODING_LINKEDLIST 4
#define OBJ_ENCODING_ZIPLIST 5
#define OBJ_ENCODING_INTSET 6
#define OBJ_ENCODING_SKIPLIST 7
#define OBJ_ENCODING_EMBSTR 8
#define OBJ_ENCODING_QUICKLIST 9
#define OBJ_ENCODING_STREAM 10
#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1 << LRU_BITS) - 1)
#define LRU_CLOCK_RESOLUTION 1000
#define OBJ_SHARED_REFCOUNT INT_MAX
typedef struct redisObject {
  unsigned type : 4;
  unsigned encoding : 4;
  unsigned lru : LRU_BITS;
  int refcount;
  void *ptr;
} robj;
char *getObjectTypeName(robj *);
struct evictionPoolEntry;
typedef struct clientReplyBlock {
  size_t size, used;
  char buf[];
} clientReplyBlock;
typedef struct redisDb {
  dict *dict;
  dict *expires;
  dict *blocking_keys;
  dict *ready_keys;
  dict *watched_keys;
  int id;
  long long avg_ttl;
  list *
      defrag_later;
} redisDb;
typedef struct multiCmd {
  robj **argv;
  int argc;
  struct redisCommand *cmd;
} multiCmd;
typedef struct multiState {
  multiCmd *commands;
  int count;
  int cmd_flags;
  int minreplicas;
  time_t minreplicas_timeout;
} multiState;
typedef struct blockingState {
  mstime_t timeout;
  dict *keys;
  robj *target;
  size_t xread_count;
  robj *xread_group;
  robj *xread_consumer;
  mstime_t xread_retry_time, xread_retry_ttl;
  int xread_group_noack;
  int numreplicas;
  long long reploffset;
  void *module_blocked_handle;
} blockingState;
typedef struct readyList {
  redisDb *db;
  robj *key;
} readyList;
#define USER_COMMAND_BITS_COUNT \
  1024
#define USER_FLAG_ENABLED (1 << 0)
#define USER_FLAG_DISABLED (1 << 1)
#define USER_FLAG_ALLKEYS (1 << 2)
#define USER_FLAG_ALLCOMMANDS (1 << 3)
#define USER_FLAG_NOPASS \
  (1 << 4)
typedef struct user {
  sds name;
  uint64_t flags;
  uint64_t allowed_commands[USER_COMMAND_BITS_COUNT / 64];
  sds **allowed_subcommands;
  list *passwords;
  list *patterns;
#define CLIENT_ID_AOF \
  (UINT64_MAX)
  connection *conn;
  int resp;
  redisDb *db;
  robj *name;
  sds querybuf;
  size_t qb_pos;
  sds pending_querybuf;
  size_t querybuf_peak;
  int argc;
  robj **argv;
  struct redisCommand *cmd, *lastcmd;
  user *user;
  int reqtype;
  int multibulklen;
  long bulklen;
  list *reply;
  unsigned long long reply_bytes;
  size_t sentlen;
  time_t ctime;
  time_t lastinteraction;
  time_t obuf_soft_limit_reached_time;
  uint64_t flags;
  int authenticated;
  int replstate;
  int repl_put_online_on_ack;
  int repldbfd;
  off_t repldboff;
  off_t repldbsize;
  sds replpreamble;
  long long read_reploff;
  long long reploff;
  long long repl_ack_off;
  long long repl_ack_time;
  long long psync_initial_offset;
  char replid[CONFIG_RUN_ID_SIZE + 1];
  int slave_listening_port;
  char slave_ip[NET_IP_STR_LEN];
  int slave_capa;
  multiState mstate;
  int btype;
  blockingState bpop;
  long long woff;
  list *watched_keys;
  dict *pubsub_channels;
  list *pubsub_patterns;
  sds peerid;
  listNode *client_list_node;
  uint64_t client_tracking_redirection;
  int bufpos;
  char buf[PROTO_REPLY_CHUNK_BYTES];
  struct saveparam {
    time_t seconds;
    int changes;
  };
  struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
  };
  struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space, *colon,
        *queued, *null[4], *nullarray[4], *emptymap[4], *emptyset[4],
        *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
        *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
        *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
        *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk,
        *subscribebulk, *unsubscribebulk, *psubscribebulk, *punsubscribebulk,
        *del, *unlink, *rpop, *lpop, *lpush, *rpoplpush, *zpopmin, *zpopmax,
        *emptyscan, *select[PROTO_SHARED_SELECT_CMDS],
        *integers[OBJ_SHARED_INTEGERS],
        *mbulkhdr[OBJ_SHARED_BULKHDR_LEN],
        *bulkhdr[OBJ_SHARED_BULKHDR_LEN];
    sds minstring, maxstring;
  };
  typedef struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
      struct zskiplistNode *forward;
      unsigned long span;
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
  typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
  } clientBufferLimitsConfig;
  extern clientBufferLimitsConfig
      clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];
  typedef struct redisOp {
    robj **argv;
    int argc, dbid, target;
    struct redisCommand *cmd;
  } redisOp;
  typedef struct redisOpArray {
    redisOp *ops;
    int numops;
  } redisOpArray;
  struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t aof_buffer;
    size_t lua_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
      size_t dbid;
      size_t overhead_ht_main;
      size_t overhead_ht_expires;
    } *db;
  };
  typedef struct rdbSaveInfo {
    int repl_stream_db;
    int repl_id_is_set;
    char repl_id[CONFIG_RUN_ID_SIZE + 1];
    long long repl_offset;
  } rdbSaveInfo;
#define RDB_SAVE_INFO_INIT \
  { -1, 0, "000000000000000000000000000000", -1 }
  struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
  };
  typedef struct redisTLSContextConfig {
    char *cert_file;
    char *key_file;
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
  } redisTLSContextConfig;
  struct clusterState;
#ifdef _AIX
#undef hz
#endif
#define CHILD_INFO_MAGIC 0xC17DDA7A12345678LL
#define CHILD_INFO_TYPE_RDB 0
#define CHILD_INFO_TYPE_AOF 1
#define CHILD_INFO_TYPE_MODULE 3
  struct redisServer {
    pid_t pid;
    char *configfile;
    char *executable;
    char **exec_argv;
    int dynamic_hz;
    int config_hz;
    int hz;
    redisDb *db;
    dict *commands;
    dict *orig_commands;
    aeEventLoop *el;
    _Atomic unsigned int lruclock;
    int shutdown_asap;
    int activerehashing;
    int active_defrag_running;
    char *pidfile;
    int arch_bits;
    int cronloops;
    char runid[CONFIG_RUN_ID_SIZE + 1];
    int sentinel_mode;
    size_t initial_memory_usage;
    int always_show_logo;
    dict *moduleapi;
    dict *sharedapi;
    list *loadmodule_queue;
    int module_blocked_pipe[2];
    pid_t module_child_pid;
    int port;
    int tls_port;
    int tcp_backlog;
    char *bindaddr[CONFIG_BINDADDR_MAX];
    int bindaddr_count;
    char *unixsocket;
    mode_t unixsocketperm;
    int ipfd[CONFIG_BINDADDR_MAX];
    int ipfd_count;
    int tlsfd[CONFIG_BINDADDR_MAX];
    int tlsfd_count;
    int sofd;
    int cfd[CONFIG_BINDADDR_MAX];
    int cfd_count;
    list *clients;
    list *clients_to_close;
    list *clients_pending_write;
    list *clients_pending_read;
    list *slaves, *monitors;
    client *current_client;
    rax *clients_index;
    int clients_paused;
    mstime_t clients_pause_end_time;
    char neterr[ANET_ERR_LEN];
    dict *migrate_cached_sockets;
    _Atomic uint64_t next_client_id;
    int protected_mode;
    int gopher_enabled;
    int io_threads_num;
    int io_threads_do_reads;
    int loading;
    off_t loading_total_bytes;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    off_t loading_process_events_interval_bytes;
    struct redisCommand *delCommand, *multiCommand, *lpushCommand, *lpopCommand,
        *rpopCommand, *zpopminCommand, *zpopmaxCommand, *sremCommand,
        *execCommand, *expireCommand, *pexpireCommand, *xclaimCommand,
        *xgroupCommand;
    time_t stat_starttime;
    long long stat_numcommands;
    long long stat_numconnections;
    long long stat_expiredkeys;
    double stat_expired_stale_perc;
    long long
        stat_expired_time_cap_reached_count;
    long long stat_evictedkeys;
    long long stat_keyspace_hits;
    long long stat_keyspace_misses;
    long long stat_active_defrag_hits;
    long long
        stat_active_defrag_misses;
    long long
        stat_active_defrag_key_hits;
    long long
        stat_active_defrag_key_misses;
    long long stat_active_defrag_scanned;
    size_t stat_peak_memory;
    long long stat_fork_time;
    double stat_fork_rate;
    long long stat_rejected_conn;
    long long stat_sync_full;
    long long stat_sync_partial_ok;
    long long stat_sync_partial_err;
    list *slowlog;
    long long slowlog_entry_id;
    long long slowlog_log_slower_than;
    unsigned long slowlog_max_len;
    struct malloc_stats cron_malloc_stats;
    _Atomic long long stat_net_input_bytes;
    _Atomic long long stat_net_output_bytes;
    size_t stat_rdb_cow_bytes;
    size_t stat_aof_cow_bytes;
    size_t stat_module_cow_bytes;
    struct {
      long long last_sample_time;
      long long last_sample_count;
      long long samples[STATS_METRIC_SAMPLES];
      int idx;
    } inst_metric[STATS_METRIC_COUNT];
    int verbosity;
    int maxidletime;
    int tcpkeepalive;
    int active_expire_enabled;
    int active_defrag_enabled;
    int jemalloc_bg_thread;
    size_t
        active_defrag_ignore_bytes;
    int active_defrag_threshold_lower;
    int active_defrag_threshold_upper;
    int active_defrag_cycle_min;
    int active_defrag_cycle_max;
    unsigned long
        active_defrag_max_scan_fields;
    _Atomic size_t
        client_max_querybuf_len;
    int dbnum;
    int supervised;
    int supervised_mode;
    int daemonize;
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];
    int aof_enabled;
    int aof_state;
    int aof_fsync;
    char *aof_filename;
    int aof_no_fsync_on_rewrite;
    int aof_rewrite_perc;
    off_t aof_rewrite_min_size;
    off_t aof_rewrite_base_size;
    off_t aof_current_size;
    off_t aof_fsync_offset;
    int aof_flush_sleep;
    int aof_rewrite_scheduled;
    pid_t aof_child_pid;
    list *aof_rewrite_buf_blocks;
    sds aof_buf;
    int aof_fd;
    int aof_selected_db;
    time_t aof_flush_postponed_start;
    time_t aof_last_fsync;
    time_t aof_rewrite_time_last;
    time_t aof_rewrite_time_start;
    int aof_lastbgrewrite_status;
    unsigned long aof_delayed_fsync;
    int aof_rewrite_incremental_fsync;
    int rdb_save_incremental_fsync;
    int aof_last_write_status;
    int aof_last_write_errno;
    int aof_load_truncated;
    int aof_use_rdb_preamble;
    int aof_pipe_write_data_to_child;
    int aof_pipe_read_data_from_parent;
    int aof_pipe_write_ack_to_parent;
    int aof_pipe_read_ack_from_child;
    int aof_pipe_write_ack_to_child;
    int aof_pipe_read_ack_from_parent;
    int aof_stop_sending_diff;
    sds aof_child_diff;
    long long dirty;
    long long dirty_before_bgsave;
    pid_t rdb_child_pid;
    struct saveparam *saveparams;
    int saveparamslen;
    char *rdb_filename;
    int rdb_compression;
    int rdb_checksum;
    time_t lastsave;
    time_t lastbgsave_try;
    time_t rdb_save_time_last;
    time_t rdb_save_time_start;
    int rdb_bgsave_scheduled;
    int rdb_child_type;
    int lastbgsave_status;
    int stop_writes_on_bgsave_err;
    int rdb_pipe_write;
    int rdb_pipe_read;
    connection **rdb_pipe_conns;
    int rdb_pipe_numconns;
    int rdb_pipe_numconns_writing;
    char *rdb_pipe_buff;
    int rdb_pipe_bufflen;
    int rdb_key_save_delay;
    int key_load_delay;
    int child_info_pipe[2];
    struct {
      int process_type;
      size_t cow_size;
      unsigned long long magic;
    } child_info_data;
    redisOpArray also_propagate;
    char *logfile;
    int syslog_enabled;
    char *syslog_ident;
    int syslog_facility;
    char replid[CONFIG_RUN_ID_SIZE + 1];
    char replid2[CONFIG_RUN_ID_SIZE + 1];
    long long master_repl_offset;
    long long second_replid_offset;
    int slaveseldb;
    int repl_ping_slave_period;
    char *repl_backlog;
    long long repl_backlog_size;
    long long repl_backlog_histlen;
    long long repl_backlog_idx;
    long long repl_backlog_off;
    time_t repl_backlog_time_limit;
    time_t repl_no_slaves_since;
    int repl_min_slaves_to_write;
    int repl_min_slaves_max_lag;
    int repl_good_slaves_count;
    int repl_diskless_sync;
    int repl_diskless_load;
    int repl_diskless_sync_delay;
    char *masteruser;
    char *masterauth;
    char *masterhost;
    int masterport;
    int repl_timeout;
    client *master;
    client *cached_master;
    int repl_syncio_timeout;
    int repl_state;
    off_t repl_transfer_size;
    off_t repl_transfer_read;
    off_t repl_transfer_last_fsync_off;
    connection *repl_transfer_s;
    int repl_transfer_fd;
    char *repl_transfer_tmpfile;
    time_t repl_transfer_lastio;
    int repl_serve_stale_data;
    int repl_slave_ro;
    int repl_slave_ignore_maxmemory;
    time_t repl_down_since;
    int repl_disable_tcp_nodelay;
    int slave_priority;
    int slave_announce_port;
    char *slave_announce_ip;
    char master_replid[CONFIG_RUN_ID_SIZE + 1];
    long long master_initial_offset;
    int repl_slave_lazy_flush;
    dict *repl_scriptcache_dict;
    list *repl_scriptcache_fifo;
    unsigned int repl_scriptcache_size;
    list *clients_waiting_acks;
    int get_ack_from_slaves;
    unsigned int maxclients;
    unsigned long long maxmemory;
    int maxmemory_policy;
    int maxmemory_samples;
    int lfu_log_factor;
    int lfu_decay_time;
    long long proto_max_bulk_len;
    unsigned int blocked_clients;
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    list *unblocked_clients;
    list *ready_keys;
    unsigned int tracking_clients;
    int tracking_table_max_fill;
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    int64_t stream_node_max_entries;
    int list_max_ziplist_size;
    int list_compress_depth;
    _Atomic time_t unixtime;
    time_t timezone;
    int daylight_active;
    long long mstime;
    dict *pubsub_channels;
    list *pubsub_patterns;
    int notify_keyspace_events;
    int cluster_enabled;
    mstime_t cluster_node_timeout;
    char *cluster_configfile;
    struct clusterState *cluster;
    int cluster_migration_barrier;
    int cluster_slave_validity_factor;
    int cluster_require_full_coverage;
    int cluster_slave_no_failover;
    char *cluster_announce_ip;
    int cluster_announce_port;
    int cluster_announce_bus_port;
    int cluster_module_flags;
    lua_State *lua;
    client *lua_client;
    client *lua_caller;
    char *lua_cur_script;
    dict *lua_scripts;
    unsigned long long lua_scripts_mem;
    mstime_t lua_time_limit;
    mstime_t lua_time_start;
    int lua_write_dirty;
    int lua_random_dirty;
    int lua_replicate_commands;
    int lua_multi_emitted;
    int lua_repl;
    int lua_timedout;
    int lua_kill;
    int lua_always_replicate_commands;
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    long long latency_monitor_threshold;
    dict *latency_events;
    char *acl_filename;
    const char *assert_failed;
    const char *assert_file;
    int assert_line;
    int bug_report_start;
    int watchdog_period;
    size_t system_memory_size;
    int tls_cluster;
    int tls_replication;
    int tls_auth_clients;
    redisTLSContextConfig tls_ctx_config;
  };
  typedef struct pubsubPattern {
    client *client;
    robj *pattern;
  } pubsubPattern;
  typedef void redisCommandProc(client *c);
  typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc,
                                int *numkeys);
  struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int arity;
    char *sflags;
    uint64_t flags;
    redisGetKeysProc *getkeys_proc;
    int firstkey;
    int lastkey;
    int keystep;
    long long microseconds, calls;
    int id;
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
    quicklistIter *iter;
  } listTypeIterator;
  typedef struct {
    listTypeIterator *li;
    quicklistEntry entry;
  } listTypeEntry;
  typedef struct {
    robj *subject;
    int encoding;
    int ii;
    dictIterator *di;
  } setTypeIterator;
  typedef struct {
    robj *subject;
    int encoding;
    unsigned char *fptr, *vptr;
    dictIterator *di;
    dictEntry *de;
  } hashTypeIterator;
#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2
  extern struct redisServer server;
  extern struct sharedObjectsStruct shared;
  extern dictType objectKeyPointerValueDictType;
  extern dictType objectKeyHeapPointerValueDictType;
  extern dictType setDictType;
  extern dictType zsetDictType;
  extern dictType clusterNodesDictType;
  extern dictType clusterNodesBlackListDictType;
  extern dictType dbDictType;
  extern dictType shaScriptObjectDictType;
  extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
  extern dictType hashDictType;
  extern dictType replScriptCacheDictType;
  extern dictType keyptrDictType;
  extern dictType modulesDictType;
  void moduleInitModulesSystem(void);
  int moduleLoad(const char *path, void **argv, int argc);
  void moduleLoadFromQueue(void);
  int *moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv,
                                  int argc, int *numkeys);
  moduleType *moduleTypeLookupModuleByID(uint64_t id);
  void moduleTypeNameByID(char *name, uint64_t moduleid);
  void moduleFreeContext(struct RedisModuleCtx *ctx);
  void unblockClientFromModule(client *c);
  void moduleHandleBlockedClients(void);
  void moduleBlockedClientTimedOut(client *c);
  void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata,
                                       int mask);
  size_t moduleCount(void);
  void moduleAcquireGIL(void);
  void moduleReleaseGIL(void);
  void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key,
                                 int dbid);
  void moduleCallCommandFilters(client *c);
  void ModuleForkDoneHandler(int exitcode, int bysignal);
  int TerminateModuleForkChild(int child_pid, int wait);
  ssize_t rdbSaveModulesAux(rio *rdb, int when);
  int moduleAllDatatypesHandleErrors();
  sds modulesCollectInfo(sds info, sds section, int for_crash_report,
                         int sections);
  void moduleFireServerEvent(uint64_t eid, int subid, void *data);
  void processModuleLoadingProgressEvent(int is_aof);
  int moduleTryServeClientBlockedOnKey(client *c, robj *key);
  void moduleUnblockClient(client *c);
  int moduleClientIsBlockedOnKeys(client *c);
  long long ustime(void);
  long long mstime(void);
  void getRandomHexChars(char *p, size_t len);
  void getRandomBytes(unsigned char *p, size_t len);
  uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
  void exitFromChild(int retcode);
  size_t redisPopcount(void *s, long count);
  void redisSetProcTitle(char *title);
  client *createClient(connection *conn);
  void closeTimedoutClients(void);
  void freeClient(client *c);
  void freeClientAsync(client *c);
  void resetClient(client *c);
  void sendReplyToClient(connection *conn);
  void *addReplyDeferredLen(client *c);
  void setDeferredArrayLen(client *c, void *node, long length);
  void setDeferredMapLen(client *c, void *node, long length);
  void setDeferredSetLen(client *c, void *node, long length);
  void setDeferredAttributeLen(client *c, void *node, long length);
  void setDeferredPushLen(client *c, void *node, long length);
  void processInputBuffer(client *c);
  void processInputBufferAndReplicate(client *c);
  void processGopherRequest(client *c);
  void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
  void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
  void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);
  void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
  void readQueryFromClient(connection *conn);
  void addReplyNull(client *c);
  void addReplyNullArray(client *c);
  void addReplyBool(client *c, int b);
  void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
  void addReplyProto(client *c, const char *s, size_t len);
  void AddReplyFromClient(client *c, client *src);
  void addReplyBulk(client *c, robj *obj);
  void addReplyBulkCString(client *c, const char *s);
  void addReplyBulkCBuffer(client *c, const void *p, size_t len);
  void addReplyBulkLongLong(client *c, long long ll);
  void addReply(client *c, robj *obj);
  void addReplySds(client *c, sds s);
  void addReplyBulkSds(client *c, sds s);
  void addReplyError(client *c, const char *err);
  void addReplyStatus(client *c, const char *status);
  void addReplyDouble(client *c, double d);
  void addReplyHumanLongDouble(client *c, long double d);
  void addReplyLongLong(client *c, long long ll);
  void addReplyArrayLen(client *c, long length);
  void addReplyMapLen(client *c, long length);
  void addReplySetLen(client *c, long length);
  void addReplyAttributeLen(client *c, long length);
  void addReplyPushLen(client *c, long length);
  void addReplyHelp(client *c, const char **help);
  void addReplySubcommandSyntaxError(client *c);
  void addReplyLoadedModules(client *c);
  void copyClientOutputBuffer(client *dst, client *src);
  size_t sdsZmallocSize(sds s);
  size_t getStringObjectSdsUsedMemory(robj *o);
  void freeClientReplyValue(void *o);
  void *dupClientReplyValue(void *o);
  void getClientsMaxBuffers(unsigned long *longest_output_list,
                            unsigned long *biggest_input_buffer);
  char *getClientPeerId(client *client);
  sds catClientInfoString(sds s, client *client);
  sds getAllClientsInfoString(int type);
  void rewriteClientCommandVector(client *c, int argc, ...);
  void rewriteClientCommandArgument(client *c, int i, robj *newval);
  void replaceClientCommandVector(client *c, int argc, robj **argv);
  unsigned long getClientOutputBufferMemoryUsage(client *c);
  void freeClientsInAsyncFreeQueue(void);
  void asyncCloseClientOnOutputBufferLimitReached(client *c);
  int getClientType(client *c);
  int getClientTypeByName(char *name);
  char *getClientTypeName(int class);
  void flushSlavesOutputBuffers(void);
  void disconnectSlaves(void);
  int listenToPort(int port, int *fds, int *count);
  void pauseClients(mstime_t duration);
  int clientsArePaused(void);
  int processEventsWhileBlocked(void);
  int handleClientsWithPendingWrites(void);
  int handleClientsWithPendingWritesUsingThreads(void);
  int handleClientsWithPendingReadsUsingThreads(void);
  int stopThreadedIOIfNeeded(void);
  int clientHasPendingReplies(client *c);
  void unlinkClient(client *c);
  int writeToClient(client *c, int handler_installed);
  void linkClient(client *c);
  void protectClient(client *c);
  void unprotectClient(client *c);
  void initThreadedIO(void);
  client *lookupClientByID(uint64_t id);
#ifdef __GNUC__
  void addReplyErrorFormat(client *c, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
  void addReplyStatusFormat(client *c, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
#else
  void addReplyErrorFormat(client *c, const char *fmt, ...);
  void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif
  void enableTracking(client *c, uint64_t redirect_to);
  void disableTracking(client *c);
  void trackingRememberKeys(client *c);
  void trackingInvalidateKey(robj *keyobj);
  void trackingInvalidateKeysOnFlush(int dbid);
  void trackingLimitUsedSlots(void);
  unsigned long long trackingGetUsedSlots(void);
  void listTypeTryConversion(robj *subject, robj *value);
  void listTypePush(robj *subject, robj *value, int where);
  robj *listTypePop(robj *subject, int where);
  unsigned long listTypeLength(const robj *subject);
  listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                         unsigned char direction);
  void listTypeReleaseIterator(listTypeIterator *li);
  int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
  robj *listTypeGet(listTypeEntry *entry);
  void listTypeInsert(listTypeEntry *entry, robj *value, int where);
  int listTypeEqual(listTypeEntry *entry, robj *o);
  void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
  void listTypeConvert(robj *subject, int enc);
  void unblockClientWaitingData(client *c);
  void popGenericCommand(client *c, int where);
  void unwatchAllKeys(client *c);
  void initClientMultiState(client *c);
  void freeClientMultiState(client *c);
  void queueMultiCommand(client *c);
  void touchWatchedKey(redisDb *db, robj *key);
  void touchWatchedKeysOnFlush(int dbid);
  void discardTransaction(client *c);
  void flagTransaction(client *c);
  void execCommandPropagateMulti(client *c);
  void decrRefCount(robj *o);
  void decrRefCountVoid(void *o);
  void incrRefCount(robj *o);
  robj *makeObjectShared(robj *o);
  robj *resetRefCount(robj *obj);
  void freeStringObject(robj *o);
  void freeListObject(robj *o);
  void freeSetObject(robj *o);
  void freeZsetObject(robj *o);
  void freeHashObject(robj *o);
  robj *createObject(int type, void *ptr);
  robj *createStringObject(const char *ptr, size_t len);
  robj *createRawStringObject(const char *ptr, size_t len);
  robj *createEmbeddedStringObject(const char *ptr, size_t len);
  robj *dupStringObject(const robj *o);
  int isSdsRepresentableAsLongLong(sds s, long long *llval);
  int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
  robj *tryObjectEncoding(robj *o);
  robj *getDecodedObject(robj *o);
  size_t stringObjectLen(robj *o);
  robj *createStringObjectFromLongLong(long long value);
  robj *createStringObjectFromLongLongForValue(long long value);
  robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
  robj *createQuicklistObject(void);
  robj *createZiplistObject(void);
  robj *createSetObject(void);
  robj *createIntsetObject(void);
  robj *createHashObject(void);
  robj *createZsetObject(void);
  robj *createZsetZiplistObject(void);
  robj *createStreamObject(void);
  robj *createModuleObject(moduleType *mt, void *value);
  int getLongFromObjectOrReply(client *c, robj *o, long *target,
                               const char *msg);
  int checkType(client *c, robj *o, int type);
  int getLongLongFromObjectOrReply(client *c, robj *o, long long *target,
                                   const char *msg);
  int getDoubleFromObjectOrReply(client *c, robj *o, double *target,
                                 const char *msg);
  int getDoubleFromObject(const robj *o, double *target);
  int getLongLongFromObject(robj *o, long long *target);
  int getLongDoubleFromObject(robj *o, long double *target);
  int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target,
                                     const char *msg);
  char *strEncoding(int encoding);
  int compareStringObjects(robj *a, robj *b);
  int collateStringObjects(robj *a, robj *b);
  int equalStringObjects(robj *a, robj *b);
  unsigned long long estimateObjectIdleTime(robj *o);
  void trimStringObjectIfNeeded(robj *o);
  ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
  ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
  ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);
  void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
  void replicationFeedSlavesFromMasterStream(list *slaves, char *buf,
                                             size_t buflen);
  void replicationFeedMonitors(client *c, list *monitors, int dictid,
                               robj **argv, int argc);
  void updateSlavesWaitingBgsave(int bgsaveerr, int type);
  void replicationCron(void);
  void replicationHandleMasterDisconnection(void);
  void replicationCacheMaster(client *c);
  void resizeReplicationBacklog(long long newsize);
  void replicationSetMaster(char *ip, int port);
  void replicationUnsetMaster(void);
  void refreshGoodSlavesCount(void);
  void replicationScriptCacheInit(void);
  void replicationScriptCacheFlush(void);
  void replicationScriptCacheAdd(sds sha1);
  int replicationScriptCacheExists(sds sha1);
  void processClientsWaitingReplicas(void);
  void unblockClientWaitingReplicas(client *c);
  int replicationCountAcksByOffset(long long offset);
  void replicationSendNewlineToMaster(void);
  long long replicationGetSlaveOffset(void);
  char *replicationGetSlaveName(client *c);
  long long getPsyncInitialOffset(void);
  int replicationSetupSlaveForFullResync(client *slave, long long offset);
  void changeReplicationId(void);
  void clearReplicationId2(void);
  void chopReplicationBacklog(void);
  void replicationCacheMasterUsingMyself(void);
  void feedReplicationBacklog(void *ptr, size_t len);
  void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd,
                          void *clientData, int mask);
  void rdbPipeWriteHandlerConnRemoved(struct connection *conn);
  void startLoadingFile(FILE *fp, char *filename, int rdbflags);
  void startLoading(size_t size, int rdbflags);
  void loadingProgress(off_t pos);
  void stopLoading(int success);
  void startSaving(int rdbflags);
  void stopSaving(int success);
#define DISK_ERROR_TYPE_AOF 1
#define DISK_ERROR_TYPE_RDB 2
#define DISK_ERROR_TYPE_NONE 0
  int writeCommandsDeniedByDiskError(void);
  void killRDBChild(void);
  void flushAppendOnlyFile(int force);
  void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv,
                          int argc);
  void aofRemoveTempFile(pid_t childpid);
  int rewriteAppendOnlyFileBackground(void);
  int loadAppendOnlyFile(char *filename);
  void stopAppendOnly(void);
  int startAppendOnly(void);
  void backgroundRewriteDoneHandler(int exitcode, int bysignal);
  void aofRewriteBufferReset(void);
  unsigned long aofRewriteBufferSize(void);
  ssize_t aofReadDiffFromParent(void);
  void killAppendOnlyChild(void);
  void openChildInfoPipe(void);
  void closeChildInfoPipe(void);
  void sendChildInfo(int process_type);
  void receiveChildInfo(void);
  int redisFork();
  int hasActiveChildProcess();
  void sendChildCOWInfo(int ptype, char *pname);
  extern rax *Users;
  extern user *DefaultUser;
  void ACLInit(void);
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
  int ACLCheckUserCredentials(robj *username, robj *password);
  int ACLAuthenticateUser(client *c, robj *username, robj *password);
  unsigned long ACLGetCommandID(const char *cmdname);
  user *ACLGetUserByName(const char *name, size_t namelen);
  int ACLCheckCommandPerm(client *c);
  int ACLSetUser(user *u, const char *op, ssize_t oplen);
  sds ACLDefaultUserFirstPassword(void);
  uint64_t ACLGetCommandCategoryFlagByName(const char *name);
  int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
  char *ACLSetUserStringError(void);
  int ACLLoadConfiguredUsers(void);
  sds ACLDescribeUser(user *u);
  void ACLLoadUsersAtStartup(void);
  void addReplyCommandCategories(client *c, struct redisCommand *cmd);
#define ZADD_NONE 0
#define ZADD_INCR (1 << 0)
#define ZADD_NX (1 << 1)
#define ZADD_XX (1 << 2)
#define ZADD_NOP (1 << 3)
#define ZADD_NAN (1 << 4)
#define ZADD_ADDED (1 << 5)
#define ZADD_UPDATED (1 << 6)
#define ZADD_CH (1 << 16)
  typedef struct {
    double min, max;
    int minex, maxex;
  } zrangespec;
  typedef struct {
    sds min, max;
    int minex, maxex;
  } zlexrangespec;
  zskiplist *zslCreate(void);
  void zslFree(zskiplist *zsl);
  zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
  unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
  int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
  zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
  zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
  double zzlGetScore(unsigned char *sptr);
  void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
  void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
  unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
  unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
  unsigned long zsetLength(const robj *zobj);
  void zsetConvert(robj *zobj, int encoding);
  void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen);
  int zsetScore(robj *zobj, sds member, double *score);
  unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
  int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore);
  long zsetRank(robj *zobj, sds ele, int reverse);
  int zsetDel(robj *zobj, sds ele);
  void genericZpopCommand(client *c, robj **keyv, int keyc, int where,
                          int emitkey, robj *countarg);
  sds ziplistGetObject(unsigned char *sptr);
  int zslValueGteMin(double value, zrangespec *spec);
  int zslValueLteMax(double value, zrangespec *spec);
  void zslFreeLexRange(zlexrangespec *spec);
  int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
  unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
  unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
  zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
  zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
  int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
  int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
  int zslLexValueGteMin(sds value, zlexrangespec *spec);
  int zslLexValueLteMax(sds value, zlexrangespec *spec);
  int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree,
                        float *level);
  size_t freeMemoryGetNotCountedMemory();
  int freeMemoryIfNeeded(void);
  int freeMemoryIfNeededAndSafe(void);
  int processCommand(client *c);
  void setupSignalHandlers(void);
  struct redisCommand *lookupCommand(sds name);
  struct redisCommand *lookupCommandByCString(char *s);
  struct redisCommand *lookupCommandOrOriginal(sds name);
  void call(client *c, int flags);
  void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                 int flags);
  void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                     int target);
  void redisOpArrayInit(redisOpArray *oa);
  void redisOpArrayFree(redisOpArray *oa);
  void forceCommandPropagation(client *c, int flags);
  void preventCommandPropagation(client *c);
  void preventCommandAOF(client *c);
  void preventCommandReplication(client *c);
  int prepareForShutdown();
#ifdef __GNUC__
  void serverLog(int level, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
#else
  void serverLog(int level, const char *fmt, ...);
#endif
  void serverLogRaw(int level, const char *msg);
  void serverLogFromHandler(int level, const char *msg);
  void usage(void);
  void updateDictResizePolicy(void);
  int htNeedsResize(dict *dict);
  void populateCommandTable(void);
  void resetCommandTableStats(void);
  void adjustOpenFilesLimit(void);
  void closeListeningSockets(int unlink_unix_socket);
  void updateCachedTime(void);
  void resetServerStats(void);
  void activeDefragCycle(void);
  unsigned int getLRUClock(void);
  unsigned int LRU_CLOCK(void);
  const char *evictPolicyToString(void);
  struct redisMemOverhead *getMemoryOverheadData(void);
  void freeMemoryOverheadData(struct redisMemOverhead *mh);
  void checkChildrenDone(void);
#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1 << 0)
#define RESTART_SERVER_CONFIG_REWRITE \
  (1 << 1)
  int restartServer(int flags, mstime_t delay);
  robj *setTypeCreate(sds value);
  int setTypeAdd(robj *subject, sds value);
  int setTypeRemove(robj *subject, sds value);
  int setTypeIsMember(robj *subject, sds value);
  setTypeIterator *setTypeInitIterator(robj *subject);
  void setTypeReleaseIterator(setTypeIterator *si);
  int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);
  sds setTypeNextObject(setTypeIterator *si);
  int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
  unsigned long setTypeRandomElements(robj *set, unsigned long count,
                                      robj *aux_set);
  unsigned long setTypeSize(const robj *subject);
  void setTypeConvert(robj *subject, int enc);
#define HASH_SET_TAKE_FIELD (1 << 0)
#define HASH_SET_TAKE_VALUE (1 << 1)
#define HASH_SET_COPY 0
  void hashTypeConvert(robj *o, int enc);
  void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
  int hashTypeExists(robj *o, sds key);
  int hashTypeDelete(robj *o, sds key);
  unsigned long hashTypeLength(const robj *o);
  hashTypeIterator *hashTypeInitIterator(robj *subject);
  void hashTypeReleaseIterator(hashTypeIterator *hi);
  int hashTypeNext(hashTypeIterator *hi);
  void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                  unsigned char **vstr, unsigned int *vlen,
                                  long long *vll);
  sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
  void hashTypeCurrentObject(hashTypeIterator *hi, int what,
                             unsigned char **vstr, unsigned int *vlen,
                             long long *vll);
  sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
  robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
  robj *hashTypeGetValueObject(robj *o, sds field);
  int hashTypeSet(robj *o, sds field, sds value, int flags);
  int pubsubUnsubscribeAllChannels(client *c, int notify);
  int pubsubUnsubscribeAllPatterns(client *c, int notify);
  void freePubsubPattern(void *p);
  int listMatchPubsubPattern(void *a, void *b);
  int pubsubPublishMessage(robj *channel, robj *message);
  void addReplyPubsubMessage(client *c, robj *channel, robj *msg);
  void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
  int keyspaceEventsStringToFlags(char *classes);
  sds keyspaceEventsFlagsToString(int flags);
  void loadServerConfig(char *filename, char *options);
  void appendServerSaveParams(time_t seconds, int changes);
  void resetServerSaveParams(void);
  struct rewriteConfigState;
  void rewriteConfigRewriteLine(struct rewriteConfigState *state,
                                const char *option, sds line, int force);
  int rewriteConfig(char *path);
  int removeExpire(redisDb *db, robj *key);
  void propagateExpire(redisDb *db, robj *key, int lazy);
  int expireIfNeeded(redisDb *db, robj *key);
  long long getExpire(redisDb *db, robj *key);
  void setExpire(client *c, redisDb *db, robj *key, long long when);
  robj *lookupKey(redisDb *db, robj *key, int flags);
  robj *lookupKeyRead(redisDb *db, robj *key);
  robj *lookupKeyWrite(redisDb *db, robj *key);
  robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
  robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
  robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
  robj *objectCommandLookup(client *c, robj *key);
  robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);
  void objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                         long long lru_clock);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1 << 0)
  void dbAdd(redisDb *db, robj *key, robj *val);
  void dbOverwrite(redisDb *db, robj *key, robj *val);
  void setKey(redisDb *db, robj *key, robj *val);
  int dbExists(redisDb *db, robj *key);
  robj *dbRandomKey(redisDb *db);
  int dbSyncDelete(redisDb *db, robj *key);
  int dbDelete(redisDb *db, robj *key);
  robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
#define EMPTYDB_NO_FLAGS 0
#define EMPTYDB_ASYNC (1 << 0)
  long long emptyDb(int dbnum, int flags, void(callback)(void *));
  long long emptyDbGeneric(redisDb *dbarray, int dbnum, int flags,
                           void(callback)(void *));
  long long dbTotalServerKeyCount();
  int selectDb(client *c, int id);
  void signalModifiedKey(redisDb *db, robj *key);
  void signalFlushedDb(int dbid);
  unsigned int getKeysInSlot(unsigned int hashslot, robj **keys,
                             unsigned int count);
  unsigned int countKeysInSlot(unsigned int hashslot);
  unsigned int delKeysInSlot(unsigned int hashslot);
  int verifyClusterConfigWithData(void);
  void scanGenericCommand(client *c, robj *o, unsigned long cursor);
  int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
  void slotToKeyAdd(robj *key);
  void slotToKeyDel(robj *key);
  void slotToKeyFlush(void);
  int dbAsyncDelete(redisDb *db, robj *key);
  void emptyDbAsync(redisDb *db);
  void slotToKeyFlushAsync(void);
  size_t lazyfreeGetPendingObjectsCount(void);
  void freeObjAsync(robj *o);
  int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc,
                          int *numkeys);
  void getKeysFreeResult(int *result);
  int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                          int *numkeys);
  int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                   int *numkeys);
  int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                   int *numkeys);
  int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                      int *numkeys);
  int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                        int *numkeys);
  int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                    int *numkeys);
  void clusterInit(void);
  unsigned short crc16(const char *buf, int len);
  unsigned int keyHashSlot(char *key, int keylen);
  void clusterCron(void);
  void clusterPropagatePublish(robj *channel, robj *message);
  void migrateCloseTimedoutSockets(void);
  void clusterBeforeSleep(void);
  int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id,
                                       uint8_t type, unsigned char *payload,
                                       uint32_t len);
  void initSentinelConfig(void);
  void initSentinel(void);
  void sentinelTimer(void);
  char *sentinelHandleConfiguration(char **argv, int argc);
  void sentinelIsRunning(void);
  int redis_check_rdb(char *rdbfilename, FILE *fp);
  int redis_check_rdb_main(int argc, char **argv, FILE *fp);
  int redis_check_aof_main(int argc, char **argv);
  void scriptingInit(int setup);
  int ldbRemoveChild(pid_t pid);
  void ldbKillForkedSessions(void);
  int ldbPendingChildren(void);
  sds luaCreateFunction(client *c, lua_State *lua, robj *body);
  void processUnblockedClients(void);
  void blockClient(client *c, int btype);
  void unblockClient(client *c);
  void queueClientForReprocessing(client *c);
  void replyToBlockedClientTimedOut(client *c);
  int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout,
                                  int unit);
  void disconnectAllBlockedClients(void);
  void handleClientsBlockedOnKeys(void);
  void signalKeyAsReady(redisDb *db, robj *key);
  void blockForKeys(client *c, int btype, robj **keys, int numkeys,
                    mstime_t timeout, robj *target, streamID *ids);
  void activeExpireCycle(int type);
  void expireSlaveKeys(void);
  void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
  void flushSlaveKeysWithExpireList(void);
  size_t getSlaveKeyWithExpireCount(void);
  void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
  unsigned long LFUGetTimeInMinutes(void);
  uint8_t LFULogIncr(uint8_t value);
  unsigned long LFUDecrAndReturn(robj *o);
  uint64_t dictSdsHash(const void *key);
  int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
  void dictSdsDestructor(void *privdata, void *val);
  char *redisGitSHA1(void);
  char *redisGitDirty(void);
  uint64_t redisBuildId(void);
  char *redisBuildIdString(void);
  void authCommand(client *c);
  void pingCommand(client *c);
  void echoCommand(client *c);
  void commandCommand(client *c);
  void setCommand(client *c);
  void setnxCommand(client *c);
  void setexCommand(client *c);
  void psetexCommand(client *c);
  void getCommand(client *c);
  void delCommand(client *c);
  void unlinkCommand(client *c);
  void existsCommand(client *c);
  void setbitCommand(client *c);
  void getbitCommand(client *c);
  void bitfieldCommand(client *c);
  void setrangeCommand(client *c);
  void getrangeCommand(client *c);
  void incrCommand(client *c);
  void decrCommand(client *c);
  void incrbyCommand(client *c);
  void decrbyCommand(client *c);
  void incrbyfloatCommand(client *c);
  void selectCommand(client *c);
  void swapdbCommand(client *c);
  void randomkeyCommand(client *c);
  void keysCommand(client *c);
  void scanCommand(client *c);
  void dbsizeCommand(client *c);
  void lastsaveCommand(client *c);
  void saveCommand(client *c);
  void bgsaveCommand(client *c);
  void bgrewriteaofCommand(client *c);
  void shutdownCommand(client *c);
  void moveCommand(client *c);
  void renameCommand(client *c);
  void renamenxCommand(client *c);
  void lpushCommand(client *c);
  void rpushCommand(client *c);
  void lpushxCommand(client *c);
  void rpushxCommand(client *c);
  void linsertCommand(client *c);
  void lpopCommand(client *c);
  void rpopCommand(client *c);
  void llenCommand(client *c);
  void lindexCommand(client *c);
  void lrangeCommand(client *c);
  void ltrimCommand(client *c);
  void typeCommand(client *c);
  void lsetCommand(client *c);
  void saddCommand(client *c);
  void sremCommand(client *c);
  void smoveCommand(client *c);
  void sismemberCommand(client *c);
  void scardCommand(client *c);
  void spopCommand(client *c);
  void srandmemberCommand(client *c);
  void sinterCommand(client *c);
  void sinterstoreCommand(client *c);
  void sunionCommand(client *c);
  void sunionstoreCommand(client *c);
  void sdiffCommand(client *c);
  void sdiffstoreCommand(client *c);
  void sscanCommand(client *c);
  void syncCommand(client *c);
  void flushdbCommand(client *c);
  void flushallCommand(client *c);
  void sortCommand(client *c);
  void lremCommand(client *c);
  void rpoplpushCommand(client *c);
  void infoCommand(client *c);
  void mgetCommand(client *c);
  void monitorCommand(client *c);
  void expireCommand(client *c);
  void expireatCommand(client *c);
  void pexpireCommand(client *c);
  void pexpireatCommand(client *c);
  void getsetCommand(client *c);
  void ttlCommand(client *c);
  void touchCommand(client *c);
  void pttlCommand(client *c);
  void persistCommand(client *c);
  void replicaofCommand(client *c);
  void roleCommand(client *c);
  void debugCommand(client *c);
  void msetCommand(client *c);
  void msetnxCommand(client *c);
  void zaddCommand(client *c);
  void zincrbyCommand(client *c);
  void zrangeCommand(client *c);
  void zrangebyscoreCommand(client *c);
  void zrevrangebyscoreCommand(client *c);
  void zrangebylexCommand(client *c);
  void zrevrangebylexCommand(client *c);
  void zcountCommand(client *c);
  void zlexcountCommand(client *c);
  void zrevrangeCommand(client *c);
  void zcardCommand(client *c);
  void zremCommand(client *c);
  void zscoreCommand(client *c);
  void zremrangebyscoreCommand(client *c);
  void zremrangebylexCommand(client *c);
  void zpopminCommand(client *c);
  void zpopmaxCommand(client *c);
  void bzpopminCommand(client *c);
  void bzpopmaxCommand(client *c);
  void multiCommand(client *c);
  void execCommand(client *c);
  void discardCommand(client *c);
  void blpopCommand(client *c);
  void brpopCommand(client *c);
  void brpoplpushCommand(client *c);
  void appendCommand(client *c);
  void strlenCommand(client *c);
  void zrankCommand(client *c);
  void zrevrankCommand(client *c);
  void hsetCommand(client *c);
  void hsetnxCommand(client *c);
  void hgetCommand(client *c);
  void hmsetCommand(client *c);
  void hmgetCommand(client *c);
  void hdelCommand(client *c);
  void hlenCommand(client *c);
  void hstrlenCommand(client *c);
  void zremrangebyrankCommand(client *c);
  void zunionstoreCommand(client *c);
  void zinterstoreCommand(client *c);
  void zscanCommand(client *c);
  void hkeysCommand(client *c);
  void hvalsCommand(client *c);
  void hgetallCommand(client *c);
  void hexistsCommand(client *c);
  void hscanCommand(client *c);
  void configCommand(client *c);
  void hincrbyCommand(client *c);
  void hincrbyfloatCommand(client *c);
  void subscribeCommand(client *c);
  void unsubscribeCommand(client *c);
  void psubscribeCommand(client *c);
  void punsubscribeCommand(client *c);
  void publishCommand(client *c);
  void pubsubCommand(client *c);
  void watchCommand(client *c);
  void unwatchCommand(client *c);
  void clusterCommand(client *c);
  void restoreCommand(client *c);
  void migrateCommand(client *c);
  void askingCommand(client *c);
  void readonlyCommand(client *c);
  void readwriteCommand(client *c);
  void dumpCommand(client *c);
  void objectCommand(client *c);
  void memoryCommand(client *c);
  void clientCommand(client *c);
  void helloCommand(client *c);
  void evalCommand(client *c);
  void evalShaCommand(client *c);
  void scriptCommand(client *c);
  void timeCommand(client *c);
  void bitopCommand(client *c);
  void bitcountCommand(client *c);
  void bitposCommand(client *c);
  void replconfCommand(client *c);
  void waitCommand(client *c);
  void geoencodeCommand(client *c);
  void geodecodeCommand(client *c);
  void georadiusbymemberCommand(client *c);
  void georadiusbymemberroCommand(client *c);
  void georadiusCommand(client *c);
  void georadiusroCommand(client *c);
  void geoaddCommand(client *c);
  void geohashCommand(client *c);
  void geoposCommand(client *c);
  void geodistCommand(client *c);
  void pfselftestCommand(client *c);
  void pfaddCommand(client *c);
  void pfcountCommand(client *c);
  void pfmergeCommand(client *c);
  void pfdebugCommand(client *c);
  void latencyCommand(client *c);
  void moduleCommand(client *c);
  void securityWarningCommand(client *c);
  void xaddCommand(client *c);
  void xrangeCommand(client *c);
  void xrevrangeCommand(client *c);
  void xlenCommand(client *c);
  void xreadCommand(client *c);
  void xgroupCommand(client *c);
  void xsetidCommand(client *c);
  void xackCommand(client *c);
  void xpendingCommand(client *c);
  void xclaimCommand(client *c);
  void xinfoCommand(client *c);
  void xdelCommand(client *c);
  void xtrimCommand(client *c);
  void lolwutCommand(client *c);
  void aclCommand(client *c);
  void _serverAssertWithInfo(const client *c, const robj *o, const char *estr,
                             const char *file, int line);
  void _serverAssert(const char *estr, const char *file, int line);
  void _serverPanic(const char *file, int line, const char *msg, ...);
  void bugReportStart(void);
  void serverLogObjectDebugInfo(const robj *o);
  void sigsegvHandler(int sig, siginfo_t *info, void *secret);
  sds genRedisInfoString(char *section);
  sds genModulesInfoString(sds info);
  void enableWatchdog(int period);
  void disableWatchdog(void);
  void watchdogScheduleSignal(int period);
  void serverLogHexDump(int level, char *descr, void *value, size_t len);
  int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
  void mixDigest(unsigned char *digest, void *ptr, size_t len);
  void xorDigest(unsigned char *digest, void *ptr, size_t len);
  int populateCommandTableParseFlags(struct redisCommand *c, char *strflags);
  void tlsInit(void);
  int tlsConfigure(redisTLSContextConfig *ctx_config);
#endif
