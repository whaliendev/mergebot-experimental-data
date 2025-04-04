#include "redis.h"
#include <math.h>
zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
    zskiplistNode *zn = zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));
    zn->score = score;
    zn->obj = obj;
    return zn;
}
zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;
    zsl = zmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->level[j].forward = NULL;
        zsl->header->level[j].span = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}
void zslFreeNode(zskiplistNode *node) {
    decrRefCount(node->obj);
    zfree(node);
}
void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward, *next;
    zfree(zsl->header);
    while(node) {
        next = node->level[0].forward;
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}
int zslRandomLevel(void) {
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,obj) < 0))) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    level = zslRandomLevel();
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level,score,obj);
    for (i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
    return x;
}
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span -= 1;
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)
        zsl->level--;
    zsl->length--;
}
int zslDelete(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,obj) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0;
    }
    return 0;
}
unsigned long zslDeleteRangeByScore(zskiplist *zsl, double min, double max, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < min)
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    while (x && x->score <= max) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}
zskiplistNode *zslFirstWithScore(zskiplist *zsl, double score) {
    zskiplistNode *x;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < score)
            x = x->level[i].forward;
    }
    return x->level[0].forward;
}
unsigned long zslistTypeGetRank(zskiplist *zsl, double score, robj *o) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,o) <= 0))) {
            rank += x->level[i].span;
            x = x->level[i].forward;
        }
        if (x->obj && equalStringObjects(x->obj,o)) {
            return rank;
        }
    }
    return 0;
}
zskiplistNode* zslistTypeGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
        {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}
typedef struct {
    double min, max;
    int minex, maxex;
} zrangespec;
int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    spec->minex = spec->maxex = 0;
    if (min->encoding == REDIS_ENCODING_INT) {
        spec->min = (long)min->ptr;
    } else {
        if (((char*)min->ptr)[0] == '(') {
            spec->min = strtod((char*)min->ptr+1,NULL);
            spec->minex = 1;
        } else {
            spec->min = strtod((char*)min->ptr,NULL);
        }
    }
    if (max->encoding == REDIS_ENCODING_INT) {
        spec->max = (long)max->ptr;
    } else {
        if (((char*)max->ptr)[0] == '(') {
            spec->max = strtod((char*)max->ptr+1,NULL);
            spec->maxex = 1;
        } else {
            spec->max = strtod((char*)max->ptr,NULL);
        }
    }
    return REDIS_OK;
}
void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double score, int incr, double scoreval, int doincrement) {
    robj *zsetobj;
    zset *zs;
    zskiplistNode *znode;
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
    if (incr) {
        dictEntry *de = dictFind(zs->dict,ele);
        if (de != NULL)
            score += *(double*)dictGetEntryVal(de);
        if (isnan(score)) {
            addReplyError(c,"resulting score is not a number (NaN)");
            return;
        }
    }
    if (dictAdd(zs->dict,ele,NULL) == DICT_OK) {
        dictEntry *de;
        incrRefCount(ele);
        znode = zslInsert(zs->zsl,score,ele);
        incrRefCount(ele);
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        dictGetEntryVal(de) = &znode->score;
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        if (incr)
            addReplyDouble(c,score);
        else
            addReply(c,shared.cone);
    } else {
        dictEntry *de;
        robj *curobj;
        double *curscore;
        int deleted;
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        curobj = dictGetEntryKey(de);
        curscore = dictGetEntryVal(de);
        if (score != *curscore) {
            deleted = zslDelete(zs->zsl,*curscore,curobj);
            redisAssert(deleted != 0);
            znode = zslInsert(zs->zsl,score,curobj);
            incrRefCount(curobj);
            dictGetEntryVal(de) = &znode->score;
            touchWatchedKey(c->db,c->argv[1]);
            server.dirty++;
        }
        if (incr)
            addReplyDouble(c,score);
        else
            addReply(c,shared.czero);
    }
}
void zaddCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}
void zincrbyCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}
void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    dictEntry *de;
    double curscore;
    int deleted;
    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    curscore = *(double*)dictGetEntryVal(de);
    deleted = zslDelete(zs->zsl,curscore,c->argv[2]);
    redisAssert(deleted != 0);
    dictDelete(zs->dict,c->argv[2]);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}
void zremrangebyscoreCommand(redisClient *c) {
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
    if (deleted) touchWatchedKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}
void zremrangebyrankCommand(redisClient *c) {
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
    if (start > end || start >= llen) {
        addReply(c,shared.czero);
        return;
    }
    if (end >= llen) end = llen-1;
    deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    if (deleted) touchWatchedKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c, deleted);
}
typedef struct {
    dict *dict;
    double weight;
} zsetopsrc;
int qsortCompareZsetopsrcByCardinality(const void *s1, const void *s2) {
    zsetopsrc *d1 = (void*) s1, *d2 = (void*) s2;
    unsigned long size1, size2;
    size1 = d1->dict ? dictSize(d1->dict) : 0;
    size2 = d2->dict ? dictSize(d2->dict) : 0;
    return size1 - size2;
}
#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        redisPanic("Unknown ZUNION/INTER aggregate type");
    }
}
void zunionInterGenericCommand(redisClient *c, robj *dstkey, int op) {
    int i, j, setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    robj *dstobj;
    zset *dstzset;
    zskiplistNode *znode;
    dictIterator *di;
    dictEntry *de;
    int touched = 0;
    setnum = atoi(c->argv[2]->ptr);
    if (setnum < 1) {
        addReplyError(c,
            "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
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
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                            "weight value is not a double") != REDIS_OK)
                    {
                        zfree(src);
                        return;
                    }
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
<<<<<<< HEAD
                double score, value;
                score = src[0].weight * zunionInterDictValue(de);
||||||| merged
                double *score = zmalloc(sizeof(double)), value;
                *score = src[0].weight * zunionInterDictValue(de);
=======
                double score, value;
>>>>>>> b4f2e412
                score = src[0].weight * zunionInterDictValue(de);
                for (j = 1; j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(&score,value,aggregate);
                    } else {
                        break;
                    }
                }
<<<<<<< HEAD
                if (j == setnum) {
||||||| merged
                if (j != setnum) {
                    zfree(score);
                } else {
=======
                if (j == setnum) {
>>>>>>> b4f2e412
                    robj *o = dictGetEntryKey(de);
                    znode = zslInsert(dstzset->zsl,score,o);
                    incrRefCount(o);
                    dictAdd(dstzset->dict,o,&znode->score);
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
                double score, value;
<<<<<<< HEAD
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL)
                    continue;
                score = src[i].weight * zunionInterDictValue(de);
||||||| merged
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL) continue;
                double *score = zmalloc(sizeof(double)), value;
                *score = src[i].weight * zunionInterDictValue(de);
=======
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL)
                    continue;
                score = src[i].weight * zunionInterDictValue(de);
>>>>>>> b4f2e412
                for (j = (i+1); j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(&score,value,aggregate);
                    }
                }
                robj *o = dictGetEntryKey(de);
                znode = zslInsert(dstzset->zsl,score,o);
                incrRefCount(o);
                dictAdd(dstzset->dict,o,&znode->score);
                incrRefCount(o);
            }
            dictReleaseIterator(di);
        }
    } else {
        redisAssert(op == REDIS_OP_INTER || op == REDIS_OP_UNION);
    }
    if (dbDelete(c->db,dstkey)) {
        touchWatchedKey(c->db,dstkey);
        touched = 1;
        server.dirty++;
    }
    if (dstzset->zsl->length) {
        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c, dstzset->zsl->length);
        if (!touched) touchWatchedKey(c->db,dstkey);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c, shared.czero);
    }
    zfree(src);
}
void zunionstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_UNION);
}
void zinterstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_INTER);
}
void zrangeGenericCommand(redisClient *c, int reverse) {
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
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;
    if (reverse) {
        ln = start == 0 ? zsl->tail : zslistTypeGetElementByRank(zsl, llen-start);
    } else {
        ln = start == 0 ?
            zsl->header->level[0].forward : zslistTypeGetElementByRank(zsl, start+1);
    }
    addReplyMultiBulkLen(c,withscores ? (rangelen*2) : rangelen);
    for (j = 0; j < rangelen; j++) {
        ele = ln->obj;
        addReplyBulk(c,ele);
        if (withscores)
            addReplyDouble(c,ln->score);
        ln = reverse ? ln->backward : ln->level[0].forward;
    }
}
void zrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,0);
}
void zrevrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,1);
}
void genericZrangebyscoreCommand(redisClient *c, int reverse, int justcount) {
    zrangespec range;
    robj *o, *emptyreply;
    zset *zsetobj;
    zskiplist *zsl;
    zskiplistNode *ln;
    int offset = 0, limit = -1;
    int withscores = 0;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    zslParseRange(c->argv[2],c->argv[3],&range);
    if (c->argc > 4) {
        int remaining = c->argc - 4;
        int pos = 4;
        while (remaining) {
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores")) {
                pos++; remaining--;
                withscores = 1;
            } else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                offset = atoi(c->argv[pos+1]->ptr);
                limit = atoi(c->argv[pos+2]->ptr);
                pos += 3; remaining -= 3;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }
    emptyreply = justcount ? shared.czero : shared.emptymultibulk;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],emptyreply)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;
    zsetobj = o->ptr;
    zsl = zsetobj->zsl;
    ln = zslFirstWithScore(zsl,range.min);
    if (reverse) {
        if (ln == NULL) ln = zsl->tail;
        while (ln && ln->score > range.min) ln = ln->backward;
        if (range.minex) {
            while (ln && ln->score == range.min) ln = ln->backward;
        } else {
            while (ln && ln->level[0].forward &&
                         ln->level[0].forward->score == range.min)
                ln = ln->level[0].forward;
        }
    } else {
<<<<<<< HEAD
        if (range.minex) {
            while (ln && ln->score == range.min) ln = ln->level[0].forward;
        }
    }
||||||| merged
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            robj *ele;
            void *replylen = NULL;
            unsigned long rangelen = 0;
            ln = zslFirstWithScore(zsl,min);
            while (minex && ln && ln->score == min) ln = ln->forward[0];
=======
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            robj *ele;
            void *replylen = NULL;
            unsigned long rangelen = 0;
            ln = zslFirstWithScore(zsl,min);
            while (minex && ln && ln->score == min) ln = ln->level[0].forward;
>>>>>>> b4f2e412
    if (ln == NULL) {
        addReply(c,emptyreply);
        return;
    }
    if (!justcount)
        replylen = addDeferredMultiBulkLength(c);
<<<<<<< HEAD
    while(ln && offset--) {
        if (reverse)
            ln = ln->backward;
        else
            ln = ln->level[0].forward;
||||||| merged
            while(ln && (maxex ? (ln->score < max) : (ln->score <= max))) {
                if (offset) {
                    offset--;
                    ln = ln->forward[0];
                    continue;
=======
            while(ln && (maxex ? (ln->score < max) : (ln->score <= max))) {
                if (offset) {
                    offset--;
                    ln = ln->level[0].forward;
                    continue;
>>>>>>> b4f2e412
    }
    while (ln && limit--) {
        if (reverse) {
            if (range.maxex) {
                if (ln->score <= range.max) break;
            } else {
                if (ln->score < range.max) break;
            }
        } else {
            if (range.maxex) {
                if (ln->score >= range.max) break;
            } else {
                if (ln->score > range.max) break;
            }
        }
        rangelen++;
        if (!justcount) {
            addReplyBulk(c,ln->obj);
            if (withscores)
                addReplyDouble(c,ln->score);
        }
<<<<<<< HEAD
        if (reverse)
            ln = ln->backward;
        else
            ln = ln->level[0].forward;
||||||| merged
                ln = ln->forward[0];
                rangelen++;
                if (limit > 0) limit--;
=======
                ln = ln->level[0].forward;
                rangelen++;
                if (limit > 0) limit--;
>>>>>>> b4f2e412
    }
    if (justcount) {
        addReplyLongLong(c,(long)rangelen);
    } else {
        setDeferredMultiBulkLength(c,replylen,
             withscores ? (rangelen*2) : rangelen);
    }
}
void zrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0,0);
}
void zrevrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,1,0);
}
void zcountCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0,1);
}
void zcardCommand(redisClient *c) {
    robj *o;
    zset *zs;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;
    zs = o->ptr;
    addReplyLongLong(c,zs->zsl->length);
}
void zscoreCommand(redisClient *c) {
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
void zrankGenericCommand(redisClient *c, int reverse) {
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
    rank = zslistTypeGetRank(zsl, *score, c->argv[2]);
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
void zrankCommand(redisClient *c) {
    zrankGenericCommand(c, 0);
}
void zrevrankCommand(redisClient *c) {
    zrankGenericCommand(c, 1);
}
