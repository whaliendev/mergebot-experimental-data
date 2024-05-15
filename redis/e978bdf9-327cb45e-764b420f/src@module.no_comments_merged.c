#include "server.h"
#include "cluster.h"
#include "rdb.h"
#include <dlfcn.h>
#include <sys/wait.h>
typedef struct RedisModuleInfoCtx {
    struct RedisModule *module;
    sds requested_section;
    sds info;
    int sections;
    int in_section;
    int in_dict_field;
} RedisModuleInfoCtx;
typedef void (*RedisModuleInfoFunc)(RedisModuleInfoCtx *ctx, int for_crash_report);
struct RedisModule {
    void *handle;
    char *name;
    int ver;
    int apiver;
    list *types;
    list *usedby;
    list *using;
    list *filters;
    int in_call;
    int in_hook;
    int options;
    int blocked_clients;
    RedisModuleInfoFunc info_cb;
};
typedef struct RedisModule RedisModule;
struct RedisModuleSharedAPI {
    void *func;
    RedisModule *module;
};
typedef struct RedisModuleSharedAPI RedisModuleSharedAPI;
static dict *modules;
struct AutoMemEntry {
    void *ptr;
    int type;
};
#define REDISMODULE_AM_KEY 0
#define REDISMODULE_AM_STRING 1
#define REDISMODULE_AM_REPLY 2
#define REDISMODULE_AM_FREED 3
#define REDISMODULE_AM_DICT 4
#define REDISMODULE_POOL_ALLOC_MIN_SIZE (1024*8)
#define REDISMODULE_POOL_ALLOC_ALIGN (sizeof(void*))
typedef struct RedisModulePoolAllocBlock {
    uint32_t size;
    uint32_t used;
    struct RedisModulePoolAllocBlock *next;
    char memory[];
} RedisModulePoolAllocBlock;
struct RedisModuleBlockedClient;
struct RedisModuleCtx {
    void *getapifuncptr;
    struct RedisModule *module;
    client *client;
    struct RedisModuleBlockedClient *blocked_client;
    struct AutoMemEntry *amqueue;
    int amqueue_len;
    int amqueue_used;
    int flags;
    void **postponed_arrays;
    int postponed_arrays_count;
    void *blocked_privdata;
    RedisModuleString *blocked_ready_key;
    int *keys_pos;
    int keys_count;
    struct RedisModulePoolAllocBlock *pa_head;
    redisOpArray saved_oparray;
};
typedef struct RedisModuleCtx RedisModuleCtx;
#define REDISMODULE_CTX_INIT {(void*)(unsigned long)&RM_GetApi, NULL, NULL, NULL, NULL, 0, 0, 0, NULL, 0, NULL, NULL, NULL, 0, NULL, {0}}
#define REDISMODULE_CTX_MULTI_EMITTED (1<<0)
#define REDISMODULE_CTX_AUTO_MEMORY (1<<1)
#define REDISMODULE_CTX_KEYS_POS_REQUEST (1<<2)
#define REDISMODULE_CTX_BLOCKED_REPLY (1<<3)
#define REDISMODULE_CTX_BLOCKED_TIMEOUT (1<<4)
#define REDISMODULE_CTX_THREAD_SAFE (1<<5)
#define REDISMODULE_CTX_BLOCKED_DISCONNECTED (1<<6)
#define REDISMODULE_CTX_MODULE_COMMAND_CALL (1<<7)
struct RedisModuleKey {
    RedisModuleCtx *ctx;
    redisDb *db;
    robj *key;
    robj *value;
    void *iter;
    int mode;
    uint32_t ztype;
    zrangespec zrs;
    zlexrangespec zlrs;
    uint32_t zstart;
    uint32_t zend;
    void *zcurrent;
    int zer;
};
typedef struct RedisModuleKey RedisModuleKey;
#define REDISMODULE_ZSET_RANGE_NONE 0
#define REDISMODULE_ZSET_RANGE_LEX 1
#define REDISMODULE_ZSET_RANGE_SCORE 2
#define REDISMODULE_ZSET_RANGE_POS 3
struct RedisModuleBlockedClient;
typedef int (*RedisModuleCmdFunc) (RedisModuleCtx *ctx, void **argv, int argc);
typedef void (*RedisModuleDisconnectFunc) (RedisModuleCtx *ctx, struct RedisModuleBlockedClient *bc);
struct RedisModuleCommandProxy {
    struct RedisModule *module;
    RedisModuleCmdFunc func;
    struct redisCommand *rediscmd;
};
typedef struct RedisModuleCommandProxy RedisModuleCommandProxy;
#define REDISMODULE_REPLYFLAG_NONE 0
#define REDISMODULE_REPLYFLAG_TOPARSE (1<<0)
#define REDISMODULE_REPLYFLAG_NESTED (1<<1)
typedef struct RedisModuleCallReply {
    RedisModuleCtx *ctx;
    int type;
    int flags;
    size_t len;
    char *proto;
    size_t protolen;
    union {
        const char *str;
        long long ll;
        struct RedisModuleCallReply *array;
    } val;
} RedisModuleCallReply;
typedef struct RedisModuleBlockedClient {
    client *client;
    RedisModule *module;
    RedisModuleCmdFunc reply_callback;
    RedisModuleCmdFunc timeout_callback;
    RedisModuleDisconnectFunc disconnect_callback;
    void (*free_privdata)(RedisModuleCtx*,void*);
    void *privdata;
    client *reply_client;
    int dbid;
    int blocked_on_keys;
    int unblocked;
} RedisModuleBlockedClient;
static pthread_mutex_t moduleUnblockedClientsMutex = PTHREAD_MUTEX_INITIALIZER;
static list *moduleUnblockedClients;
static pthread_mutex_t moduleGIL = PTHREAD_MUTEX_INITIALIZER;
typedef int (*RedisModuleNotificationFunc) (RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key);
typedef struct RedisModuleKeyspaceSubscriber {
    RedisModule *module;
    RedisModuleNotificationFunc notify_callback;
    int event_mask;
    int active;
} RedisModuleKeyspaceSubscriber;
static list *moduleKeyspaceSubscribers;
static client *moduleFreeContextReusedClient;
typedef struct RedisModuleDict {
    rax *rax;
} RedisModuleDict;
typedef struct RedisModuleDictIter {
    RedisModuleDict *dict;
    raxIterator ri;
} RedisModuleDictIter;
typedef struct RedisModuleCommandFilterCtx {
    RedisModuleString **argv;
    int argc;
} RedisModuleCommandFilterCtx;
typedef void (*RedisModuleCommandFilterFunc) (RedisModuleCommandFilterCtx *filter);
typedef struct RedisModuleCommandFilter {
    RedisModule *module;
    RedisModuleCommandFilterFunc callback;
    int flags;
} RedisModuleCommandFilter;
static list *moduleCommandFilters;
typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);
static struct RedisModuleForkInfo {
    RedisModuleForkDoneHandler done_handler;
    void* done_handler_user_data;
} moduleForkInfo = {0};
#define REDISMODULE_ARGV_REPLICATE (1<<0)
#define REDISMODULE_ARGV_NO_AOF (1<<1)
#define REDISMODULE_ARGV_NO_REPLICAS (1<<2)
#define SHOULD_SIGNAL_MODIFIED_KEYS(ctx) \
    ctx->module? !(ctx->module->options & REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED) : 1
typedef struct RedisModuleEventListener {
    RedisModule *module;
    RedisModuleEvent event;
    RedisModuleEventCallback callback;
} RedisModuleEventListener;
list *RedisModule_EventListeners;
unsigned long long ModulesInHooks = 0;
void RM_FreeCallReply(RedisModuleCallReply *reply);
void RM_CloseKey(RedisModuleKey *key);
void autoMemoryCollect(RedisModuleCtx *ctx);
robj **moduleCreateArgvFromUserFormat(const char *cmdname, const char *fmt, int *argcp, int *flags, va_list ap);
void moduleReplicateMultiIfNeeded(RedisModuleCtx *ctx);
void RM_ZsetRangeStop(RedisModuleKey *kp);
static void zsetKeyReset(RedisModuleKey *key);
void RM_FreeDict(RedisModuleCtx *ctx, RedisModuleDict *d);
void *RM_Alloc(size_t bytes) {
    return zmalloc(bytes);
}
void *RM_Calloc(size_t nmemb, size_t size) {
    return zcalloc(nmemb*size);
}
void* RM_Realloc(void *ptr, size_t bytes) {
    return zrealloc(ptr,bytes);
}
void RM_Free(void *ptr) {
    zfree(ptr);
}
char *RM_Strdup(const char *str) {
    return zstrdup(str);
}
void poolAllocRelease(RedisModuleCtx *ctx) {
    RedisModulePoolAllocBlock *head = ctx->pa_head, *next;
    while(head != NULL) {
        next = head->next;
        zfree(head);
        head = next;
    }
    ctx->pa_head = NULL;
}
void *RM_PoolAlloc(RedisModuleCtx *ctx, size_t bytes) {
    if (bytes == 0) return NULL;
    RedisModulePoolAllocBlock *b = ctx->pa_head;
    size_t left = b ? b->size - b->used : 0;
    if (left >= bytes) {
        size_t alignment = REDISMODULE_POOL_ALLOC_ALIGN;
        while (bytes < alignment && alignment/2 >= bytes) alignment /= 2;
        if (b->used % alignment)
            b->used += alignment - (b->used % alignment);
        left = (b->used > b->size) ? 0 : b->size - b->used;
    }
    if (left < bytes) {
        size_t blocksize = REDISMODULE_POOL_ALLOC_MIN_SIZE;
        if (blocksize < bytes) blocksize = bytes;
        b = zmalloc(sizeof(*b) + blocksize);
        b->size = blocksize;
        b->used = 0;
        b->next = ctx->pa_head;
        ctx->pa_head = b;
    }
    char *retval = b->memory + b->used;
    b->used += bytes;
    return retval;
}
int moduleCreateEmptyKey(RedisModuleKey *key, int type) {
    robj *obj;
    if (!(key->mode & REDISMODULE_WRITE) || key->value)
        return REDISMODULE_ERR;
    switch(type) {
    case REDISMODULE_KEYTYPE_LIST:
        obj = createQuicklistObject();
        quicklistSetOptions(obj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        break;
    case REDISMODULE_KEYTYPE_ZSET:
        obj = createZsetZiplistObject();
        break;
    case REDISMODULE_KEYTYPE_HASH:
        obj = createHashObject();
        break;
    default: return REDISMODULE_ERR;
    }
    dbAdd(key->db,key->key,obj);
    key->value = obj;
    return REDISMODULE_OK;
}
int moduleDelKeyIfEmpty(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL) return 0;
    int isempty;
    robj *o = key->value;
    switch(o->type) {
    case OBJ_LIST: isempty = listTypeLength(o) == 0; break;
    case OBJ_SET: isempty = setTypeSize(o) == 0; break;
    case OBJ_ZSET: isempty = zsetLength(o) == 0; break;
    case OBJ_HASH : isempty = hashTypeLength(o) == 0; break;
    default: isempty = 0;
    }
    if (isempty) {
        dbDelete(key->db,key->key);
        key->value = NULL;
        return 1;
    } else {
        return 0;
    }
}
int RM_GetApi(const char *funcname, void **targetPtrPtr) {
    dictEntry *he = dictFind(server.moduleapi, funcname);
    if (!he) return REDISMODULE_ERR;
    *targetPtrPtr = dictGetVal(he);
    return REDISMODULE_OK;
}
void moduleHandlePropagationAfterCommandCallback(RedisModuleCtx *ctx) {
    client *c = ctx->client;
    if (!(ctx->flags & REDISMODULE_CTX_MULTI_EMITTED)) return;
    if (c->flags & CLIENT_LUA) return;
    robj *propargv[1];
    propargv[0] = createStringObject("EXEC",4);
    alsoPropagate(server.execCommand,c->db->id,propargv,1,
        PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(propargv[0]);
    if (!(ctx->flags & REDISMODULE_CTX_MODULE_COMMAND_CALL) &&
        server.also_propagate.numops)
    {
        for (int j = 0; j < server.also_propagate.numops; j++) {
            redisOp *rop = &server.also_propagate.ops[j];
            int target = rop->target;
            if (target)
                propagate(rop->cmd,rop->dbid,rop->argv,rop->argc,target);
        }
        redisOpArrayFree(&server.also_propagate);
        server.also_propagate = ctx->saved_oparray;
    }
}
void moduleFreeContext(RedisModuleCtx *ctx) {
    moduleHandlePropagationAfterCommandCallback(ctx);
    autoMemoryCollect(ctx);
    poolAllocRelease(ctx);
    if (ctx->postponed_arrays) {
        zfree(ctx->postponed_arrays);
        ctx->postponed_arrays_count = 0;
        serverLog(LL_WARNING,
            "API misuse detected in module %s: "
            "RedisModule_ReplyWithArray(REDISMODULE_POSTPONED_ARRAY_LEN) "
            "not matched by the same number of RedisModule_SetReplyArrayLen() "
            "calls.",
            ctx->module->name);
    }
    if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) freeClient(ctx->client);
}
void RedisModuleCommandDispatcher(client *c) {
    RedisModuleCommandProxy *cp = (void*)(unsigned long)c->cmd->getkeys_proc;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
    ctx.flags |= REDISMODULE_CTX_MODULE_COMMAND_CALL;
    ctx.module = cp->module;
    ctx.client = c;
    cp->func(&ctx,(void**)c->argv,c->argc);
    moduleFreeContext(&ctx);
    for (int i = 0; i < c->argc; i++) {
        if (c->argv[i]->refcount > 1)
            trimStringObjectIfNeeded(c->argv[i]);
    }
}
int *moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    RedisModuleCommandProxy *cp = (void*)(unsigned long)cmd->getkeys_proc;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
    ctx.module = cp->module;
    ctx.client = NULL;
    ctx.flags |= REDISMODULE_CTX_KEYS_POS_REQUEST;
    cp->func(&ctx,(void**)argv,argc);
    int *res = ctx.keys_pos;
    if (numkeys) *numkeys = ctx.keys_count;
    moduleFreeContext(&ctx);
    return res;
}
int RM_IsKeysPositionRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_KEYS_POS_REQUEST) != 0;
}
void RM_KeyAtPos(RedisModuleCtx *ctx, int pos) {
    if (!(ctx->flags & REDISMODULE_CTX_KEYS_POS_REQUEST)) return;
    if (pos <= 0) return;
    ctx->keys_pos = zrealloc(ctx->keys_pos,sizeof(int)*(ctx->keys_count+1));
    ctx->keys_pos[ctx->keys_count++] = pos;
}
int commandFlagsFromString(char *s) {
    int count, j;
    int flags = 0;
    sds *tokens = sdssplitlen(s,strlen(s)," ",1,&count);
    for (j = 0; j < count; j++) {
        char *t = tokens[j];
        if (!strcasecmp(t,"write")) flags |= CMD_WRITE;
        else if (!strcasecmp(t,"readonly")) flags |= CMD_READONLY;
        else if (!strcasecmp(t,"admin")) flags |= CMD_ADMIN;
        else if (!strcasecmp(t,"deny-oom")) flags |= CMD_DENYOOM;
        else if (!strcasecmp(t,"deny-script")) flags |= CMD_NOSCRIPT;
        else if (!strcasecmp(t,"allow-loading")) flags |= CMD_LOADING;
        else if (!strcasecmp(t,"pubsub")) flags |= CMD_PUBSUB;
        else if (!strcasecmp(t,"random")) flags |= CMD_RANDOM;
        else if (!strcasecmp(t,"allow-stale")) flags |= CMD_STALE;
        else if (!strcasecmp(t,"no-monitor")) flags |= CMD_SKIP_MONITOR;
        else if (!strcasecmp(t,"fast")) flags |= CMD_FAST;
        else if (!strcasecmp(t,"getkeys-api")) flags |= CMD_MODULE_GETKEYS;
        else if (!strcasecmp(t,"no-cluster")) flags |= CMD_MODULE_NO_CLUSTER;
        else break;
    }
    sdsfreesplitres(tokens,count);
    if (j != count) return -1;
    return flags;
}
int RM_CreateCommand(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) {
    int flags = strflags ? commandFlagsFromString((char*)strflags) : 0;
    if (flags == -1) return REDISMODULE_ERR;
    if ((flags & CMD_MODULE_NO_CLUSTER) && server.cluster_enabled)
        return REDISMODULE_ERR;
    struct redisCommand *rediscmd;
    RedisModuleCommandProxy *cp;
    sds cmdname = sdsnew(name);
    if (lookupCommand(cmdname) != NULL) {
        sdsfree(cmdname);
        return REDISMODULE_ERR;
    }
    cp = zmalloc(sizeof(*cp));
    cp->module = ctx->module;
    cp->func = cmdfunc;
    cp->rediscmd = zmalloc(sizeof(*rediscmd));
    cp->rediscmd->name = cmdname;
    cp->rediscmd->proc = RedisModuleCommandDispatcher;
    cp->rediscmd->arity = -1;
    cp->rediscmd->flags = flags | CMD_MODULE;
    cp->rediscmd->getkeys_proc = (redisGetKeysProc*)(unsigned long)cp;
    cp->rediscmd->firstkey = firstkey;
    cp->rediscmd->lastkey = lastkey;
    cp->rediscmd->keystep = keystep;
    cp->rediscmd->microseconds = 0;
    cp->rediscmd->calls = 0;
    dictAdd(server.commands,sdsdup(cmdname),cp->rediscmd);
    dictAdd(server.orig_commands,sdsdup(cmdname),cp->rediscmd);
    cp->rediscmd->id = ACLGetCommandID(cmdname);
    return REDISMODULE_OK;
}
void RM_SetModuleAttribs(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    RedisModule *module;
    if (ctx->module != NULL) return;
    module = zmalloc(sizeof(*module));
    module->name = sdsnew((char*)name);
    module->ver = ver;
    module->apiver = apiver;
    module->types = listCreate();
    module->usedby = listCreate();
    module->using = listCreate();
    module->filters = listCreate();
    module->in_call = 0;
    module->in_hook = 0;
    module->options = 0;
    ctx->module = module;
}
int RM_IsModuleNameBusy(const char *name) {
    sds modulename = sdsnew(name);
    dictEntry *de = dictFind(modules,modulename);
    sdsfree(modulename);
    return de != NULL;
}
long long RM_Milliseconds(void) {
    return mstime();
}
void RM_SetModuleOptions(RedisModuleCtx *ctx, int options) {
    ctx->module->options = options;
}
int RM_SignalModifiedKey(RedisModuleCtx *ctx, RedisModuleString *keyname) {
    signalModifiedKey(ctx->client->db,keyname);
    return REDISMODULE_OK;
}
void RM_AutoMemory(RedisModuleCtx *ctx) {
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
}
void autoMemoryAdd(RedisModuleCtx *ctx, int type, void *ptr) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return;
    if (ctx->amqueue_used == ctx->amqueue_len) {
        ctx->amqueue_len *= 2;
        if (ctx->amqueue_len < 16) ctx->amqueue_len = 16;
        ctx->amqueue = zrealloc(ctx->amqueue,sizeof(struct AutoMemEntry)*ctx->amqueue_len);
    }
    ctx->amqueue[ctx->amqueue_used].type = type;
    ctx->amqueue[ctx->amqueue_used].ptr = ptr;
    ctx->amqueue_used++;
}
int autoMemoryFreed(RedisModuleCtx *ctx, int type, void *ptr) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return 0;
    int count = (ctx->amqueue_used+1)/2;
    for (int j = 0; j < count; j++) {
        for (int side = 0; side < 2; side++) {
            int i = (side == 0) ? (ctx->amqueue_used - 1 - j) : j;
            if (ctx->amqueue[i].type == type &&
                ctx->amqueue[i].ptr == ptr)
            {
                ctx->amqueue[i].type = REDISMODULE_AM_FREED;
                if (i != ctx->amqueue_used-1) {
                    ctx->amqueue[i] = ctx->amqueue[ctx->amqueue_used-1];
                }
                ctx->amqueue_used--;
                return 1;
            }
        }
    }
    return 0;
}
void autoMemoryCollect(RedisModuleCtx *ctx) {
    if (!(ctx->flags & REDISMODULE_CTX_AUTO_MEMORY)) return;
    ctx->flags &= ~REDISMODULE_CTX_AUTO_MEMORY;
    int j;
    for (j = 0; j < ctx->amqueue_used; j++) {
        void *ptr = ctx->amqueue[j].ptr;
        switch(ctx->amqueue[j].type) {
        case REDISMODULE_AM_STRING: decrRefCount(ptr); break;
        case REDISMODULE_AM_REPLY: RM_FreeCallReply(ptr); break;
        case REDISMODULE_AM_KEY: RM_CloseKey(ptr); break;
        case REDISMODULE_AM_DICT: RM_FreeDict(NULL,ptr); break;
        }
    }
    ctx->flags |= REDISMODULE_CTX_AUTO_MEMORY;
    zfree(ctx->amqueue);
    ctx->amqueue = NULL;
    ctx->amqueue_len = 0;
    ctx->amqueue_used = 0;
}
RedisModuleString *RM_CreateString(RedisModuleCtx *ctx, const char *ptr, size_t len) {
    RedisModuleString *o = createStringObject(ptr,len);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}
RedisModuleString *RM_CreateStringPrintf(RedisModuleCtx *ctx, const char *fmt, ...) {
    sds s = sdsempty();
    va_list ap;
    va_start(ap, fmt);
    s = sdscatvprintf(s, fmt, ap);
    va_end(ap);
    RedisModuleString *o = createObject(OBJ_STRING, s);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}
RedisModuleString *RM_CreateStringFromLongLong(RedisModuleCtx *ctx, long long ll) {
    char buf[LONG_STR_SIZE];
    size_t len = ll2string(buf,sizeof(buf),ll);
    return RM_CreateString(ctx,buf,len);
}
RedisModuleString *RM_CreateStringFromString(RedisModuleCtx *ctx, const RedisModuleString *str) {
    RedisModuleString *o = dupStringObject(str);
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_STRING,o);
    return o;
}
void RM_FreeString(RedisModuleCtx *ctx, RedisModuleString *str) {
    decrRefCount(str);
    if (ctx != NULL) autoMemoryFreed(ctx,REDISMODULE_AM_STRING,str);
}
void RM_RetainString(RedisModuleCtx *ctx, RedisModuleString *str) {
    if (ctx == NULL || !autoMemoryFreed(ctx,REDISMODULE_AM_STRING,str)) {
        incrRefCount(str);
    }
}
const char *RM_StringPtrLen(const RedisModuleString *str, size_t *len) {
    if (str == NULL) {
        const char *errmsg = "(NULL string reply referenced in module)";
        if (len) *len = strlen(errmsg);
        return errmsg;
    }
    if (len) *len = sdslen(str->ptr);
    return str->ptr;
}
int RM_StringToLongLong(const RedisModuleString *str, long long *ll) {
    return string2ll(str->ptr,sdslen(str->ptr),ll) ? REDISMODULE_OK :
                                                     REDISMODULE_ERR;
}
int RM_StringToDouble(const RedisModuleString *str, double *d) {
    int retval = getDoubleFromObject(str,d);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_StringCompare(RedisModuleString *a, RedisModuleString *b) {
    return compareStringObjects(a,b);
}
RedisModuleString *moduleAssertUnsharedString(RedisModuleString *str) {
    if (str->refcount != 1) {
        serverLog(LL_WARNING,
            "Module attempted to use an in-place string modify operation "
            "with a string referenced multiple times. Please check the code "
            "for API usage correctness.");
        return NULL;
    }
    if (str->encoding == OBJ_ENCODING_EMBSTR) {
        str->ptr = sdsnewlen(str->ptr,sdslen(str->ptr));
        str->encoding = OBJ_ENCODING_RAW;
    } else if (str->encoding == OBJ_ENCODING_INT) {
        str->ptr = sdsfromlonglong((long)str->ptr);
        str->encoding = OBJ_ENCODING_RAW;
    }
    return str;
}
int RM_StringAppendBuffer(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len) {
    UNUSED(ctx);
    str = moduleAssertUnsharedString(str);
    if (str == NULL) return REDISMODULE_ERR;
    str->ptr = sdscatlen(str->ptr,buf,len);
    return REDISMODULE_OK;
}
int RM_WrongArity(RedisModuleCtx *ctx) {
    addReplyErrorFormat(ctx->client,
        "wrong number of arguments for '%s' command",
        (char*)ctx->client->argv[0]->ptr);
    return REDISMODULE_OK;
}
client *moduleGetReplyClient(RedisModuleCtx *ctx) {
    if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) {
        if (ctx->blocked_client)
            return ctx->blocked_client->reply_client;
        else
            return NULL;
    } else {
        return ctx->client;
    }
}
int RM_ReplyWithLongLong(RedisModuleCtx *ctx, long long ll) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyLongLong(c,ll);
    return REDISMODULE_OK;
}
int replyWithStatus(RedisModuleCtx *ctx, const char *msg, char *prefix) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyProto(c,prefix,strlen(prefix));
    addReplyProto(c,msg,strlen(msg));
    addReplyProto(c,"\r\n",2);
    return REDISMODULE_OK;
}
int RM_ReplyWithError(RedisModuleCtx *ctx, const char *err) {
    return replyWithStatus(ctx,err,"-");
}
int RM_ReplyWithSimpleString(RedisModuleCtx *ctx, const char *msg) {
    return replyWithStatus(ctx,msg,"+");
}
int RM_ReplyWithArray(RedisModuleCtx *ctx, long len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    if (len == REDISMODULE_POSTPONED_ARRAY_LEN) {
        ctx->postponed_arrays = zrealloc(ctx->postponed_arrays,sizeof(void*)*
                (ctx->postponed_arrays_count+1));
        ctx->postponed_arrays[ctx->postponed_arrays_count] =
            addReplyDeferredLen(c);
        ctx->postponed_arrays_count++;
    } else {
        addReplyArrayLen(c,len);
    }
    return REDISMODULE_OK;
}
int RM_ReplyWithNullArray(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyNullArray(c);
    return REDISMODULE_OK;
}
int RM_ReplyWithEmptyArray(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReply(c,shared.emptyarray);
    return REDISMODULE_OK;
}
void RM_ReplySetArrayLength(RedisModuleCtx *ctx, long len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return;
    if (ctx->postponed_arrays_count == 0) {
        serverLog(LL_WARNING,
            "API misuse detected in module %s: "
            "RedisModule_ReplySetArrayLength() called without previous "
            "RedisModule_ReplyWithArray(ctx,REDISMODULE_POSTPONED_ARRAY_LEN) "
            "call.", ctx->module->name);
            return;
    }
    ctx->postponed_arrays_count--;
    setDeferredArrayLen(c,
            ctx->postponed_arrays[ctx->postponed_arrays_count],
            len);
    if (ctx->postponed_arrays_count == 0) {
        zfree(ctx->postponed_arrays);
        ctx->postponed_arrays = NULL;
    }
}
int RM_ReplyWithStringBuffer(RedisModuleCtx *ctx, const char *buf, size_t len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulkCBuffer(c,(char*)buf,len);
    return REDISMODULE_OK;
}
int RM_ReplyWithCString(RedisModuleCtx *ctx, const char *buf) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulkCString(c,(char*)buf);
    return REDISMODULE_OK;
}
int RM_ReplyWithString(RedisModuleCtx *ctx, RedisModuleString *str) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulk(c,str);
    return REDISMODULE_OK;
}
int RM_ReplyWithEmptyString(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyBulkCBuffer(c, "", 0);
    return REDISMODULE_OK;
}
int RM_ReplyWithVerbatimString(RedisModuleCtx *ctx, const char *buf, size_t len) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyVerbatim(c, buf, len, "txt");
    return REDISMODULE_OK;
}
int RM_ReplyWithNull(RedisModuleCtx *ctx) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyNull(c);
    return REDISMODULE_OK;
}
int RM_ReplyWithCallReply(RedisModuleCtx *ctx, RedisModuleCallReply *reply) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    sds proto = sdsnewlen(reply->proto, reply->protolen);
    addReplySds(c,proto);
    return REDISMODULE_OK;
}
int RM_ReplyWithDouble(RedisModuleCtx *ctx, double d) {
    client *c = moduleGetReplyClient(ctx);
    if (c == NULL) return REDISMODULE_OK;
    addReplyDouble(c,d);
    return REDISMODULE_OK;
}
void moduleReplicateMultiIfNeeded(RedisModuleCtx *ctx) {
    if (ctx->client->flags & (CLIENT_MULTI|CLIENT_LUA)) return;
    if (ctx->flags & REDISMODULE_CTX_MULTI_EMITTED) return;
    if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) return;
    if (!(ctx->flags & REDISMODULE_CTX_MODULE_COMMAND_CALL)) {
        ctx->saved_oparray = server.also_propagate;
        redisOpArrayInit(&server.also_propagate);
    }
    execCommandPropagateMulti(ctx->client);
    ctx->flags |= REDISMODULE_CTX_MULTI_EMITTED;
}
int RM_Replicate(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) {
    struct redisCommand *cmd;
    robj **argv = NULL;
    int argc = 0, flags = 0, j;
    va_list ap;
    cmd = lookupCommandByCString((char*)cmdname);
    if (!cmd) return REDISMODULE_ERR;
    va_start(ap, fmt);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    va_end(ap);
    if (argv == NULL) return REDISMODULE_ERR;
    int target = 0;
    if (!(flags & REDISMODULE_ARGV_NO_AOF)) target |= PROPAGATE_AOF;
    if (!(flags & REDISMODULE_ARGV_NO_REPLICAS)) target |= PROPAGATE_REPL;
    if (ctx->flags & REDISMODULE_CTX_THREAD_SAFE) {
        propagate(cmd,ctx->client->db->id,argv,argc,target);
    } else {
        moduleReplicateMultiIfNeeded(ctx);
        alsoPropagate(cmd,ctx->client->db->id,argv,argc,target);
    }
    for (j = 0; j < argc; j++) decrRefCount(argv[j]);
    zfree(argv);
    server.dirty++;
    return REDISMODULE_OK;
}
int RM_ReplicateVerbatim(RedisModuleCtx *ctx) {
    alsoPropagate(ctx->client->cmd,ctx->client->db->id,
        ctx->client->argv,ctx->client->argc,
        PROPAGATE_AOF|PROPAGATE_REPL);
    server.dirty++;
    return REDISMODULE_OK;
}
unsigned long long RM_GetClientId(RedisModuleCtx *ctx) {
    if (ctx->client == NULL) return 0;
    return ctx->client->id;
}
int modulePopulateClientInfoStructure(void *ci, client *client, int structver) {
    if (structver != 1) return REDISMODULE_ERR;
    RedisModuleClientInfoV1 *ci1 = ci;
    memset(ci1,0,sizeof(*ci1));
    ci1->version = structver;
    if (client->flags & CLIENT_MULTI)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_MULTI;
    if (client->flags & CLIENT_PUBSUB)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_PUBSUB;
    if (client->flags & CLIENT_UNIX_SOCKET)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_UNIXSOCKET;
    if (client->flags & CLIENT_TRACKING)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_TRACKING;
    if (client->flags & CLIENT_BLOCKED)
        ci1->flags |= REDISMODULE_CLIENTINFO_FLAG_BLOCKED;
    int port;
    connPeerToString(client->conn,ci1->addr,sizeof(ci1->addr),&port);
    ci1->port = port;
    ci1->db = client->db->id;
    ci1->id = client->id;
    return REDISMODULE_OK;
}
int modulePopulateReplicationInfoStructure(void *ri, int structver) {
    if (structver != 1) return REDISMODULE_ERR;
    RedisModuleReplicationInfoV1 *ri1 = ri;
    memset(ri1,0,sizeof(*ri1));
    ri1->version = structver;
    ri1->master = server.masterhost==NULL;
    ri1->masterhost = server.masterhost? server.masterhost: "";
    ri1->masterport = server.masterport;
    ri1->replid1 = server.replid;
    ri1->replid2 = server.replid2;
    ri1->repl1_offset = server.master_repl_offset;
    ri1->repl2_offset = server.second_replid_offset;
    return REDISMODULE_OK;
}
int RM_GetClientInfoById(void *ci, uint64_t id) {
    client *client = lookupClientByID(id);
    if (client == NULL) return REDISMODULE_ERR;
    if (ci == NULL) return REDISMODULE_OK;
    uint64_t structver = ((uint64_t*)ci)[0];
    return modulePopulateClientInfoStructure(ci,client,structver);
}
int RM_PublishMessage(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) {
    UNUSED(ctx);
    int receivers = pubsubPublishMessage(channel, message);
    if (server.cluster_enabled)
        clusterPropagatePublish(channel, message);
    return receivers;
}
int RM_GetSelectedDb(RedisModuleCtx *ctx) {
    return ctx->client->db->id;
}
int RM_GetContextFlags(RedisModuleCtx *ctx) {
    int flags = 0;
    if (ctx->client) {
        if (ctx->client->flags & CLIENT_LUA)
         flags |= REDISMODULE_CTX_FLAGS_LUA;
        if (ctx->client->flags & CLIENT_MULTI)
         flags |= REDISMODULE_CTX_FLAGS_MULTI;
        if (ctx->client->flags & CLIENT_MASTER)
         flags |= REDISMODULE_CTX_FLAGS_REPLICATED;
    }
    if (server.cluster_enabled)
        flags |= REDISMODULE_CTX_FLAGS_CLUSTER;
    if (server.loading)
        flags |= REDISMODULE_CTX_FLAGS_LOADING;
    if (server.maxmemory > 0) {
        flags |= REDISMODULE_CTX_FLAGS_MAXMEMORY;
        if (server.maxmemory_policy != MAXMEMORY_NO_EVICTION)
            flags |= REDISMODULE_CTX_FLAGS_EVICT;
    }
    if (server.aof_state != AOF_OFF)
        flags |= REDISMODULE_CTX_FLAGS_AOF;
    if (server.saveparamslen > 0)
        flags |= REDISMODULE_CTX_FLAGS_RDB;
    if (server.masterhost == NULL) {
        flags |= REDISMODULE_CTX_FLAGS_MASTER;
    } else {
        flags |= REDISMODULE_CTX_FLAGS_SLAVE;
        if (server.repl_slave_ro)
            flags |= REDISMODULE_CTX_FLAGS_READONLY;
        if (server.repl_state == REPL_STATE_CONNECT ||
            server.repl_state == REPL_STATE_CONNECTING)
        {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING;
        } else if (server.repl_state == REPL_STATE_TRANSFER) {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING;
        } else if (server.repl_state == REPL_STATE_CONNECTED) {
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_ONLINE;
        }
        if (server.repl_state != REPL_STATE_CONNECTED)
            flags |= REDISMODULE_CTX_FLAGS_REPLICA_IS_STALE;
    }
    float level;
    int retval = getMaxmemoryState(NULL,NULL,NULL,&level);
    if (retval == C_ERR) flags |= REDISMODULE_CTX_FLAGS_OOM;
    if (level > 0.75) flags |= REDISMODULE_CTX_FLAGS_OOM_WARNING;
    if (hasActiveChildProcess()) flags |= REDISMODULE_CTX_FLAGS_ACTIVE_CHILD;
    return flags;
}
int RM_SelectDb(RedisModuleCtx *ctx, int newid) {
    int retval = selectDb(ctx->client,newid);
    return (retval == C_OK) ? REDISMODULE_OK : REDISMODULE_ERR;
}
void *RM_OpenKey(RedisModuleCtx *ctx, robj *keyname, int mode) {
    RedisModuleKey *kp;
    robj *value;
    int flags = mode & REDISMODULE_OPEN_KEY_NOTOUCH? LOOKUP_NOTOUCH: 0;
    if (mode & REDISMODULE_WRITE) {
        value = lookupKeyWriteWithFlags(ctx->client->db,keyname, flags);
    } else {
        value = lookupKeyReadWithFlags(ctx->client->db,keyname, flags);
        if (value == NULL) {
            return NULL;
        }
    }
    kp = zmalloc(sizeof(*kp));
    kp->ctx = ctx;
    kp->db = ctx->client->db;
    kp->key = keyname;
    incrRefCount(keyname);
    kp->value = value;
    kp->iter = NULL;
    kp->mode = mode;
    zsetKeyReset(kp);
    autoMemoryAdd(ctx,REDISMODULE_AM_KEY,kp);
    return (void*)kp;
}
void RM_CloseKey(RedisModuleKey *key) {
    if (key == NULL) return;
    int signal = SHOULD_SIGNAL_MODIFIED_KEYS(key->ctx);
    if ((key->mode & REDISMODULE_WRITE) && signal)
        signalModifiedKey(key->db,key->key);
    RM_ZsetRangeStop(key);
    decrRefCount(key->key);
    autoMemoryFreed(key->ctx,REDISMODULE_AM_KEY,key);
    zfree(key);
}
int RM_KeyType(RedisModuleKey *key) {
    if (key == NULL || key->value == NULL) return REDISMODULE_KEYTYPE_EMPTY;
    switch(key->value->type) {
    case OBJ_STRING: return REDISMODULE_KEYTYPE_STRING;
    case OBJ_LIST: return REDISMODULE_KEYTYPE_LIST;
    case OBJ_SET: return REDISMODULE_KEYTYPE_SET;
    case OBJ_ZSET: return REDISMODULE_KEYTYPE_ZSET;
    case OBJ_HASH: return REDISMODULE_KEYTYPE_HASH;
    case OBJ_MODULE: return REDISMODULE_KEYTYPE_MODULE;
    default: return 0;
    }
}
size_t RM_ValueLength(RedisModuleKey *key) {
    if (key == NULL || key->value == NULL) return 0;
    switch(key->value->type) {
    case OBJ_STRING: return stringObjectLen(key->value);
    case OBJ_LIST: return listTypeLength(key->value);
    case OBJ_SET: return setTypeSize(key->value);
    case OBJ_ZSET: return zsetLength(key->value);
    case OBJ_HASH: return hashTypeLength(key->value);
    default: return 0;
    }
}
int RM_DeleteKey(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value) {
        dbDelete(key->db,key->key);
        key->value = NULL;
    }
    return REDISMODULE_OK;
}
int RM_UnlinkKey(RedisModuleKey *key) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value) {
        dbAsyncDelete(key->db,key->key);
        key->value = NULL;
    }
    return REDISMODULE_OK;
}
mstime_t RM_GetExpire(RedisModuleKey *key) {
    mstime_t expire = getExpire(key->db,key->key);
    if (expire == -1 || key->value == NULL) return -1;
    expire -= mstime();
    return expire >= 0 ? expire : 0;
}
int RM_SetExpire(RedisModuleKey *key, mstime_t expire) {
    if (!(key->mode & REDISMODULE_WRITE) || key->value == NULL)
        return REDISMODULE_ERR;
    if (expire != REDISMODULE_NO_EXPIRE) {
        expire += mstime();
        setExpire(key->ctx->client,key->db,key->key,expire);
    } else {
        removeExpire(key->db,key->key);
    }
    return REDISMODULE_OK;
}
void RM_ResetDataset(int restart_aof, int async) {
    if (restart_aof && server.aof_state != AOF_OFF) stopAppendOnly();
    flushAllDataAndResetRDB(async? EMPTYDB_ASYNC: EMPTYDB_NO_FLAGS);
    if (server.aof_enabled && restart_aof) restartAOFAfterSYNC();
}
unsigned long long RM_DbSize(RedisModuleCtx *ctx) {
    return dictSize(ctx->client->db->dict);
}
RedisModuleString *RM_RandomKey(RedisModuleCtx *ctx) {
    robj *key = dbRandomKey(ctx->client->db);
    autoMemoryAdd(ctx,REDISMODULE_AM_STRING,key);
    return key;
}
int RM_StringSet(RedisModuleKey *key, RedisModuleString *str) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter) return REDISMODULE_ERR;
    RM_DeleteKey(key);
    setKey(key->db,key->key,str);
    key->value = str;
    return REDISMODULE_OK;
}
char *RM_StringDMA(RedisModuleKey *key, size_t *len, int mode) {
    char *emptystring = "<dma-empty-string>";
    if (key->value == NULL) {
        *len = 0;
        return emptystring;
    }
    if (key->value->type != OBJ_STRING) return NULL;
    if ((mode & REDISMODULE_WRITE) || key->value->encoding != OBJ_ENCODING_RAW)
        key->value = dbUnshareStringValue(key->db, key->key, key->value);
    *len = sdslen(key->value->ptr);
    return key->value->ptr;
}
int RM_StringTruncate(RedisModuleKey *key, size_t newlen) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_STRING) return REDISMODULE_ERR;
    if (newlen > 512*1024*1024) return REDISMODULE_ERR;
    if (key->value == NULL && newlen == 0) return REDISMODULE_OK;
    if (key->value == NULL) {
        robj *o = createObject(OBJ_STRING,sdsnewlen(NULL, newlen));
        setKey(key->db,key->key,o);
        key->value = o;
        decrRefCount(o);
    } else {
        key->value = dbUnshareStringValue(key->db, key->key, key->value);
        size_t curlen = sdslen(key->value->ptr);
        if (newlen > curlen) {
            key->value->ptr = sdsgrowzero(key->value->ptr,newlen);
        } else if (newlen < curlen) {
            sdsrange(key->value->ptr,0,newlen-1);
            if (sdslen(key->value->ptr) < sdsavail(key->value->ptr))
                key->value->ptr = sdsRemoveFreeSpace(key->value->ptr);
        }
    }
    return REDISMODULE_OK;
}
int RM_ListPush(RedisModuleKey *key, int where, RedisModuleString *ele) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_LIST) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_LIST);
    listTypePush(key->value, ele,
        (where == REDISMODULE_LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL);
    return REDISMODULE_OK;
}
RedisModuleString *RM_ListPop(RedisModuleKey *key, int where) {
    if (!(key->mode & REDISMODULE_WRITE) ||
        key->value == NULL ||
        key->value->type != OBJ_LIST) return NULL;
    robj *ele = listTypePop(key->value,
        (where == REDISMODULE_LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL);
    robj *decoded = getDecodedObject(ele);
    decrRefCount(ele);
    moduleDelKeyIfEmpty(key);
    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,decoded);
    return decoded;
}
int RM_ZsetAddFlagsToCoreFlags(int flags) {
    int retflags = 0;
    if (flags & REDISMODULE_ZADD_XX) retflags |= ZADD_XX;
    if (flags & REDISMODULE_ZADD_NX) retflags |= ZADD_NX;
    return retflags;
}
int RM_ZsetAddFlagsFromCoreFlags(int flags) {
    int retflags = 0;
    if (flags & ZADD_ADDED) retflags |= REDISMODULE_ZADD_ADDED;
    if (flags & ZADD_UPDATED) retflags |= REDISMODULE_ZADD_UPDATED;
    if (flags & ZADD_NOP) retflags |= REDISMODULE_ZADD_NOP;
    return retflags;
}
int RM_ZsetAdd(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr) {
    int flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) flags = RM_ZsetAddFlagsToCoreFlags(*flagsptr);
    if (zsetAdd(key->value,score,ele->ptr,&flags,NULL) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = RM_ZsetAddFlagsFromCoreFlags(flags);
    return REDISMODULE_OK;
}
int RM_ZsetIncrby(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore) {
    int flags = 0;
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_ZSET);
    if (flagsptr) flags = RM_ZsetAddFlagsToCoreFlags(*flagsptr);
    flags |= ZADD_INCR;
    if (zsetAdd(key->value,score,ele->ptr,&flags,newscore) == 0) {
        if (flagsptr) *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr && (*flagsptr & ZADD_NAN)) {
        *flagsptr = 0;
        return REDISMODULE_ERR;
    }
    if (flagsptr) *flagsptr = RM_ZsetAddFlagsFromCoreFlags(flags);
    return REDISMODULE_OK;
}
int RM_ZsetRem(RedisModuleKey *key, RedisModuleString *ele, int *deleted) {
    if (!(key->mode & REDISMODULE_WRITE)) return REDISMODULE_ERR;
    if (key->value && key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (key->value != NULL && zsetDel(key->value,ele->ptr)) {
        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return REDISMODULE_OK;
}
int RM_ZsetScore(RedisModuleKey *key, RedisModuleString *ele, double *score) {
    if (key->value == NULL) return REDISMODULE_ERR;
    if (key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    if (zsetScore(key->value,ele->ptr,score) == C_ERR) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
void zsetKeyReset(RedisModuleKey *key) {
    key->ztype = REDISMODULE_ZSET_RANGE_NONE;
    key->zcurrent = NULL;
    key->zer = 1;
}
void RM_ZsetRangeStop(RedisModuleKey *key) {
    if (key->ztype == REDISMODULE_ZSET_RANGE_LEX)
        zslFreeLexRange(&key->zlrs);
    zsetKeyReset(key);
}
int RM_ZsetRangeEndReached(RedisModuleKey *key) {
    return key->zer;
}
int zsetInitScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex, int first) {
    if (!key->value || key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    RM_ZsetRangeStop(key);
    key->ztype = REDISMODULE_ZSET_RANGE_SCORE;
    key->zer = 0;
    zrangespec *zrs = &key->zrs;
    zrs->min = min;
    zrs->max = max;
    zrs->minex = minex;
    zrs->maxex = maxex;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        key->zcurrent = first ? zzlFirstInRange(key->value->ptr,zrs) :
                                zzlLastInRange(key->value->ptr,zrs);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = key->value->ptr;
        zskiplist *zsl = zs->zsl;
        key->zcurrent = first ? zslFirstInRange(zsl,zrs) :
                                zslLastInRange(zsl,zrs);
    } else {
        serverPanic("Unsupported zset encoding");
    }
    if (key->zcurrent == NULL) key->zer = 1;
    return REDISMODULE_OK;
}
int RM_ZsetFirstInScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex) {
    return zsetInitScoreRange(key,min,max,minex,maxex,1);
}
int RM_ZsetLastInScoreRange(RedisModuleKey *key, double min, double max, int minex, int maxex) {
    return zsetInitScoreRange(key,min,max,minex,maxex,0);
}
int zsetInitLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max, int first) {
    if (!key->value || key->value->type != OBJ_ZSET) return REDISMODULE_ERR;
    RM_ZsetRangeStop(key);
    key->zer = 0;
    zlexrangespec *zlrs = &key->zlrs;
    if (zslParseLexRange(min, max, zlrs) == C_ERR) return REDISMODULE_ERR;
    key->ztype = REDISMODULE_ZSET_RANGE_LEX;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        key->zcurrent = first ? zzlFirstInLexRange(key->value->ptr,zlrs) :
                                zzlLastInLexRange(key->value->ptr,zlrs);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = key->value->ptr;
        zskiplist *zsl = zs->zsl;
        key->zcurrent = first ? zslFirstInLexRange(zsl,zlrs) :
                                zslLastInLexRange(zsl,zlrs);
    } else {
        serverPanic("Unsupported zset encoding");
    }
    if (key->zcurrent == NULL) key->zer = 1;
    return REDISMODULE_OK;
}
int RM_ZsetFirstInLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) {
    return zsetInitLexRange(key,min,max,1);
}
int RM_ZsetLastInLexRange(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) {
    return zsetInitLexRange(key,min,max,0);
}
RedisModuleString *RM_ZsetRangeCurrentElement(RedisModuleKey *key, double *score) {
    RedisModuleString *str;
    if (key->zcurrent == NULL) return NULL;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr, *sptr;
        eptr = key->zcurrent;
        sds ele = ziplistGetObject(eptr);
        if (score) {
            sptr = ziplistNext(key->value->ptr,eptr);
            *score = zzlGetScore(sptr);
        }
        str = createObject(OBJ_STRING,ele);
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent;
        if (score) *score = ln->score;
        str = createStringObject(ln->ele,sdslen(ln->ele));
    } else {
        serverPanic("Unsupported zset encoding");
    }
    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,str);
    return str;
}
int RM_ZsetRangeNext(RedisModuleKey *key) {
    if (!key->ztype || !key->zcurrent) return 0;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->zcurrent;
        unsigned char *next;
        next = ziplistNext(zl,eptr);
        if (next) next = ziplistNext(zl,next);
        if (next == NULL) {
            key->zer = 1;
            return 0;
        } else {
            if (key->ztype == REDISMODULE_ZSET_RANGE_SCORE) {
                unsigned char *saved_next = next;
                next = ziplistNext(zl,next);
                double score = zzlGetScore(next);
                if (!zslValueLteMax(score,&key->zrs)) {
                    key->zer = 1;
                    return 0;
                }
                next = saved_next;
            } else if (key->ztype == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zzlLexValueLteMax(next,&key->zlrs)) {
                    key->zer = 1;
                    return 0;
                }
            }
            key->zcurrent = next;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent, *next = ln->level[0].forward;
        if (next == NULL) {
            key->zer = 1;
            return 0;
        } else {
            if (key->ztype == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueLteMax(next->score,&key->zrs))
            {
                key->zer = 1;
                return 0;
            } else if (key->ztype == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zslLexValueLteMax(next->ele,&key->zlrs)) {
                    key->zer = 1;
                    return 0;
                }
            }
            key->zcurrent = next;
            return 1;
        }
    } else {
        serverPanic("Unsupported zset encoding");
    }
}
int RM_ZsetRangePrev(RedisModuleKey *key) {
    if (!key->ztype || !key->zcurrent) return 0;
    if (key->value->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = key->value->ptr;
        unsigned char *eptr = key->zcurrent;
        unsigned char *prev;
        prev = ziplistPrev(zl,eptr);
        if (prev) prev = ziplistPrev(zl,prev);
        if (prev == NULL) {
            key->zer = 1;
            return 0;
        } else {
            if (key->ztype == REDISMODULE_ZSET_RANGE_SCORE) {
                unsigned char *saved_prev = prev;
                prev = ziplistNext(zl,prev);
                double score = zzlGetScore(prev);
                if (!zslValueGteMin(score,&key->zrs)) {
                    key->zer = 1;
                    return 0;
                }
                prev = saved_prev;
            } else if (key->ztype == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zzlLexValueGteMin(prev,&key->zlrs)) {
                    key->zer = 1;
                    return 0;
                }
            }
            key->zcurrent = prev;
            return 1;
        }
    } else if (key->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zskiplistNode *ln = key->zcurrent, *prev = ln->backward;
        if (prev == NULL) {
            key->zer = 1;
            return 0;
        } else {
            if (key->ztype == REDISMODULE_ZSET_RANGE_SCORE &&
                !zslValueGteMin(prev->score,&key->zrs))
            {
                key->zer = 1;
                return 0;
            } else if (key->ztype == REDISMODULE_ZSET_RANGE_LEX) {
                if (!zslLexValueGteMin(prev->ele,&key->zlrs)) {
                    key->zer = 1;
                    return 0;
                }
            }
            key->zcurrent = prev;
            return 1;
        }
    } else {
        serverPanic("Unsupported zset encoding");
    }
}
int RM_HashSet(RedisModuleKey *key, int flags, ...) {
    va_list ap;
    if (!(key->mode & REDISMODULE_WRITE)) return 0;
    if (key->value && key->value->type != OBJ_HASH) return 0;
    if (key->value == NULL) moduleCreateEmptyKey(key,REDISMODULE_KEYTYPE_HASH);
    int updated = 0;
    va_start(ap, flags);
    while(1) {
        RedisModuleString *field, *value;
        if (flags & REDISMODULE_HASH_CFIELDS) {
            char *cfield = va_arg(ap,char*);
            if (cfield == NULL) break;
            field = createRawStringObject(cfield,strlen(cfield));
        } else {
            field = va_arg(ap,RedisModuleString*);
            if (field == NULL) break;
        }
        value = va_arg(ap,RedisModuleString*);
        if (flags & (REDISMODULE_HASH_XX|REDISMODULE_HASH_NX)) {
            int exists = hashTypeExists(key->value, field->ptr);
            if (((flags & REDISMODULE_HASH_XX) && !exists) ||
                ((flags & REDISMODULE_HASH_NX) && exists))
            {
                if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
                continue;
            }
        }
        if (value == REDISMODULE_HASH_DELETE) {
            updated += hashTypeDelete(key->value, field->ptr);
            if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
            continue;
        }
        int low_flags = HASH_SET_COPY;
        if (flags & REDISMODULE_HASH_CFIELDS)
            low_flags |= HASH_SET_TAKE_FIELD;
        robj *argv[2] = {field,value};
        hashTypeTryConversion(key->value,argv,0,1);
        updated += hashTypeSet(key->value, field->ptr, value->ptr, low_flags);
        if (flags & REDISMODULE_HASH_CFIELDS) {
           field->ptr = NULL;
           decrRefCount(field);
        }
    }
    va_end(ap);
    moduleDelKeyIfEmpty(key);
    return updated;
}
int RM_HashGet(RedisModuleKey *key, int flags, ...) {
    va_list ap;
    if (key->value && key->value->type != OBJ_HASH) return REDISMODULE_ERR;
    va_start(ap, flags);
    while(1) {
        RedisModuleString *field, **valueptr;
        int *existsptr;
        if (flags & REDISMODULE_HASH_CFIELDS) {
            char *cfield = va_arg(ap,char*);
            if (cfield == NULL) break;
            field = createRawStringObject(cfield,strlen(cfield));
        } else {
            field = va_arg(ap,RedisModuleString*);
            if (field == NULL) break;
        }
        if (flags & REDISMODULE_HASH_EXISTS) {
            existsptr = va_arg(ap,int*);
            if (key->value)
                *existsptr = hashTypeExists(key->value,field->ptr);
            else
                *existsptr = 0;
        } else {
            valueptr = va_arg(ap,RedisModuleString**);
            if (key->value) {
                *valueptr = hashTypeGetValueObject(key->value,field->ptr);
                if (*valueptr) {
                    robj *decoded = getDecodedObject(*valueptr);
                    decrRefCount(*valueptr);
                    *valueptr = decoded;
                }
                if (*valueptr)
                    autoMemoryAdd(key->ctx,REDISMODULE_AM_STRING,*valueptr);
            } else {
                *valueptr = NULL;
            }
        }
        if (flags & REDISMODULE_HASH_CFIELDS) decrRefCount(field);
    }
    va_end(ap);
    return REDISMODULE_OK;
}
RedisModuleCallReply *moduleCreateCallReplyFromProto(RedisModuleCtx *ctx, sds proto) {
    RedisModuleCallReply *reply = zmalloc(sizeof(*reply));
    reply->ctx = ctx;
    reply->proto = proto;
    reply->protolen = sdslen(proto);
    reply->flags = REDISMODULE_REPLYFLAG_TOPARSE;
    switch(proto[0]) {
    case '$':
    case '+': reply->type = REDISMODULE_REPLY_STRING; break;
    case '-': reply->type = REDISMODULE_REPLY_ERROR; break;
    case ':': reply->type = REDISMODULE_REPLY_INTEGER; break;
    case '*': reply->type = REDISMODULE_REPLY_ARRAY; break;
    default: reply->type = REDISMODULE_REPLY_UNKNOWN; break;
    }
    if ((proto[0] == '*' || proto[0] == '$') && proto[1] == '-')
        reply->type = REDISMODULE_REPLY_NULL;
    return reply;
}
void moduleParseCallReply_Int(RedisModuleCallReply *reply);
void moduleParseCallReply_BulkString(RedisModuleCallReply *reply);
void moduleParseCallReply_SimpleString(RedisModuleCallReply *reply);
void moduleParseCallReply_Array(RedisModuleCallReply *reply);
void moduleParseCallReply(RedisModuleCallReply *reply) {
    if (!(reply->flags & REDISMODULE_REPLYFLAG_TOPARSE)) return;
    reply->flags &= ~REDISMODULE_REPLYFLAG_TOPARSE;
    switch(reply->proto[0]) {
    case ':': moduleParseCallReply_Int(reply); break;
    case '$': moduleParseCallReply_BulkString(reply); break;
    case '-':
    case '+': moduleParseCallReply_SimpleString(reply); break;
    case '*': moduleParseCallReply_Array(reply); break;
    }
}
void moduleParseCallReply_Int(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    string2ll(proto+1,p-proto-1,&reply->val.ll);
    reply->protolen = p-proto+2;
    reply->type = REDISMODULE_REPLY_INTEGER;
}
void moduleParseCallReply_BulkString(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    long long bulklen;
    string2ll(proto+1,p-proto-1,&bulklen);
    if (bulklen == -1) {
        reply->protolen = p-proto+2;
        reply->type = REDISMODULE_REPLY_NULL;
    } else {
        reply->val.str = p+2;
        reply->len = bulklen;
        reply->protolen = p-proto+2+bulklen+2;
        reply->type = REDISMODULE_REPLY_STRING;
    }
}
void moduleParseCallReply_SimpleString(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    reply->val.str = proto+1;
    reply->len = p-proto-1;
    reply->protolen = p-proto+2;
    reply->type = proto[0] == '+' ? REDISMODULE_REPLY_STRING :
                                    REDISMODULE_REPLY_ERROR;
}
void moduleParseCallReply_Array(RedisModuleCallReply *reply) {
    char *proto = reply->proto;
    char *p = strchr(proto+1,'\r');
    long long arraylen, j;
    string2ll(proto+1,p-proto-1,&arraylen);
    p += 2;
    if (arraylen == -1) {
        reply->protolen = p-proto;
        reply->type = REDISMODULE_REPLY_NULL;
        return;
    }
    reply->val.array = zmalloc(sizeof(RedisModuleCallReply)*arraylen);
    reply->len = arraylen;
    for (j = 0; j < arraylen; j++) {
        RedisModuleCallReply *ele = reply->val.array+j;
        ele->flags = REDISMODULE_REPLYFLAG_NESTED |
                     REDISMODULE_REPLYFLAG_TOPARSE;
        ele->proto = p;
        ele->ctx = reply->ctx;
        moduleParseCallReply(ele);
        p += ele->protolen;
    }
    reply->protolen = p-proto;
    reply->type = REDISMODULE_REPLY_ARRAY;
}
void RM_FreeCallReply_Rec(RedisModuleCallReply *reply, int freenested){
    if (!freenested && reply->flags & REDISMODULE_REPLYFLAG_NESTED) return;
    if (!(reply->flags & REDISMODULE_REPLYFLAG_TOPARSE)) {
        if (reply->type == REDISMODULE_REPLY_ARRAY) {
            size_t j;
            for (j = 0; j < reply->len; j++)
                RM_FreeCallReply_Rec(reply->val.array+j,1);
            zfree(reply->val.array);
        }
    }
    if (!(reply->flags & REDISMODULE_REPLYFLAG_NESTED)) {
        if (reply->proto) sdsfree(reply->proto);
        zfree(reply);
    }
}
void RM_FreeCallReply(RedisModuleCallReply *reply) {
    RedisModuleCtx *ctx = reply->ctx;
    RM_FreeCallReply_Rec(reply,0);
    autoMemoryFreed(ctx,REDISMODULE_AM_REPLY,reply);
}
int RM_CallReplyType(RedisModuleCallReply *reply) {
    if (!reply) return REDISMODULE_REPLY_UNKNOWN;
    return reply->type;
}
size_t RM_CallReplyLength(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    switch(reply->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
    case REDISMODULE_REPLY_ARRAY:
        return reply->len;
    default:
        return 0;
    }
}
RedisModuleCallReply *RM_CallReplyArrayElement(RedisModuleCallReply *reply, size_t idx) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_ARRAY) return NULL;
    if (idx >= reply->len) return NULL;
    return reply->val.array+idx;
}
long long RM_CallReplyInteger(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_INTEGER) return LLONG_MIN;
    return reply->val.ll;
}
const char *RM_CallReplyStringPtr(RedisModuleCallReply *reply, size_t *len) {
    moduleParseCallReply(reply);
    if (reply->type != REDISMODULE_REPLY_STRING &&
        reply->type != REDISMODULE_REPLY_ERROR) return NULL;
    if (len) *len = reply->len;
    return reply->val.str;
}
RedisModuleString *RM_CreateStringFromCallReply(RedisModuleCallReply *reply) {
    moduleParseCallReply(reply);
    switch(reply->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
        return RM_CreateString(reply->ctx,reply->val.str,reply->len);
    case REDISMODULE_REPLY_INTEGER: {
        char buf[64];
        int len = ll2string(buf,sizeof(buf),reply->val.ll);
        return RM_CreateString(reply->ctx,buf,len);
        }
    default: return NULL;
    }
}
robj **moduleCreateArgvFromUserFormat(const char *cmdname, const char *fmt, int *argcp, int *flags, va_list ap) {
    int argc = 0, argv_size, j;
    robj **argv = NULL;
    argv_size = strlen(fmt)+1;
    argv = zrealloc(argv,sizeof(robj*)*argv_size);
    argv[0] = createStringObject(cmdname,strlen(cmdname));
    argc++;
    const char *p = fmt;
    while(*p) {
        if (*p == 'c') {
            char *cstr = va_arg(ap,char*);
            argv[argc++] = createStringObject(cstr,strlen(cstr));
        } else if (*p == 's') {
            robj *obj = va_arg(ap,void*);
            argv[argc++] = obj;
            incrRefCount(obj);
        } else if (*p == 'b') {
            char *buf = va_arg(ap,char*);
            size_t len = va_arg(ap,size_t);
            argv[argc++] = createStringObject(buf,len);
        } else if (*p == 'l') {
            long long ll = va_arg(ap,long long);
            argv[argc++] = createObject(OBJ_STRING,sdsfromlonglong(ll));
        } else if (*p == 'v') {
             robj **v = va_arg(ap, void*);
             size_t vlen = va_arg(ap, size_t);
             argv_size += vlen-1;
             argv = zrealloc(argv,sizeof(robj*)*argv_size);
             size_t i = 0;
             for (i = 0; i < vlen; i++) {
                 incrRefCount(v[i]);
                 argv[argc++] = v[i];
             }
        } else if (*p == '!') {
            if (flags) (*flags) |= REDISMODULE_ARGV_REPLICATE;
        } else if (*p == 'A') {
            if (flags) (*flags) |= REDISMODULE_ARGV_NO_AOF;
        } else if (*p == 'R') {
            if (flags) (*flags) |= REDISMODULE_ARGV_NO_REPLICAS;
        } else {
            goto fmterr;
        }
        p++;
    }
    *argcp = argc;
    return argv;
fmterr:
    for (j = 0; j < argc; j++)
        decrRefCount(argv[j]);
    zfree(argv);
    return NULL;
}
RedisModuleCallReply *RM_Call(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) {
    struct redisCommand *cmd;
    client *c = NULL;
    robj **argv = NULL;
    int argc = 0, flags = 0;
    va_list ap;
    RedisModuleCallReply *reply = NULL;
    int replicate = 0;
    va_start(ap, fmt);
    c = createClient(NULL);
    c->user = NULL;
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    replicate = flags & REDISMODULE_ARGV_REPLICATE;
    va_end(ap);
    c->flags |= CLIENT_MODULE;
    c->db = ctx->client->db;
    c->argv = argv;
    c->argc = argc;
    if (ctx->module) ctx->module->in_call++;
    if (argv == NULL) {
        errno = EINVAL;
        goto cleanup;
    }
    moduleCallCommandFilters(c);
    cmd = lookupCommand(c->argv[0]->ptr);
    if (!cmd) {
        errno = EINVAL;
        goto cleanup;
    }
    c->cmd = c->lastcmd = cmd;
    if ((cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity)) {
        errno = EINVAL;
        goto cleanup;
    }
    if (server.cluster_enabled && !(ctx->client->flags & CLIENT_MASTER)) {
        c->flags &= ~(CLIENT_READONLY|CLIENT_ASKING);
        c->flags |= ctx->client->flags & (CLIENT_READONLY|CLIENT_ASKING);
        if (getNodeByQuery(c,c->cmd,c->argv,c->argc,NULL,NULL) !=
                           server.cluster->myself)
        {
            errno = EPERM;
            goto cleanup;
        }
    }
    if (replicate) moduleReplicateMultiIfNeeded(ctx);
    int call_flags = CMD_CALL_SLOWLOG | CMD_CALL_STATS;
    if (replicate) {
        if (!(flags & REDISMODULE_ARGV_NO_AOF))
            call_flags |= CMD_CALL_PROPAGATE_AOF;
        if (!(flags & REDISMODULE_ARGV_NO_REPLICAS))
            call_flags |= CMD_CALL_PROPAGATE_REPL;
    }
    call(c,call_flags);
    sds proto = sdsnewlen(c->buf,c->bufpos);
    c->bufpos = 0;
    while(listLength(c->reply)) {
        clientReplyBlock *o = listNodeValue(listFirst(c->reply));
        proto = sdscatlen(proto,o->buf,o->used);
        listDelNode(c->reply,listFirst(c->reply));
    }
    reply = moduleCreateCallReplyFromProto(ctx,proto);
    autoMemoryAdd(ctx,REDISMODULE_AM_REPLY,reply);
cleanup:
    if (ctx->module) ctx->module->in_call--;
    freeClient(c);
    return reply;
}
const char *RM_CallReplyProto(RedisModuleCallReply *reply, size_t *len) {
    if (reply->proto) *len = sdslen(reply->proto);
    return reply->proto;
}
const char *ModuleTypeNameCharSet =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789-_";
uint64_t moduleTypeEncodeId(const char *name, int encver) {
    const char *cset = ModuleTypeNameCharSet;
    if (strlen(name) != 9) return 0;
    if (encver < 0 || encver > 1023) return 0;
    uint64_t id = 0;
    for (int j = 0; j < 9; j++) {
        char *p = strchr(cset,name[j]);
        if (!p) return 0;
        unsigned long pos = p-cset;
        id = (id << 6) | pos;
    }
    id = (id << 10) | encver;
    return id;
}
moduleType *moduleTypeLookupModuleByName(const char *name) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;
        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *mt = ln->value;
            if (memcmp(name,mt->name,sizeof(mt->name)) == 0) {
                dictReleaseIterator(di);
                return mt;
            }
        }
    }
    dictReleaseIterator(di);
    return NULL;
}
#define MODULE_LOOKUP_CACHE_SIZE 3
moduleType *moduleTypeLookupModuleByID(uint64_t id) {
    static struct {
        uint64_t id;
        moduleType *mt;
    } cache[MODULE_LOOKUP_CACHE_SIZE];
    int j;
    for (j = 0; j < MODULE_LOOKUP_CACHE_SIZE && cache[j].mt != NULL; j++)
        if (cache[j].id == id) return cache[j].mt;
    moduleType *mt = NULL;
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL && mt == NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;
        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *this_mt = ln->value;
            if (this_mt->id >> 10 == id >> 10) {
                mt = this_mt;
                break;
            }
        }
    }
    dictReleaseIterator(di);
    if (mt && j < MODULE_LOOKUP_CACHE_SIZE) {
        cache[j].id = id;
        cache[j].mt = mt;
    }
    return mt;
}
void moduleTypeNameByID(char *name, uint64_t moduleid) {
    const char *cset = ModuleTypeNameCharSet;
    name[9] = '\0';
    char *p = name+8;
    moduleid >>= 10;
    for (int j = 0; j < 9; j++) {
        *p-- = cset[moduleid & 63];
        moduleid >>= 6;
    }
}
moduleType *RM_CreateDataType(RedisModuleCtx *ctx, const char *name, int encver, void *typemethods_ptr) {
    uint64_t id = moduleTypeEncodeId(name,encver);
    if (id == 0) return NULL;
    if (moduleTypeLookupModuleByName(name) != NULL) return NULL;
    long typemethods_version = ((long*)typemethods_ptr)[0];
    if (typemethods_version == 0) return NULL;
    struct typemethods {
        uint64_t version;
        moduleTypeLoadFunc rdb_load;
        moduleTypeSaveFunc rdb_save;
        moduleTypeRewriteFunc aof_rewrite;
        moduleTypeMemUsageFunc mem_usage;
        moduleTypeDigestFunc digest;
        moduleTypeFreeFunc free;
        struct {
            moduleTypeAuxLoadFunc aux_load;
            moduleTypeAuxSaveFunc aux_save;
            int aux_save_triggers;
        } v2;
    } *tms = (struct typemethods*) typemethods_ptr;
    moduleType *mt = zcalloc(sizeof(*mt));
    mt->id = id;
    mt->module = ctx->module;
    mt->rdb_load = tms->rdb_load;
    mt->rdb_save = tms->rdb_save;
    mt->aof_rewrite = tms->aof_rewrite;
    mt->mem_usage = tms->mem_usage;
    mt->digest = tms->digest;
    mt->free = tms->free;
    if (tms->version >= 2) {
        mt->aux_load = tms->v2.aux_load;
        mt->aux_save = tms->v2.aux_save;
        mt->aux_save_triggers = tms->v2.aux_save_triggers;
    }
    memcpy(mt->name,name,sizeof(mt->name));
    listAddNodeTail(ctx->module->types,mt);
    return mt;
}
int RM_ModuleTypeSetValue(RedisModuleKey *key, moduleType *mt, void *value) {
    if (!(key->mode & REDISMODULE_WRITE) || key->iter) return REDISMODULE_ERR;
    RM_DeleteKey(key);
    robj *o = createModuleObject(mt,value);
    setKey(key->db,key->key,o);
    decrRefCount(o);
    key->value = o;
    return REDISMODULE_OK;
}
moduleType *RM_ModuleTypeGetType(RedisModuleKey *key) {
    if (key == NULL ||
        key->value == NULL ||
        RM_KeyType(key) != REDISMODULE_KEYTYPE_MODULE) return NULL;
    moduleValue *mv = key->value->ptr;
    return mv->type;
}
void *RM_ModuleTypeGetValue(RedisModuleKey *key) {
    if (key == NULL ||
        key->value == NULL ||
        RM_KeyType(key) != REDISMODULE_KEYTYPE_MODULE) return NULL;
    moduleValue *mv = key->value->ptr;
    return mv->value;
}
void moduleRDBLoadError(RedisModuleIO *io) {
    if (io->type->module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS) {
        io->error = 1;
        return;
    }
    serverLog(LL_WARNING,
        "Error loading data from RDB (short read or EOF). "
        "Read performed by module '%s' about type '%s' "
        "after reading '%llu' bytes of a value.",
        io->type->module->name,
        io->type->name,
        (unsigned long long)io->bytes);
    exit(1);
}
int moduleAllDatatypesHandleErrors() {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (listLength(module->types) &&
            !(module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS))
        {
            dictReleaseIterator(di);
            return 0;
        }
    }
    dictReleaseIterator(di);
    return 1;
}
int RM_IsIOError(RedisModuleIO *io) {
    return io->error;
}
void RM_SaveUnsigned(RedisModuleIO *io, uint64_t value) {
    if (io->error) return;
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_UINT);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    retval = rdbSaveLen(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;
saveerr:
    io->error = 1;
}
uint64_t RM_LoadUnsigned(RedisModuleIO *io) {
    if (io->error) return 0;
    if (io->ver == 2) {
        uint64_t opcode = rdbLoadLen(io->rio,NULL);
        if (opcode != RDB_MODULE_OPCODE_UINT) goto loaderr;
    }
    uint64_t value;
    int retval = rdbLoadLenByRef(io->rio, NULL, &value);
    if (retval == -1) goto loaderr;
    return value;
loaderr:
    moduleRDBLoadError(io);
    return 0;
}
void RM_SaveSigned(RedisModuleIO *io, int64_t value) {
    union {uint64_t u; int64_t i;} conv;
    conv.i = value;
    RM_SaveUnsigned(io,conv.u);
}
int64_t RM_LoadSigned(RedisModuleIO *io) {
    union {uint64_t u; int64_t i;} conv;
    conv.u = RM_LoadUnsigned(io);
    return conv.i;
}
void RM_SaveString(RedisModuleIO *io, RedisModuleString *s) {
    if (io->error) return;
    ssize_t retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_STRING);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    retval = rdbSaveStringObject(io->rio, s);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;
saveerr:
    io->error = 1;
}
void RM_SaveStringBuffer(RedisModuleIO *io, const char *str, size_t len) {
    if (io->error) return;
    ssize_t retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_STRING);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    retval = rdbSaveRawString(io->rio, (unsigned char*)str,len);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;
saveerr:
    io->error = 1;
}
void *moduleLoadString(RedisModuleIO *io, int plain, size_t *lenptr) {
    if (io->error) return NULL;
    if (io->ver == 2) {
        uint64_t opcode = rdbLoadLen(io->rio,NULL);
        if (opcode != RDB_MODULE_OPCODE_STRING) goto loaderr;
    }
    void *s = rdbGenericLoadStringObject(io->rio,
              plain ? RDB_LOAD_PLAIN : RDB_LOAD_NONE, lenptr);
    if (s == NULL) goto loaderr;
    return s;
loaderr:
    moduleRDBLoadError(io);
    return NULL;
}
RedisModuleString *RM_LoadString(RedisModuleIO *io) {
    return moduleLoadString(io,0,NULL);
}
char *RM_LoadStringBuffer(RedisModuleIO *io, size_t *lenptr) {
    return moduleLoadString(io,1,lenptr);
}
void RM_SaveDouble(RedisModuleIO *io, double value) {
    if (io->error) return;
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_DOUBLE);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    retval = rdbSaveBinaryDoubleValue(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;
saveerr:
    io->error = 1;
}
double RM_LoadDouble(RedisModuleIO *io) {
    if (io->error) return 0;
    if (io->ver == 2) {
        uint64_t opcode = rdbLoadLen(io->rio,NULL);
        if (opcode != RDB_MODULE_OPCODE_DOUBLE) goto loaderr;
    }
    double value;
    int retval = rdbLoadBinaryDoubleValue(io->rio, &value);
    if (retval == -1) goto loaderr;
    return value;
loaderr:
    moduleRDBLoadError(io);
    return 0;
}
void RM_SaveFloat(RedisModuleIO *io, float value) {
    if (io->error) return;
    int retval = rdbSaveLen(io->rio, RDB_MODULE_OPCODE_FLOAT);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    retval = rdbSaveBinaryFloatValue(io->rio, value);
    if (retval == -1) goto saveerr;
    io->bytes += retval;
    return;
saveerr:
    io->error = 1;
}
float RM_LoadFloat(RedisModuleIO *io) {
    if (io->error) return 0;
    if (io->ver == 2) {
        uint64_t opcode = rdbLoadLen(io->rio,NULL);
        if (opcode != RDB_MODULE_OPCODE_FLOAT) goto loaderr;
    }
    float value;
    int retval = rdbLoadBinaryFloatValue(io->rio, &value);
    if (retval == -1) goto loaderr;
    return value;
loaderr:
    moduleRDBLoadError(io);
    return 0;
}
ssize_t rdbSaveModulesAux(rio *rdb, int when) {
    size_t total_written = 0;
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        listIter li;
        listNode *ln;
        listRewind(module->types,&li);
        while((ln = listNext(&li))) {
            moduleType *mt = ln->value;
            if (!mt->aux_save || !(mt->aux_save_triggers & when))
                continue;
            ssize_t ret = rdbSaveSingleModuleAux(rdb, when, mt);
            if (ret==-1) {
                dictReleaseIterator(di);
                return -1;
            }
            total_written += ret;
        }
    }
    dictReleaseIterator(di);
    return total_written;
}
void RM_DigestAddStringBuffer(RedisModuleDigest *md, unsigned char *ele, size_t len) {
    mixDigest(md->o,ele,len);
}
void RM_DigestAddLongLong(RedisModuleDigest *md, long long ll) {
    char buf[LONG_STR_SIZE];
    size_t len = ll2string(buf,sizeof(buf),ll);
    mixDigest(md->o,buf,len);
}
void RM_DigestEndSequence(RedisModuleDigest *md) {
    xorDigest(md->x,md->o,sizeof(md->o));
    memset(md->o,0,sizeof(md->o));
}
void RM_EmitAOF(RedisModuleIO *io, const char *cmdname, const char *fmt, ...) {
    if (io->error) return;
    struct redisCommand *cmd;
    robj **argv = NULL;
    int argc = 0, flags = 0, j;
    va_list ap;
    cmd = lookupCommandByCString((char*)cmdname);
    if (!cmd) {
        serverLog(LL_WARNING,
            "Fatal: AOF method for module data type '%s' tried to "
            "emit unknown command '%s'",
            io->type->name, cmdname);
        io->error = 1;
        errno = EINVAL;
        return;
    }
    va_start(ap, fmt);
    argv = moduleCreateArgvFromUserFormat(cmdname,fmt,&argc,&flags,ap);
    va_end(ap);
    if (argv == NULL) {
        serverLog(LL_WARNING,
            "Fatal: AOF method for module data type '%s' tried to "
            "call RedisModule_EmitAOF() with wrong format specifiers '%s'",
            io->type->name, fmt);
        io->error = 1;
        errno = EINVAL;
        return;
    }
    if (!io->error && rioWriteBulkCount(io->rio,'*',argc) == 0)
        io->error = 1;
    for (j = 0; j < argc; j++) {
        if (!io->error && rioWriteBulkObject(io->rio,argv[j]) == 0)
            io->error = 1;
        decrRefCount(argv[j]);
    }
    zfree(argv);
    return;
}
RedisModuleCtx *RM_GetContextFromIO(RedisModuleIO *io) {
    if (io->ctx) return io->ctx;
    RedisModuleCtx ctxtemplate = REDISMODULE_CTX_INIT;
    io->ctx = zmalloc(sizeof(RedisModuleCtx));
    *(io->ctx) = ctxtemplate;
    io->ctx->module = io->type->module;
    io->ctx->client = NULL;
    return io->ctx;
}
const RedisModuleString *RM_GetKeyNameFromIO(RedisModuleIO *io) {
    return io->key;
}
const RedisModuleString *RM_GetKeyNameFromModuleKey(RedisModuleKey *key) {
    return key ? key->key : NULL;
}
void RM_LogRaw(RedisModule *module, const char *levelstr, const char *fmt, va_list ap) {
    char msg[LOG_MAX_LEN];
    size_t name_len;
    int level;
    if (!strcasecmp(levelstr,"debug")) level = LL_DEBUG;
    else if (!strcasecmp(levelstr,"verbose")) level = LL_VERBOSE;
    else if (!strcasecmp(levelstr,"notice")) level = LL_NOTICE;
    else if (!strcasecmp(levelstr,"warning")) level = LL_WARNING;
    else level = LL_VERBOSE;
    if (level < server.verbosity) return;
    name_len = snprintf(msg, sizeof(msg),"<%s> ", module? module->name: "module");
    vsnprintf(msg + name_len, sizeof(msg) - name_len, fmt, ap);
    serverLogRaw(level,msg);
}
void RM_Log(RedisModuleCtx *ctx, const char *levelstr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    RM_LogRaw(ctx? ctx->module: NULL,levelstr,fmt,ap);
    va_end(ap);
}
void RM_LogIOError(RedisModuleIO *io, const char *levelstr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    RM_LogRaw(io->type->module,levelstr,fmt,ap);
    va_end(ap);
}
void RM__Assert(const char *estr, const char *file, int line) {
    _serverAssert(estr, file, line);
}
void RM_LatencyAddSample(const char *event, mstime_t latency) {
    if (latency >= server.latency_monitor_threshold)
        latencyAddSample(event, latency);
}
void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    UNUSED(privdata);
}
void unblockClientFromModule(client *c) {
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    if (bc->disconnect_callback) {
        RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
        ctx.blocked_privdata = bc->privdata;
        ctx.module = bc->module;
        ctx.client = bc->client;
        bc->disconnect_callback(&ctx,bc);
        moduleFreeContext(&ctx);
    }
    bc->client = NULL;
    resetClient(c);
}
RedisModuleBlockedClient *moduleBlockClient(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata) {
    client *c = ctx->client;
    int islua = c->flags & CLIENT_LUA;
    int ismulti = c->flags & CLIENT_MULTI;
    c->bpop.module_blocked_handle = zmalloc(sizeof(RedisModuleBlockedClient));
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    ctx->module->blocked_clients++;
    mstime_t timeout = timeout_ms ? (mstime()+timeout_ms) : 0;
    bc->client = (islua || ismulti) ? NULL : c;
    bc->module = ctx->module;
    bc->reply_callback = reply_callback;
    bc->timeout_callback = timeout_callback;
    bc->disconnect_callback = NULL;
    bc->free_privdata = free_privdata;
    bc->privdata = privdata;
    bc->reply_client = createClient(NULL);
    bc->reply_client->flags |= CLIENT_MODULE;
    bc->dbid = c->db->id;
    bc->blocked_on_keys = keys != NULL;
    bc->unblocked = 0;
    c->bpop.timeout = timeout;
    if (islua || ismulti) {
        c->bpop.module_blocked_handle = NULL;
        addReplyError(c, islua ?
            "Blocking module command called from Lua script" :
            "Blocking module command called from transaction");
    } else {
        if (keys) {
            blockForKeys(c,BLOCKED_MODULE,keys,numkeys,timeout,NULL,NULL);
        } else {
            blockClient(c,BLOCKED_MODULE);
        }
    }
    return bc;
}
int moduleTryServeClientBlockedOnKey(client *c, robj *key) {
    int served = 0;
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    if (bc->unblocked) return REDISMODULE_ERR;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
    ctx.flags |= REDISMODULE_CTX_BLOCKED_REPLY;
    ctx.blocked_ready_key = key;
    ctx.blocked_privdata = bc->privdata;
    ctx.module = bc->module;
    ctx.client = bc->client;
    ctx.blocked_client = bc;
    if (bc->reply_callback(&ctx,(void**)c->argv,c->argc) == REDISMODULE_OK)
        served = 1;
    moduleFreeContext(&ctx);
    return served;
}
RedisModuleBlockedClient *RM_BlockClient(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms) {
    return moduleBlockClient(ctx,reply_callback,timeout_callback,free_privdata,timeout_ms, NULL,0,NULL);
}
RedisModuleBlockedClient *RM_BlockClientOnKeys(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata) {
    return moduleBlockClient(ctx,reply_callback,timeout_callback,free_privdata,timeout_ms, keys,numkeys,privdata);
}
void RM_SignalKeyAsReady(RedisModuleCtx *ctx, RedisModuleString *key) {
    signalKeyAsReady(ctx->client->db, key);
}
int moduleUnblockClientByHandle(RedisModuleBlockedClient *bc, void *privdata) {
    pthread_mutex_lock(&moduleUnblockedClientsMutex);
    if (!bc->blocked_on_keys) bc->privdata = privdata;
    bc->unblocked = 1;
    listAddNodeTail(moduleUnblockedClients,bc);
    if (write(server.module_blocked_pipe[1],"A",1) != 1) {
    }
    pthread_mutex_unlock(&moduleUnblockedClientsMutex);
    return REDISMODULE_OK;
}
void moduleUnblockClient(client *c) {
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    moduleUnblockClientByHandle(bc,NULL);
}
int moduleClientIsBlockedOnKeys(client *c) {
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    return bc->blocked_on_keys;
}
int RM_UnblockClient(RedisModuleBlockedClient *bc, void *privdata) {
    if (bc->blocked_on_keys) {
        if (bc->timeout_callback == NULL) return REDISMODULE_ERR;
        if (bc->unblocked) return REDISMODULE_OK;
        if (bc->client) moduleBlockedClientTimedOut(bc->client);
    }
    moduleUnblockClientByHandle(bc,privdata);
    return REDISMODULE_OK;
}
int RM_AbortBlock(RedisModuleBlockedClient *bc) {
    bc->reply_callback = NULL;
    bc->disconnect_callback = NULL;
    return RM_UnblockClient(bc,NULL);
}
void RM_SetDisconnectCallback(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback) {
    bc->disconnect_callback = callback;
}
void moduleHandleBlockedClients(void) {
    listNode *ln;
    RedisModuleBlockedClient *bc;
    pthread_mutex_lock(&moduleUnblockedClientsMutex);
    char buf[1];
    while (read(server.module_blocked_pipe[0],buf,1) == 1);
    while (listLength(moduleUnblockedClients)) {
        ln = listFirst(moduleUnblockedClients);
        bc = ln->value;
        client *c = bc->client;
        listDelNode(moduleUnblockedClients,ln);
        pthread_mutex_unlock(&moduleUnblockedClientsMutex);
        if (c && !bc->blocked_on_keys && bc->reply_callback) {
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            ctx.flags |= REDISMODULE_CTX_BLOCKED_REPLY;
            ctx.blocked_privdata = bc->privdata;
            ctx.blocked_ready_key = NULL;
            ctx.module = bc->module;
            ctx.client = bc->client;
            ctx.blocked_client = bc;
            bc->reply_callback(&ctx,(void**)c->argv,c->argc);
            moduleFreeContext(&ctx);
        }
        if (bc->privdata && bc->free_privdata) {
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            if (c == NULL)
                ctx.flags |= REDISMODULE_CTX_BLOCKED_DISCONNECTED;
            ctx.blocked_privdata = bc->privdata;
            ctx.module = bc->module;
            ctx.client = bc->client;
            bc->free_privdata(&ctx,bc->privdata);
            moduleFreeContext(&ctx);
        }
        if (c) AddReplyFromClient(c, bc->reply_client);
        freeClient(bc->reply_client);
        if (c != NULL) {
            bc->disconnect_callback = NULL;
            unblockClient(c);
            if (clientHasPendingReplies(c) &&
                !(c->flags & CLIENT_PENDING_WRITE))
            {
                c->flags |= CLIENT_PENDING_WRITE;
                listAddNodeHead(server.clients_pending_write,c);
            }
        }
        bc->module->blocked_clients--;
        zfree(bc);
        pthread_mutex_lock(&moduleUnblockedClientsMutex);
    }
    pthread_mutex_unlock(&moduleUnblockedClientsMutex);
}
void moduleBlockedClientTimedOut(client *c) {
    RedisModuleBlockedClient *bc = c->bpop.module_blocked_handle;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
    ctx.flags |= REDISMODULE_CTX_BLOCKED_TIMEOUT;
    ctx.module = bc->module;
    ctx.client = bc->client;
    ctx.blocked_client = bc;
    bc->timeout_callback(&ctx,(void**)c->argv,c->argc);
    moduleFreeContext(&ctx);
    bc->disconnect_callback = NULL;
}
int RM_IsBlockedReplyRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_REPLY) != 0;
}
int RM_IsBlockedTimeoutRequest(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_TIMEOUT) != 0;
}
void *RM_GetBlockedClientPrivateData(RedisModuleCtx *ctx) {
    return ctx->blocked_privdata;
}
RedisModuleString *RM_GetBlockedClientReadyKey(RedisModuleCtx *ctx) {
    return ctx->blocked_ready_key;
}
RedisModuleBlockedClient *RM_GetBlockedClientHandle(RedisModuleCtx *ctx) {
    return ctx->blocked_client;
}
int RM_BlockedClientDisconnected(RedisModuleCtx *ctx) {
    return (ctx->flags & REDISMODULE_CTX_BLOCKED_DISCONNECTED) != 0;
}
RedisModuleCtx *RM_GetThreadSafeContext(RedisModuleBlockedClient *bc) {
    RedisModuleCtx *ctx = zmalloc(sizeof(*ctx));
    RedisModuleCtx empty = REDISMODULE_CTX_INIT;
    memcpy(ctx,&empty,sizeof(empty));
    if (bc) {
        ctx->blocked_client = bc;
        ctx->module = bc->module;
    }
    ctx->flags |= REDISMODULE_CTX_THREAD_SAFE;
    ctx->client = createClient(NULL);
    if (bc) {
        selectDb(ctx->client,bc->dbid);
        if (bc->client) ctx->client->id = bc->client->id;
    }
    return ctx;
}
void RM_FreeThreadSafeContext(RedisModuleCtx *ctx) {
    moduleFreeContext(ctx);
    zfree(ctx);
}
void RM_ThreadSafeContextLock(RedisModuleCtx *ctx) {
    UNUSED(ctx);
    moduleAcquireGIL();
}
void RM_ThreadSafeContextUnlock(RedisModuleCtx *ctx) {
    UNUSED(ctx);
    moduleReleaseGIL();
}
void moduleAcquireGIL(void) {
    pthread_mutex_lock(&moduleGIL);
}
void moduleReleaseGIL(void) {
    pthread_mutex_unlock(&moduleGIL);
}
int RM_SubscribeToKeyspaceEvents(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc callback) {
    RedisModuleKeyspaceSubscriber *sub = zmalloc(sizeof(*sub));
    sub->module = ctx->module;
    sub->event_mask = types;
    sub->notify_callback = callback;
    sub->active = 0;
    listAddNodeTail(moduleKeyspaceSubscribers, sub);
    return REDISMODULE_OK;
}
int RM_GetNotifyKeyspaceEvents() {
    return server.notify_keyspace_events;
}
int RM_NotifyKeyspaceEvent(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    if (!ctx || !ctx->client)
        return REDISMODULE_ERR;
    notifyKeyspaceEvent(type, (char *)event, key, ctx->client->db->id);
    return REDISMODULE_OK;
}
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid) {
    if (listLength(moduleKeyspaceSubscribers) == 0) return;
    listIter li;
    listNode *ln;
    listRewind(moduleKeyspaceSubscribers,&li);
    type &= ~(NOTIFY_KEYEVENT | NOTIFY_KEYSPACE);
    while((ln = listNext(&li))) {
        RedisModuleKeyspaceSubscriber *sub = ln->value;
        if ((sub->event_mask & type) && sub->active == 0) {
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            ctx.module = sub->module;
            ctx.client = moduleFreeContextReusedClient;
            selectDb(ctx.client, dbid);
            sub->active = 1;
            sub->notify_callback(&ctx, type, event, key);
            sub->active = 0;
            moduleFreeContext(&ctx);
        }
    }
}
void moduleUnsubscribeNotifications(RedisModule *module) {
    listIter li;
    listNode *ln;
    listRewind(moduleKeyspaceSubscribers,&li);
    while((ln = listNext(&li))) {
        RedisModuleKeyspaceSubscriber *sub = ln->value;
        if (sub->module == module) {
            listDelNode(moduleKeyspaceSubscribers, ln);
            zfree(sub);
        }
    }
}
typedef void (*RedisModuleClusterMessageReceiver)(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len);
typedef struct moduleClusterReceiver {
    uint64_t module_id;
    RedisModuleClusterMessageReceiver callback;
    struct RedisModule *module;
    struct moduleClusterReceiver *next;
} moduleClusterReceiver;
typedef struct moduleClusterNodeInfo {
    int flags;
    char ip[NET_IP_STR_LEN];
    int port;
    char master_id[40];
} mdouleClusterNodeInfo;
static moduleClusterReceiver *clusterReceivers[UINT8_MAX];
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    moduleClusterReceiver *r = clusterReceivers[type];
    while(r) {
        if (r->module_id == module_id) {
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            ctx.module = r->module;
            ctx.client = moduleFreeContextReusedClient;
            selectDb(ctx.client, 0);
            r->callback(&ctx,sender_id,type,payload,len);
            moduleFreeContext(&ctx);
            return;
        }
        r = r->next;
    }
}
void RM_RegisterClusterMessageReceiver(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback) {
    if (!server.cluster_enabled) return;
    uint64_t module_id = moduleTypeEncodeId(ctx->module->name,0);
    moduleClusterReceiver *r = clusterReceivers[type], *prev = NULL;
    while(r) {
        if (r->module_id == module_id) {
            if (callback) {
                r->callback = callback;
            } else {
                if (prev)
                    prev->next = r->next;
                else
                    clusterReceivers[type]->next = r->next;
                zfree(r);
            }
            return;
        }
        prev = r;
        r = r->next;
    }
    if (callback) {
        r = zmalloc(sizeof(*r));
        r->module_id = module_id;
        r->module = ctx->module;
        r->callback = callback;
        r->next = clusterReceivers[type];
        clusterReceivers[type] = r;
    }
}
int RM_SendClusterMessage(RedisModuleCtx *ctx, char *target_id, uint8_t type, unsigned char *msg, uint32_t len) {
    if (!server.cluster_enabled) return REDISMODULE_ERR;
    uint64_t module_id = moduleTypeEncodeId(ctx->module->name,0);
    if (clusterSendModuleMessageToTarget(target_id,module_id,type,msg,len) == C_OK)
        return REDISMODULE_OK;
    else
        return REDISMODULE_ERR;
}
char **RM_GetClusterNodesList(RedisModuleCtx *ctx, size_t *numnodes) {
    UNUSED(ctx);
    if (!server.cluster_enabled) return NULL;
    size_t count = dictSize(server.cluster->nodes);
    char **ids = zmalloc((count+1)*REDISMODULE_NODE_ID_LEN);
    dictIterator *di = dictGetIterator(server.cluster->nodes);
    dictEntry *de;
    int j = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->flags & (CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE)) continue;
        ids[j] = zmalloc(REDISMODULE_NODE_ID_LEN);
        memcpy(ids[j],node->name,REDISMODULE_NODE_ID_LEN);
        j++;
    }
    *numnodes = j;
    ids[j] = NULL;
    dictReleaseIterator(di);
    return ids;
}
void RM_FreeClusterNodesList(char **ids) {
    if (ids == NULL) return;
    for (int j = 0; ids[j]; j++) zfree(ids[j]);
    zfree(ids);
}
const char *RM_GetMyClusterID(void) {
    if (!server.cluster_enabled) return NULL;
    return server.cluster->myself->name;
}
size_t RM_GetClusterSize(void) {
    if (!server.cluster_enabled) return 0;
    return dictSize(server.cluster->nodes);
}
clusterNode *clusterLookupNode(const char *name);
int RM_GetClusterNodeInfo(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags) {
    UNUSED(ctx);
    clusterNode *node = clusterLookupNode(id);
    if (node->flags & (CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
        return REDISMODULE_ERR;
    if (ip) memcpy(ip,node->name,REDISMODULE_NODE_ID_LEN);
    if (master_id) {
        if (node->flags & CLUSTER_NODE_MASTER && node->slaveof)
            memcpy(master_id,node->slaveof->name,REDISMODULE_NODE_ID_LEN);
        else
            memset(master_id,0,REDISMODULE_NODE_ID_LEN);
    }
    if (port) *port = node->port;
    if (flags) {
        *flags = 0;
        if (node->flags & CLUSTER_NODE_MYSELF) *flags |= REDISMODULE_NODE_MYSELF;
        if (node->flags & CLUSTER_NODE_MASTER) *flags |= REDISMODULE_NODE_MASTER;
        if (node->flags & CLUSTER_NODE_SLAVE) *flags |= REDISMODULE_NODE_SLAVE;
        if (node->flags & CLUSTER_NODE_PFAIL) *flags |= REDISMODULE_NODE_PFAIL;
        if (node->flags & CLUSTER_NODE_FAIL) *flags |= REDISMODULE_NODE_FAIL;
        if (node->flags & CLUSTER_NODE_NOFAILOVER) *flags |= REDISMODULE_NODE_NOFAILOVER;
    }
    return REDISMODULE_OK;
}
void RM_SetClusterFlags(RedisModuleCtx *ctx, uint64_t flags) {
    UNUSED(ctx);
    if (flags & REDISMODULE_CLUSTER_FLAG_NO_FAILOVER)
        server.cluster_module_flags |= CLUSTER_MODULE_FLAG_NO_FAILOVER;
    if (flags & REDISMODULE_CLUSTER_FLAG_NO_REDIRECTION)
        server.cluster_module_flags |= CLUSTER_MODULE_FLAG_NO_REDIRECTION;
}
static rax *Timers;
long long aeTimer = -1;
typedef void (*RedisModuleTimerProc)(RedisModuleCtx *ctx, void *data);
typedef struct RedisModuleTimer {
    RedisModule *module;
    RedisModuleTimerProc callback;
    void *data;
    int dbid;
} RedisModuleTimer;
int moduleTimerHandler(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    raxIterator ri;
    raxStart(&ri,Timers);
    uint64_t now = ustime();
    long long next_period = 0;
    while(1) {
        raxSeek(&ri,"^",NULL,0);
        if (!raxNext(&ri)) break;
        uint64_t expiretime;
        memcpy(&expiretime,ri.key,sizeof(expiretime));
        expiretime = ntohu64(expiretime);
        if (now >= expiretime) {
            RedisModuleTimer *timer = ri.data;
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            ctx.module = timer->module;
            ctx.client = moduleFreeContextReusedClient;
            selectDb(ctx.client, timer->dbid);
            timer->callback(&ctx,timer->data);
            moduleFreeContext(&ctx);
            raxRemove(Timers,(unsigned char*)ri.key,ri.key_len,NULL);
            zfree(timer);
        } else {
            next_period = (expiretime-now)/1000;
            break;
        }
    }
    raxStop(&ri);
    if (next_period <= 0) next_period = 1;
    return (raxSize(Timers) > 0) ? next_period : AE_NOMORE;
}
RedisModuleTimerID RM_CreateTimer(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data) {
    RedisModuleTimer *timer = zmalloc(sizeof(*timer));
    timer->module = ctx->module;
    timer->callback = callback;
    timer->data = data;
    timer->dbid = ctx->client->db->id;
    uint64_t expiretime = ustime()+period*1000;
    uint64_t key;
    while(1) {
        key = htonu64(expiretime);
        if (raxFind(Timers, (unsigned char*)&key,sizeof(key)) == raxNotFound) {
            raxInsert(Timers,(unsigned char*)&key,sizeof(key),timer,NULL);
            break;
        } else {
            expiretime++;
        }
    }
    if (aeTimer != -1) {
        raxIterator ri;
        raxStart(&ri,Timers);
        raxSeek(&ri,"^",NULL,0);
        raxNext(&ri);
        if (memcmp(ri.key,&key,sizeof(key)) == 0) {
            aeDeleteTimeEvent(server.el,aeTimer);
            aeTimer = -1;
        }
        raxStop(&ri);
    }
    if (aeTimer == -1)
        aeTimer = aeCreateTimeEvent(server.el,period,moduleTimerHandler,NULL,NULL);
    return key;
}
int RM_StopTimer(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data) {
    RedisModuleTimer *timer = raxFind(Timers,(unsigned char*)&id,sizeof(id));
    if (timer == raxNotFound || timer->module != ctx->module)
        return REDISMODULE_ERR;
    if (data) *data = timer->data;
    raxRemove(Timers,(unsigned char*)&id,sizeof(id),NULL);
    zfree(timer);
    return REDISMODULE_OK;
}
int RM_GetTimerInfo(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data) {
    RedisModuleTimer *timer = raxFind(Timers,(unsigned char*)&id,sizeof(id));
    if (timer == raxNotFound || timer->module != ctx->module)
        return REDISMODULE_ERR;
    if (remaining) {
        int64_t rem = ntohu64(id)-ustime();
        if (rem < 0) rem = 0;
        *remaining = rem/1000;
    }
    if (data) *data = timer->data;
    return REDISMODULE_OK;
}
RedisModuleDict *RM_CreateDict(RedisModuleCtx *ctx) {
    struct RedisModuleDict *d = zmalloc(sizeof(*d));
    d->rax = raxNew();
    if (ctx != NULL) autoMemoryAdd(ctx,REDISMODULE_AM_DICT,d);
    return d;
}
void RM_FreeDict(RedisModuleCtx *ctx, RedisModuleDict *d) {
    if (ctx != NULL) autoMemoryFreed(ctx,REDISMODULE_AM_DICT,d);
    raxFree(d->rax);
    zfree(d);
}
uint64_t RM_DictSize(RedisModuleDict *d) {
    return raxSize(d->rax);
}
int RM_DictSetC(RedisModuleDict *d, void *key, size_t keylen, void *ptr) {
    int retval = raxTryInsert(d->rax,key,keylen,ptr,NULL);
    return (retval == 1) ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_DictReplaceC(RedisModuleDict *d, void *key, size_t keylen, void *ptr) {
    int retval = raxInsert(d->rax,key,keylen,ptr,NULL);
    return (retval == 1) ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_DictSet(RedisModuleDict *d, RedisModuleString *key, void *ptr) {
    return RM_DictSetC(d,key->ptr,sdslen(key->ptr),ptr);
}
int RM_DictReplace(RedisModuleDict *d, RedisModuleString *key, void *ptr) {
    return RM_DictReplaceC(d,key->ptr,sdslen(key->ptr),ptr);
}
void *RM_DictGetC(RedisModuleDict *d, void *key, size_t keylen, int *nokey) {
    void *res = raxFind(d->rax,key,keylen);
    if (nokey) *nokey = (res == raxNotFound);
    return (res == raxNotFound) ? NULL : res;
}
void *RM_DictGet(RedisModuleDict *d, RedisModuleString *key, int *nokey) {
    return RM_DictGetC(d,key->ptr,sdslen(key->ptr),nokey);
}
int RM_DictDelC(RedisModuleDict *d, void *key, size_t keylen, void *oldval) {
    int retval = raxRemove(d->rax,key,keylen,oldval);
    return retval ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_DictDel(RedisModuleDict *d, RedisModuleString *key, void *oldval) {
    return RM_DictDelC(d,key->ptr,sdslen(key->ptr),oldval);
}
RedisModuleDictIter *RM_DictIteratorStartC(RedisModuleDict *d, const char *op, void *key, size_t keylen) {
    RedisModuleDictIter *di = zmalloc(sizeof(*di));
    di->dict = d;
    raxStart(&di->ri,d->rax);
    raxSeek(&di->ri,op,key,keylen);
    return di;
}
RedisModuleDictIter *RM_DictIteratorStart(RedisModuleDict *d, const char *op, RedisModuleString *key) {
    return RM_DictIteratorStartC(d,op,key->ptr,sdslen(key->ptr));
}
void RM_DictIteratorStop(RedisModuleDictIter *di) {
    raxStop(&di->ri);
    zfree(di);
}
int RM_DictIteratorReseekC(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) {
    return raxSeek(&di->ri,op,key,keylen);
}
int RM_DictIteratorReseek(RedisModuleDictIter *di, const char *op, RedisModuleString *key) {
    return RM_DictIteratorReseekC(di,op,key->ptr,sdslen(key->ptr));
}
void *RM_DictNextC(RedisModuleDictIter *di, size_t *keylen, void **dataptr) {
    if (!raxNext(&di->ri)) return NULL;
    if (keylen) *keylen = di->ri.key_len;
    if (dataptr) *dataptr = di->ri.data;
    return di->ri.key;
}
void *RM_DictPrevC(RedisModuleDictIter *di, size_t *keylen, void **dataptr) {
    if (!raxPrev(&di->ri)) return NULL;
    if (keylen) *keylen = di->ri.key_len;
    if (dataptr) *dataptr = di->ri.data;
    return di->ri.key;
}
RedisModuleString *RM_DictNext(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) {
    size_t keylen;
    void *key = RM_DictNextC(di,&keylen,dataptr);
    if (key == NULL) return NULL;
    return RM_CreateString(ctx,key,keylen);
}
RedisModuleString *RM_DictPrev(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) {
    size_t keylen;
    void *key = RM_DictPrevC(di,&keylen,dataptr);
    if (key == NULL) return NULL;
    return RM_CreateString(ctx,key,keylen);
}
int RM_DictCompareC(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) {
    if (raxEOF(&di->ri)) return REDISMODULE_ERR;
    int res = raxCompare(&di->ri,op,key,keylen);
    return res ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_DictCompare(RedisModuleDictIter *di, const char *op, RedisModuleString *key) {
    if (raxEOF(&di->ri)) return REDISMODULE_ERR;
    int res = raxCompare(&di->ri,op,key->ptr,sdslen(key->ptr));
    return res ? REDISMODULE_OK : REDISMODULE_ERR;
}
int RM_InfoEndDictField(RedisModuleInfoCtx *ctx);
int RM_InfoAddSection(RedisModuleInfoCtx *ctx, char *name) {
    sds full_name = sdsdup(ctx->module->name);
    if (name != NULL && strlen(name) > 0)
        full_name = sdscatfmt(full_name, "_%s", name);
    if (ctx->in_dict_field)
        RM_InfoEndDictField(ctx);
    if (ctx->requested_section) {
        if (strcasecmp(ctx->requested_section, full_name) &&
            strcasecmp(ctx->requested_section, ctx->module->name)) {
            sdsfree(full_name);
            ctx->in_section = 0;
            return REDISMODULE_ERR;
        }
    }
    if (ctx->sections++) ctx->info = sdscat(ctx->info,"\r\n");
    ctx->info = sdscatfmt(ctx->info, "# %S\r\n", full_name);
    ctx->in_section = 1;
    sdsfree(full_name);
    return REDISMODULE_OK;
}
int RM_InfoBeginDictField(RedisModuleInfoCtx *ctx, char *name) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field)
        RM_InfoEndDictField(ctx);
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:",
        ctx->module->name,
        name);
    ctx->in_dict_field = 1;
    return REDISMODULE_OK;
}
int RM_InfoEndDictField(RedisModuleInfoCtx *ctx) {
    if (!ctx->in_dict_field)
        return REDISMODULE_ERR;
    if (ctx->info[sdslen(ctx->info)-1]==',')
        sdsIncrLen(ctx->info, -1);
    ctx->info = sdscat(ctx->info, "\r\n");
    ctx->in_dict_field = 0;
    return REDISMODULE_OK;
}
int RM_InfoAddFieldString(RedisModuleInfoCtx *ctx, char *field, RedisModuleString *value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%S,",
            field,
            (sds)value->ptr);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%S\r\n",
        ctx->module->name,
        field,
        (sds)value->ptr);
    return REDISMODULE_OK;
}
int RM_InfoAddFieldCString(RedisModuleInfoCtx *ctx, char *field, char *value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%s,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%s\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}
int RM_InfoAddFieldDouble(RedisModuleInfoCtx *ctx, char *field, double value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatprintf(ctx->info,
            "%s=%.17g,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatprintf(ctx->info,
        "%s_%s:%.17g\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}
int RM_InfoAddFieldLongLong(RedisModuleInfoCtx *ctx, char *field, long long value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%I,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%I\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}
int RM_InfoAddFieldULongLong(RedisModuleInfoCtx *ctx, char *field, unsigned long long value) {
    if (!ctx->in_section)
        return REDISMODULE_ERR;
    if (ctx->in_dict_field) {
        ctx->info = sdscatfmt(ctx->info,
            "%s=%U,",
            field,
            value);
        return REDISMODULE_OK;
    }
    ctx->info = sdscatfmt(ctx->info,
        "%s_%s:%U\r\n",
        ctx->module->name,
        field,
        value);
    return REDISMODULE_OK;
}
int RM_RegisterInfoFunc(RedisModuleCtx *ctx, RedisModuleInfoFunc cb) {
    ctx->module->info_cb = cb;
    return REDISMODULE_OK;
}
sds modulesCollectInfo(sds info, sds section, int for_crash_report, int sections) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct RedisModule *module = dictGetVal(de);
        if (!module->info_cb)
            continue;
        RedisModuleInfoCtx info_ctx = {module, section, info, sections, 0};
        module->info_cb(&info_ctx, for_crash_report);
        if (info_ctx.in_dict_field)
            RM_InfoEndDictField(&info_ctx);
        info = info_ctx.info;
        sections = info_ctx.sections;
    }
    dictReleaseIterator(di);
    return info;
}
void RM_GetRandomBytes(unsigned char *dst, size_t len) {
    getRandomBytes(dst,len);
}
void RM_GetRandomHexChars(char *dst, size_t len) {
    getRandomHexChars(dst,len);
}
int RM_ExportSharedAPI(RedisModuleCtx *ctx, const char *apiname, void *func) {
    RedisModuleSharedAPI *sapi = zmalloc(sizeof(*sapi));
    sapi->module = ctx->module;
    sapi->func = func;
    if (dictAdd(server.sharedapi, (char*)apiname, sapi) != DICT_OK) {
        zfree(sapi);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
void *RM_GetSharedAPI(RedisModuleCtx *ctx, const char *apiname) {
    dictEntry *de = dictFind(server.sharedapi, apiname);
    if (de == NULL) return NULL;
    RedisModuleSharedAPI *sapi = dictGetVal(de);
    if (listSearchKey(sapi->module->usedby,ctx->module) == NULL) {
        listAddNodeTail(sapi->module->usedby,ctx->module);
        listAddNodeTail(ctx->module->using,sapi->module);
    }
    return sapi->func;
}
int moduleUnregisterSharedAPI(RedisModule *module) {
    int count = 0;
    dictIterator *di = dictGetSafeIterator(server.sharedapi);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        const char *apiname = dictGetKey(de);
        RedisModuleSharedAPI *sapi = dictGetVal(de);
        if (sapi->module == module) {
            dictDelete(server.sharedapi,apiname);
            zfree(sapi);
            count++;
        }
    }
    dictReleaseIterator(di);
    return count;
}
int moduleUnregisterUsedAPI(RedisModule *module) {
    listIter li;
    listNode *ln;
    int count = 0;
    listRewind(module->using,&li);
    while((ln = listNext(&li))) {
        RedisModule *used = ln->value;
        listNode *ln = listSearchKey(used->usedby,module);
        if (ln) {
            listDelNode(module->using,ln);
            count++;
        }
    }
    return count;
}
int moduleUnregisterFilters(RedisModule *module) {
    listIter li;
    listNode *ln;
    int count = 0;
    listRewind(module->filters,&li);
    while((ln = listNext(&li))) {
        RedisModuleCommandFilter *filter = ln->value;
        listNode *ln = listSearchKey(moduleCommandFilters,filter);
        if (ln) {
            listDelNode(moduleCommandFilters,ln);
            count++;
        }
        zfree(filter);
    }
    return count;
}
RedisModuleCommandFilter *RM_RegisterCommandFilter(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc callback, int flags) {
    RedisModuleCommandFilter *filter = zmalloc(sizeof(*filter));
    filter->module = ctx->module;
    filter->callback = callback;
    filter->flags = flags;
    listAddNodeTail(moduleCommandFilters, filter);
    listAddNodeTail(ctx->module->filters, filter);
    return filter;
}
int RM_UnregisterCommandFilter(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter) {
    listNode *ln;
    if (filter->module != ctx->module) return REDISMODULE_ERR;
    ln = listSearchKey(moduleCommandFilters,filter);
    if (!ln) return REDISMODULE_ERR;
    listDelNode(moduleCommandFilters,ln);
    ln = listSearchKey(ctx->module->filters,filter);
    if (!ln) return REDISMODULE_ERR;
    listDelNode(ctx->module->filters,ln);
    zfree(filter);
    return REDISMODULE_OK;
}
void moduleCallCommandFilters(client *c) {
    if (listLength(moduleCommandFilters) == 0) return;
    listIter li;
    listNode *ln;
    listRewind(moduleCommandFilters,&li);
    RedisModuleCommandFilterCtx filter = {
        .argv = c->argv,
        .argc = c->argc
    };
    while((ln = listNext(&li))) {
        RedisModuleCommandFilter *f = ln->value;
        if ((f->flags & REDISMODULE_CMDFILTER_NOSELF) && f->module->in_call) continue;
        f->callback(&filter);
    }
    c->argv = filter.argv;
    c->argc = filter.argc;
}
int RM_CommandFilterArgsCount(RedisModuleCommandFilterCtx *fctx)
{
    return fctx->argc;
}
const RedisModuleString *RM_CommandFilterArgGet(RedisModuleCommandFilterCtx *fctx, int pos)
{
    if (pos < 0 || pos >= fctx->argc) return NULL;
    return fctx->argv[pos];
}
int RM_CommandFilterArgInsert(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)
{
    int i;
    if (pos < 0 || pos > fctx->argc) return REDISMODULE_ERR;
    fctx->argv = zrealloc(fctx->argv, (fctx->argc+1)*sizeof(RedisModuleString *));
    for (i = fctx->argc; i > pos; i--) {
        fctx->argv[i] = fctx->argv[i-1];
    }
    fctx->argv[pos] = arg;
    fctx->argc++;
    return REDISMODULE_OK;
}
int RM_CommandFilterArgReplace(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg)
{
    if (pos < 0 || pos >= fctx->argc) return REDISMODULE_ERR;
    decrRefCount(fctx->argv[pos]);
    fctx->argv[pos] = arg;
    return REDISMODULE_OK;
}
int RM_CommandFilterArgDelete(RedisModuleCommandFilterCtx *fctx, int pos)
{
    int i;
    if (pos < 0 || pos >= fctx->argc) return REDISMODULE_ERR;
    decrRefCount(fctx->argv[pos]);
    for (i = pos; i < fctx->argc-1; i++) {
        fctx->argv[i] = fctx->argv[i+1];
    }
    fctx->argc--;
    return REDISMODULE_OK;
}
int RM_Fork(RedisModuleForkDoneHandler cb, void *user_data) {
    pid_t childpid;
    if (hasActiveChildProcess()) {
        return -1;
    }
    openChildInfoPipe();
    if ((childpid = redisFork()) == 0) {
        redisSetProcTitle("redis-module-fork");
    } else if (childpid == -1) {
        closeChildInfoPipe();
        serverLog(LL_WARNING,"Can't fork for module: %s", strerror(errno));
    } else {
        server.module_child_pid = childpid;
        moduleForkInfo.done_handler = cb;
        moduleForkInfo.done_handler_user_data = user_data;
        serverLog(LL_NOTICE, "Module fork started pid: %d ", childpid);
    }
    return childpid;
}
int RM_ExitFromChild(int retcode) {
    sendChildCOWInfo(CHILD_INFO_TYPE_MODULE, "Module fork");
    exitFromChild(retcode);
    return REDISMODULE_OK;
}
int TerminateModuleForkChild(int child_pid, int wait) {
    if (server.module_child_pid == -1 ||
        server.module_child_pid != child_pid) return C_ERR;
    int statloc;
    serverLog(LL_NOTICE,"Killing running module fork child: %ld",
        (long) server.module_child_pid);
    if (kill(server.module_child_pid,SIGUSR1) != -1 && wait) {
        while(wait4(server.module_child_pid,&statloc,0,NULL) !=
              server.module_child_pid);
    }
    server.module_child_pid = -1;
    moduleForkInfo.done_handler = NULL;
    moduleForkInfo.done_handler_user_data = NULL;
    closeChildInfoPipe();
    updateDictResizePolicy();
    return C_OK;
}
int RM_KillForkChild(int child_pid) {
    if (TerminateModuleForkChild(child_pid,1) == C_OK)
        return REDISMODULE_OK;
    else
        return REDISMODULE_ERR;
}
void ModuleForkDoneHandler(int exitcode, int bysignal) {
    serverLog(LL_NOTICE,
        "Module fork exited pid: %d, retcode: %d, bysignal: %d",
        server.module_child_pid, exitcode, bysignal);
    if (moduleForkInfo.done_handler) {
        moduleForkInfo.done_handler(exitcode, bysignal,
            moduleForkInfo.done_handler_user_data);
    }
    server.module_child_pid = -1;
    moduleForkInfo.done_handler = NULL;
    moduleForkInfo.done_handler_user_data = NULL;
}
int RM_SubscribeToServerEvent(RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback) {
    RedisModuleEventListener *el;
    if (ctx->module == NULL) return REDISMODULE_ERR;
    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);
    while((ln = listNext(&li))) {
        el = ln->value;
        if (el->module == ctx->module && el->event.id == event.id)
            break;
    }
    if (ln) {
        if (callback == NULL) {
            listDelNode(RedisModule_EventListeners,ln);
            zfree(el);
        } else {
            el->callback = callback;
        }
        return REDISMODULE_OK;
    }
    el = zmalloc(sizeof(*el));
    el->module = ctx->module;
    el->event = event;
    el->callback = callback;
    listAddNodeTail(RedisModule_EventListeners,el);
    return REDISMODULE_OK;
}
void moduleFireServerEvent(uint64_t eid, int subid, void *data) {
    if (listLength(RedisModule_EventListeners) == 0) return;
    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);
    while((ln = listNext(&li))) {
        RedisModuleEventListener *el = ln->value;
        if (el->event.id == eid) {
            RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
            ctx.module = el->module;
            if (ModulesInHooks == 0) {
                ctx.client = moduleFreeContextReusedClient;
            } else {
                ctx.client = createClient(NULL);
                ctx.client->flags |= CLIENT_MODULE;
                ctx.client->user = NULL;
            }
            void *moduledata = NULL;
            RedisModuleClientInfoV1 civ1;
            RedisModuleReplicationInfoV1 riv1;
            RedisModuleModuleChangeV1 mcv1;
            selectDb(ctx.client, 0);
            if (eid == REDISMODULE_EVENT_CLIENT_CHANGE) {
                modulePopulateClientInfoStructure(&civ1,data,
                                                  el->event.dataver);
                moduledata = &civ1;
            } else if (eid == REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED) {
                modulePopulateReplicationInfoStructure(&riv1,el->event.dataver);
                moduledata = &riv1;
            } else if (eid == REDISMODULE_EVENT_FLUSHDB) {
                moduledata = data;
                RedisModuleFlushInfoV1 *fi = data;
                if (fi->dbnum != -1)
                    selectDb(ctx.client, fi->dbnum);
            } else if (eid == REDISMODULE_EVENT_MODULE_CHANGE) {
                RedisModule *m = data;
                if (m == el->module)
                    continue;
                mcv1.version = REDISMODULE_MODULE_CHANGE_VERSION;
                mcv1.module_name = m->name;
                mcv1.module_version = m->ver;
                moduledata = &mcv1;
            } else if (eid == REDISMODULE_EVENT_LOADING_PROGRESS) {
                moduledata = data;
            } else if (eid == REDISMODULE_EVENT_CRON_LOOP) {
                moduledata = data;
            }
            ModulesInHooks++;
            el->module->in_hook++;
            el->callback(&ctx,el->event,subid,moduledata);
            el->module->in_hook--;
            ModulesInHooks--;
            if (ModulesInHooks != 0) freeClient(ctx.client);
            moduleFreeContext(&ctx);
        }
    }
}
void moduleUnsubscribeAllServerEvents(RedisModule *module) {
    RedisModuleEventListener *el;
    listIter li;
    listNode *ln;
    listRewind(RedisModule_EventListeners,&li);
    while((ln = listNext(&li))) {
        el = ln->value;
        if (el->module == module) {
            listDelNode(RedisModule_EventListeners,ln);
            zfree(el);
        }
    }
}
void processModuleLoadingProgressEvent(int is_aof) {
    long long now = ustime();
    static long long next_event = 0;
    if (now >= next_event) {
        int progress = -1;
        if (server.loading_total_bytes)
            progress = (server.loading_total_bytes<<10) / server.loading_total_bytes;
        RedisModuleFlushInfoV1 fi = {REDISMODULE_LOADING_PROGRESS_VERSION,
                                     server.hz,
                                     progress};
        moduleFireServerEvent(REDISMODULE_EVENT_LOADING_PROGRESS,
                              is_aof?
                                REDISMODULE_SUBEVENT_LOADING_PROGRESS_AOF:
                                REDISMODULE_SUBEVENT_LOADING_PROGRESS_RDB,
                              &fi);
        next_event = now + 1000000 / server.hz;
    }
}
uint64_t dictCStringKeyHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}
int dictCStringKeyCompare(void *privdata, const void *key1, const void *key2) {
    UNUSED(privdata);
    return strcmp(key1,key2) == 0;
}
dictType moduleAPIDictType = {
    dictCStringKeyHash,
    NULL,
    NULL,
    dictCStringKeyCompare,
    NULL,
    NULL
};
int moduleRegisterApi(const char *funcname, void *funcptr) {
    return dictAdd(server.moduleapi, (char*)funcname, funcptr);
}
#define REGISTER_API(name) \
    moduleRegisterApi("RedisModule_" #name, (void *)(unsigned long)RM_ ## name)
void moduleRegisterCoreAPI(void);
void moduleInitModulesSystem(void) {
    moduleUnblockedClients = listCreate();
    server.loadmodule_queue = listCreate();
    modules = dictCreate(&modulesDictType,NULL);
    moduleKeyspaceSubscribers = listCreate();
    moduleFreeContextReusedClient = createClient(NULL);
    moduleFreeContextReusedClient->flags |= CLIENT_MODULE;
    moduleFreeContextReusedClient->user = NULL;
    moduleCommandFilters = listCreate();
    moduleRegisterCoreAPI();
    if (pipe(server.module_blocked_pipe) == -1) {
        serverLog(LL_WARNING,
            "Can't create the pipe for module blocking commands: %s",
            strerror(errno));
        exit(1);
    }
    anetNonBlock(NULL,server.module_blocked_pipe[0]);
    anetNonBlock(NULL,server.module_blocked_pipe[1]);
    Timers = raxNew();
    RedisModule_EventListeners = listCreate();
    pthread_mutex_lock(&moduleGIL);
}
void moduleLoadFromQueue(void) {
    listIter li;
    listNode *ln;
    listRewind(server.loadmodule_queue,&li);
    while((ln = listNext(&li))) {
        struct moduleLoadQueueEntry *loadmod = ln->value;
        if (moduleLoad(loadmod->path,(void **)loadmod->argv,loadmod->argc)
            == C_ERR)
        {
            serverLog(LL_WARNING,
                "Can't load module from %s: server aborting",
                loadmod->path);
            exit(1);
        }
    }
}
void moduleFreeModuleStructure(struct RedisModule *module) {
    listRelease(module->types);
    listRelease(module->filters);
    listRelease(module->usedby);
    listRelease(module->using);
    sdsfree(module->name);
    zfree(module);
}
void moduleUnregisterCommands(struct RedisModule *module) {
    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->proc == RedisModuleCommandDispatcher) {
            RedisModuleCommandProxy *cp =
                (void*)(unsigned long)cmd->getkeys_proc;
            sds cmdname = cp->rediscmd->name;
            if (cp->module == module) {
                dictDelete(server.commands,cmdname);
                dictDelete(server.orig_commands,cmdname);
                sdsfree(cmdname);
                zfree(cp->rediscmd);
                zfree(cp);
            }
        }
    }
    dictReleaseIterator(di);
}
int moduleLoad(const char *path, void **module_argv, int module_argc) {
    int (*onload)(void *, void **, int);
    void *handle;
    RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
    handle = dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (handle == NULL) {
        serverLog(LL_WARNING, "Module %s failed to load: %s", path, dlerror());
        return C_ERR;
    }
    onload = (int (*)(void *, void **, int))(unsigned long) dlsym(handle,"RedisModule_OnLoad");
    if (onload == NULL) {
        dlclose(handle);
        serverLog(LL_WARNING,
            "Module %s does not export RedisModule_OnLoad() "
            "symbol. Module not loaded.",path);
        return C_ERR;
    }
    if (onload((void*)&ctx,module_argv,module_argc) == REDISMODULE_ERR) {
        if (ctx.module) {
            moduleUnregisterCommands(ctx.module);
            moduleUnregisterSharedAPI(ctx.module);
            moduleUnregisterUsedAPI(ctx.module);
            moduleFreeModuleStructure(ctx.module);
        }
        dlclose(handle);
        serverLog(LL_WARNING,
            "Module %s initialization failed. Module not loaded",path);
        return C_ERR;
    }
    dictAdd(modules,ctx.module->name,ctx.module);
    ctx.module->blocked_clients = 0;
    ctx.module->handle = handle;
    serverLog(LL_NOTICE,"Module '%s' loaded from %s",ctx.module->name,path);
    moduleFireServerEvent(REDISMODULE_EVENT_MODULE_CHANGE,
                          REDISMODULE_SUBEVENT_MODULE_LOADED,
                          ctx.module);
    moduleFreeContext(&ctx);
    return C_OK;
}
int moduleUnload(sds name) {
    struct RedisModule *module = dictFetchValue(modules,name);
    if (module == NULL) {
        errno = ENOENT;
        return REDISMODULE_ERR;
    } else if (listLength(module->types)) {
        errno = EBUSY;
        return REDISMODULE_ERR;
    } else if (listLength(module->usedby)) {
        errno = EPERM;
        return REDISMODULE_ERR;
    } else if (module->blocked_clients) {
        errno = EAGAIN;
        return REDISMODULE_ERR;
    }
    int (*onunload)(void *);
    onunload = (int (*)(void *))(unsigned long) dlsym(module->handle, "RedisModule_OnUnload");
    if (onunload) {
        RedisModuleCtx ctx = REDISMODULE_CTX_INIT;
        ctx.module = module;
        ctx.client = moduleFreeContextReusedClient;
        int unload_status = onunload((void*)&ctx);
        moduleFreeContext(&ctx);
        if (unload_status == REDISMODULE_ERR) {
            serverLog(LL_WARNING, "Module %s OnUnload failed.  Unload canceled.", name);
            errno = ECANCELED;
            return REDISMODULE_ERR;
        }
    }
    moduleUnregisterCommands(module);
    moduleUnregisterSharedAPI(module);
    moduleUnregisterUsedAPI(module);
    moduleUnregisterFilters(module);
    moduleUnsubscribeNotifications(module);
    moduleUnsubscribeAllServerEvents(module);
    if (dlclose(module->handle) == -1) {
        char *error = dlerror();
        if (error == NULL) error = "Unknown error";
        serverLog(LL_WARNING,"Error when trying to close the %s module: %s",
            module->name, error);
    }
    moduleFireServerEvent(REDISMODULE_EVENT_MODULE_CHANGE,
                          REDISMODULE_SUBEVENT_MODULE_UNLOADED,
                          module);
    serverLog(LL_NOTICE,"Module %s unloaded",module->name);
    dictDelete(modules,module->name);
    module->name = NULL;
    moduleFreeModuleStructure(module);
    return REDISMODULE_OK;
}
void addReplyLoadedModules(client *c) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    addReplyArrayLen(c,dictSize(modules));
    while ((de = dictNext(di)) != NULL) {
        sds name = dictGetKey(de);
        struct RedisModule *module = dictGetVal(de);
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"name");
        addReplyBulkCBuffer(c,name,sdslen(name));
        addReplyBulkCString(c,"ver");
        addReplyLongLong(c,module->ver);
    }
    dictReleaseIterator(di);
}
sds genModulesInfoStringRenderModulesList(list *l) {
    listIter li;
    listNode *ln;
    listRewind(l,&li);
    sds output = sdsnew("[");
    while((ln = listNext(&li))) {
        RedisModule *module = ln->value;
        output = sdscat(output,module->name);
    }
    output = sdstrim(output,"|");
    output = sdscat(output,"]");
    return output;
}
sds genModulesInfoStringRenderModuleOptions(struct RedisModule *module) {
    sds output = sdsnew("[");
    if (module->options & REDISMODULE_OPTIONS_HANDLE_IO_ERRORS)
        output = sdscat(output,"handle-io-errors|");
    output = sdstrim(output,"|");
    output = sdscat(output,"]");
    return output;
}
sds genModulesInfoString(sds info) {
    dictIterator *di = dictGetIterator(modules);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        sds name = dictGetKey(de);
        struct RedisModule *module = dictGetVal(de);
        sds usedby = genModulesInfoStringRenderModulesList(module->usedby);
        sds using = genModulesInfoStringRenderModulesList(module->using);
        sds options = genModulesInfoStringRenderModuleOptions(module);
        info = sdscatfmt(info,
            "module:name=%S,ver=%i,api=%i,filters=%i,"
            "usedby=%S,using=%S,options=%S\r\n",
                name, module->ver, module->apiver,
                (int)listLength(module->filters), usedby, using, options);
        sdsfree(usedby);
        sdsfree(using);
        sdsfree(options);
    }
    dictReleaseIterator(di);
    return info;
}
void moduleCommand(client *c) {
    char *subcmd = c->argv[1]->ptr;
    if (c->argc == 2 && !strcasecmp(subcmd,"help")) {
        const char *help[] = {
"LIST -- Return a list of loaded modules.",
"LOAD <path> [arg ...] -- Load a module library from <path>.",
"UNLOAD <name> -- Unload a module.",
NULL
        };
        addReplyHelp(c, help);
    } else
    if (!strcasecmp(subcmd,"load") && c->argc >= 3) {
        robj **argv = NULL;
        int argc = 0;
        if (c->argc > 3) {
            argc = c->argc - 3;
            argv = &c->argv[3];
        }
        if (moduleLoad(c->argv[2]->ptr,(void **)argv,argc) == C_OK)
            addReply(c,shared.ok);
        else
            addReplyError(c,
                "Error loading the extension. Please check the server logs.");
    } else if (!strcasecmp(subcmd,"unload") && c->argc == 3) {
        if (moduleUnload(c->argv[2]->ptr) == C_OK)
            addReply(c,shared.ok);
        else {
            char *errmsg;
            switch(errno) {
            case ENOENT:
                errmsg = "no such module with that name";
                break;
            case EBUSY:
                errmsg = "the module exports one or more module-side data "
                         "types, can't unload";
                break;
            case EPERM:
                errmsg = "the module exports APIs used by other modules. "
                         "Please unload them first and try again";
                break;
            case EAGAIN:
                errmsg = "the module has blocked clients. "
                         "Please wait them unblocked and try again";
                break;
            default:
                errmsg = "operation not possible.";
                break;
            }
            addReplyErrorFormat(c,"Error unloading module: %s",errmsg);
        }
    } else if (!strcasecmp(subcmd,"list") && c->argc == 2) {
        addReplyLoadedModules(c);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}
size_t moduleCount(void) {
    return dictSize(modules);
}
int RM_SetLRUOrLFU(RedisModuleKey *key, long long lfu_freq, long long lru_idle) {
    if (!key->value)
        return REDISMODULE_ERR;
    if (objectSetLRUOrLFU(key->value, lfu_freq, lru_idle, lru_idle>=0 ? LRU_CLOCK() : 0))
        return REDISMODULE_OK;
    return REDISMODULE_ERR;
}
int RM_GetLRUOrLFU(RedisModuleKey *key, long long *lfu_freq, long long *lru_idle) {
    *lru_idle = *lfu_freq = -1;
    if (!key->value)
        return REDISMODULE_ERR;
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        *lfu_freq = LFUDecrAndReturn(key->value);
    } else {
        *lru_idle = estimateObjectIdleTime(key->value)/1000;
    }
    return REDISMODULE_OK;
}
void moduleRegisterCoreAPI(void) {
    server.moduleapi = dictCreate(&moduleAPIDictType,NULL);
    server.sharedapi = dictCreate(&moduleAPIDictType,NULL);
    REGISTER_API(Alloc);
    REGISTER_API(Calloc);
    REGISTER_API(Realloc);
    REGISTER_API(Free);
    REGISTER_API(Strdup);
    REGISTER_API(CreateCommand);
    REGISTER_API(SetModuleAttribs);
    REGISTER_API(IsModuleNameBusy);
    REGISTER_API(WrongArity);
    REGISTER_API(ReplyWithLongLong);
    REGISTER_API(ReplyWithError);
    REGISTER_API(ReplyWithSimpleString);
    REGISTER_API(ReplyWithArray);
    REGISTER_API(ReplyWithNullArray);
    REGISTER_API(ReplyWithEmptyArray);
    REGISTER_API(ReplySetArrayLength);
    REGISTER_API(ReplyWithString);
    REGISTER_API(ReplyWithEmptyString);
    REGISTER_API(ReplyWithVerbatimString);
    REGISTER_API(ReplyWithStringBuffer);
    REGISTER_API(ReplyWithCString);
    REGISTER_API(ReplyWithNull);
    REGISTER_API(ReplyWithCallReply);
    REGISTER_API(ReplyWithDouble);
    REGISTER_API(GetSelectedDb);
    REGISTER_API(SelectDb);
    REGISTER_API(OpenKey);
    REGISTER_API(CloseKey);
    REGISTER_API(KeyType);
    REGISTER_API(ValueLength);
    REGISTER_API(ListPush);
    REGISTER_API(ListPop);
    REGISTER_API(StringToLongLong);
    REGISTER_API(StringToDouble);
    REGISTER_API(Call);
    REGISTER_API(CallReplyProto);
    REGISTER_API(FreeCallReply);
    REGISTER_API(CallReplyInteger);
    REGISTER_API(CallReplyType);
    REGISTER_API(CallReplyLength);
    REGISTER_API(CallReplyArrayElement);
    REGISTER_API(CallReplyStringPtr);
    REGISTER_API(CreateStringFromCallReply);
    REGISTER_API(CreateString);
    REGISTER_API(CreateStringFromLongLong);
    REGISTER_API(CreateStringFromString);
    REGISTER_API(CreateStringPrintf);
    REGISTER_API(FreeString);
    REGISTER_API(StringPtrLen);
    REGISTER_API(AutoMemory);
    REGISTER_API(Replicate);
    REGISTER_API(ReplicateVerbatim);
    REGISTER_API(DeleteKey);
    REGISTER_API(UnlinkKey);
    REGISTER_API(StringSet);
    REGISTER_API(StringDMA);
    REGISTER_API(StringTruncate);
    REGISTER_API(SetExpire);
    REGISTER_API(GetExpire);
    REGISTER_API(ResetDataset);
    REGISTER_API(DbSize);
    REGISTER_API(RandomKey);
    REGISTER_API(ZsetAdd);
    REGISTER_API(ZsetIncrby);
    REGISTER_API(ZsetScore);
    REGISTER_API(ZsetRem);
    REGISTER_API(ZsetRangeStop);
    REGISTER_API(ZsetFirstInScoreRange);
    REGISTER_API(ZsetLastInScoreRange);
    REGISTER_API(ZsetFirstInLexRange);
    REGISTER_API(ZsetLastInLexRange);
    REGISTER_API(ZsetRangeCurrentElement);
    REGISTER_API(ZsetRangeNext);
    REGISTER_API(ZsetRangePrev);
    REGISTER_API(ZsetRangeEndReached);
    REGISTER_API(HashSet);
    REGISTER_API(HashGet);
    REGISTER_API(IsKeysPositionRequest);
    REGISTER_API(KeyAtPos);
    REGISTER_API(GetClientId);
    REGISTER_API(GetContextFlags);
    REGISTER_API(PoolAlloc);
    REGISTER_API(CreateDataType);
    REGISTER_API(ModuleTypeSetValue);
    REGISTER_API(ModuleTypeGetType);
    REGISTER_API(ModuleTypeGetValue);
    REGISTER_API(IsIOError);
    REGISTER_API(SetModuleOptions);
    REGISTER_API(SignalModifiedKey);
    REGISTER_API(SaveUnsigned);
    REGISTER_API(LoadUnsigned);
    REGISTER_API(SaveSigned);
    REGISTER_API(LoadSigned);
    REGISTER_API(SaveString);
    REGISTER_API(SaveStringBuffer);
    REGISTER_API(LoadString);
    REGISTER_API(LoadStringBuffer);
    REGISTER_API(SaveDouble);
    REGISTER_API(LoadDouble);
    REGISTER_API(SaveFloat);
    REGISTER_API(LoadFloat);
    REGISTER_API(EmitAOF);
    REGISTER_API(Log);
    REGISTER_API(LogIOError);
    REGISTER_API(_Assert);
    REGISTER_API(LatencyAddSample);
    REGISTER_API(StringAppendBuffer);
    REGISTER_API(RetainString);
    REGISTER_API(StringCompare);
    REGISTER_API(GetContextFromIO);
    REGISTER_API(GetKeyNameFromIO);
    REGISTER_API(GetKeyNameFromModuleKey);
    REGISTER_API(BlockClient);
    REGISTER_API(UnblockClient);
    REGISTER_API(IsBlockedReplyRequest);
    REGISTER_API(IsBlockedTimeoutRequest);
    REGISTER_API(GetBlockedClientPrivateData);
    REGISTER_API(AbortBlock);
    REGISTER_API(Milliseconds);
    REGISTER_API(GetThreadSafeContext);
    REGISTER_API(FreeThreadSafeContext);
    REGISTER_API(ThreadSafeContextLock);
    REGISTER_API(ThreadSafeContextUnlock);
    REGISTER_API(DigestAddStringBuffer);
    REGISTER_API(DigestAddLongLong);
    REGISTER_API(DigestEndSequence);
    REGISTER_API(NotifyKeyspaceEvent);
    REGISTER_API(GetNotifyKeyspaceEvents);
    REGISTER_API(SubscribeToKeyspaceEvents);
    REGISTER_API(RegisterClusterMessageReceiver);
    REGISTER_API(SendClusterMessage);
    REGISTER_API(GetClusterNodeInfo);
    REGISTER_API(GetClusterNodesList);
    REGISTER_API(FreeClusterNodesList);
    REGISTER_API(CreateTimer);
    REGISTER_API(StopTimer);
    REGISTER_API(GetTimerInfo);
    REGISTER_API(GetMyClusterID);
    REGISTER_API(GetClusterSize);
    REGISTER_API(GetRandomBytes);
    REGISTER_API(GetRandomHexChars);
    REGISTER_API(BlockedClientDisconnected);
    REGISTER_API(SetDisconnectCallback);
    REGISTER_API(GetBlockedClientHandle);
    REGISTER_API(SetClusterFlags);
    REGISTER_API(CreateDict);
    REGISTER_API(FreeDict);
    REGISTER_API(DictSize);
    REGISTER_API(DictSetC);
    REGISTER_API(DictReplaceC);
    REGISTER_API(DictSet);
    REGISTER_API(DictReplace);
    REGISTER_API(DictGetC);
    REGISTER_API(DictGet);
    REGISTER_API(DictDelC);
    REGISTER_API(DictDel);
    REGISTER_API(DictIteratorStartC);
    REGISTER_API(DictIteratorStart);
    REGISTER_API(DictIteratorStop);
    REGISTER_API(DictIteratorReseekC);
    REGISTER_API(DictIteratorReseek);
    REGISTER_API(DictNextC);
    REGISTER_API(DictPrevC);
    REGISTER_API(DictNext);
    REGISTER_API(DictPrev);
    REGISTER_API(DictCompareC);
    REGISTER_API(DictCompare);
    REGISTER_API(ExportSharedAPI);
    REGISTER_API(GetSharedAPI);
    REGISTER_API(RegisterCommandFilter);
    REGISTER_API(UnregisterCommandFilter);
    REGISTER_API(CommandFilterArgsCount);
    REGISTER_API(CommandFilterArgGet);
    REGISTER_API(CommandFilterArgInsert);
    REGISTER_API(CommandFilterArgReplace);
    REGISTER_API(CommandFilterArgDelete);
    REGISTER_API(Fork);
    REGISTER_API(ExitFromChild);
    REGISTER_API(KillForkChild);
    REGISTER_API(RegisterInfoFunc);
    REGISTER_API(InfoAddSection);
    REGISTER_API(InfoBeginDictField);
    REGISTER_API(InfoEndDictField);
    REGISTER_API(InfoAddFieldString);
    REGISTER_API(InfoAddFieldCString);
    REGISTER_API(InfoAddFieldDouble);
    REGISTER_API(InfoAddFieldLongLong);
    REGISTER_API(InfoAddFieldULongLong);
    REGISTER_API(GetClientInfoById);
    REGISTER_API(PublishMessage);
    REGISTER_API(SubscribeToServerEvent);
    REGISTER_API(SetLRUOrLFU);
    REGISTER_API(GetLRUOrLFU);
    REGISTER_API(BlockClientOnKeys);
    REGISTER_API(SignalKeyAsReady);
    REGISTER_API(GetBlockedClientReadyKey);
}
