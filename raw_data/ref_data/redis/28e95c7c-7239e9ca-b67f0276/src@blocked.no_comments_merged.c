#include "server.h"
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit) {
    long long tval;
    if (getLongLongFromObjectOrReply(c,object,&tval,
        "timeout is not an integer or out of range") != C_OK)
        return C_ERR;
    if (tval < 0) {
        addReplyError(c,"timeout is negative");
        return C_ERR;
    }
    if (tval > 0) {
        if (unit == UNIT_SECONDS) tval *= 1000;
        tval += mstime();
    }
    *timeout = tval;
    return C_OK;
}
void blockClient(client *c, int btype) {
    c->flags |= CLIENT_BLOCKED;
    c->btype = btype;
    server.blocked_clients++;
    server.blocked_clients_by_type[btype]++;
}
void processUnblockedClients(void) {
    listNode *ln;
    client *c;
    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;
        if (!(c->flags & CLIENT_BLOCKED)) {
            if (c->querybuf && sdslen(c->querybuf) > 0) {
                processInputBuffer(c);
            }
        }
    }
}
void unblockClient(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    } else if (c->btype == BLOCKED_MODULE) {
        unblockClientFromModule(c);
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }
    server.blocked_clients--;
    server.blocked_clients_by_type[c->btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->btype = BLOCKED_NONE;
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;
        listAddNodeTail(server.unblocked_clients,c);
    }
}
void replyToBlockedClientTimedOut(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        addReply(c,shared.nullmultibulk);
    } else if (c->btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bpop.reploffset));
    } else if (c->btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    } else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;
    listRewind(server.clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        if (c->flags & CLIENT_BLOCKED) {
            addReplySds(c,sdsnew(
                "-UNBLOCKED force unblock from blocking operation, "
                "instance state changed (master -> slave?)\r\n"));
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}
void handleClientsBlockedOnKeys(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;
        l = server.ready_keys;
        server.ready_keys = listCreate();
        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;
            dictDelete(rl->db->ready_keys,rl->key);
            robj *o = lookupKeyWrite(rl->db,rl->key);
            if (o != NULL && o->type == OBJ_LIST) {
                dictEntry *de;
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);
                    while(numclients--) {
                        listNode *clientnode = listFirst(clients);
                        client *receiver = clientnode->value;
                        if (receiver->btype != BLOCKED_LIST) {
                            listDelNode(clients,clientnode);
                            listAddNodeTail(clients,receiver);
                            continue;
                        }
                        robj *dstkey = receiver->bpop.target;
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    LIST_HEAD : LIST_TAIL;
                        robj *value = listTypePop(o,where);
                        if (value) {
                            if (dstkey) incrRefCount(dstkey);
                            unblockClient(receiver);
                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == C_ERR)
                            {
                                    listTypePush(o,value,where);
                            }
                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }
                if (listTypeLength(o) == 0) {
                    dbDelete(rl->db,rl->key);
                    notifyKeyspaceEvent(NOTIFY_GENERIC,"del",rl->key,rl->db->id);
                }
            }
            else if (o != NULL && o->type == OBJ_ZSET) {
                dictEntry *de;
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);
                    unsigned long zcard = zsetLength(o);
                    while(numclients-- && zcard) {
                        listNode *clientnode = listFirst(clients);
                        client *receiver = clientnode->value;
                        if (receiver->btype != BLOCKED_ZSET) {
                            listDelNode(clients,clientnode);
                            listAddNodeTail(clients,receiver);
                            continue;
                        }
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == bzpopminCommand)
                                     ? ZSET_MIN : ZSET_MAX;
                        unblockClient(receiver);
                        genericZpopCommand(receiver,&rl->key,1,where,1,NULL);
                        zcard--;
                        robj *argv[2];
                        struct redisCommand *cmd = where == ZSET_MIN ?
                                                   server.zpopminCommand :
                                                   server.zpopmaxCommand;
                        argv[0] = createStringObject(cmd->name,strlen(cmd->name));
                        argv[1] = rl->key;
                        incrRefCount(rl->key);
                        propagate(cmd,receiver->db->id,
                                  argv,2,PROPAGATE_AOF|PROPAGATE_REPL);
                        decrRefCount(argv[0]);
                        decrRefCount(argv[1]);
                    }
                }
            }
            else if (o != NULL && o->type == OBJ_STREAM) {
                dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
                stream *s = o->ptr;
                if (de) {
                    list *clients = dictGetVal(de);
                    listNode *ln;
                    listIter li;
                    listRewind(clients,&li);
                    while((ln = listNext(&li))) {
                        client *receiver = listNodeValue(ln);
                        if (receiver->btype != BLOCKED_STREAM) continue;
                        streamID *gt = dictFetchValue(receiver->bpop.keys,
                                                      rl->key);
                        streamCG *group = NULL;
                        if (receiver->bpop.xread_group) {
                            group = streamLookupCG(s,
                                    receiver->bpop.xread_group->ptr);
                            if (!group) {
                                addReplyError(receiver,
                                    "-NOGROUP the consumer group this client "
                                    "was blocked on no longer exists");
                                unblockClient(receiver);
                                continue;
                            } else {
                                *gt = group->last_id;
                            }
                        }
                        if (s->last_id.ms > gt->ms ||
                            (s->last_id.ms == gt->ms &&
                             s->last_id.seq > gt->seq))
                        {
                            streamID start = *gt;
                            start.seq++;
                            streamConsumer *consumer = NULL;
                            int noack = 0;
                            if (group) {
                                consumer = streamLookupConsumer(group,
                                           receiver->bpop.xread_consumer->ptr,
                                           1);
                                noack = receiver->bpop.xread_group_noack;
                            }
                            addReplyMultiBulkLen(receiver,1);
                            addReplyMultiBulkLen(receiver,2);
                            addReplyBulk(receiver,rl->key);
                            streamPropInfo pi = {
                                rl->key,
                                receiver->bpop.xread_group
                            };
                            streamReplyWithRange(receiver,s,&start,NULL,
                                                 receiver->bpop.xread_count,
                                                 0, group, consumer, noack, &pi);
                            unblockClient(receiver);
                        }
                    }
                }
            }
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l);
    }
}
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, streamID *ids) {
    dictEntry *de;
    list *l;
    int j;
    c->bpop.timeout = timeout;
    c->bpop.target = target;
    if (target != NULL) incrRefCount(target);
    for (j = 0; j < numkeys; j++) {
        void *key_data = NULL;
        if (btype == BLOCKED_STREAM) {
            key_data = zmalloc(sizeof(streamID));
            memcpy(key_data,ids+j,sizeof(streamID));
        }
        if (dictAdd(c->bpop.keys,keys[j],key_data) != DICT_OK) {
            zfree(key_data);
            continue;
        }
        incrRefCount(keys[j]);
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = dictGetVal(de);
        }
        listAddNodeTail(l,c);
    }
    blockClient(c,btype);
}
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;
    serverAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listDelNode(l,listSearchKey(l,c));
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);
    dictEmpty(c->bpop.keys,NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    if (c->bpop.xread_group) {
        decrRefCount(c->bpop.xread_group);
        decrRefCount(c->bpop.xread_consumer);
        c->bpop.xread_group = NULL;
        c->bpop.xread_consumer = NULL;
    }
}
void signalKeyAsReady(redisDb *db, robj *key) {
    readyList *rl;
    if (dictFind(db->blocking_keys,key) == NULL) return;
    if (dictFind(db->ready_keys,key) != NULL) return;
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}
