#include "redis.h"
robj *setTypeCreate(robj *value) {
    if (getLongLongFromObject(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}
int setTypeAdd(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (getLongLongFromObject(value,&llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            if (success) {
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            setTypeConvert(subject,REDIS_ENCODING_HT);
            redisAssert(dictAdd(subject->ptr,value,NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
int setTypeRemove(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(subject->ptr,value) == DICT_OK) {
            if (htNeedsResize(subject->ptr)) dictResize(subject->ptr);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (getLongLongFromObject(value,&llval) == REDIS_OK) {
            uint8_t success;
            subject->ptr = intsetRemove(subject->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
int setTypeIsMember(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr,value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (getLongLongFromObject(value,&llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}
setIterator *setTypeInitIterator(robj *subject) {
    setIterator *si = zmalloc(sizeof(setIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}
void setTypeReleaseIterator(setIterator *si) {
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}
robj *setTypeNext(setIterator *si) {
    robj *ret = NULL;
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de != NULL) {
            ret = dictGetEntryKey(de);
            incrRefCount(ret);
        }
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        long long llval;
        if (intsetGet(si->subject->ptr,si->ii++,&llval))
            ret = createStringObjectFromLongLong(llval);
    }
    return ret;
}
robj *setTypeRandomElement(robj *subject) {
    robj *ret = NULL;
    if (subject->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictGetRandomKey(subject->ptr);
        ret = dictGetEntryKey(de);
        incrRefCount(ret);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        long long llval = intsetRandom(subject->ptr);
        ret = createStringObjectFromLongLong(llval);
    } else {
        redisPanic("Unknown set encoding");
    }
    return ret;
}
unsigned long setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict*)subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}
void setTypeConvert(robj *subject, int enc) {
    setIterator *si;
    robj *element;
    redisAssert(subject->type == REDIS_SET);
    if (enc == REDIS_ENCODING_HT) {
        dict *d = dictCreate(&setDictType,NULL);
        dictExpand(d,intsetLen(subject->ptr));
        si = setTypeInitIterator(subject);
        while ((element = setTypeNext(si)) != NULL)
            redisAssert(dictAdd(d,element,NULL) == DICT_OK);
        setTypeReleaseIterator(si);
        subject->encoding = REDIS_ENCODING_HT;
        zfree(subject->ptr);
        subject->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}
void saddCommand(redisClient *c) {
    robj *set;
    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (setTypeAdd(set,c->argv[2])) {
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}
void sremCommand(redisClient *c) {
    robj *set;
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    if (setTypeRemove(set,c->argv[2])) {
        if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}
void smoveCommand(redisClient *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    ele = c->argv[3];
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }
    if (checkType(c,srcset,REDIS_SET) ||
        (dstset && checkType(c,dstset,REDIS_SET))) return;
    if (srcset == dstset) {
        addReply(c,shared.cone);
        return;
    }
    if (!setTypeRemove(srcset,ele)) {
        addReply(c,shared.czero);
        return;
    }
    if (setTypeSize(srcset) == 0) dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[2]);
    server.dirty++;
    if (!dstset) {
        dstset = setTypeCreate(ele);
        dbAdd(c->db,c->argv[2],dstset);
    }
    if (setTypeAdd(dstset,ele)) server.dirty++;
    addReply(c,shared.cone);
}
void sismemberCommand(redisClient *c) {
    robj *set;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    if (setTypeIsMember(set,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}
void scardCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;
    addReplyUlong(c,setTypeSize(o));
}
void spopCommand(redisClient *c) {
    robj *set, *ele;
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    ele = setTypeRandomElement(set);
    if (ele == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        setTypeRemove(set,ele);
        addReplyBulk(c,ele);
        decrRefCount(ele);
        if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}
void srandmemberCommand(redisClient *c) {
    robj *set, *ele;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;
    ele = setTypeRandomElement(set);
    if (ele == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulk(c,ele);
        decrRefCount(ele);
    }
}
int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    return setTypeSize(*(robj**)s1)-setTypeSize(*(robj**)s2);
}
void sinterGenericCommand(redisClient *c, robj **setkeys, unsigned long setnum, robj *dstkey) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setIterator *si;
    robj *ele, *lenobj = NULL, *dstset = NULL;
    unsigned long j, cardinality = 0;
    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->db,dstkey)) {
                    touchWatchedKey(c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);
    if (!dstkey) {
        lenobj = createObject(REDIS_STRING,NULL);
        addReply(c,lenobj);
        decrRefCount(lenobj);
    } else {
        dstset = createIntsetObject();
    }
    si = setTypeInitIterator(sets[0]);
    while((ele = setTypeNext(si)) != NULL) {
        for (j = 1; j < setnum; j++)
            if (!setTypeIsMember(sets[j],ele)) break;
        if (j == setnum) {
            if (!dstkey) {
                addReplyBulk(c,ele);
                cardinality++;
            } else {
                setTypeAdd(dstset,ele);
            }
        }
        decrRefCount(ele);
    }
    setTypeReleaseIterator(si);
    if (dstkey) {
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        touchWatchedKey(c->db,dstkey);
        server.dirty++;
    } else {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    }
    zfree(sets);
}
void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}
void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2
void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;
    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    dstset = createIntsetObject();
    for (j = 0; j < setnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !sets[j]) break;
        if (!sets[j]) continue;
        si = setTypeInitIterator(sets[j]);
        while((ele = setTypeNext(si)) != NULL) {
            if (op == REDIS_OP_UNION || j == 0) {
                if (setTypeAdd(dstset,ele)) {
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (setTypeRemove(dstset,ele)) {
                    cardinality--;
                }
            }
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
        if (op == REDIS_OP_DIFF && cardinality == 0) break;
    }
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNext(si)) != NULL) {
            addReplyBulk(c,ele);
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
        decrRefCount(dstset);
    } else {
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        touchWatchedKey(c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);
}
void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}
void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}
void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}
void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}
