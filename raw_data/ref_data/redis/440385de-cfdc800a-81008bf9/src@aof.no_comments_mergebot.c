#include "server.h"
#include "bio.h"
#include "rio.h"
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>
void aofUpdateCurrentSize(void);
void aofClosePipes(void);
#define AOF_RW_BUF_BLOCK_SIZE (1024 * 1024 * 10)
typedef struct aofrwblock {
  unsigned long used, free;
  char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;
void aofRewriteBufferReset(void) {
  if (server.aof_rewrite_buf_blocks) listRelease(server.aof_rewrite_buf_blocks);
  server.aof_rewrite_buf_blocks = listCreate();
  listSetFreeMethod(server.aof_rewrite_buf_blocks, zfree);
}
unsigned long aofRewriteBufferSize(void) {
  listNode *ln;
  listIter li;
  unsigned long size = 0;
  listRewind(server.aof_rewrite_buf_blocks, &li);
  while ((ln = listNext(&li))) {
    aofrwblock *block = listNodeValue(ln);
    size += block->used;
  }
  return size;
}
void aofChildWriteDiffData(aeEventLoop *el, int fd, void *privdata, int mask) {
  listNode *ln;
  aofrwblock *block;
  ssize_t nwritten;
  UNUSED(el);
  UNUSED(fd);
  UNUSED(privdata);
  UNUSED(mask);
  while (1) {
    ln = listFirst(server.aof_rewrite_buf_blocks);
    block = ln ? ln->value : NULL;
    if (server.aof_stop_sending_diff || !block) {
      aeDeleteFileEvent(server.el, server.aof_pipe_write_data_to_child,
                        AE_WRITABLE);
      return;
    }
    if (block->used > 0) {
      nwritten =
          write(server.aof_pipe_write_data_to_child, block->buf, block->used);
      if (nwritten <= 0) return;
      memmove(block->buf, block->buf + nwritten, block->used - nwritten);
      block->used -= nwritten;
      block->free += nwritten;
    }
    if (block->used == 0) listDelNode(server.aof_rewrite_buf_blocks, ln);
  }
}
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
  listNode *ln = listLast(server.aof_rewrite_buf_blocks);
  aofrwblock *block = ln ? ln->value : NULL;
  while (len) {
    if (block) {
      unsigned long thislen = (block->free < len) ? block->free : len;
      if (thislen) {
        memcpy(block->buf + block->used, s, thislen);
        block->used += thislen;
        block->free -= thislen;
        s += thislen;
        len -= thislen;
      }
    }
    if (len) {
      int numblocks;
      block = zmalloc(sizeof(*block));
      block->free = AOF_RW_BUF_BLOCK_SIZE;
      block->used = 0;
      listAddNodeTail(server.aof_rewrite_buf_blocks, block);
      numblocks = listLength(server.aof_rewrite_buf_blocks);
      if (((numblocks + 1) % 10) == 0) {
        int level = ((numblocks + 1) % 100) == 0 ? LL_WARNING : LL_NOTICE;
        serverLog(level, "Background AOF buffer size: %lu MB",
                  aofRewriteBufferSize() / (1024 * 1024));
      }
    }
  }
  if (aeGetFileEvents(server.el, server.aof_pipe_write_data_to_child) == 0) {
    aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
                      AE_WRITABLE, aofChildWriteDiffData, NULL);
  }
}
ssize_t aofRewriteBufferWrite(int fd) {
  listNode *ln;
  listIter li;
  ssize_t count = 0;
  listRewind(server.aof_rewrite_buf_blocks, &li);
  while ((ln = listNext(&li))) {
    aofrwblock *block = listNodeValue(ln);
    ssize_t nwritten;
    if (block->used) {
      nwritten = write(fd, block->buf, block->used);
      if (nwritten != (ssize_t)block->used) {
        if (nwritten == 0) errno = EIO;
        return -1;
      }
      count += nwritten;
    }
  }
  return count;
}
void aof_background_fsync(int fd) {
  bioCreateBackgroundJob(BIO_AOF_FSYNC, (void *)(long)fd, NULL, NULL);
}
static void killAppendOnlyChild(void) {
  int statloc;
  if (server.aof_child_pid == -1) return;
  serverLog(LL_NOTICE, "Killing running AOF rewrite child: %ld",
            (long)server.aof_child_pid);
  if (kill(server.aof_child_pid, SIGUSR1) != -1) {
    while (wait3(&statloc, 0, NULL) != server.aof_child_pid)
      ;
  }
  aofRewriteBufferReset();
  aofRemoveTempFile(server.aof_child_pid);
  server.aof_child_pid = -1;
  server.aof_rewrite_time_start = -1;
  aofClosePipes();
<<<<<<< HEAD
  closeChildInfoPipe();
|||||||
=======
  updateDictResizePolicy();
>>>>>>> cfdc800a5ff5a2bb02ccd1e21c1c36e6cb5a474d
}
void stopAppendOnly(void) {
  serverAssert(server.aof_state != AOF_OFF);
  flushAppendOnlyFile(1);
  redis_fsync(server.aof_fd);
  close(server.aof_fd);
  server.aof_fd = -1;
  server.aof_selected_db = -1;
  server.aof_state = AOF_OFF;
  killAppendOnlyChild();
}
int startAppendOnly(void) {
  char cwd[MAXPATHLEN];
  int newfd;
  newfd = open(server.aof_filename, O_WRONLY | O_APPEND | O_CREAT, 0644);
  serverAssert(server.aof_state == AOF_OFF);
  if (newfd == -1) {
    char *cwdp = getcwd(cwd, MAXPATHLEN);
    serverLog(LL_WARNING,
              "Redis needs to enable the AOF but can't open the "
              "append only file %s (in server root dir %s): %s",
              server.aof_filename, cwdp ? cwdp : "unknown", strerror(errno));
    return C_ERR;
  }
  if (server.rdb_child_pid != -1) {
    server.aof_rewrite_scheduled = 1;
    serverLog(LL_WARNING,
              "AOF was enabled but there is already a child process saving an "
              "RDB file on disk. An AOF background was scheduled to start when "
              "possible.");
  } else {
    if (server.aof_child_pid != -1) {
      serverLog(
          LL_WARNING,
          "AOF was enabled but there is already an AOF rewriting in "
          "background. Stopping background AOF and starting a rewrite now.");
      killAppendOnlyChild();
    }
    if (rewriteAppendOnlyFileBackground() == C_ERR) {
      close(newfd);
      serverLog(LL_WARNING,
                "Redis needs to enable the AOF but can't trigger a background "
                "AOF rewrite operation. Check the above logs for more info "
                "about the error.");
      return C_ERR;
    }
  }
  server.aof_state = AOF_WAIT_REWRITE;
  server.aof_last_fsync = server.unixtime;
  server.aof_fd = newfd;
  return C_OK;
}
ssize_t aofWrite(int fd, const char *buf, size_t len) {
  ssize_t nwritten = 0, totwritten = 0;
  while (len) {
    nwritten = write(fd, buf, len);
    if (nwritten < 0) {
      if (errno == EINTR) {
        continue;
      }
      return totwritten ? totwritten : -1;
    }
    len -= nwritten;
    buf += nwritten;
    totwritten += nwritten;
  }
  return totwritten;
}
#define AOF_WRITE_LOG_ERROR_RATE 30
void flushAppendOnlyFile(int force) {
  ssize_t nwritten;
  int sync_in_progress = 0;
  mstime_t latency;
  if (sdslen(server.aof_buf) == 0) return;
  if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
    sync_in_progress = bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
  if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
    if (sync_in_progress) {
      if (server.aof_flush_postponed_start == 0) {
        server.aof_flush_postponed_start = server.unixtime;
        return;
      } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
        return;
      }
      server.aof_delayed_fsync++;
      serverLog(LL_NOTICE,
                "Asynchronous AOF fsync is taking too long (disk is busy?). "
                "Writing the AOF buffer without waiting for fsync to complete, "
                "this may slow down Redis.");
    }
  }
  latencyStartMonitor(latency);
  nwritten = aofWrite(server.aof_fd, server.aof_buf, sdslen(server.aof_buf));
  latencyEndMonitor(latency);
  if (sync_in_progress) {
    latencyAddSampleIfNeeded("aof-write-pending-fsync", latency);
  } else if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) {
    latencyAddSampleIfNeeded("aof-write-active-child", latency);
  } else {
    latencyAddSampleIfNeeded("aof-write-alone", latency);
  }
  latencyAddSampleIfNeeded("aof-write", latency);
  server.aof_flush_postponed_start = 0;
  if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
    static time_t last_write_error_log = 0;
    int can_log = 0;
    if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
      can_log = 1;
      last_write_error_log = server.unixtime;
    }
    if (nwritten == -1) {
      if (can_log) {
        serverLog(LL_WARNING, "Error writing to the AOF file: %s",
                  strerror(errno));
        server.aof_last_write_errno = errno;
      }
    } else {
      if (can_log) {
        serverLog(LL_WARNING,
                  "Short write while writing to "
                  "the AOF file: (nwritten=%lld, "
                  "expected=%lld)",
                  (long long)nwritten, (long long)sdslen(server.aof_buf));
      }
      if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
        if (can_log) {
          serverLog(LL_WARNING,
                    "Could not remove short write "
                    "from the append-only file.  Redis may refuse "
                    "to load the AOF the next time it starts.  "
                    "ftruncate: %s",
                    strerror(errno));
        }
      } else {
        nwritten = -1;
      }
      server.aof_last_write_errno = ENOSPC;
    }
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
      serverLog(LL_WARNING,
                "Can't recover from AOF write error when the AOF fsync policy "
                "is 'always'. Exiting...");
      exit(1);
    } else {
      server.aof_last_write_status = C_ERR;
      if (nwritten > 0) {
        server.aof_current_size += nwritten;
        sdsrange(server.aof_buf, nwritten, -1);
      }
      return;
    }
  } else {
    if (server.aof_last_write_status == C_ERR) {
      serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
      server.aof_last_write_status = C_OK;
    }
  }
  server.aof_current_size += nwritten;
  if ((sdslen(server.aof_buf) + sdsavail(server.aof_buf)) < 4000) {
    sdsclear(server.aof_buf);
  } else {
    sdsfree(server.aof_buf);
    server.aof_buf = sdsempty();
  }
  if (server.aof_no_fsync_on_rewrite &&
      (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
    return;
  if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
    latencyStartMonitor(latency);
    redis_fsync(server.aof_fd);
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fsync-always", latency);
    server.aof_last_fsync = server.unixtime;
  } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
              server.unixtime > server.aof_last_fsync)) {
    if (!sync_in_progress) aof_background_fsync(server.aof_fd);
    server.aof_last_fsync = server.unixtime;
  }
}
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
  char buf[32];
  int len, j;
  robj *o;
  buf[0] = '*';
  len = 1 + ll2string(buf + 1, sizeof(buf) - 1, argc);
  buf[len++] = '\r';
  buf[len++] = '\n';
  dst = sdscatlen(dst, buf, len);
  for (j = 0; j < argc; j++) {
    o = getDecodedObject(argv[j]);
    buf[0] = '$';
    len = 1 + ll2string(buf + 1, sizeof(buf) - 1, sdslen(o->ptr));
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst, buf, len);
    dst = sdscatlen(dst, o->ptr, sdslen(o->ptr));
    dst = sdscatlen(dst, "\r\n", 2);
    decrRefCount(o);
  }
  return dst;
}
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key,
                                 robj *seconds) {
  long long when;
  robj *argv[3];
  seconds = getDecodedObject(seconds);
  when = strtoll(seconds->ptr, NULL, 10);
  if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
      cmd->proc == expireatCommand) {
    when *= 1000;
  }
  if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
      cmd->proc == setexCommand || cmd->proc == psetexCommand) {
    when += mstime();
  }
  decrRefCount(seconds);
  argv[0] = createStringObject("PEXPIREAT", 9);
  argv[1] = key;
  argv[2] = createStringObjectFromLongLong(when);
  buf = catAppendOnlyGenericCommand(buf, 3, argv);
  decrRefCount(argv[0]);
  decrRefCount(argv[2]);
  return buf;
}
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv,
                        int argc) {
  sds buf = sdsempty();
  robj *tmpargv[3];
  if (dictid != server.aof_selected_db) {
    char seldb[64];
    snprintf(seldb, sizeof(seldb), "%d", dictid);
    buf = sdscatprintf(buf, "*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
                       (unsigned long)strlen(seldb), seldb);
    server.aof_selected_db = dictid;
  }
  if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
      cmd->proc == expireatCommand) {
    buf = catAppendOnlyExpireAtCommand(buf, cmd, argv[1], argv[2]);
  } else if (cmd->proc == setexCommand || cmd->proc == psetexCommand) {
    tmpargv[0] = createStringObject("SET", 3);
    tmpargv[1] = argv[1];
    tmpargv[2] = argv[3];
    buf = catAppendOnlyGenericCommand(buf, 3, tmpargv);
    decrRefCount(tmpargv[0]);
    buf = catAppendOnlyExpireAtCommand(buf, cmd, argv[1], argv[2]);
  } else if (cmd->proc == setCommand && argc > 3) {
    int i;
    robj *exarg = NULL, *pxarg = NULL;
    buf = catAppendOnlyGenericCommand(buf, 3, argv);
    for (i = 3; i < argc; i++) {
      if (!strcasecmp(argv[i]->ptr, "ex")) exarg = argv[i + 1];
      if (!strcasecmp(argv[i]->ptr, "px")) pxarg = argv[i + 1];
    }
    serverAssert(!(exarg && pxarg));
    if (exarg)
      buf = catAppendOnlyExpireAtCommand(buf, server.expireCommand, argv[1],
                                         exarg);
    if (pxarg)
      buf = catAppendOnlyExpireAtCommand(buf, server.pexpireCommand, argv[1],
                                         pxarg);
  } else {
    buf = catAppendOnlyGenericCommand(buf, argc, argv);
  }
  if (server.aof_state == AOF_ON)
    server.aof_buf = sdscatlen(server.aof_buf, buf, sdslen(buf));
  if (server.aof_child_pid != -1)
    aofRewriteBufferAppend((unsigned char *)buf, sdslen(buf));
  sdsfree(buf);
}
struct client *createFakeClient(void) {
  struct client *c = zmalloc(sizeof(*c));
  selectDb(c, 0);
  c->fd = -1;
  c->name = NULL;
  c->querybuf = sdsempty();
  c->querybuf_peak = 0;
  c->argc = 0;
  c->argv = NULL;
  c->bufpos = 0;
  c->flags = 0;
  c->btype = BLOCKED_NONE;
  c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
  c->reply = listCreate();
  c->reply_bytes = 0;
  c->obuf_soft_limit_reached_time = 0;
  c->watched_keys = listCreate();
  c->peerid = NULL;
  c->resp = 2;
  c->user = NULL;
  listSetFreeMethod(c->reply, freeClientReplyValue);
  listSetDupMethod(c->reply, dupClientReplyValue);
  initClientMultiState(c);
  return c;
}
void freeFakeClientArgv(struct client *c) {
  int j;
  for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
  zfree(c->argv);
}
void freeFakeClient(struct client *c) {
  sdsfree(c->querybuf);
  listRelease(c->reply);
  listRelease(c->watched_keys);
  freeClientMultiState(c);
  zfree(c);
}
int loadAppendOnlyFile(char *filename) {
  struct client *fakeClient;
  FILE *fp = fopen(filename, "r");
  struct redis_stat sb;
  int old_aof_state = server.aof_state;
  long loops = 0;
  off_t valid_up_to = 0;
  off_t valid_before_multi = 0;
  if (fp == NULL) {
    serverLog(LL_WARNING,
              "Fatal error: can't open the append log file for reading: %s",
              strerror(errno));
    exit(1);
  }
  if (fp && redis_fstat(fileno(fp), &sb) != -1 && sb.st_size == 0) {
    server.aof_current_size = 0;
    fclose(fp);
    return C_ERR;
  }
  server.aof_state = AOF_OFF;
  fakeClient = createFakeClient();
  startLoading(fp);
  char sig[5];
  if (fread(sig, 1, 5, fp) != 5 || memcmp(sig, "REDIS", 5) != 0) {
    if (fseek(fp, 0, SEEK_SET) == -1) goto readerr;
  } else {
    rio rdb;
    serverLog(LL_NOTICE, "Reading RDB preamble from AOF file...");
    if (fseek(fp, 0, SEEK_SET) == -1) goto readerr;
    rioInitWithFile(&rdb, fp);
    if (rdbLoadRio(&rdb, NULL, 1) != C_OK) {
      serverLog(LL_WARNING,
                "Error reading the RDB preamble of the AOF file, AOF loading "
                "aborted");
      goto readerr;
    } else {
      serverLog(LL_NOTICE, "Reading the remaining AOF tail...");
    }
  }
  while (1) {
    int argc, j;
    unsigned long len;
    robj **argv;
    char buf[128];
    sds argsds;
    struct redisCommand *cmd;
    if (!(loops++ % 1000)) {
      loadingProgress(ftello(fp));
      processEventsWhileBlocked();
    }
    if (fgets(buf, sizeof(buf), fp) == NULL) {
      if (feof(fp))
        break;
      else
        goto readerr;
    }
    if (buf[0] != '*') goto fmterr;
    if (buf[1] == '\0') goto readerr;
    argc = atoi(buf + 1);
    if (argc < 1) goto fmterr;
    argv = zmalloc(sizeof(robj *) * argc);
    fakeClient->argc = argc;
    fakeClient->argv = argv;
    for (j = 0; j < argc; j++) {
      if (fgets(buf, sizeof(buf), fp) == NULL) {
        fakeClient->argc = j;
        freeFakeClientArgv(fakeClient);
        goto readerr;
      }
      if (buf[0] != '$') goto fmterr;
      len = strtol(buf + 1, NULL, 10);
      argsds = sdsnewlen(SDS_NOINIT, len);
      if (len && fread(argsds, len, 1, fp) == 0) {
        sdsfree(argsds);
        fakeClient->argc = j;
        freeFakeClientArgv(fakeClient);
        goto readerr;
      }
      argv[j] = createObject(OBJ_STRING, argsds);
      if (fread(buf, 2, 1, fp) == 0) {
        fakeClient->argc = j + 1;
        freeFakeClientArgv(fakeClient);
        goto readerr;
      }
    }
    cmd = lookupCommand(argv[0]->ptr);
    if (!cmd) {
      serverLog(LL_WARNING, "Unknown command '%s' reading the append only file",
                (char *)argv[0]->ptr);
      exit(1);
    }
    if (cmd == server.multiCommand) valid_before_multi = valid_up_to;
    fakeClient->cmd = cmd;
    if (fakeClient->flags & CLIENT_MULTI &&
        fakeClient->cmd->proc != execCommand) {
      queueMultiCommand(fakeClient);
    } else {
      cmd->proc(fakeClient);
    }
    serverAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
    serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);
    freeFakeClientArgv(fakeClient);
    fakeClient->cmd = NULL;
    if (server.aof_load_truncated) valid_up_to = ftello(fp);
  }
  if (fakeClient->flags & CLIENT_MULTI) {
    serverLog(LL_WARNING,
              "Revert incomplete MULTI/EXEC transaction in AOF file");
    valid_up_to = valid_before_multi;
    goto uxeof;
  }
loaded_ok:
  fclose(fp);
  freeFakeClient(fakeClient);
  server.aof_state = old_aof_state;
  stopLoading();
  aofUpdateCurrentSize();
  server.aof_rewrite_base_size = server.aof_current_size;
  return C_OK;
readerr:
  if (!feof(fp)) {
    if (fakeClient) freeFakeClient(fakeClient);
    serverLog(LL_WARNING,
              "Unrecoverable error reading the append only file: %s",
              strerror(errno));
    exit(1);
  }
uxeof:
  if (server.aof_load_truncated) {
    serverLog(LL_WARNING,
              "!!! Warning: short read while loading the AOF file !!!");
    serverLog(LL_WARNING, "!!! Truncating the AOF at offset %llu !!!",
              (unsigned long long)valid_up_to);
    if (valid_up_to == -1 || truncate(filename, valid_up_to) == -1) {
      if (valid_up_to == -1) {
        serverLog(LL_WARNING, "Last valid command offset is invalid");
      } else {
        serverLog(LL_WARNING, "Error truncating the AOF file: %s",
                  strerror(errno));
      }
    } else {
      if (server.aof_fd != -1 && lseek(server.aof_fd, 0, SEEK_END) == -1) {
        serverLog(LL_WARNING, "Can't seek the end of the AOF file: %s",
                  strerror(errno));
      } else {
        serverLog(LL_WARNING,
                  "AOF loaded anyway because aof-load-truncated is enabled");
        goto loaded_ok;
      }
    }
  }
  if (fakeClient) freeFakeClient(fakeClient);
  serverLog(LL_WARNING,
            "Unexpected end of file reading the append only file. You can: 1) "
            "Make a backup of your AOF file, then use ./redis-check-aof --fix "
            "<filename>. 2) Alternatively you can set the 'aof-load-truncated' "
            "configuration option to yes and restart the server.");
  exit(1);
fmterr:
  if (fakeClient) freeFakeClient(fakeClient);
  serverLog(LL_WARNING,
            "Bad file format reading the append only file: make a backup of "
            "your AOF file, then use ./redis-check-aof --fix <filename>");
  exit(1);
}
int rioWriteBulkObject(rio *r, robj *obj) {
  if (obj->encoding == OBJ_ENCODING_INT) {
    return rioWriteBulkLongLong(r, (long)obj->ptr);
  } else if (sdsEncodedObject(obj)) {
    return rioWriteBulkString(r, obj->ptr, sdslen(obj->ptr));
  } else {
    serverPanic("Unknown string encoding");
  }
}
int rewriteListObject(rio *r, robj *key, robj *o) {
  long long count = 0, items = listTypeLength(o);
  if (o->encoding == OBJ_ENCODING_QUICKLIST) {
    quicklist *list = o->ptr;
    quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
    quicklistEntry entry;
    while (quicklistNext(li, &entry)) {
      if (count == 0) {
        int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                            ? AOF_REWRITE_ITEMS_PER_CMD
                            : items;
        if (rioWriteBulkCount(r, '*', 2 + cmd_items) == 0) return 0;
        if (rioWriteBulkString(r, "RPUSH", 5) == 0) return 0;
        if (rioWriteBulkObject(r, key) == 0) return 0;
      }
      if (entry.value) {
        if (rioWriteBulkString(r, (char *)entry.value, entry.sz) == 0) return 0;
      } else {
        if (rioWriteBulkLongLong(r, entry.longval) == 0) return 0;
      }
      if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
      items--;
    }
    quicklistReleaseIterator(li);
  } else {
    serverPanic("Unknown list encoding");
  }
  return 1;
}
int rewriteSetObject(rio *r, robj *key, robj *o) {
  long long count = 0, items = setTypeSize(o);
  if (o->encoding == OBJ_ENCODING_INTSET) {
    int ii = 0;
    int64_t llval;
    while (intsetGet(o->ptr, ii++, &llval)) {
      if (count == 0) {
        int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                            ? AOF_REWRITE_ITEMS_PER_CMD
                            : items;
        if (rioWriteBulkCount(r, '*', 2 + cmd_items) == 0) return 0;
        if (rioWriteBulkString(r, "SADD", 4) == 0) return 0;
        if (rioWriteBulkObject(r, key) == 0) return 0;
      }
      if (rioWriteBulkLongLong(r, llval) == 0) return 0;
      if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
      items--;
    }
  } else if (o->encoding == OBJ_ENCODING_HT) {
    dictIterator *di = dictGetIterator(o->ptr);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
      sds ele = dictGetKey(de);
      if (count == 0) {
        int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                            ? AOF_REWRITE_ITEMS_PER_CMD
                            : items;
        if (rioWriteBulkCount(r, '*', 2 + cmd_items) == 0) return 0;
        if (rioWriteBulkString(r, "SADD", 4) == 0) return 0;
        if (rioWriteBulkObject(r, key) == 0) return 0;
      }
      if (rioWriteBulkString(r, ele, sdslen(ele)) == 0) return 0;
      if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
      items--;
    }
    dictReleaseIterator(di);
  } else {
    serverPanic("Unknown set encoding");
  }
  return 1;
}
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
  long long count = 0, items = zsetLength(o);
  if (o->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = o->ptr;
    unsigned char *eptr, *sptr;
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;
    double score;
    eptr = ziplistIndex(zl, 0);
    serverAssert(eptr != NULL);
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);
    while (eptr != NULL) {
      serverAssert(ziplistGet(eptr, &vstr, &vlen, &vll));
      score = zzlGetScore(sptr);
      if (count == 0) {
        int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                            ? AOF_REWRITE_ITEMS_PER_CMD
                            : items;
        if (rioWriteBulkCount(r, '*', 2 + cmd_items * 2) == 0) return 0;
        if (rioWriteBulkString(r, "ZADD", 4) == 0) return 0;
        if (rioWriteBulkObject(r, key) == 0) return 0;
      }
      if (rioWriteBulkDouble(r, score) == 0) return 0;
      if (vstr != NULL) {
        if (rioWriteBulkString(r, (char *)vstr, vlen) == 0) return 0;
      } else {
        if (rioWriteBulkLongLong(r, vll) == 0) return 0;
      }
      zzlNext(zl, &eptr, &sptr);
      if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
      items--;
    }
  } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = o->ptr;
    dictIterator *di = dictGetIterator(zs->dict);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
      sds ele = dictGetKey(de);
      double *score = dictGetVal(de);
      if (count == 0) {
        int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                            ? AOF_REWRITE_ITEMS_PER_CMD
                            : items;
        if (rioWriteBulkCount(r, '*', 2 + cmd_items * 2) == 0) return 0;
        if (rioWriteBulkString(r, "ZADD", 4) == 0) return 0;
        if (rioWriteBulkObject(r, key) == 0) return 0;
      }
      if (rioWriteBulkDouble(r, *score) == 0) return 0;
      if (rioWriteBulkString(r, ele, sdslen(ele)) == 0) return 0;
      if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
      items--;
    }
    dictReleaseIterator(di);
  } else {
    serverPanic("Unknown sorted zset encoding");
  }
  return 1;
}
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
  if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;
    hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
    if (vstr)
      return rioWriteBulkString(r, (char *)vstr, vlen);
    else
      return rioWriteBulkLongLong(r, vll);
  } else if (hi->encoding == OBJ_ENCODING_HT) {
    sds value = hashTypeCurrentFromHashTable(hi, what);
    return rioWriteBulkString(r, value, sdslen(value));
  }
  serverPanic("Unknown hash encoding");
  return 0;
}
int rewriteHashObject(rio *r, robj *key, robj *o) {
  hashTypeIterator *hi;
  long long count = 0, items = hashTypeLength(o);
  hi = hashTypeInitIterator(o);
  while (hashTypeNext(hi) != C_ERR) {
    if (count == 0) {
      int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD)
                          ? AOF_REWRITE_ITEMS_PER_CMD
                          : items;
      if (rioWriteBulkCount(r, '*', 2 + cmd_items * 2) == 0) return 0;
      if (rioWriteBulkString(r, "HMSET", 5) == 0) return 0;
      if (rioWriteBulkObject(r, key) == 0) return 0;
    }
    if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) == 0) return 0;
    if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE) == 0) return 0;
    if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
    items--;
  }
  hashTypeReleaseIterator(hi);
  return 1;
}
int rioWriteBulkStreamID(rio *r, streamID *id) {
  int retval;
  sds replyid = sdscatfmt(sdsempty(), "%U-%U", id->ms, id->seq);
  if ((retval = rioWriteBulkString(r, replyid, sdslen(replyid))) == 0) return 0;
  sdsfree(replyid);
  return retval;
}
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname,
                               size_t groupname_len, streamConsumer *consumer,
                               unsigned char *rawid, streamNACK *nack) {
  streamID id;
  streamDecodeID(rawid, &id);
  if (rioWriteBulkCount(r, '*', 12) == 0) return 0;
  if (rioWriteBulkString(r, "XCLAIM", 6) == 0) return 0;
  if (rioWriteBulkObject(r, key) == 0) return 0;
  if (rioWriteBulkString(r, groupname, groupname_len) == 0) return 0;
  if (rioWriteBulkString(r, consumer->name, sdslen(consumer->name)) == 0)
    return 0;
  if (rioWriteBulkString(r, "0", 1) == 0) return 0;
  if (rioWriteBulkStreamID(r, &id) == 0) return 0;
  if (rioWriteBulkString(r, "TIME", 4) == 0) return 0;
  if (rioWriteBulkLongLong(r, nack->delivery_time) == 0) return 0;
  if (rioWriteBulkString(r, "RETRYCOUNT", 10) == 0) return 0;
  if (rioWriteBulkLongLong(r, nack->delivery_count) == 0) return 0;
  if (rioWriteBulkString(r, "JUSTID", 6) == 0) return 0;
  if (rioWriteBulkString(r, "FORCE", 5) == 0) return 0;
  return 1;
}
int rewriteStreamObject(rio *r, robj *key, robj *o) {
  stream *s = o->ptr;
  streamIterator si;
  streamIteratorStart(&si, s, NULL, NULL, 0);
  streamID id;
  int64_t numfields;
  if (s->length) {
    while (streamIteratorGetID(&si, &id, &numfields)) {
      if (rioWriteBulkCount(r, '*', 3 + numfields * 2) == 0) return 0;
      if (rioWriteBulkString(r, "XADD", 4) == 0) return 0;
      if (rioWriteBulkObject(r, key) == 0) return 0;
      if (rioWriteBulkStreamID(r, &id) == 0) return 0;
      while (numfields--) {
        unsigned char *field, *value;
        int64_t field_len, value_len;
        streamIteratorGetField(&si, &field, &value, &field_len, &value_len);
        if (rioWriteBulkString(r, (char *)field, field_len) == 0) return 0;
        if (rioWriteBulkString(r, (char *)value, value_len) == 0) return 0;
      }
    }
  } else {
    if (rioWriteBulkCount(r, '*', 7) == 0) return 0;
    if (rioWriteBulkString(r, "XADD", 4) == 0) return 0;
    if (rioWriteBulkObject(r, key) == 0) return 0;
    if (rioWriteBulkString(r, "MAXLEN", 6) == 0) return 0;
    if (rioWriteBulkString(r, "0", 1) == 0) return 0;
    if (rioWriteBulkStreamID(r, &s->last_id) == 0) return 0;
    if (rioWriteBulkString(r, "x", 1) == 0) return 0;
    if (rioWriteBulkString(r, "y", 1) == 0) return 0;
  }
  if (rioWriteBulkCount(r, '*', 3) == 0) return 0;
  if (rioWriteBulkString(r, "XSETID", 6) == 0) return 0;
  if (rioWriteBulkObject(r, key) == 0) return 0;
  if (rioWriteBulkStreamID(r, &s->last_id) == 0) return 0;
  if (s->cgroups) {
    raxIterator ri;
    raxStart(&ri, s->cgroups);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
      streamCG *group = ri.data;
      if (rioWriteBulkCount(r, '*', 5) == 0) return 0;
      if (rioWriteBulkString(r, "XGROUP", 6) == 0) return 0;
      if (rioWriteBulkString(r, "CREATE", 6) == 0) return 0;
      if (rioWriteBulkObject(r, key) == 0) return 0;
      if (rioWriteBulkString(r, (char *)ri.key, ri.key_len) == 0) return 0;
      if (rioWriteBulkStreamID(r, &group->last_id) == 0) return 0;
      raxIterator ri_cons;
      raxStart(&ri_cons, group->consumers);
      raxSeek(&ri_cons, "^", NULL, 0);
      while (raxNext(&ri_cons)) {
        streamConsumer *consumer = ri_cons.data;
        raxIterator ri_pel;
        raxStart(&ri_pel, consumer->pel);
        raxSeek(&ri_pel, "^", NULL, 0);
        while (raxNext(&ri_pel)) {
          streamNACK *nack = ri_pel.data;
          if (rioWriteStreamPendingEntry(r, key, (char *)ri.key, ri.key_len,
                                         consumer, ri_pel.key, nack) == 0) {
            return 0;
          }
        }
        raxStop(&ri_pel);
      }
      raxStop(&ri_cons);
    }
    raxStop(&ri);
  }
  streamIteratorStop(&si);
  return 1;
}
int rewriteModuleObject(rio *r, robj *key, robj *o) {
  RedisModuleIO io;
  moduleValue *mv = o->ptr;
  moduleType *mt = mv->type;
  moduleInitIOContext(io, mt, r);
  mt->aof_rewrite(&io, key, mv->value);
  if (io.ctx) {
    moduleFreeContext(io.ctx);
    zfree(io.ctx);
  }
  return io.error ? 0 : 1;
}
ssize_t aofReadDiffFromParent(void) {
  char buf[65536];
  ssize_t nread, total = 0;
  while ((nread = read(server.aof_pipe_read_data_from_parent, buf,
                       sizeof(buf))) > 0) {
    server.aof_child_diff = sdscatlen(server.aof_child_diff, buf, nread);
    total += nread;
  }
  return total;
}
int rewriteAppendOnlyFileRio(rio *aof) {
  dictIterator *di = NULL;
  dictEntry *de;
  size_t processed = 0;
  int j;
  for (j = 0; j < server.dbnum; j++) {
    char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
    redisDb *db = server.db + j;
    dict *d = db->dict;
    if (dictSize(d) == 0) continue;
    di = dictGetSafeIterator(d);
    if (rioWrite(aof, selectcmd, sizeof(selectcmd) - 1) == 0) goto werr;
    if (rioWriteBulkLongLong(aof, j) == 0) goto werr;
    while ((de = dictNext(di)) != NULL) {
      sds keystr;
      robj key, *o;
      long long expiretime;
      keystr = dictGetKey(de);
      o = dictGetVal(de);
      initStaticStringObject(key, keystr);
      expiretime = getExpire(db, &key);
      if (o->type == OBJ_STRING) {
        char cmd[] = "*3\r\n$3\r\nSET\r\n";
        if (rioWrite(aof, cmd, sizeof(cmd) - 1) == 0) goto werr;
        if (rioWriteBulkObject(aof, &key) == 0) goto werr;
        if (rioWriteBulkObject(aof, o) == 0) goto werr;
      } else if (o->type == OBJ_LIST) {
        if (rewriteListObject(aof, &key, o) == 0) goto werr;
      } else if (o->type == OBJ_SET) {
        if (rewriteSetObject(aof, &key, o) == 0) goto werr;
      } else if (o->type == OBJ_ZSET) {
        if (rewriteSortedSetObject(aof, &key, o) == 0) goto werr;
      } else if (o->type == OBJ_HASH) {
        if (rewriteHashObject(aof, &key, o) == 0) goto werr;
      } else if (o->type == OBJ_STREAM) {
        if (rewriteStreamObject(aof, &key, o) == 0) goto werr;
      } else if (o->type == OBJ_MODULE) {
        if (rewriteModuleObject(aof, &key, o) == 0) goto werr;
      } else {
        serverPanic("Unknown object type");
      }
      if (expiretime != -1) {
        char cmd[] = "*3\r\n$9\r\nPEXPIREAT\r\n";
        if (rioWrite(aof, cmd, sizeof(cmd) - 1) == 0) goto werr;
        if (rioWriteBulkObject(aof, &key) == 0) goto werr;
        if (rioWriteBulkLongLong(aof, expiretime) == 0) goto werr;
      }
      if (aof->processed_bytes > processed + AOF_READ_DIFF_INTERVAL_BYTES) {
        processed = aof->processed_bytes;
        aofReadDiffFromParent();
      }
    }
    dictReleaseIterator(di);
    di = NULL;
  }
  return C_OK;
werr:
  if (di) dictReleaseIterator(di);
  return C_ERR;
}
int rewriteAppendOnlyFile(char *filename) {
  rio aof;
  FILE *fp;
  char tmpfile[256];
  char byte;
  snprintf(tmpfile, 256, "temp-rewriteaof-%d.aof", (int)getpid());
  fp = fopen(tmpfile, "w");
  if (!fp) {
    serverLog(
        LL_WARNING,
        "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s",
        strerror(errno));
    return C_ERR;
  }
  server.aof_child_diff = sdsempty();
  rioInitWithFile(&aof, fp);
  if (server.aof_rewrite_incremental_fsync)
    rioSetAutoSync(&aof, REDIS_AUTOSYNC_BYTES);
  if (server.aof_use_rdb_preamble) {
    int error;
    if (rdbSaveRio(&aof, &error, RDB_SAVE_AOF_PREAMBLE, NULL) == C_ERR) {
      errno = error;
      goto werr;
    }
  } else {
    if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
  }
  if (fflush(fp) == EOF) goto werr;
  if (fsync(fileno(fp)) == -1) goto werr;
  int nodata = 0;
  mstime_t start = mstime();
  while (mstime() - start < 1000 && nodata < 20) {
    if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0) {
      nodata++;
      continue;
    }
    nodata = 0;
    aofReadDiffFromParent();
  }
  if (write(server.aof_pipe_write_ack_to_parent, "!", 1) != 1) goto werr;
  if (anetNonBlock(NULL, server.aof_pipe_read_ack_from_parent) != ANET_OK)
    goto werr;
  if (syncRead(server.aof_pipe_read_ack_from_parent, &byte, 1, 5000) != 1 ||
      byte != '!')
    goto werr;
  serverLog(LL_NOTICE,
            "Parent agreed to stop sending diffs. Finalizing AOF...");
  aofReadDiffFromParent();
  serverLog(LL_NOTICE,
            "Concatenating %.2f MB of AOF diff received from parent.",
            (double)sdslen(server.aof_child_diff) / (1024 * 1024));
  if (rioWrite(&aof, server.aof_child_diff, sdslen(server.aof_child_diff)) == 0)
    goto werr;
  if (fflush(fp) == EOF) goto werr;
  if (fsync(fileno(fp)) == -1) goto werr;
  if (fclose(fp) == EOF) goto werr;
  if (rename(tmpfile, filename) == -1) {
    serverLog(LL_WARNING,
              "Error moving temp append only file on the final destination: %s",
              strerror(errno));
    unlink(tmpfile);
    return C_ERR;
  }
  serverLog(LL_NOTICE, "SYNC append only file rewrite performed");
  return C_OK;
werr:
  serverLog(LL_WARNING, "Write error writing append only file on disk: %s",
            strerror(errno));
  fclose(fp);
  unlink(tmpfile);
  return C_ERR;
}
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
  char byte;
  UNUSED(el);
  UNUSED(privdata);
  UNUSED(mask);
  if (read(fd, &byte, 1) == 1 && byte == '!') {
    serverLog(LL_NOTICE, "AOF rewrite child asks to stop sending diffs.");
    server.aof_stop_sending_diff = 1;
    if (write(server.aof_pipe_write_ack_to_child, "!", 1) != 1) {
      serverLog(LL_WARNING, "Can't send ACK to AOF child: %s", strerror(errno));
    }
  }
  aeDeleteFileEvent(server.el, server.aof_pipe_read_ack_from_child,
                    AE_READABLE);
}
int aofCreatePipes(void) {
  int fds[6] = {-1, -1, -1, -1, -1, -1};
  int j;
  if (pipe(fds) == -1) goto error;
  if (pipe(fds + 2) == -1) goto error;
  if (pipe(fds + 4) == -1) goto error;
  if (anetNonBlock(NULL, fds[0]) != ANET_OK) goto error;
  if (anetNonBlock(NULL, fds[1]) != ANET_OK) goto error;
  if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable,
                        NULL) == AE_ERR)
    goto error;
  server.aof_pipe_write_data_to_child = fds[1];
  server.aof_pipe_read_data_from_parent = fds[0];
  server.aof_pipe_write_ack_to_parent = fds[3];
  server.aof_pipe_read_ack_from_child = fds[2];
  server.aof_pipe_write_ack_to_child = fds[5];
  server.aof_pipe_read_ack_from_parent = fds[4];
  server.aof_stop_sending_diff = 0;
  return C_OK;
error:
  serverLog(LL_WARNING, "Error opening /setting AOF rewrite IPC pipes: %s",
            strerror(errno));
  for (j = 0; j < 6; j++)
    if (fds[j] != -1) close(fds[j]);
  return C_ERR;
}
void aofClosePipes(void) {
  aeDeleteFileEvent(server.el, server.aof_pipe_read_ack_from_child,
                    AE_READABLE);
  aeDeleteFileEvent(server.el, server.aof_pipe_write_data_to_child,
                    AE_WRITABLE);
  close(server.aof_pipe_write_data_to_child);
  close(server.aof_pipe_read_data_from_parent);
  close(server.aof_pipe_write_ack_to_parent);
  close(server.aof_pipe_read_ack_from_child);
  close(server.aof_pipe_write_ack_to_child);
  close(server.aof_pipe_read_ack_from_parent);
}
int rewriteAppendOnlyFileBackground(void) {
  pid_t childpid;
  long long start;
  if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;
  if (aofCreatePipes() != C_OK) return C_ERR;
  openChildInfoPipe();
  start = ustime();
  if ((childpid = fork()) == 0) {
    char tmpfile[256];
    closeListeningSockets(0);
    redisSetProcTitle("redis-aof-rewrite");
    snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int)getpid());
    if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
      size_t private_dirty = zmalloc_get_private_dirty(-1);
      if (private_dirty) {
        serverLog(LL_NOTICE,
                  "AOF rewrite: %zu MB of memory used by copy-on-write",
                  private_dirty / (1024 * 1024));
      }
      server.child_info_data.cow_size = private_dirty;
      sendChildInfo(CHILD_INFO_TYPE_AOF);
      exitFromChild(0);
    } else {
      exitFromChild(1);
    }
  } else {
    server.stat_fork_time = ustime() - start;
    server.stat_fork_rate = (double)zmalloc_used_memory() * 1000000 /
                            server.stat_fork_time /
                            (1024 * 1024 * 1024);
    latencyAddSampleIfNeeded("fork", server.stat_fork_time / 1000);
    if (childpid == -1) {
      closeChildInfoPipe();
      serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
      aofClosePipes();
      return C_ERR;
    }
    serverLog(LL_NOTICE,
              "Background append only file rewriting started by pid %d",
              childpid);
    server.aof_rewrite_scheduled = 0;
    server.aof_rewrite_time_start = time(NULL);
    server.aof_child_pid = childpid;
    updateDictResizePolicy();
    server.aof_selected_db = -1;
    replicationScriptCacheFlush();
    return C_OK;
  }
  return C_OK;
}
void bgrewriteaofCommand(client *c) {
  if (server.aof_child_pid != -1) {
    addReplyError(c,
                  "Background append only file rewriting already in progress");
  } else if (server.rdb_child_pid != -1) {
    server.aof_rewrite_scheduled = 1;
    addReplyStatus(c, "Background append only file rewriting scheduled");
  } else if (rewriteAppendOnlyFileBackground() == C_OK) {
    addReplyStatus(c, "Background append only file rewriting started");
  } else {
    addReply(c, shared.err);
  }
}
void aofRemoveTempFile(pid_t childpid) {
  char tmpfile[256];
  snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof", (int)childpid);
  unlink(tmpfile);
}
void aofUpdateCurrentSize(void) {
  struct redis_stat sb;
  mstime_t latency;
  latencyStartMonitor(latency);
  if (redis_fstat(server.aof_fd, &sb) == -1) {
    serverLog(LL_WARNING, "Unable to obtain the AOF file length. stat: %s",
              strerror(errno));
  } else {
    server.aof_current_size = sb.st_size;
  }
  latencyEndMonitor(latency);
  latencyAddSampleIfNeeded("aof-fstat", latency);
}
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
  if (!bysignal && exitcode == 0) {
    int newfd, oldfd;
    char tmpfile[256];
    long long now = ustime();
    mstime_t latency;
    serverLog(LL_NOTICE, "Background AOF rewrite terminated with success");
    latencyStartMonitor(latency);
    snprintf(tmpfile, 256, "temp-rewriteaof-bg-%d.aof",
             (int)server.aof_child_pid);
    newfd = open(tmpfile, O_WRONLY | O_APPEND);
    if (newfd == -1) {
      serverLog(LL_WARNING,
                "Unable to open the temporary AOF produced by the child: %s",
                strerror(errno));
      goto cleanup;
    }
    if (aofRewriteBufferWrite(newfd) == -1) {
      serverLog(
          LL_WARNING,
          "Error trying to flush the parent diff to the rewritten AOF: %s",
          strerror(errno));
      close(newfd);
      goto cleanup;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-rewrite-diff-write", latency);
    serverLog(LL_NOTICE,
              "Residual parent diff successfully flushed to the rewritten AOF "
              "(%.2f MB)",
              (double)aofRewriteBufferSize() / (1024 * 1024));
    if (server.aof_fd == -1) {
      oldfd = open(server.aof_filename, O_RDONLY | O_NONBLOCK);
    } else {
      oldfd = -1;
    }
    latencyStartMonitor(latency);
    if (rename(tmpfile, server.aof_filename) == -1) {
      serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile, server.aof_filename, strerror(errno));
      close(newfd);
      if (oldfd != -1) close(oldfd);
      goto cleanup;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-rename", latency);
    if (server.aof_fd == -1) {
      close(newfd);
    } else {
      oldfd = server.aof_fd;
      server.aof_fd = newfd;
      if (server.aof_fsync == AOF_FSYNC_ALWAYS)
        redis_fsync(newfd);
      else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        aof_background_fsync(newfd);
      server.aof_selected_db = -1;
      aofUpdateCurrentSize();
      server.aof_rewrite_base_size = server.aof_current_size;
      sdsfree(server.aof_buf);
      server.aof_buf = sdsempty();
    }
    server.aof_lastbgrewrite_status = C_OK;
    serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
    if (server.aof_state == AOF_WAIT_REWRITE) server.aof_state = AOF_ON;
    if (oldfd != -1)
      bioCreateBackgroundJob(BIO_CLOSE_FILE, (void *)(long)oldfd, NULL, NULL);
    serverLog(LL_VERBOSE, "Background AOF rewrite signal handler took %lldus",
              ustime() - now);
  } else if (!bysignal && exitcode != 0) {
    if (bysignal != SIGUSR1) server.aof_lastbgrewrite_status = C_ERR;
    serverLog(LL_WARNING, "Background AOF rewrite terminated with error");
  } else {
    server.aof_lastbgrewrite_status = C_ERR;
    serverLog(LL_WARNING, "Background AOF rewrite terminated by signal %d",
              bysignal);
  }
cleanup:
  aofClosePipes();
  aofRewriteBufferReset();
  aofRemoveTempFile(server.aof_child_pid);
  server.aof_child_pid = -1;
  server.aof_rewrite_time_last = time(NULL) - server.aof_rewrite_time_start;
  server.aof_rewrite_time_start = -1;
  if (server.aof_state == AOF_WAIT_REWRITE) server.aof_rewrite_scheduled = 1;
}
