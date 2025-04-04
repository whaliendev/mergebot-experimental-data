#include "server.h"
int clientSubscriptionsCount(client *c);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.messagebulk);
    addReplyBulk(c,channel);
    if (msg) addReplyBulk(c,msg);
}
void addReplyPubsubPatMessage(client *c, robj *pat, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[4]);
    else
        addReplyPushLen(c,4);
    addReply(c,shared.pmessagebulk);
    addReplyBulk(c,pat);
    addReplyBulk(c,channel);
    addReplyBulk(c,msg);
}
void addReplyPubsubSubscribed(client *c, robj *channel) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}
void addReplyPubsubUnsubscribed(client *c, robj *channel) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.unsubscribebulk);
    if (channel)
        addReplyBulk(c,channel);
    else
        addReplyNull(c);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}
void addReplyPubsubPatSubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}
void addReplyPubsubPatUnsubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.punsubscribebulk);
    if (pattern)
        addReplyBulk(c,pattern);
    else
        addReplyNull(c);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}
void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;
    decrRefCount(pat->pattern);
    zfree(pat);
}
int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;
    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels)+
           listLength(c->pubsub_patterns);
}
int pubsubSubscribeChannel(client *c, robj *channel) {
    dictEntry *de;
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
            clients = dictGetVal(de);
        }
        listAddNodeTail(clients,c);
    }
    addReplyPubsubSubscribed(c,channel);
    return retval;
}
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;
    incrRefCount(channel);
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {
        retval = 1;
        de = dictFind(server.pubsub_channels,channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            dictDelete(server.pubsub_channels,channel);
        }
    }
    if (notify) addReplyPubsubUnsubscribed(c,channel);
    decrRefCount(channel);
    return retval;
}
int pubsubSubscribePattern(client *c, robj *pattern) {
    dictEntry *de;
    list *clients;
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
        de = dictFind(server.pubsub_patterns_dict,pattern);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_patterns_dict,pattern,clients);
            incrRefCount(pattern);
        } else {
            clients = dictGetVal(de);
        }
        listAddNodeTail(clients,c);
    }
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    dictEntry *de;
    list *clients;
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
        de = dictFind(server.pubsub_patterns_dict,pattern);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            dictDelete(server.pubsub_patterns_dict,pattern);
        }
    }
    if (notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
    return retval;
}
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;
    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetKey(de);
        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    if (notify && count == 0) addReplyPubsubUnsubscribed(c,NULL);
    dictReleaseIterator(di);
    return count;
}
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;
    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;
        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}
int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    dictEntry *de;
    dictIterator *di;
    listNode *ln;
    listIter li;
    de = dictFind(server.pubsub_channels,channel);
    if (de) {
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = ln->value;
            addReplyPubsubMessage(c,channel,message);
            receivers++;
        }
    }
    di = dictGetIterator(server.pubsub_patterns_dict);
    if (di) {
        channel = getDecodedObject(channel);
        while((de = dictNext(di)) != NULL) {
            robj *pattern = dictGetKey(de);
            list *clients = dictGetVal(de);
            if (!stringmatchlen((char*)pattern->ptr,
                                sdslen(pattern->ptr),
                                (char*)channel->ptr,
<<<<<<< HEAD
                                sdslen(channel->ptr),0))
            {
                addReplyPubsubPatMessage(pat->client,
                    pat->pattern,channel,message);
||||||| 3a5bf75ed
                                sdslen(channel->ptr),0)) {
                addReply(pat->client,shared.mbulkhdr[4]);
                addReply(pat->client,shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);
                addReplyBulk(pat->client,channel);
                addReplyBulk(pat->client,message);
=======
                                sdslen(channel->ptr),0)) {
                continue;
            }
            listRewind(clients,&li);
            while ((ln = listNext(&li)) != NULL) {
                client *c = listNodeValue(ln);
                addReply(c,shared.mbulkhdr[4]);
                addReply(c,shared.pmessagebulk);
                addReplyBulk(c,pattern);
                addReplyBulk(c,channel);
                addReplyBulk(c,message);
>>>>>>> dfb12f06
                receivers++;
            }
        }
        decrRefCount(channel);
        dictReleaseIterator(di);
    }
    return receivers;
}
void subscribeCommand(client *c) {
    int j;
    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}
void unsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;
        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}
void psubscribeCommand(client *c) {
    int j;
    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}
void punsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;
        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}
void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    if (server.cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}
void pubsubCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CHANNELS [<pattern>] -- Return the currently active channels matching a pattern (default: all).",
"NUMPAT -- Return number of subscriptions to patterns.",
"NUMSUB [channel-1 .. channel-N] -- Returns the number of subscribers for the specified channels (excluding patterns, default: none).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"channels") &&
        (c->argc == 2 || c->argc == 3))
    {
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;
        dictIterator *di = dictGetIterator(server.pubsub_channels);
        dictEntry *de;
        long mblen = 0;
        void *replylen;
        replylen = addReplyDeferredLen(c);
        while((de = dictNext(di)) != NULL) {
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;
            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        dictReleaseIterator(di);
        setDeferredArrayLen(c,replylen,mblen);
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        int j;
        addReplyArrayLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            list *l = dictFetchValue(server.pubsub_channels,c->argv[j]);
            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c,l ? listLength(l) : 0);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        addReplyLongLong(c,listLength(server.pubsub_patterns));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
