#include "redis.h"
#include <signal.h>
robj *lookupKey(redisDb *db, robj *key) {
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
robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}
robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}
robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}
robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}
int dbAdd(redisDb *db, robj *key, robj *val) {
    if (dictFind(db->dict, key->ptr) != NULL) {
        return REDIS_ERR;
    } else {
        sds copy = sdsdup(key->ptr);
        dictAdd(db->dict, copy, val);
        return REDIS_OK;
    }
}
int dbReplace(redisDb *db, robj *key, robj *val) {
    if (dictFind(db->dict,key->ptr) == NULL) {
        sds copy = sdsdup(key->ptr);
        dictAdd(db->dict, copy, val);
        return 1;
    } else {
        dictReplace(db->dict, key->ptr, val);
        return 0;
    }
}
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}
robj *dbRandomKey(redisDb *db) {
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
int dbDelete(redisDb *db, robj *key) {
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    return dictDelete(db->dict,key->ptr) == DICT_OK;
}
long long emptyDb() {
    int j;
    long long removed = 0;
    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict);
        dictEmpty(server.db[j].expires);
    }
    return removed;
}
int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}
void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    touchWatchedKeysOnFlush(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}
void flushallCommand(redisClient *c) {
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
void delCommand(redisClient *c) {
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
void existsCommand(redisClient *c) {
    expireIfNeeded(c->db,c->argv[1]);
    if (dbExists(c->db,c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}
void selectCommand(redisClient *c) {
    int id = atoi(c->argv[1]->ptr);
    if (selectDb(c,id) == REDIS_ERR) {
        addReplyError(c,"invalid DB index");
    } else {
        addReply(c,shared.ok);
    }
}
void randomkeyCommand(redisClient *c) {
    robj *key;
    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }
    addReplyBulk(c,key);
    decrRefCount(key);
}
void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);
    di = dictGetIterator(c->db->dict);
allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetEntryKey(de);
        robj *keyobj;
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}
void dbsizeCommand(redisClient *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}
void lastsaveCommand(redisClient *c) {
    addReplyLongLong(c,server.lastsave);
}
void typeCommand(redisClient *c) {
    robj *o;
    char *type;
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "string"; break;
        case REDIS_LIST: type = "list"; break;
        case REDIS_SET: type = "set"; break;
        case REDIS_ZSET: type = "zset"; break;
        case REDIS_HASH: type = "hash"; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}
void saveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
void bgsaveCommand(redisClient *c) {
    if (server.bgsavechildpid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}
void shutdownCommand(redisClient *c) {
    if (prepareForShutdown() == REDIS_OK)
        exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}
void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;
    incrRefCount(o);
    if (dbAdd(c->db,c->argv[2],o) == REDIS_ERR) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dbReplace(c->db,c->argv[2],o);
    }
    dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[2]);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}
void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}
void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}
void moveCommand(redisClient *c) {
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
    if (dbAdd(dst,c->argv[1],o) == REDIS_ERR) {
        addReply(c,shared.czero);
        return;
    }
    incrRefCount(o);
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}
int removeExpire(redisDb *db, robj *key) {
    redisAssert(dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}
void setExpire(redisDb *db, robj *key, time_t when) {
    dictEntry *de;
    de = dictFind(db->dict,key->ptr);
    redisAssert(de != NULL);
    dictReplace(db->expires,dictGetEntryKey(de),(void*)when);
}
time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;
    redisAssert(dictFind(db->dict,key->ptr) != NULL);
    return (time_t) dictGetEntryVal(de);
}
void propagateExpire(redisDb *db, robj *key) {
    struct redisCommand *cmd;
    robj *argv[2];
    cmd = lookupCommand("del");
    argv[0] = createStringObject("DEL",3);
    argv[1] = key;
    incrRefCount(key);
    if (server.appendonly)
        feedAppendOnlyFile(cmd,db->id,argv,2);
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,db->id,argv,2);
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}
int expireIfNeeded(redisDb *db, robj *key) {
    time_t when = getExpire(db,key);
    if (server.masterhost != NULL) {
        return time(NULL) > when;
    }
    if (when < 0) return 0;
    if (time(NULL) <= when) return 0;
    server.stat_expiredkeys++;
    server.dirty++;
    propagateExpire(db,key);
    return dbDelete(db,key);
}
void expireGenericCommand(redisClient *c, robj *key, robj *param, long offset) {
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
        touchWatchedKey(c->db,key);
        return;
    } else {
        time_t when = time(NULL)+seconds;
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
        touchWatchedKey(c->db,key);
        server.dirty++;
        return;
    }
}
void expireCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],0);
}
void expireatCommand(redisClient *c) {
    expireGenericCommand(c,c->argv[1],c->argv[2],time(NULL));
}
void ttlCommand(redisClient *c) {
    time_t expire, ttl = -1;
    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = (expire-time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplyLongLong(c,(long long)ttl);
}
void persistCommand(redisClient *c) {
    dictEntry *de;
    de = dictFind(c->db->dict,c->argv[1]->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
    } else {
        if (removeExpire(c->db,c->argv[1])) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    }
}
