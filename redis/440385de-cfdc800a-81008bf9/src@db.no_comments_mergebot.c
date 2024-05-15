#include "server.h"
#include "cluster.h"
#include "atomicvar.h"
#include <signal.h>
#include <ctype.h>
int keyIsExpired(redisDb *db, robj *key);
void updateLFU(robj *val) {
  unsigned long counter = LFUDecrAndReturn(val);
  counter = LFULogIncr(counter);
  val->lru = (LFUGetTimeInMinutes() << 8) | counter;
}
robj *lookupKey(redisDb *db, robj *key, int flags) {
  dictEntry *de = dictFind(db->dict, key->ptr);
  if (de) {
    robj *val = dictGetVal(de);
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 &&
        !(flags & LOOKUP_NOTOUCH)) {
      if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        updateLFU(val);
      } else {
        val->lru = LRU_CLOCK();
      }
    }
    return val;
  } else {
    return NULL;
  }
}
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
  robj *val;
  if (expireIfNeeded(db, key) == 1) {
    if (server.masterhost == NULL) {
      server.stat_keyspace_misses++;
      return NULL;
    }
    if (server.current_client && server.current_client != server.master &&
        server.current_client->cmd &&
        server.current_client->cmd->flags & CMD_READONLY) {
      server.stat_keyspace_misses++;
      return NULL;
    }
  }
  val = lookupKey(db, key, flags);
  if (val == NULL)
    server.stat_keyspace_misses++;
  else
    server.stat_keyspace_hits++;
  return val;
}
robj *lookupKeyRead(redisDb *db, robj *key) {
  return lookupKeyReadWithFlags(db, key, LOOKUP_NONE);
}
robj *lookupKeyWrite(redisDb *db, robj *key) {
  expireIfNeeded(db, key);
  return lookupKey(db, key, LOOKUP_NONE);
}
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
  robj *o = lookupKeyRead(c->db, key);
  if (!o) addReply(c, reply);
  return o;
}
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
  robj *o = lookupKeyWrite(c->db, key);
  if (!o) addReply(c, reply);
  return o;
}
void dbAdd(redisDb *db, robj *key, robj *val) {
  sds copy = sdsdup(key->ptr);
  int retval = dictAdd(db->dict, copy, val);
  serverAssertWithInfo(NULL, key, retval == DICT_OK);
  if (val->type == OBJ_LIST || val->type == OBJ_ZSET) signalKeyAsReady(db, key);
  if (server.cluster_enabled) slotToKeyAdd(key);
}
void dbOverwrite(redisDb *db, robj *key, robj *val) {
  dictEntry *de = dictFind(db->dict, key->ptr);
  serverAssertWithInfo(NULL, key, de != NULL);
  dictEntry auxentry = *de;
  robj *old = dictGetVal(de);
  if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
    val->lru = old->lru;
  }
  dictSetVal(db->dict, de, val);
  if (server.lazyfree_lazy_server_del) {
    freeObjAsync(old);
    dictSetVal(db->dict, &auxentry, NULL);
  }
  dictFreeVal(db->dict, &auxentry);
}
void setKey(redisDb *db, robj *key, robj *val) {
  if (lookupKeyWrite(db, key) == NULL) {
    dbAdd(db, key, val);
  } else {
    dbOverwrite(db, key, val);
  }
  incrRefCount(val);
  removeExpire(db, key);
  signalModifiedKey(db, key);
}
int dbExists(redisDb *db, robj *key) {
  return dictFind(db->dict, key->ptr) != NULL;
}
robj *dbRandomKey(redisDb *db) {
  dictEntry *de;
  int maxtries = 100;
  int allvolatile = dictSize(db->dict) == dictSize(db->expires);
  while (1) {
    sds key;
    robj *keyobj;
    de = dictGetRandomKey(db->dict);
    if (de == NULL) return NULL;
    key = dictGetKey(de);
    keyobj = createStringObject(key, sdslen(key));
    if (dictFind(db->expires, key)) {
      if (allvolatile && server.masterhost && --maxtries == 0) {
        return keyobj;
      }
      if (expireIfNeeded(db, keyobj)) {
        decrRefCount(keyobj);
        continue;
      }
    }
    return keyobj;
  }
}
int dbSyncDelete(redisDb *db, robj *key) {
  if (dictSize(db->expires) > 0) dictDelete(db->expires, key->ptr);
  if (dictDelete(db->dict, key->ptr) == DICT_OK) {
    if (server.cluster_enabled) slotToKeyDel(key);
    return 1;
  } else {
    return 0;
  }
}
int dbDelete(redisDb *db, robj *key) {
  return server.lazyfree_lazy_server_del ? dbAsyncDelete(db, key)
                                         : dbSyncDelete(db, key);
}
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
  serverAssert(o->type == OBJ_STRING);
  if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
    robj *decoded = getDecodedObject(o);
    o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
    decrRefCount(decoded);
    dbOverwrite(db, key, o);
  }
  return o;
}
long long emptyDb(int dbnum, int flags, void(callback)(void *)) {
  int async = (flags & EMPTYDB_ASYNC);
  long long removed = 0;
  if (dbnum < -1 || dbnum >= server.dbnum) {
    errno = EINVAL;
    return -1;
  }
  int startdb, enddb;
  if (dbnum == -1) {
    startdb = 0;
    enddb = server.dbnum - 1;
  } else {
    startdb = enddb = dbnum;
  }
  for (int j = startdb; j <= enddb; j++) {
    removed += dictSize(server.db[j].dict);
    if (async) {
      emptyDbAsync(&server.db[j]);
    } else {
      dictEmpty(server.db[j].dict, callback);
      dictEmpty(server.db[j].expires, callback);
    }
  }
  if (server.cluster_enabled) {
    if (async) {
      slotToKeyFlushAsync();
    } else {
      slotToKeyFlush();
    }
  }
  if (dbnum == -1) flushSlaveKeysWithExpireList();
  return removed;
}
int selectDb(client *c, int id) {
  if (id < 0 || id >= server.dbnum) return C_ERR;
  c->db = &server.db[id];
  return C_OK;
}
void signalModifiedKey(redisDb *db, robj *key) { touchWatchedKey(db, key); }
void signalFlushedDb(int dbid) { touchWatchedKeysOnFlush(dbid); }
int getFlushCommandFlags(client *c, int *flags) {
  if (c->argc > 1) {
    if (c->argc > 2 || strcasecmp(c->argv[1]->ptr, "async")) {
      addReply(c, shared.syntaxerr);
      return C_ERR;
    }
    *flags = EMPTYDB_ASYNC;
  } else {
    *flags = EMPTYDB_NO_FLAGS;
  }
  return C_OK;
}
void flushdbCommand(client *c) {
  int flags;
  if (getFlushCommandFlags(c, &flags) == C_ERR) return;
  signalFlushedDb(c->db->id);
  server.dirty += emptyDb(c->db->id, flags, NULL);
  addReply(c, shared.ok);
}
void flushallCommand(client *c) {
  int flags;
  if (getFlushCommandFlags(c, &flags) == C_ERR) return;
  signalFlushedDb(-1);
  server.dirty += emptyDb(-1, flags, NULL);
  addReply(c, shared.ok);
  if (server.rdb_child_pid != -1) {
    kill(server.rdb_child_pid, SIGUSR1);
    rdbRemoveTempFile(server.rdb_child_pid);
<<<<<<< HEAD
    closeChildInfoPipe();
|||||||
=======
    updateDictResizePolicy();
>>>>>>> cfdc800a5ff5a2bb02ccd1e21c1c36e6cb5a474d
  }
  if (server.saveparamslen > 0) {
    int saved_dirty = server.dirty;
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    rdbSave(server.rdb_filename, rsiptr);
    server.dirty = saved_dirty;
  }
  server.dirty++;
}
void delGenericCommand(client *c, int lazy) {
  int numdel = 0, j;
  for (j = 1; j < c->argc; j++) {
    expireIfNeeded(c->db, c->argv[j]);
    int deleted = lazy ? dbAsyncDelete(c->db, c->argv[j])
                       : dbSyncDelete(c->db, c->argv[j]);
    if (deleted) {
      signalModifiedKey(c->db, c->argv[j]);
      notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[j], c->db->id);
      server.dirty++;
      numdel++;
    }
  }
  addReplyLongLong(c, numdel);
}
void delCommand(client *c) { delGenericCommand(c, 0); }
void unlinkCommand(client *c) { delGenericCommand(c, 1); }
void existsCommand(client *c) {
  long long count = 0;
  int j;
  for (j = 1; j < c->argc; j++) {
    if (lookupKeyRead(c->db, c->argv[j])) count++;
  }
  addReplyLongLong(c, count);
}
void selectCommand(client *c) {
  long id;
  if (getLongFromObjectOrReply(c, c->argv[1], &id, "invalid DB index") != C_OK)
    return;
  if (server.cluster_enabled && id != 0) {
    addReplyError(c, "SELECT is not allowed in cluster mode");
    return;
  }
  if (selectDb(c, id) == C_ERR) {
    addReplyError(c, "DB index is out of range");
  } else {
    addReply(c, shared.ok);
  }
}
void randomkeyCommand(client *c) {
  robj *key;
  if ((key = dbRandomKey(c->db)) == NULL) {
    addReplyNull(c);
    return;
  }
  addReplyBulk(c, key);
  decrRefCount(key);
}
void keysCommand(client *c) {
  dictIterator *di;
  dictEntry *de;
  sds pattern = c->argv[1]->ptr;
  int plen = sdslen(pattern), allkeys;
  unsigned long numkeys = 0;
  void *replylen = addReplyDeferredLen(c);
  di = dictGetSafeIterator(c->db->dict);
  allkeys = (pattern[0] == '*' && pattern[1] == '\0');
  while ((de = dictNext(di)) != NULL) {
    sds key = dictGetKey(de);
    robj *keyobj;
    if (allkeys || stringmatchlen(pattern, plen, key, sdslen(key), 0)) {
      keyobj = createStringObject(key, sdslen(key));
      if (!keyIsExpired(c->db, keyobj)) {
        addReplyBulk(c, keyobj);
        numkeys++;
      }
      decrRefCount(keyobj);
    }
  }
  dictReleaseIterator(di);
  setDeferredArrayLen(c, replylen, numkeys);
}
void scanCallback(void *privdata, const dictEntry *de) {
  void **pd = (void **)privdata;
  list *keys = pd[0];
  robj *o = pd[1];
  robj *key, *val = NULL;
  if (o == NULL) {
    sds sdskey = dictGetKey(de);
    key = createStringObject(sdskey, sdslen(sdskey));
  } else if (o->type == OBJ_SET) {
    sds keysds = dictGetKey(de);
    key = createStringObject(keysds, sdslen(keysds));
  } else if (o->type == OBJ_HASH) {
    sds sdskey = dictGetKey(de);
    sds sdsval = dictGetVal(de);
    key = createStringObject(sdskey, sdslen(sdskey));
    val = createStringObject(sdsval, sdslen(sdsval));
  } else if (o->type == OBJ_ZSET) {
    sds sdskey = dictGetKey(de);
    key = createStringObject(sdskey, sdslen(sdskey));
    val = createStringObjectFromLongDouble(*(double *)dictGetVal(de), 0);
  } else {
    serverPanic("Type not handled in SCAN callback.");
  }
  listAddNodeTail(keys, key);
  if (val) listAddNodeTail(keys, val);
}
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
  char *eptr;
  errno = 0;
  *cursor = strtoul(o->ptr, &eptr, 10);
  if (isspace(((char *)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE) {
    addReplyError(c, "invalid cursor");
    return C_ERR;
  }
  return C_OK;
}
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
  int i, j;
  list *keys = listCreate();
  listNode *node, *nextnode;
  long count = 10;
  sds pat = NULL;
  int patlen = 0, use_pattern = 0;
  dict *ht;
  serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
               o->type == OBJ_ZSET);
  i = (o == NULL) ? 2 : 3;
  while (i < c->argc) {
    j = c->argc - i;
    if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
      if (getLongFromObjectOrReply(c, c->argv[i + 1], &count, NULL) != C_OK) {
        goto cleanup;
      }
      if (count < 1) {
        addReply(c, shared.syntaxerr);
        goto cleanup;
      }
      i += 2;
    } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
      pat = c->argv[i + 1]->ptr;
      patlen = sdslen(pat);
      use_pattern = !(pat[0] == '*' && patlen == 1);
      i += 2;
    } else {
      addReply(c, shared.syntaxerr);
      goto cleanup;
    }
  }
  ht = NULL;
  if (o == NULL) {
    ht = c->db->dict;
  } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
    ht = o->ptr;
  } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
    ht = o->ptr;
    count *= 2;
  } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = o->ptr;
    ht = zs->dict;
    count *= 2;
  }
  if (ht) {
    void *privdata[2];
    long maxiterations = count * 10;
    privdata[0] = keys;
    privdata[1] = o;
    do {
      cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
    } while (cursor && maxiterations-- &&
             listLength(keys) < (unsigned long)count);
  } else if (o->type == OBJ_SET) {
    int pos = 0;
    int64_t ll;
    while (intsetGet(o->ptr, pos++, &ll))
      listAddNodeTail(keys, createStringObjectFromLongLong(ll));
    cursor = 0;
  } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
    unsigned char *p = ziplistIndex(o->ptr, 0);
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;
    while (p) {
      ziplistGet(p, &vstr, &vlen, &vll);
      listAddNodeTail(keys, (vstr != NULL)
                                ? createStringObject((char *)vstr, vlen)
                                : createStringObjectFromLongLong(vll));
      p = ziplistNext(o->ptr, p);
    }
    cursor = 0;
  } else {
    serverPanic("Not handled encoding in SCAN.");
  }
  node = listFirst(keys);
  while (node) {
    robj *kobj = listNodeValue(node);
    nextnode = listNextNode(node);
    int filter = 0;
    if (!filter && use_pattern) {
      if (sdsEncodedObject(kobj)) {
        if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
          filter = 1;
      } else {
        char buf[LONG_STR_SIZE];
        int len;
        serverAssert(kobj->encoding == OBJ_ENCODING_INT);
        len = ll2string(buf, sizeof(buf), (long)kobj->ptr);
        if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
      }
    }
    if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;
    if (filter) {
      decrRefCount(kobj);
      listDelNode(keys, node);
    }
    if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
      node = nextnode;
      nextnode = listNextNode(node);
      if (filter) {
        kobj = listNodeValue(node);
        decrRefCount(kobj);
        listDelNode(keys, node);
      }
    }
    node = nextnode;
  }
  addReplyArrayLen(c, 2);
  addReplyBulkLongLong(c, cursor);
  addReplyArrayLen(c, listLength(keys));
  while ((node = listFirst(keys)) != NULL) {
    robj *kobj = listNodeValue(node);
    addReplyBulk(c, kobj);
    decrRefCount(kobj);
    listDelNode(keys, node);
  }
cleanup:
  listSetFreeMethod(keys, decrRefCountVoid);
  listRelease(keys);
}
void scanCommand(client *c) {
  unsigned long cursor;
  if (parseScanCursorOrReply(c, c->argv[1], &cursor) == C_ERR) return;
  scanGenericCommand(c, NULL, cursor);
}
void dbsizeCommand(client *c) { addReplyLongLong(c, dictSize(c->db->dict)); }
void lastsaveCommand(client *c) { addReplyLongLong(c, server.lastsave); }
void typeCommand(client *c) {
  robj *o;
  char *type;
  o = lookupKeyReadWithFlags(c->db, c->argv[1], LOOKUP_NOTOUCH);
  if (o == NULL) {
    type = "none";
  } else {
    switch (o->type) {
      case OBJ_STRING:
        type = "string";
        break;
      case OBJ_LIST:
        type = "list";
        break;
      case OBJ_SET:
        type = "set";
        break;
      case OBJ_ZSET:
        type = "zset";
        break;
      case OBJ_HASH:
        type = "hash";
        break;
      case OBJ_STREAM:
        type = "stream";
        break;
      case OBJ_MODULE: {
        moduleValue *mv = o->ptr;
        type = mv->type->name;
      }; break;
      default:
        type = "unknown";
        break;
    }
  }
  addReplyStatus(c, type);
}
void shutdownCommand(client *c) {
  int flags = 0;
  if (c->argc > 2) {
    addReply(c, shared.syntaxerr);
    return;
  } else if (c->argc == 2) {
    if (!strcasecmp(c->argv[1]->ptr, "nosave")) {
      flags |= SHUTDOWN_NOSAVE;
    } else if (!strcasecmp(c->argv[1]->ptr, "save")) {
      flags |= SHUTDOWN_SAVE;
    } else {
      addReply(c, shared.syntaxerr);
      return;
    }
  }
  if (server.loading || server.sentinel_mode)
    flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;
  if (prepareForShutdown(flags) == C_OK) exit(0);
  addReplyError(c, "Errors trying to SHUTDOWN. Check logs.");
}
void renameGenericCommand(client *c, int nx) {
  robj *o;
  long long expire;
  int samekey = 0;
  if (sdscmp(c->argv[1]->ptr, c->argv[2]->ptr) == 0) samekey = 1;
  if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.nokeyerr)) == NULL)
    return;
  if (samekey) {
    addReply(c, nx ? shared.czero : shared.ok);
    return;
  }
  incrRefCount(o);
  expire = getExpire(c->db, c->argv[1]);
  if (lookupKeyWrite(c->db, c->argv[2]) != NULL) {
    if (nx) {
      decrRefCount(o);
      addReply(c, shared.czero);
      return;
    }
    dbDelete(c->db, c->argv[2]);
  }
  dbAdd(c->db, c->argv[2], o);
  if (expire != -1) setExpire(c, c->db, c->argv[2], expire);
  dbDelete(c->db, c->argv[1]);
  signalModifiedKey(c->db, c->argv[1]);
  signalModifiedKey(c->db, c->argv[2]);
  notifyKeyspaceEvent(NOTIFY_GENERIC, "rename_from", c->argv[1], c->db->id);
  notifyKeyspaceEvent(NOTIFY_GENERIC, "rename_to", c->argv[2], c->db->id);
  server.dirty++;
  addReply(c, nx ? shared.cone : shared.ok);
}
void renameCommand(client *c) { renameGenericCommand(c, 0); }
void renamenxCommand(client *c) { renameGenericCommand(c, 1); }
void moveCommand(client *c) {
  robj *o;
  redisDb *src, *dst;
  int srcid;
  long long dbid, expire;
  if (server.cluster_enabled) {
    addReplyError(c, "MOVE is not allowed in cluster mode");
    return;
  }
  src = c->db;
  srcid = c->db->id;
  if (getLongLongFromObject(c->argv[2], &dbid) == C_ERR || dbid < INT_MIN ||
      dbid > INT_MAX || selectDb(c, dbid) == C_ERR) {
    addReply(c, shared.outofrangeerr);
    return;
  }
  dst = c->db;
  selectDb(c, srcid);
  if (src == dst) {
    addReply(c, shared.sameobjecterr);
    return;
  }
  o = lookupKeyWrite(c->db, c->argv[1]);
  if (!o) {
    addReply(c, shared.czero);
    return;
  }
  expire = getExpire(c->db, c->argv[1]);
  if (lookupKeyWrite(dst, c->argv[1]) != NULL) {
    addReply(c, shared.czero);
    return;
  }
  dbAdd(dst, c->argv[1], o);
  if (expire != -1) setExpire(c, dst, c->argv[1], expire);
  incrRefCount(o);
  dbDelete(src, c->argv[1]);
  server.dirty++;
  addReply(c, shared.cone);
}
void scanDatabaseForReadyLists(redisDb *db) {
  dictEntry *de;
  dictIterator *di = dictGetSafeIterator(db->blocking_keys);
  while ((de = dictNext(di)) != NULL) {
    robj *key = dictGetKey(de);
    robj *value = lookupKey(db, key, LOOKUP_NOTOUCH);
    if (value && (value->type == OBJ_LIST || value->type == OBJ_STREAM ||
                  value->type == OBJ_ZSET))
      signalKeyAsReady(db, key);
  }
  dictReleaseIterator(di);
}
int dbSwapDatabases(int id1, int id2) {
  if (id1 < 0 || id1 >= server.dbnum || id2 < 0 || id2 >= server.dbnum)
    return C_ERR;
  if (id1 == id2) return C_OK;
  redisDb aux = server.db[id1];
  redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];
  db1->dict = db2->dict;
  db1->expires = db2->expires;
  db1->avg_ttl = db2->avg_ttl;
  db2->dict = aux.dict;
  db2->expires = aux.expires;
  db2->avg_ttl = aux.avg_ttl;
  scanDatabaseForReadyLists(db1);
  scanDatabaseForReadyLists(db2);
  return C_OK;
}
void swapdbCommand(client *c) {
  long id1, id2;
  if (server.cluster_enabled) {
    addReplyError(c, "SWAPDB is not allowed in cluster mode");
    return;
  }
  if (getLongFromObjectOrReply(c, c->argv[1], &id1, "invalid first DB index") !=
      C_OK)
    return;
  if (getLongFromObjectOrReply(c, c->argv[2], &id2,
                               "invalid second DB index") != C_OK)
    return;
  if (dbSwapDatabases(id1, id2) == C_ERR) {
    addReplyError(c, "DB index is out of range");
    return;
  } else {
    server.dirty++;
    addReply(c, shared.ok);
  }
}
int removeExpire(redisDb *db, robj *key) {
  serverAssertWithInfo(NULL, key, dictFind(db->dict, key->ptr) != NULL);
  return dictDelete(db->expires, key->ptr) == DICT_OK;
}
void setExpire(client *c, redisDb *db, robj *key, long long when) {
  dictEntry *kde, *de;
  kde = dictFind(db->dict, key->ptr);
  serverAssertWithInfo(NULL, key, kde != NULL);
  de = dictAddOrFind(db->expires, dictGetKey(kde));
  dictSetSignedIntegerVal(de, when);
  int writable_slave = server.masterhost && server.repl_slave_ro == 0;
  if (c && writable_slave && !(c->flags & CLIENT_MASTER))
    rememberSlaveKeyWithExpire(db, key);
}
long long getExpire(redisDb *db, robj *key) {
  dictEntry *de;
  if (dictSize(db->expires) == 0 ||
      (de = dictFind(db->expires, key->ptr)) == NULL)
    return -1;
  serverAssertWithInfo(NULL, key, dictFind(db->dict, key->ptr) != NULL);
  return dictGetSignedIntegerVal(de);
}
void propagateExpire(redisDb *db, robj *key, int lazy) {
  robj *argv[2];
  argv[0] = lazy ? shared.unlink : shared.del;
  argv[1] = key;
  incrRefCount(argv[0]);
  incrRefCount(argv[1]);
  if (server.aof_state != AOF_OFF)
    feedAppendOnlyFile(server.delCommand, db->id, argv, 2);
  replicationFeedSlaves(server.slaves, db->id, argv, 2);
  decrRefCount(argv[0]);
  decrRefCount(argv[1]);
}
int keyIsExpired(redisDb *db, robj *key) {
  mstime_t when = getExpire(db, key);
  if (when < 0) return 0;
  if (server.loading) return 0;
  mstime_t now = server.lua_caller ? server.lua_time_start : mstime();
  return now > when;
}
int expireIfNeeded(redisDb *db, robj *key) {
  if (!keyIsExpired(db, key)) return 0;
  if (server.masterhost != NULL) return 1;
  server.stat_expiredkeys++;
  propagateExpire(db, key, server.lazyfree_lazy_expire);
  notifyKeyspaceEvent(NOTIFY_EXPIRED, "expired", key, db->id);
  return server.lazyfree_lazy_expire ? dbAsyncDelete(db, key)
                                     : dbSyncDelete(db, key);
}
int *getKeysUsingCommandTable(struct redisCommand *cmd, robj **argv, int argc,
                              int *numkeys) {
  int j, i = 0, last, *keys;
  UNUSED(argv);
  if (cmd->firstkey == 0) {
    *numkeys = 0;
    return NULL;
  }
  last = cmd->lastkey;
  if (last < 0) last = argc + last;
  keys = zmalloc(sizeof(int) * ((last - cmd->firstkey) + 1));
  for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
    if (j >= argc) {
      if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
        zfree(keys);
        *numkeys = 0;
        return NULL;
      } else {
        serverPanic(
            "Redis built-in command declared keys positions not matching the "
            "arity requirements.");
      }
    }
    keys[i++] = j;
  }
  *numkeys = i;
  return keys;
}
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc,
                        int *numkeys) {
  if (cmd->flags & CMD_MODULE_GETKEYS) {
    return moduleGetCommandKeysViaAPI(cmd, argv, argc, numkeys);
  } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
    return cmd->getkeys_proc(cmd, argv, argc, numkeys);
  } else {
    return getKeysUsingCommandTable(cmd, argv, argc, numkeys);
  }
}
void getKeysFreeResult(int *result) { zfree(result); }
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                        int *numkeys) {
  int i, num, *keys;
  UNUSED(cmd);
  num = atoi(argv[2]->ptr);
  if (num < 1 || num > (argc - 3)) {
    *numkeys = 0;
    return NULL;
  }
  keys = zmalloc(sizeof(int) * (num + 1));
  for (i = 0; i < num; i++) keys[i] = 3 + i;
  keys[num] = 1;
  *numkeys = num + 1;
  return keys;
}
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                 int *numkeys) {
  int i, num, *keys;
  UNUSED(cmd);
  num = atoi(argv[2]->ptr);
  if (num <= 0 || num > (argc - 3)) {
    *numkeys = 0;
    return NULL;
  }
  keys = zmalloc(sizeof(int) * num);
  *numkeys = num;
  for (i = 0; i < num; i++) keys[i] = 3 + i;
  return keys;
}
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                 int *numkeys) {
  int i, j, num, *keys, found_store = 0;
  UNUSED(cmd);
  num = 0;
  keys = zmalloc(sizeof(int) * 2);
  keys[num++] = 1;
  struct {
    char *name;
    int skip;
  } skiplist[] = {
      {"limit", 2}, {"get", 1}, {"by", 1}, {NULL, 0}
  };
  for (i = 2; i < argc; i++) {
    for (j = 0; skiplist[j].name != NULL; j++) {
      if (!strcasecmp(argv[i]->ptr, skiplist[j].name)) {
        i += skiplist[j].skip;
        break;
      } else if (!strcasecmp(argv[i]->ptr, "store") && i + 1 < argc) {
        found_store = 1;
        keys[num] = i + 1;
        break;
      }
    }
  }
  *numkeys = num + found_store;
  return keys;
}
int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                    int *numkeys) {
  int i, num, first, *keys;
  UNUSED(cmd);
  first = 3;
  num = 1;
  if (argc > 6) {
    for (i = 6; i < argc; i++) {
      if (!strcasecmp(argv[i]->ptr, "keys") && sdslen(argv[3]->ptr) == 0) {
        first = i + 1;
        num = argc - first;
        break;
      }
    }
  }
  keys = zmalloc(sizeof(int) * num);
  for (i = 0; i < num; i++) keys[i] = first + i;
  *numkeys = num;
  return keys;
}
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                      int *numkeys) {
  int i, num, *keys;
  UNUSED(cmd);
  int stored_key = -1;
  for (i = 5; i < argc; i++) {
    char *arg = argv[i]->ptr;
    if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) &&
        ((i + 1) < argc)) {
      stored_key = i + 1;
      i++;
    }
  }
  num = 1 + (stored_key == -1 ? 0 : 1);
  keys = zmalloc(sizeof(int) * num);
  keys[0] = 1;
  if (num > 1) {
    keys[1] = stored_key;
  }
  *numkeys = num;
  return keys;
}
int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc,
                  int *numkeys) {
  int i, num = 0, *keys;
  UNUSED(cmd);
  int streams_pos = -1;
  for (i = 1; i < argc; i++) {
    char *arg = argv[i]->ptr;
    if (!strcasecmp(arg, "block")) {
      i++;
    } else if (!strcasecmp(arg, "count")) {
      i++;
    } else if (!strcasecmp(arg, "group")) {
      i += 2;
    } else if (!strcasecmp(arg, "noack")) {
    } else if (!strcasecmp(arg, "streams")) {
      streams_pos = i;
      break;
    } else {
      break;
    }
  }
  if (streams_pos != -1) num = argc - streams_pos - 1;
  if (streams_pos == -1 || num == 0 || num % 2 != 0) {
    *numkeys = 0;
    return NULL;
  }
  num /= 2;
  keys = zmalloc(sizeof(int) * num);
  for (i = streams_pos + 1; i < argc - num; i++) keys[i - streams_pos - 1] = i;
  *numkeys = num;
  return keys;
}
void slotToKeyUpdateKey(robj *key, int add) {
  unsigned int hashslot = keyHashSlot(key->ptr, sdslen(key->ptr));
  unsigned char buf[64];
  unsigned char *indexed = buf;
  size_t keylen = sdslen(key->ptr);
  server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
  if (keylen + 2 > 64) indexed = zmalloc(keylen + 2);
  indexed[0] = (hashslot >> 8) & 0xff;
  indexed[1] = hashslot & 0xff;
  memcpy(indexed + 2, key->ptr, keylen);
  if (add) {
    raxInsert(server.cluster->slots_to_keys, indexed, keylen + 2, NULL, NULL);
  } else {
    raxRemove(server.cluster->slots_to_keys, indexed, keylen + 2, NULL);
  }
  if (indexed != buf) zfree(indexed);
}
void slotToKeyAdd(robj *key) { slotToKeyUpdateKey(key, 1); }
void slotToKeyDel(robj *key) { slotToKeyUpdateKey(key, 0); }
void slotToKeyFlush(void) {
  raxFree(server.cluster->slots_to_keys);
  server.cluster->slots_to_keys = raxNew();
  memset(server.cluster->slots_keys_count, 0,
         sizeof(server.cluster->slots_keys_count));
}
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys,
                           unsigned int count) {
  raxIterator iter;
  int j = 0;
  unsigned char indexed[2];
  indexed[0] = (hashslot >> 8) & 0xff;
  indexed[1] = hashslot & 0xff;
  raxStart(&iter, server.cluster->slots_to_keys);
  raxSeek(&iter, ">=", indexed, 2);
  while (count-- && raxNext(&iter)) {
    if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
    keys[j++] = createStringObject((char *)iter.key + 2, iter.key_len - 2);
  }
  raxStop(&iter);
  return j;
}
unsigned int delKeysInSlot(unsigned int hashslot) {
  raxIterator iter;
  int j = 0;
  unsigned char indexed[2];
  indexed[0] = (hashslot >> 8) & 0xff;
  indexed[1] = hashslot & 0xff;
  raxStart(&iter, server.cluster->slots_to_keys);
  while (server.cluster->slots_keys_count[hashslot]) {
    raxSeek(&iter, ">=", indexed, 2);
    raxNext(&iter);
    robj *key = createStringObject((char *)iter.key + 2, iter.key_len - 2);
    dbDelete(&server.db[0], key);
    decrRefCount(key);
    j++;
  }
  raxStop(&iter);
  return j;
}
unsigned int countKeysInSlot(unsigned int hashslot) {
  return server.cluster->slots_keys_count[hashslot];
}
