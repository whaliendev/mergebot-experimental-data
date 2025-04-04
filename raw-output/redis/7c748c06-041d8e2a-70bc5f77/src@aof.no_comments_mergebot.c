#include "redis.h"
#include "bio.h"
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "rio.h"
void stopAppendOnly(void) {
    flushAppendOnlyFile(1);
    aof_fsync(server.appendfd);
    close(server.appendfd);
    server.appendfd = -1;
    server.appendseldb = -1;
    server.appendonly = 0;
    if (server.bgrewritechildpid != -1) {
        int statloc;
        if (kill(server.bgrewritechildpid,SIGKILL) != -1)
            wait3(&statloc,0,NULL);
        sdsfree(server.bgrewritebuf);
        server.bgrewritebuf = sdsempty();
        server.bgrewritechildpid = -1;
    }
}
int startAppendOnly(void) {
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
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    if (sdslen(server.aofbuf) == 0) return;
    if (server.appendfsync == APPENDFSYNC_EVERYSEC)
        sync_in_progress = bioPendingJobsOfType(REDIS_BIO_AOF_FSYNC) != 0;
    if (server.appendfsync == APPENDFSYNC_EVERYSEC && !force) {
        if (sync_in_progress) {
            if (server.aof_flush_postponed_start == 0) {
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                return;
            }
            redisLog(REDIS_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    server.aof_flush_postponed_start = 0;
    nwritten = write(server.appendfd,server.aofbuf,sdslen(server.aofbuf));
    if (nwritten != (signed)sdslen(server.aofbuf)) {
        if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
        } else {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to the append-only file: %s",strerror(errno));
        }
        exit(1);
    }
    server.appendonly_current_size += nwritten;
    if ((sdslen(server.aofbuf)+sdsavail(server.aofbuf)) < 4000) {
        sdsclear(server.aofbuf);
    } else {
        sdsfree(server.aofbuf);
        server.aofbuf = sdsempty();
    }
    if (server.no_appendfsync_on_rewrite &&
        (server.bgrewritechildpid != -1 || server.bgsavechildpid != -1))
            return;
    if (server.appendfsync == APPENDFSYNC_ALWAYS) {
        aof_fsync(server.appendfd);
        server.lastfsync = server.unixtime;
    } else if ((server.appendfsync == APPENDFSYNC_EVERYSEC &&
                server.unixtime > server.lastfsync)) {
        if (!sync_in_progress) aof_background_fsync(server.appendfd);
        server.lastfsync = server.unixtime;
    }
}
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;
    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);
    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}
sds catAppendOnlyExpireAtCommand(sds buf, robj *key, robj *seconds) {
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
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
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
struct redisClient *createFakeClient(void) {
    struct redisClient *c = zmalloc(sizeof(*c));
    selectDb(c,0);
    c->fd = -1;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->watched_keys = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}
void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int appendonly = server.appendonly;
    long loops = 0;
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.appendonly_current_size = 0;
        fclose(fp);
        return REDIS_ERR;
    }
    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }
    server.appendonly = 0;
    fakeClient = createFakeClient();
    startLoading(fp);
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }
        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;
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
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);
        redisAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
        redisAssert((fakeClient->flags & REDIS_BLOCKED) == 0);
        for (j = 0; j < fakeClient->argc; j++)
            decrRefCount(fakeClient->argv[j]);
        zfree(fakeClient->argv);
    }
    if (fakeClient->flags & REDIS_MULTI) goto readerr;
    fclose(fp);
    freeFakeClient(fakeClient);
    server.appendonly = appendonly;
    stopLoading();
    aofUpdateCurrentSize();
    server.auto_aofrewrite_base_size = server.appendonly_current_size;
    return REDIS_OK;
readerr:
    if (feof(fp)) {
        redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file");
    } else {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
    }
    exit(1);
fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}
int rioWriteBulkObject(rio *r, robj *obj) {
    if (obj->encoding == REDIS_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (obj->encoding == REDIS_ENCODING_RAW) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        redisPanic("Unknown string encoding");
    }
}
int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    rio aof;
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
    aof = rioInitWithFile(fp);
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }
        if (rioWrite(&aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(&aof,j) == 0) goto werr;
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            time_t expiretime;
            keystr = dictGetEntryKey(de);
            o = dictGetEntryVal(de);
            initStaticStringObject(key,keystr);
            expiretime = getExpire(db,&key);
            if (o->type == REDIS_STRING) {
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(&aof,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                char cmd[]="*3\r\n$5\r\nRPUSH\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *p = ziplistIndex(zl,0);
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vlong;
                    while(ziplistGet(p,&vstr,&vlen,&vlong)) {
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (vstr) {
                            if (rioWriteBulkString(&aof,(char*)vstr,vlen) == 0)
                                goto werr;
                        } else {
                            if (rioWriteBulkLongLong(&aof,vlong) == 0)
                                goto werr;
                        }
                        p = ziplistNext(zl,p);
                    }
                } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
                    list *list = o->ptr;
                    listNode *ln;
                    listIter li;
                    listRewind(list,&li);
                    while((ln = listNext(&li))) {
                        robj *eleobj = listNodeValue(ln);
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,eleobj) == 0) goto werr;
                    }
                } else {
                    redisPanic("Unknown list encoding");
                }
            } else if (o->type == REDIS_SET) {
                char cmd[]="*3\r\n$4\r\nSADD\r\n";
                if (o->encoding == REDIS_ENCODING_INTSET) {
                    int ii = 0;
                    int64_t llval;
                    while(intsetGet(o->ptr,ii++,&llval)) {
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkLongLong(&aof,llval) == 0) goto werr;
                    }
                } else if (o->encoding == REDIS_ENCODING_HT) {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;
                    while((de = dictNext(di)) != NULL) {
                        robj *eleobj = dictGetEntryKey(de);
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,eleobj) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                } else {
                    redisPanic("Unknown set encoding");
                }
            } else if (o->type == REDIS_ZSET) {
                char cmd[]="*4\r\n$4\r\nZADD\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *eptr, *sptr;
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vll;
                    double score;
                    eptr = ziplistIndex(zl,0);
                    redisAssert(eptr != NULL);
                    sptr = ziplistNext(zl,eptr);
                    redisAssert(sptr != NULL);
                    while (eptr != NULL) {
                        redisAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
                        score = zzlGetScore(sptr);
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkDouble(&aof,score) == 0) goto werr;
                        if (vstr != NULL) {
                            if (rioWriteBulkString(&aof,(char*)vstr,vlen) == 0)
                                goto werr;
                        } else {
                            if (rioWriteBulkLongLong(&aof,vll) == 0)
                                goto werr;
                        }
                        zzlNext(zl,&eptr,&sptr);
                    }
                } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
                    zset *zs = o->ptr;
                    dictIterator *di = dictGetIterator(zs->dict);
                    dictEntry *de;
                    while((de = dictNext(di)) != NULL) {
                        robj *eleobj = dictGetEntryKey(de);
                        double *score = dictGetEntryVal(de);
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkDouble(&aof,*score) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,eleobj) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                } else {
                    redisPanic("Unknown sorted set encoding");
                }
            } else if (o->type == REDIS_HASH) {
                char cmd[]="*4\r\n$4\r\nHSET\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                    unsigned char *p = zipmapRewind(o->ptr);
                    unsigned char *field, *val;
                    unsigned int flen, vlen;
                    while((p = zipmapNext(p,&field,&flen,&val,&vlen)) != NULL) {
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkString(&aof,(char*)field,flen) == 0)
                            goto werr;
                        if (rioWriteBulkString(&aof,(char*)val,vlen) == 0)
                            goto werr;
                    }
                } else {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;
                    while((de = dictNext(di)) != NULL) {
                        robj *field = dictGetEntryKey(de);
                        robj *val = dictGetEntryVal(de);
                        if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,field) == 0) goto werr;
                        if (rioWriteBulkObject(&aof,val) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                }
            } else {
                redisPanic("Unknown object type");
            }
            if (expiretime != -1) {
                char cmd[]="*3\r\n$8\r\nEXPIREAT\r\n";
                if (expiretime < now) continue;
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(&aof,expiretime) == 0) goto werr;
            }
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
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    long long start;
    if (server.bgrewritechildpid != -1) return REDIS_ERR;
    start = ustime();
    if ((childpid = fork()) == 0) {
        char tmpfile[256];
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        server.stat_fork_time = ustime()-start;
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
void bgrewriteaofCommand(redisClient *c) {
    if (server.bgrewritechildpid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (server.bgsavechildpid != -1) {
        server.aofrewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}
void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];
    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    if (redis_fstat(server.appendfd,&sb) == -1) {
        redisLog(REDIS_WARNING,"Unable to check the AOF length: %s",
            strerror(errno));
    } else {
        server.appendonly_current_size = sb.st_size;
    }
}
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        int nwritten;
        char tmpfile[256];
        long long now = ustime();
        redisLog(REDIS_NOTICE,
            "Background AOF rewrite terminated with success");
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.bgrewritechildpid);
        newfd = open(tmpfile,O_WRONLY|O_APPEND);
        if (newfd == -1) {
            redisLog(REDIS_WARNING,
                "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
            goto cleanup;
        }
        nwritten = write(newfd,server.bgrewritebuf,sdslen(server.bgrewritebuf));
        if (nwritten != (signed)sdslen(server.bgrewritebuf)) {
            if (nwritten == -1) {
                redisLog(REDIS_WARNING,
                    "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            } else {
                redisLog(REDIS_WARNING,
                    "Short write trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            }
            close(newfd);
            goto cleanup;
        }
        redisLog(REDIS_NOTICE,
            "Parent diff successfully flushed to the rewritten AOF (%lu bytes)", nwritten);
        if (server.appendfd == -1) {
             oldfd = open(server.appendfilename,O_RDONLY|O_NONBLOCK);
        } else {
            oldfd = -1;
        }
        if (rename(tmpfile,server.appendfilename) == -1) {
            redisLog(REDIS_WARNING,
                "Error trying to rename the temporary AOF: %s", strerror(errno));
            close(newfd);
            if (oldfd != -1) close(oldfd);
            goto cleanup;
        }
        if (server.appendfd == -1) {
            close(newfd);
        } else {
            oldfd = server.appendfd;
            server.appendfd = newfd;
            if (server.appendfsync == APPENDFSYNC_ALWAYS)
                aof_fsync(newfd);
            else if (server.appendfsync == APPENDFSYNC_EVERYSEC)
                aof_background_fsync(newfd);
            server.appendseldb = -1;
            aofUpdateCurrentSize();
            server.auto_aofrewrite_base_size = server.appendonly_current_size;
            sdsfree(server.aofbuf);
            server.aofbuf = sdsempty();
        }
        redisLog(REDIS_NOTICE, "Background AOF rewrite successful");
        if (oldfd != -1) bioCreateBackgroundJob(REDIS_BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);
        redisLog(REDIS_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        redisLog(REDIS_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }
cleanup:
    sdsfree(server.bgrewritebuf);
    server.bgrewritebuf = sdsempty();
    aofRemoveTempFile(server.bgrewritechildpid);
    server.bgrewritechildpid = -1;
}void aofUpdateCurrentSize(void);
void aof_background_fsync(int fd) {
    bioCreateBackgroundJob(REDIS_BIO_AOF_FSYNC,(void*)(long)fd,NULL,NULL);
}
