#include "server.h"
#include "cluster.h"
#include "endianconv.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>
clusterNode *myself = NULL;
clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(connection *conn);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(const char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id,
                                uint8_t type, const unsigned char *payload,
                                uint32_t len);
int clusterLoadConfig(char *filename) {
  FILE *fp = fopen(filename, "r");
  struct stat sb;
  char *line;
  int maxline, j;
  if (fp == NULL) {
    if (errno == ENOENT) {
      return C_ERR;
    } else {
      serverLog(LL_WARNING, "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
      exit(1);
    }
  }
  if (fstat(fileno(fp), &sb) != -1 && sb.st_size == 0) {
    fclose(fp);
    return C_ERR;
  }
  maxline = 1024 + CLUSTER_SLOTS * 128;
  line = zmalloc(maxline);
  while (fgets(line, maxline, fp) != NULL) {
    int argc;
    sds *argv;
    clusterNode *n, *master;
    char *p, *s;
    if (line[0] == '\n' || line[0] == '\0') continue;
    argv = sdssplitargs(line, &argc);
    if (argv == NULL) goto fmterr;
    if (strcasecmp(argv[0], "vars") == 0) {
      if (!(argc % 2)) goto fmterr;
      for (j = 1; j < argc; j += 2) {
        if (strcasecmp(argv[j], "currentEpoch") == 0) {
          server.cluster->currentEpoch = strtoull(argv[j + 1], NULL, 10);
        } else if (strcasecmp(argv[j], "lastVoteEpoch") == 0) {
          server.cluster->lastVoteEpoch = strtoull(argv[j + 1], NULL, 10);
        } else {
          serverLog(LL_WARNING, "Skipping unknown cluster config variable '%s'",
                    argv[j]);
        }
      }
      sdsfreesplitres(argv, argc);
      continue;
    }
    if (argc < 8) goto fmterr;
    n = clusterLookupNode(argv[0]);
    if (!n) {
      n = createClusterNode(argv[0], 0);
      clusterAddNode(n);
    }
    if ((p = strrchr(argv[1], ':')) == NULL) goto fmterr;
    *p = '\0';
    memcpy(n->ip, argv[1], strlen(argv[1]) + 1);
    char *port = p + 1;
    char *busp = strchr(port, '@');
    if (busp) {
      *busp = '\0';
      busp++;
    }
    n->port = atoi(port);
    n->cport = busp ? atoi(busp) : n->port + CLUSTER_PORT_INCR;
    p = s = argv[2];
    while (p) {
      p = strchr(s, ',');
      if (p) *p = '\0';
      if (!strcasecmp(s, "myself")) {
        serverAssert(server.cluster->myself == NULL);
        myself = server.cluster->myself = n;
        n->flags |= CLUSTER_NODE_MYSELF;
      } else if (!strcasecmp(s, "master")) {
        n->flags |= CLUSTER_NODE_MASTER;
      } else if (!strcasecmp(s, "slave")) {
        n->flags |= CLUSTER_NODE_SLAVE;
      } else if (!strcasecmp(s, "fail?")) {
        n->flags |= CLUSTER_NODE_PFAIL;
      } else if (!strcasecmp(s, "fail")) {
        n->flags |= CLUSTER_NODE_FAIL;
        n->fail_time = mstime();
      } else if (!strcasecmp(s, "handshake")) {
        n->flags |= CLUSTER_NODE_HANDSHAKE;
      } else if (!strcasecmp(s, "noaddr")) {
        n->flags |= CLUSTER_NODE_NOADDR;
      } else if (!strcasecmp(s, "nofailover")) {
        n->flags |= CLUSTER_NODE_NOFAILOVER;
      } else if (!strcasecmp(s, "noflags")) {
      } else {
        serverPanic("Unknown flag in redis cluster config file");
      }
      if (p) s = p + 1;
    }
    if (argv[3][0] != '-') {
      master = clusterLookupNode(argv[3]);
      if (!master) {
        master = createClusterNode(argv[3], 0);
        clusterAddNode(master);
      }
      n->slaveof = master;
      clusterNodeAddSlave(master, n);
    }
    if (atoi(argv[4])) n->ping_sent = mstime();
    if (atoi(argv[5])) n->pong_received = mstime();
    n->configEpoch = strtoull(argv[6], NULL, 10);
    for (j = 8; j < argc; j++) {
      int start, stop;
      if (argv[j][0] == '[') {
        int slot;
        char direction;
        clusterNode *cn;
        p = strchr(argv[j], '-');
        serverAssert(p != NULL);
        *p = '\0';
        direction = p[1];
        slot = atoi(argv[j] + 1);
        if (slot < 0 || slot >= CLUSTER_SLOTS) goto fmterr;
        p += 3;
        cn = clusterLookupNode(p);
        if (!cn) {
          cn = createClusterNode(p, 0);
          clusterAddNode(cn);
        }
        if (direction == '>') {
          server.cluster->migrating_slots_to[slot] = cn;
        } else {
          server.cluster->importing_slots_from[slot] = cn;
        }
        continue;
      } else if ((p = strchr(argv[j], '-')) != NULL) {
        *p = '\0';
        start = atoi(argv[j]);
        stop = atoi(p + 1);
      } else {
        start = stop = atoi(argv[j]);
      }
      if (start < 0 || start >= CLUSTER_SLOTS) goto fmterr;
      if (stop < 0 || stop >= CLUSTER_SLOTS) goto fmterr;
      while (start <= stop) clusterAddSlot(n, start++);
    }
    sdsfreesplitres(argv, argc);
  }
  if (server.cluster->myself == NULL) goto fmterr;
  zfree(line);
  fclose(fp);
  serverLog(LL_NOTICE, "Node configuration loaded, I'm %.40s", myself->name);
  if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
    server.cluster->currentEpoch = clusterGetMaxEpoch();
  }
  return C_OK;
fmterr:
  serverLog(LL_WARNING, "Unrecoverable error: corrupted cluster config file.");
  zfree(line);
  if (fp) fclose(fp);
  exit(1);
}
int clusterSaveConfig(int do_fsync) {
  sds ci;
  size_t content_size;
  struct stat sb;
  int fd;
  server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;
  ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE);
  ci = sdscatprintf(ci, "vars currentEpoch %llu lastVoteEpoch %llu\n",
                    (unsigned long long)server.cluster->currentEpoch,
                    (unsigned long long)server.cluster->lastVoteEpoch);
  content_size = sdslen(ci);
  if ((fd = open(server.cluster_configfile, O_WRONLY | O_CREAT, 0644)) == -1)
    goto err;
  if (fstat(fd, &sb) != -1) {
    if (sb.st_size > (off_t)content_size) {
      ci = sdsgrowzero(ci, sb.st_size);
      memset(ci + content_size, '\n', sb.st_size - content_size);
    }
  }
  if (write(fd, ci, sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
  if (do_fsync) {
    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
    fsync(fd);
  }
  if (content_size != sdslen(ci) && ftruncate(fd, content_size) == -1) {
  }
  close(fd);
  sdsfree(ci);
  return 0;
err:
  if (fd != -1) close(fd);
  sdsfree(ci);
  return -1;
}
void clusterSaveConfigOrDie(int do_fsync) {
  if (clusterSaveConfig(do_fsync) == -1) {
    serverLog(LL_WARNING, "Fatal: can't update cluster config file.");
    exit(1);
  }
}
int clusterLockConfig(char *filename) {
#if !defined(__sun)
  int fd = open(filename, O_WRONLY | O_CREAT, 0644);
  if (fd == -1) {
    serverLog(LL_WARNING, "Can't open %s in order to acquire a lock: %s",
              filename, strerror(errno));
    return C_ERR;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
    if (errno == EWOULDBLOCK) {
      serverLog(LL_WARNING,
                "Sorry, the cluster configuration file %s is already used "
                "by a different Redis Cluster node. Please make sure that "
                "different nodes use different cluster configuration "
                "files.",
                filename);
    } else {
      serverLog(LL_WARNING, "Impossible to lock %s: %s", filename,
                strerror(errno));
    }
    close(fd);
    return C_ERR;
  }
#endif
  return C_OK;
}
void clusterUpdateMyselfFlags(void) {
  int oldflags = myself->flags;
  int nofailover =
      server.cluster_slave_no_failover ? CLUSTER_NODE_NOFAILOVER : 0;
  myself->flags &= ~CLUSTER_NODE_NOFAILOVER;
  myself->flags |= nofailover;
  if (myself->flags != oldflags) {
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
  }
}
void clusterInit(void) {
  int saveconf = 0;
  server.cluster = zmalloc(sizeof(clusterState));
  server.cluster->myself = NULL;
  server.cluster->currentEpoch = 0;
  server.cluster->state = CLUSTER_FAIL;
  server.cluster->size = 1;
  server.cluster->todo_before_sleep = 0;
  server.cluster->nodes = dictCreate(&clusterNodesDictType, NULL);
  server.cluster->nodes_black_list =
      dictCreate(&clusterNodesBlackListDictType, NULL);
  server.cluster->failover_auth_time = 0;
  server.cluster->failover_auth_count = 0;
  server.cluster->failover_auth_rank = 0;
  server.cluster->failover_auth_epoch = 0;
  server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
  server.cluster->lastVoteEpoch = 0;
  for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
    server.cluster->stats_bus_messages_sent[i] = 0;
    server.cluster->stats_bus_messages_received[i] = 0;
  }
  server.cluster->stats_pfail_nodes = 0;
  memset(server.cluster->slots, 0, sizeof(server.cluster->slots));
  clusterCloseAllSlots();
  if (clusterLockConfig(server.cluster_configfile) == C_ERR) exit(1);
  if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
    myself = server.cluster->myself =
        createClusterNode(NULL, CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER);
    serverLog(LL_NOTICE, "No cluster configuration found, I'm %.40s",
              myself->name);
    clusterAddNode(myself);
    saveconf = 1;
  }
  if (saveconf) clusterSaveConfigOrDie(1);
  server.cfd_count = 0;
  int port = server.tls_cluster ? server.tls_port : server.port;
  if (port > (65535 - CLUSTER_PORT_INCR)) {
    serverLog(LL_WARNING,
              "Redis port number too high. "
              "Cluster communication port is 10,000 port "
              "numbers higher than your Redis port. "
              "Your Redis port number must be "
              "lower than 55535.");
    exit(1);
  }
  if (listenToPort(port + CLUSTER_PORT_INCR, server.cfd, &server.cfd_count) ==
      C_ERR) {
    exit(1);
  } else {
    int j;
    for (j = 0; j < server.cfd_count; j++) {
      if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                            clusterAcceptHandler, NULL) == AE_ERR)
        serverPanic(
            "Unrecoverable error creating Redis Cluster "
            "file event.");
    }
  }
  server.cluster->slots_to_keys = raxNew();
  memset(server.cluster->slots_keys_count, 0,
         sizeof(server.cluster->slots_keys_count));
  myself->port = port;
  myself->cport = port + CLUSTER_PORT_INCR;
  if (server.cluster_announce_port) myself->port = server.cluster_announce_port;
  if (server.cluster_announce_bus_port)
    myself->cport = server.cluster_announce_bus_port;
  server.cluster->mf_end = 0;
  resetManualFailover();
  clusterUpdateMyselfFlags();
}
void clusterReset(int hard) {
  dictIterator *di;
  dictEntry *de;
  int j;
  if (nodeIsSlave(myself)) {
    clusterSetNodeAsMaster(myself);
    replicationUnsetMaster();
    emptyDb(-1, EMPTYDB_NO_FLAGS, NULL);
  }
  clusterCloseAllSlots();
  resetManualFailover();
  for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (node == myself) continue;
    clusterDelNode(node);
  }
  dictReleaseIterator(di);
  if (hard) {
    sds oldname;
    server.cluster->currentEpoch = 0;
    server.cluster->lastVoteEpoch = 0;
    myself->configEpoch = 0;
    serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");
    oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
    dictDelete(server.cluster->nodes, oldname);
    sdsfree(oldname);
    getRandomHexChars(myself->name, CLUSTER_NAMELEN);
    clusterAddNode(myself);
    serverLog(LL_NOTICE, "Node hard reset, now I'm %.40s", myself->name);
  }
  clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE |
                       CLUSTER_TODO_FSYNC_CONFIG);
}
clusterLink *createClusterLink(clusterNode *node) {
  clusterLink *link = zmalloc(sizeof(*link));
  link->ctime = mstime();
  link->sndbuf = sdsempty();
  link->rcvbuf = sdsempty();
  link->node = node;
  link->conn = NULL;
  return link;
}
void freeClusterLink(clusterLink *link) {
  if (link->conn) {
    connClose(link->conn);
    link->conn = NULL;
  }
  sdsfree(link->sndbuf);
  sdsfree(link->rcvbuf);
  if (link->node) link->node->link = NULL;
  zfree(link);
}
static void clusterConnAcceptHandler(connection *conn) {
  clusterLink *link;
  if (connGetState(conn) != CONN_STATE_CONNECTED) {
    serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s",
              connGetLastError(conn));
    connClose(conn);
    return;
  }
  link = createClusterLink(NULL);
  link->conn = conn;
  connSetPrivateData(conn, link);
  connSetReadHandler(conn, clusterReadHandler);
}
#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
  int cport, cfd;
  int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
  char cip[NET_IP_STR_LEN];
  UNUSED(el);
  UNUSED(mask);
  UNUSED(privdata);
  if (server.masterhost == NULL && server.loading) return;
  while (max--) {
    cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
    if (cfd == ANET_ERR) {
      if (errno != EWOULDBLOCK)
        serverLog(LL_VERBOSE, "Error accepting cluster node: %s",
                  server.neterr);
      return;
    }
    connection *conn = server.tls_cluster ? connCreateAcceptedTLS(cfd, 1)
                                          : connCreateAcceptedSocket(cfd);
    connNonBlock(conn);
    connEnableTcpNoDelay(conn);
    serverLog(LL_VERBOSE, "Accepting cluster node connection from %s:%d", cip,
              cport);
    if (connAccept(conn, clusterConnAcceptHandler) == C_ERR) {
      serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s",
                connGetLastError(conn));
      connClose(conn);
      return;
    }
  }
}
unsigned int keyHashSlot(char *key, int keylen) {
  int s, e;
  for (s = 0; s < keylen; s++)
    if (key[s] == '{') break;
  if (s == keylen) return crc16(key, keylen) & 0x3FFF;
  for (e = s + 1; e < keylen; e++)
    if (key[e] == '}') break;
  if (e == keylen || e == s + 1) return crc16(key, keylen) & 0x3FFF;
  return crc16(key + s + 1, e - s - 1) & 0x3FFF;
}
clusterNode *createClusterNode(char *nodename, int flags) {
  clusterNode *node = zmalloc(sizeof(*node));
  if (nodename)
    memcpy(node->name, nodename, CLUSTER_NAMELEN);
  else
    getRandomHexChars(node->name, CLUSTER_NAMELEN);
  node->ctime = mstime();
  node->configEpoch = 0;
  node->flags = flags;
  memset(node->slots, 0, sizeof(node->slots));
  node->numslots = 0;
  node->numslaves = 0;
  node->slaves = NULL;
  node->slaveof = NULL;
  node->ping_sent = node->pong_received = 0;
  node->fail_time = 0;
  node->link = NULL;
  memset(node->ip, 0, sizeof(node->ip));
  node->port = 0;
  node->cport = 0;
  node->fail_reports = listCreate();
  node->voted_time = 0;
  node->orphaned_time = 0;
  node->repl_offset_time = 0;
  node->repl_offset = 0;
  listSetFreeMethod(node->fail_reports, zfree);
  return node;
}
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
  list *l = failing->fail_reports;
  listNode *ln;
  listIter li;
  clusterNodeFailReport *fr;
  listRewind(l, &li);
  while ((ln = listNext(&li)) != NULL) {
    fr = ln->value;
    if (fr->node == sender) {
      fr->time = mstime();
      return 0;
    }
  }
  fr = zmalloc(sizeof(*fr));
  fr->node = sender;
  fr->time = mstime();
  listAddNodeTail(l, fr);
  return 1;
}
void clusterNodeCleanupFailureReports(clusterNode *node) {
  list *l = node->fail_reports;
  listNode *ln;
  listIter li;
  clusterNodeFailReport *fr;
  mstime_t maxtime =
      server.cluster_node_timeout * CLUSTER_FAIL_REPORT_VALIDITY_MULT;
  mstime_t now = mstime();
  listRewind(l, &li);
  while ((ln = listNext(&li)) != NULL) {
    fr = ln->value;
    if (now - fr->time > maxtime) listDelNode(l, ln);
  }
}
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
  list *l = node->fail_reports;
  listNode *ln;
  listIter li;
  clusterNodeFailReport *fr;
  listRewind(l, &li);
  while ((ln = listNext(&li)) != NULL) {
    fr = ln->value;
    if (fr->node == sender) break;
  }
  if (!ln) return 0;
  listDelNode(l, ln);
  clusterNodeCleanupFailureReports(node);
  return 1;
}
int clusterNodeFailureReportsCount(clusterNode *node) {
  clusterNodeCleanupFailureReports(node);
  return listLength(node->fail_reports);
}
int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
  int j;
  for (j = 0; j < master->numslaves; j++) {
    if (master->slaves[j] == slave) {
      if ((j + 1) < master->numslaves) {
        int remaining_slaves = (master->numslaves - j) - 1;
        memmove(master->slaves + j, master->slaves + (j + 1),
                (sizeof(*master->slaves) * remaining_slaves));
      }
      master->numslaves--;
      if (master->numslaves == 0) master->flags &= ~CLUSTER_NODE_MIGRATE_TO;
      return C_OK;
    }
  }
  return C_ERR;
}
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
  int j;
  for (j = 0; j < master->numslaves; j++)
    if (master->slaves[j] == slave) return C_ERR;
  master->slaves =
      zrealloc(master->slaves, sizeof(clusterNode *) * (master->numslaves + 1));
  master->slaves[master->numslaves] = slave;
  master->numslaves++;
  master->flags |= CLUSTER_NODE_MIGRATE_TO;
  return C_OK;
}
int clusterCountNonFailingSlaves(clusterNode *n) {
  int j, okslaves = 0;
  for (j = 0; j < n->numslaves; j++)
    if (!nodeFailed(n->slaves[j])) okslaves++;
  return okslaves;
}
void freeClusterNode(clusterNode *n) {
  sds nodename;
  int j;
  for (j = 0; j < n->numslaves; j++) n->slaves[j]->slaveof = NULL;
  if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof, n);
  nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
  serverAssert(dictDelete(server.cluster->nodes, nodename) == DICT_OK);
  sdsfree(nodename);
  if (n->link) freeClusterLink(n->link);
  listRelease(n->fail_reports);
  zfree(n->slaves);
  zfree(n);
}
int clusterAddNode(clusterNode *node) {
  int retval;
  retval = dictAdd(server.cluster->nodes,
                   sdsnewlen(node->name, CLUSTER_NAMELEN), node);
  return (retval == DICT_OK) ? C_OK : C_ERR;
}
void clusterDelNode(clusterNode *delnode) {
  int j;
  dictIterator *di;
  dictEntry *de;
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (server.cluster->importing_slots_from[j] == delnode)
      server.cluster->importing_slots_from[j] = NULL;
    if (server.cluster->migrating_slots_to[j] == delnode)
      server.cluster->migrating_slots_to[j] = NULL;
    if (server.cluster->slots[j] == delnode) clusterDelSlot(j);
  }
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (node == delnode) continue;
    clusterNodeDelFailureReport(node, delnode);
  }
  dictReleaseIterator(di);
  freeClusterNode(delnode);
}
clusterNode *clusterLookupNode(const char *name) {
  sds s = sdsnewlen(name, CLUSTER_NAMELEN);
  dictEntry *de;
  de = dictFind(server.cluster->nodes, s);
  sdsfree(s);
  if (de == NULL) return NULL;
  return dictGetVal(de);
}
void clusterRenameNode(clusterNode *node, char *newname) {
  int retval;
  sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);
  serverLog(LL_DEBUG, "Renaming node %.40s into %.40s", node->name, newname);
  retval = dictDelete(server.cluster->nodes, s);
  sdsfree(s);
  serverAssert(retval == DICT_OK);
  memcpy(node->name, newname, CLUSTER_NAMELEN);
  clusterAddNode(node);
}
uint64_t clusterGetMaxEpoch(void) {
  uint64_t max = 0;
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (node->configEpoch > max) max = node->configEpoch;
  }
  dictReleaseIterator(di);
  if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
  return max;
}
int clusterBumpConfigEpochWithoutConsensus(void) {
  uint64_t maxEpoch = clusterGetMaxEpoch();
  if (myself->configEpoch == 0 || myself->configEpoch != maxEpoch) {
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
    serverLog(LL_WARNING, "New configEpoch set to %llu",
              (unsigned long long)myself->configEpoch);
    return C_OK;
  } else {
    return C_ERR;
  }
}
void clusterHandleConfigEpochCollision(clusterNode *sender) {
  if (sender->configEpoch != myself->configEpoch || !nodeIsMaster(sender) ||
      !nodeIsMaster(myself))
    return;
  if (memcmp(sender->name, myself->name, CLUSTER_NAMELEN) <= 0) return;
  server.cluster->currentEpoch++;
  myself->configEpoch = server.cluster->currentEpoch;
  clusterSaveConfigOrDie(1);
  serverLog(LL_VERBOSE,
            "WARNING: configEpoch collision with node %.40s."
            " configEpoch set to %llu",
            sender->name, (unsigned long long)myself->configEpoch);
}
#define CLUSTER_BLACKLIST_TTL 60
void clusterBlacklistCleanup(void) {
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes_black_list);
  while ((de = dictNext(di)) != NULL) {
    int64_t expire = dictGetUnsignedIntegerVal(de);
    if (expire < server.unixtime)
      dictDelete(server.cluster->nodes_black_list, dictGetKey(de));
  }
  dictReleaseIterator(di);
}
void clusterBlacklistAddNode(clusterNode *node) {
  dictEntry *de;
  sds id = sdsnewlen(node->name, CLUSTER_NAMELEN);
  clusterBlacklistCleanup();
  if (dictAdd(server.cluster->nodes_black_list, id, NULL) == DICT_OK) {
    id = sdsdup(id);
  }
  de = dictFind(server.cluster->nodes_black_list, id);
  dictSetUnsignedIntegerVal(de, time(NULL) + CLUSTER_BLACKLIST_TTL);
  sdsfree(id);
}
int clusterBlacklistExists(char *nodeid) {
  sds id = sdsnewlen(nodeid, CLUSTER_NAMELEN);
  int retval;
  clusterBlacklistCleanup();
  retval = dictFind(server.cluster->nodes_black_list, id) != NULL;
  sdsfree(id);
  return retval;
}
void markNodeAsFailingIfNeeded(clusterNode *node) {
  int failures;
  int needed_quorum = (server.cluster->size / 2) + 1;
  if (!nodeTimedOut(node)) return;
  if (nodeFailed(node)) return;
  failures = clusterNodeFailureReportsCount(node);
  if (nodeIsMaster(myself)) failures++;
  if (failures < needed_quorum) return;
  serverLog(LL_NOTICE, "Marking node %.40s as failing (quorum reached).",
            node->name);
  node->flags &= ~CLUSTER_NODE_PFAIL;
  node->flags |= CLUSTER_NODE_FAIL;
  node->fail_time = mstime();
  if (nodeIsMaster(myself)) clusterSendFail(node->name);
  clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
}
void clearNodeFailureIfNeeded(clusterNode *node) {
  mstime_t now = mstime();
  serverAssert(nodeFailed(node));
  if (nodeIsSlave(node) || node->numslots == 0) {
    serverLog(
        LL_NOTICE, "Clear FAIL state for node %.40s: %s is reachable again.",
        node->name, nodeIsSlave(node) ? "replica" : "master without slots");
    node->flags &= ~CLUSTER_NODE_FAIL;
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
  }
  if (nodeIsMaster(node) && node->numslots > 0 &&
      (now - node->fail_time) >
          (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT)) {
    serverLog(LL_NOTICE,
              "Clear FAIL state for node %.40s: is reachable again and nobody "
              "is serving its slots after some time.",
              node->name);
    node->flags &= ~CLUSTER_NODE_FAIL;
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
  }
}
int clusterHandshakeInProgress(char *ip, int port, int cport) {
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (!nodeInHandshake(node)) continue;
    if (!strcasecmp(node->ip, ip) && node->port == port && node->cport == cport)
      break;
  }
  dictReleaseIterator(di);
  return de != NULL;
}
int clusterStartHandshake(char *ip, int port, int cport) {
  clusterNode *n;
  char norm_ip[NET_IP_STR_LEN];
  struct sockaddr_storage sa;
  if (inet_pton(AF_INET, ip, &(((struct sockaddr_in *)&sa)->sin_addr))) {
    sa.ss_family = AF_INET;
  } else if (inet_pton(AF_INET6, ip,
                       &(((struct sockaddr_in6 *)&sa)->sin6_addr))) {
    sa.ss_family = AF_INET6;
  } else {
    errno = EINVAL;
    return 0;
  }
  if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
    errno = EINVAL;
    return 0;
  }
  memset(norm_ip, 0, NET_IP_STR_LEN);
  if (sa.ss_family == AF_INET)
    inet_ntop(AF_INET, (void *)&(((struct sockaddr_in *)&sa)->sin_addr),
              norm_ip, NET_IP_STR_LEN);
  else
    inet_ntop(AF_INET6, (void *)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
              norm_ip, NET_IP_STR_LEN);
  if (clusterHandshakeInProgress(norm_ip, port, cport)) {
    errno = EAGAIN;
    return 0;
  }
  n = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_MEET);
  memcpy(n->ip, norm_ip, sizeof(n->ip));
  n->port = port;
  n->cport = cport;
  clusterAddNode(n);
  return 1;
}
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
  uint16_t count = ntohs(hdr->count);
  clusterMsgDataGossip *g = (clusterMsgDataGossip *)hdr->data.ping.gossip;
  clusterNode *sender =
      link->node ? link->node : clusterLookupNode(hdr->sender);
  while (count--) {
    uint16_t flags = ntohs(g->flags);
    clusterNode *node;
    sds ci;
    if (server.verbosity == LL_DEBUG) {
      ci = representClusterNodeFlags(sdsempty(), flags);
      serverLog(LL_DEBUG, "GOSSIP %.40s %s:%d@%d %s", g->nodename, g->ip,
                ntohs(g->port), ntohs(g->cport), ci);
      sdsfree(ci);
    }
    node = clusterLookupNode(g->nodename);
    if (node) {
      if (sender && nodeIsMaster(sender) && node != myself) {
        if (flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) {
          if (clusterNodeAddFailureReport(node, sender)) {
            serverLog(LL_VERBOSE,
                      "Node %.40s reported node %.40s as not reachable.",
                      sender->name, node->name);
          }
          markNodeAsFailingIfNeeded(node);
        } else {
          if (clusterNodeDelFailureReport(node, sender)) {
            serverLog(LL_VERBOSE,
                      "Node %.40s reported node %.40s is back online.",
                      sender->name, node->name);
          }
        }
      }
      if (!(flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) &&
          node->ping_sent == 0 && clusterNodeFailureReportsCount(node) == 0) {
        mstime_t pongtime = ntohl(g->pong_received);
        pongtime *= 1000;
        if (pongtime <= (server.mstime + 500) &&
            pongtime > node->pong_received) {
          node->pong_received = pongtime;
        }
      }
      if (node->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL) &&
          !(flags & CLUSTER_NODE_NOADDR) &&
          !(flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) &&
          (strcasecmp(node->ip, g->ip) || node->port != ntohs(g->port) ||
           node->cport != ntohs(g->cport))) {
        if (node->link) freeClusterLink(node->link);
        memcpy(node->ip, g->ip, NET_IP_STR_LEN);
        node->port = ntohs(g->port);
        node->cport = ntohs(g->cport);
        node->flags &= ~CLUSTER_NODE_NOADDR;
      }
    } else {
      if (sender && !(flags & CLUSTER_NODE_NOADDR) &&
          !clusterBlacklistExists(g->nodename)) {
        clusterStartHandshake(g->ip, ntohs(g->port), ntohs(g->cport));
      }
    }
    g++;
  }
}
void nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
  if (announced_ip[0] != '\0') {
    memcpy(buf, announced_ip, NET_IP_STR_LEN);
    buf[NET_IP_STR_LEN - 1] = '\0';
  } else {
    connPeerToString(link->conn, buf, NET_IP_STR_LEN, NULL);
  }
}
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link,
                              clusterMsg *hdr) {
  char ip[NET_IP_STR_LEN] = {0};
  int port = ntohs(hdr->port);
  int cport = ntohs(hdr->cport);
  if (link == node->link) return 0;
  nodeIp2String(ip, link, hdr->myip);
  if (node->port == port && node->cport == cport && strcmp(ip, node->ip) == 0)
    return 0;
  memcpy(node->ip, ip, sizeof(ip));
  node->port = port;
  node->cport = cport;
  if (node->link) freeClusterLink(node->link);
  node->flags &= ~CLUSTER_NODE_NOADDR;
  serverLog(LL_WARNING, "Address updated for node %.40s, now %s:%d", node->name,
            node->ip, node->port);
  if (nodeIsSlave(myself) && myself->slaveof == node)
    replicationSetMaster(node->ip, node->port);
  return 1;
}
void clusterSetNodeAsMaster(clusterNode *n) {
  if (nodeIsMaster(n)) return;
  if (n->slaveof) {
    clusterNodeRemoveSlave(n->slaveof, n);
    if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
  }
  n->flags &= ~CLUSTER_NODE_SLAVE;
  n->flags |= CLUSTER_NODE_MASTER;
  n->slaveof = NULL;
  clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
}
void clusterUpdateSlotsConfigWith(clusterNode *sender,
                                  uint64_t senderConfigEpoch,
                                  unsigned char *slots) {
  int j;
  clusterNode *curmaster, *newmaster = NULL;
  uint16_t dirty_slots[CLUSTER_SLOTS];
  int dirty_slots_count = 0;
  curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;
  if (sender == myself) {
    serverLog(LL_WARNING, "Discarding UPDATE message about myself.");
    return;
  }
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (bitmapTestBit(slots, j)) {
      if (server.cluster->slots[j] == sender) continue;
      if (server.cluster->importing_slots_from[j]) continue;
      if (server.cluster->slots[j] == NULL ||
          server.cluster->slots[j]->configEpoch < senderConfigEpoch) {
        if (server.cluster->slots[j] == myself && countKeysInSlot(j) &&
            sender != myself) {
          dirty_slots[dirty_slots_count] = j;
          dirty_slots_count++;
        }
        if (server.cluster->slots[j] == curmaster) newmaster = sender;
        clusterDelSlot(j);
        clusterAddSlot(sender, j);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                             CLUSTER_TODO_UPDATE_STATE |
                             CLUSTER_TODO_FSYNC_CONFIG);
      }
    }
  }
  if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION) return;
  if (newmaster && curmaster->numslots == 0) {
    serverLog(LL_WARNING,
              "Configuration change detected. Reconfiguring myself "
              "as a replica of %.40s",
              sender->name);
    clusterSetMaster(sender);
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE |
                         CLUSTER_TODO_FSYNC_CONFIG);
  } else if (dirty_slots_count) {
    for (j = 0; j < dirty_slots_count; j++) delKeysInSlot(dirty_slots[j]);
  }
}
int clusterProcessPacket(clusterLink *link) {
  clusterMsg *hdr = (clusterMsg *)link->rcvbuf;
  uint32_t totlen = ntohl(hdr->totlen);
  uint16_t type = ntohs(hdr->type);
  if (type < CLUSTERMSG_TYPE_COUNT)
    server.cluster->stats_bus_messages_received[type]++;
  serverLog(LL_DEBUG, "--- Processing packet of type %d, %lu bytes", type,
            (unsigned long)totlen);
  if (totlen < 16) return 1;
  if (totlen > sdslen(link->rcvbuf)) return 1;
  if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
    return 1;
  }
  uint16_t flags = ntohs(hdr->flags);
  uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
  clusterNode *sender;
  if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
      type == CLUSTERMSG_TYPE_MEET) {
    uint16_t count = ntohs(hdr->count);
    uint32_t explen;
    explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    explen += (sizeof(clusterMsgDataGossip) * count);
    if (totlen != explen) return 1;
  } else if (type == CLUSTERMSG_TYPE_FAIL) {
    uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    explen += sizeof(clusterMsgDataFail);
    if (totlen != explen) return 1;
  } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
    uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    explen += sizeof(clusterMsgDataPublish) - 8 +
              ntohl(hdr->data.publish.msg.channel_len) +
              ntohl(hdr->data.publish.msg.message_len);
    if (totlen != explen) return 1;
  } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
             type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
             type == CLUSTERMSG_TYPE_MFSTART) {
    uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    if (totlen != explen) return 1;
  } else if (type == CLUSTERMSG_TYPE_UPDATE) {
    uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    explen += sizeof(clusterMsgDataUpdate);
    if (totlen != explen) return 1;
  } else if (type == CLUSTERMSG_TYPE_MODULE) {
    uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    explen +=
        sizeof(clusterMsgDataPublish) - 3 + ntohl(hdr->data.module.msg.len);
    if (totlen != explen) return 1;
  }
  sender = clusterLookupNode(hdr->sender);
  if (sender && !nodeInHandshake(sender)) {
    senderCurrentEpoch = ntohu64(hdr->currentEpoch);
    senderConfigEpoch = ntohu64(hdr->configEpoch);
    if (senderCurrentEpoch > server.cluster->currentEpoch)
      server.cluster->currentEpoch = senderCurrentEpoch;
    if (senderConfigEpoch > sender->configEpoch) {
      sender->configEpoch = senderConfigEpoch;
      clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                           CLUSTER_TODO_FSYNC_CONFIG);
    }
    sender->repl_offset = ntohu64(hdr->offset);
    sender->repl_offset_time = mstime();
    if (server.cluster->mf_end && nodeIsSlave(myself) &&
        myself->slaveof == sender && hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
        server.cluster->mf_master_offset == 0) {
      server.cluster->mf_master_offset = sender->repl_offset;
      serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
    }
  }
  if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
    serverLog(LL_DEBUG, "Ping packet received: %p", (void *)link->node);
    if ((type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') &&
        server.cluster_announce_ip == NULL) {
      char ip[NET_IP_STR_LEN];
      if (connSockName(link->conn, ip, sizeof(ip), NULL) != -1 &&
          strcmp(ip, myself->ip)) {
        memcpy(myself->ip, ip, NET_IP_STR_LEN);
        serverLog(LL_WARNING, "IP address for this node updated to %s",
                  myself->ip);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
      }
    }
    if (!sender && type == CLUSTERMSG_TYPE_MEET) {
      clusterNode *node;
      node = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE);
      nodeIp2String(node->ip, link, hdr->myip);
      node->port = ntohs(hdr->port);
      node->cport = ntohs(hdr->cport);
      clusterAddNode(node);
      clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
    }
    if (!sender && type == CLUSTERMSG_TYPE_MEET)
      clusterProcessGossipSection(hdr, link);
    clusterSendPing(link, CLUSTERMSG_TYPE_PONG);
  }
  if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
      type == CLUSTERMSG_TYPE_MEET) {
    serverLog(LL_DEBUG, "%s packet received: %p",
              type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
              (void *)link->node);
    if (link->node) {
      if (nodeInHandshake(link->node)) {
        if (sender) {
          serverLog(LL_VERBOSE,
                    "Handshake: we already know node %.40s, "
                    "updating the address if needed.",
                    sender->name);
          if (nodeUpdateAddressIfNeeded(sender, link, hdr)) {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                                 CLUSTER_TODO_UPDATE_STATE);
          }
          clusterDelNode(link->node);
          return 0;
        }
        clusterRenameNode(link->node, hdr->sender);
        serverLog(LL_DEBUG, "Handshake with node %.40s completed.",
                  link->node->name);
        link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
        link->node->flags |= flags & (CLUSTER_NODE_MASTER | CLUSTER_NODE_SLAVE);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
      } else if (memcmp(link->node->name, hdr->sender, CLUSTER_NAMELEN) != 0) {
        serverLog(LL_DEBUG,
                  "PONG contains mismatching sender ID. About node %.40s added "
                  "%d ms ago, having flags %d",
                  link->node->name, (int)(mstime() - (link->node->ctime)),
                  link->node->flags);
        link->node->flags |= CLUSTER_NODE_NOADDR;
        link->node->ip[0] = '\0';
        link->node->port = 0;
        link->node->cport = 0;
        freeClusterLink(link);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        return 0;
      }
    }
    if (sender) {
      int nofailover = flags & CLUSTER_NODE_NOFAILOVER;
      sender->flags &= ~CLUSTER_NODE_NOFAILOVER;
      sender->flags |= nofailover;
    }
    if (sender && type == CLUSTERMSG_TYPE_PING && !nodeInHandshake(sender) &&
        nodeUpdateAddressIfNeeded(sender, link, hdr)) {
      clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                           CLUSTER_TODO_UPDATE_STATE);
    }
    if (link->node && type == CLUSTERMSG_TYPE_PONG) {
      link->node->pong_received = mstime();
      link->node->ping_sent = 0;
      if (nodeTimedOut(link->node)) {
        link->node->flags &= ~CLUSTER_NODE_PFAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                             CLUSTER_TODO_UPDATE_STATE);
      } else if (nodeFailed(link->node)) {
        clearNodeFailureIfNeeded(link->node);
      }
    }
    if (sender) {
      if (!memcmp(hdr->slaveof, CLUSTER_NODE_NULL_NAME, sizeof(hdr->slaveof))) {
        clusterSetNodeAsMaster(sender);
      } else {
        clusterNode *master = clusterLookupNode(hdr->slaveof);
        if (nodeIsMaster(sender)) {
          clusterDelNodeSlots(sender);
          sender->flags &= ~(CLUSTER_NODE_MASTER | CLUSTER_NODE_MIGRATE_TO);
          sender->flags |= CLUSTER_NODE_SLAVE;
          clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                               CLUSTER_TODO_UPDATE_STATE);
        }
        if (master && sender->slaveof != master) {
          if (sender->slaveof) clusterNodeRemoveSlave(sender->slaveof, sender);
          clusterNodeAddSlave(master, sender);
          sender->slaveof = master;
          clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }
      }
    }
    clusterNode *sender_master = NULL;
    int dirty_slots = 0;
    if (sender) {
      sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
      if (sender_master) {
        dirty_slots = memcmp(sender_master->slots, hdr->myslots,
                             sizeof(hdr->myslots)) != 0;
      }
    }
    if (sender && nodeIsMaster(sender) && dirty_slots)
      clusterUpdateSlotsConfigWith(sender, senderConfigEpoch, hdr->myslots);
    if (sender && dirty_slots) {
      int j;
      for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(hdr->myslots, j)) {
          if (server.cluster->slots[j] == sender ||
              server.cluster->slots[j] == NULL)
            continue;
          if (server.cluster->slots[j]->configEpoch > senderConfigEpoch) {
            serverLog(LL_VERBOSE,
                      "Node %.40s has old slots configuration, sending "
                      "an UPDATE message about %.40s",
                      sender->name, server.cluster->slots[j]->name);
            clusterSendUpdate(sender->link, server.cluster->slots[j]);
            break;
          }
        }
      }
    }
    if (sender && nodeIsMaster(myself) && nodeIsMaster(sender) &&
        senderConfigEpoch == myself->configEpoch) {
      clusterHandleConfigEpochCollision(sender);
    }
    if (sender) clusterProcessGossipSection(hdr, link);
  } else if (type == CLUSTERMSG_TYPE_FAIL) {
    clusterNode *failing;
    if (sender) {
      failing = clusterLookupNode(hdr->data.fail.about.nodename);
      if (failing &&
          !(failing->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_MYSELF))) {
        serverLog(LL_NOTICE, "FAIL message received from %.40s about %.40s",
                  hdr->sender, hdr->data.fail.about.nodename);
        failing->flags |= CLUSTER_NODE_FAIL;
        failing->fail_time = mstime();
        failing->flags &= ~CLUSTER_NODE_PFAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                             CLUSTER_TODO_UPDATE_STATE);
      }
    } else {
      serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
    }
  } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
    robj *channel, *message;
    uint32_t channel_len, message_len;
    if (dictSize(server.pubsub_channels) ||
        listLength(server.pubsub_patterns)) {
      channel_len = ntohl(hdr->data.publish.msg.channel_len);
      message_len = ntohl(hdr->data.publish.msg.message_len);
      channel = createStringObject((char *)hdr->data.publish.msg.bulk_data,
                                   channel_len);
      message = createStringObject(
          (char *)hdr->data.publish.msg.bulk_data + channel_len, message_len);
      pubsubPublishMessage(channel, message);
      decrRefCount(channel);
      decrRefCount(message);
    }
  } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
    if (!sender) return 1;
    clusterSendFailoverAuthIfNeeded(sender, hdr);
  } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
    if (!sender) return 1;
    if (nodeIsMaster(sender) && sender->numslots > 0 &&
        senderCurrentEpoch >= server.cluster->failover_auth_epoch) {
      server.cluster->failover_auth_count++;
      clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
    }
  } else if (type == CLUSTERMSG_TYPE_MFSTART) {
    if (!sender || sender->slaveof != myself) return 1;
    resetManualFailover();
    server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
    server.cluster->mf_slave = sender;
    pauseClients(mstime() + (CLUSTER_MF_TIMEOUT * 2));
    serverLog(LL_WARNING, "Manual failover requested by replica %.40s.",
              sender->name);
  } else if (type == CLUSTERMSG_TYPE_UPDATE) {
    clusterNode *n;
    uint64_t reportedConfigEpoch =
        ntohu64(hdr->data.update.nodecfg.configEpoch);
    if (!sender) return 1;
    n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
    if (!n) return 1;
    if (n->configEpoch >= reportedConfigEpoch) return 1;
    if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);
    n->configEpoch = reportedConfigEpoch;
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
    clusterUpdateSlotsConfigWith(n, reportedConfigEpoch,
                                 hdr->data.update.nodecfg.slots);
  } else if (type == CLUSTERMSG_TYPE_MODULE) {
    if (!sender) return 1;
    uint64_t module_id = hdr->data.module.msg.module_id;
    uint32_t len = ntohl(hdr->data.module.msg.len);
    uint8_t type = hdr->data.module.msg.type;
    unsigned char *payload = hdr->data.module.msg.bulk_data;
    moduleCallClusterReceivers(sender->name, module_id, type, payload, len);
  } else {
    serverLog(LL_WARNING, "Received unknown packet type: %d", type);
  }
  return 1;
}
void handleLinkIOError(clusterLink *link) { freeClusterLink(link); }
void clusterWriteHandler(connection *conn) {
  clusterLink *link = connGetPrivateData(conn);
  ssize_t nwritten;
  nwritten = connWrite(conn, link->sndbuf, sdslen(link->sndbuf));
  if (nwritten <= 0) {
    serverLog(LL_DEBUG, "I/O error writing to node link: %s",
              (nwritten == -1) ? connGetLastError(conn) : "short write");
    handleLinkIOError(link);
    return;
  }
  sdsrange(link->sndbuf, nwritten, -1);
  if (sdslen(link->sndbuf) == 0) connSetWriteHandler(link->conn, NULL);
}
void clusterLinkConnectHandler(connection *conn) {
  clusterLink *link = connGetPrivateData(conn);
  clusterNode *node = link->node;
  if (connGetState(conn) != CONN_STATE_CONNECTED) {
    serverLog(LL_VERBOSE, "Connection with Node %.40s at %s:%d failed: %s",
              node->name, node->ip, node->cport, connGetLastError(conn));
    freeClusterLink(link);
    return;
  }
  connSetReadHandler(conn, clusterReadHandler);
  mstime_t old_ping_sent = node->ping_sent;
  clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ? CLUSTERMSG_TYPE_MEET
                                                        : CLUSTERMSG_TYPE_PING);
  if (old_ping_sent) {
    node->ping_sent = old_ping_sent;
  }
  node->flags &= ~CLUSTER_NODE_MEET;
  serverLog(LL_DEBUG, "Connecting with Node %.40s at %s:%d", node->name,
            node->ip, node->cport);
}
void clusterReadHandler(connection *conn) {
  clusterMsg buf[1];
  ssize_t nread;
  clusterMsg *hdr;
  clusterLink *link = connGetPrivateData(conn);
  unsigned int readlen, rcvbuflen;
  while (1) {
    rcvbuflen = sdslen(link->rcvbuf);
    if (rcvbuflen < 8) {
      readlen = 8 - rcvbuflen;
    } else {
      hdr = (clusterMsg *)link->rcvbuf;
      if (rcvbuflen == 8) {
        if (memcmp(hdr->sig, "RCmb", 4) != 0 ||
            ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN) {
          serverLog(LL_WARNING,
                    "Bad message length or signature received "
                    "from Cluster bus.");
          handleLinkIOError(link);
          return;
        }
      }
      readlen = ntohl(hdr->totlen) - rcvbuflen;
      if (readlen > sizeof(buf)) readlen = sizeof(buf);
    }
    nread = connRead(conn, buf, readlen);
    if (nread == -1 && (connGetState(conn) == CONN_STATE_CONNECTED))
      return;
    if (nread <= 0) {
      serverLog(LL_DEBUG, "I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : connGetLastError(conn));
      handleLinkIOError(link);
      return;
    } else {
      link->rcvbuf = sdscatlen(link->rcvbuf, buf, nread);
      hdr = (clusterMsg *)link->rcvbuf;
      rcvbuflen += nread;
    }
    if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
      if (clusterProcessPacket(link)) {
        sdsfree(link->rcvbuf);
        link->rcvbuf = sdsempty();
      } else {
        return;
      }
    }
  }
}
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
  if (sdslen(link->sndbuf) == 0 && msglen != 0)
    connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);
  link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);
  clusterMsg *hdr = (clusterMsg *)msg;
  uint16_t type = ntohs(hdr->type);
  if (type < CLUSTERMSG_TYPE_COUNT)
    server.cluster->stats_bus_messages_sent[type]++;
}
void clusterBroadcastMessage(void *buf, size_t len) {
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (!node->link) continue;
    if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_HANDSHAKE)) continue;
    clusterSendMessage(node->link, buf, len);
  }
  dictReleaseIterator(di);
}
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
  int totlen = 0;
  uint64_t offset;
  clusterNode *master;
  master = (nodeIsSlave(myself) && myself->slaveof) ? myself->slaveof : myself;
  memset(hdr, 0, sizeof(*hdr));
  hdr->ver = htons(CLUSTER_PROTO_VER);
  hdr->sig[0] = 'R';
  hdr->sig[1] = 'C';
  hdr->sig[2] = 'm';
  hdr->sig[3] = 'b';
  hdr->type = htons(type);
  memcpy(hdr->sender, myself->name, CLUSTER_NAMELEN);
  memset(hdr->myip, 0, NET_IP_STR_LEN);
  if (server.cluster_announce_ip) {
    strncpy(hdr->myip, server.cluster_announce_ip, NET_IP_STR_LEN);
    hdr->myip[NET_IP_STR_LEN - 1] = '\0';
  }
  int port = server.tls_cluster ? server.tls_port : server.port;
  int announced_port =
      server.cluster_announce_port ? server.cluster_announce_port : port;
  int announced_cport = server.cluster_announce_bus_port
                            ? server.cluster_announce_bus_port
                            : (port + CLUSTER_PORT_INCR);
  memcpy(hdr->myslots, master->slots, sizeof(hdr->myslots));
  memset(hdr->slaveof, 0, CLUSTER_NAMELEN);
  if (myself->slaveof != NULL)
    memcpy(hdr->slaveof, myself->slaveof->name, CLUSTER_NAMELEN);
  hdr->port = htons(announced_port);
  hdr->cport = htons(announced_cport);
  hdr->flags = htons(myself->flags);
  hdr->state = server.cluster->state;
  hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
  hdr->configEpoch = htonu64(master->configEpoch);
  if (nodeIsSlave(myself))
    offset = replicationGetSlaveOffset();
  else
    offset = server.master_repl_offset;
  hdr->offset = htonu64(offset);
  if (nodeIsMaster(myself) && server.cluster->mf_end)
    hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;
  if (type == CLUSTERMSG_TYPE_FAIL) {
    totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataFail);
  } else if (type == CLUSTERMSG_TYPE_UPDATE) {
    totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataUpdate);
  }
  hdr->totlen = htonl(totlen);
}
int clusterNodeIsInGossipSection(clusterMsg *hdr, int count, clusterNode *n) {
  int j;
  for (j = 0; j < count; j++) {
    if (memcmp(hdr->data.ping.gossip[j].nodename, n->name, CLUSTER_NAMELEN) ==
        0)
      break;
  }
  return j != count;
}
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
  clusterMsgDataGossip *gossip;
  gossip = &(hdr->data.ping.gossip[i]);
  memcpy(gossip->nodename, n->name, CLUSTER_NAMELEN);
  gossip->ping_sent = htonl(n->ping_sent / 1000);
  gossip->pong_received = htonl(n->pong_received / 1000);
  memcpy(gossip->ip, n->ip, sizeof(n->ip));
  gossip->port = htons(n->port);
  gossip->cport = htons(n->cport);
  gossip->flags = htons(n->flags);
  gossip->notused1 = 0;
}
void clusterSendPing(clusterLink *link, int type) {
  unsigned char *buf;
  clusterMsg *hdr;
  int gossipcount = 0;
  int wanted;
  int totlen;
  int freshnodes = dictSize(server.cluster->nodes) - 2;
  wanted = floor(dictSize(server.cluster->nodes) / 10);
  if (wanted < 3) wanted = 3;
  if (wanted > freshnodes) wanted = freshnodes;
  int pfail_wanted = server.cluster->stats_pfail_nodes;
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  totlen += (sizeof(clusterMsgDataGossip) * (wanted + pfail_wanted));
  if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
  buf = zcalloc(totlen);
  hdr = (clusterMsg *)buf;
  if (link->node && type == CLUSTERMSG_TYPE_PING)
    link->node->ping_sent = mstime();
  clusterBuildMessageHdr(hdr, type);
  int maxiterations = wanted * 3;
  while (freshnodes > 0 && gossipcount < wanted && maxiterations--) {
    dictEntry *de = dictGetRandomKey(server.cluster->nodes);
    clusterNode *this = dictGetVal(de);
    if (this == myself) continue;
    if (this->flags & CLUSTER_NODE_PFAIL) continue;
    if (this->flags & (CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_NOADDR) ||
        (this->link == NULL && this->numslots == 0)) {
      freshnodes--;
      continue;
    }
    if (clusterNodeIsInGossipSection(hdr, gossipcount, this)) continue;
    clusterSetGossipEntry(hdr, gossipcount, this);
    freshnodes--;
    gossipcount++;
  }
  if (pfail_wanted) {
    dictIterator *di;
    dictEntry *de;
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL && pfail_wanted > 0) {
      clusterNode *node = dictGetVal(de);
      if (node->flags & CLUSTER_NODE_HANDSHAKE) continue;
      if (node->flags & CLUSTER_NODE_NOADDR) continue;
      if (!(node->flags & CLUSTER_NODE_PFAIL)) continue;
      clusterSetGossipEntry(hdr, gossipcount, node);
      freshnodes--;
      gossipcount++;
      pfail_wanted--;
    }
    dictReleaseIterator(di);
  }
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  totlen += (sizeof(clusterMsgDataGossip) * gossipcount);
  hdr->count = htons(gossipcount);
  hdr->totlen = htonl(totlen);
  clusterSendMessage(link, buf, totlen);
  zfree(buf);
}
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (!node->link) continue;
    if (node == myself || nodeInHandshake(node)) continue;
    if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
      int local_slave =
          nodeIsSlave(node) && node->slaveof &&
          (node->slaveof == myself || node->slaveof == myself->slaveof);
      if (!local_slave) continue;
    }
    clusterSendPing(node->link, CLUSTERMSG_TYPE_PONG);
  }
  dictReleaseIterator(di);
}
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
  unsigned char *payload;
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  uint32_t totlen;
  uint32_t channel_len, message_len;
  channel = getDecodedObject(channel);
  message = getDecodedObject(message);
  channel_len = sdslen(channel->ptr);
  message_len = sdslen(message->ptr);
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_PUBLISH);
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;
  hdr->data.publish.msg.channel_len = htonl(channel_len);
  hdr->data.publish.msg.message_len = htonl(message_len);
  hdr->totlen = htonl(totlen);
  if (totlen < sizeof(buf)) {
    payload = (unsigned char *)buf;
  } else {
    payload = zmalloc(totlen);
    memcpy(payload, hdr, sizeof(*hdr));
    hdr = (clusterMsg *)payload;
  }
  memcpy(hdr->data.publish.msg.bulk_data, channel->ptr, sdslen(channel->ptr));
  memcpy(hdr->data.publish.msg.bulk_data + sdslen(channel->ptr), message->ptr,
         sdslen(message->ptr));
  if (link)
    clusterSendMessage(link, payload, totlen);
  else
    clusterBroadcastMessage(payload, totlen);
  decrRefCount(channel);
  decrRefCount(message);
  if (payload != (unsigned char *)buf) zfree(payload);
}
void clusterSendFail(char *nodename) {
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAIL);
  memcpy(hdr->data.fail.about.nodename, nodename, CLUSTER_NAMELEN);
  clusterBroadcastMessage(buf, ntohl(hdr->totlen));
}
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  if (link == NULL) return;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_UPDATE);
  memcpy(hdr->data.update.nodecfg.nodename, node->name, CLUSTER_NAMELEN);
  hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
  memcpy(hdr->data.update.nodecfg.slots, node->slots, sizeof(node->slots));
  clusterSendMessage(link, (unsigned char *)buf, ntohl(hdr->totlen));
}
void clusterSendModule(clusterLink *link, uint64_t module_id, uint8_t type,
                       unsigned char *payload, uint32_t len) {
  unsigned char *heapbuf;
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  uint32_t totlen;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_MODULE);
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  totlen += sizeof(clusterMsgModule) - 3 + len;
  hdr->data.module.msg.module_id = module_id;
  hdr->data.module.msg.type = type;
  hdr->data.module.msg.len = htonl(len);
  hdr->totlen = htonl(totlen);
  if (totlen < sizeof(buf)) {
    heapbuf = (unsigned char *)buf;
  } else {
    heapbuf = zmalloc(totlen);
    memcpy(heapbuf, hdr, sizeof(*hdr));
    hdr = (clusterMsg *)heapbuf;
  }
  memcpy(hdr->data.module.msg.bulk_data, payload, len);
  if (link)
    clusterSendMessage(link, heapbuf, totlen);
  else
    clusterBroadcastMessage(heapbuf, totlen);
  if (heapbuf != (unsigned char *)buf) zfree(heapbuf);
}
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id,
                                     uint8_t type, unsigned char *payload,
                                     uint32_t len) {
  clusterNode *node = NULL;
  if (target != NULL) {
    node = clusterLookupNode(target);
    if (node == NULL || node->link == NULL) return C_ERR;
  }
  clusterSendModule(target ? node->link : NULL, module_id, type, payload, len);
  return C_OK;
}
void clusterPropagatePublish(robj *channel, robj *message) {
  clusterSendPublish(NULL, channel, message);
}
void clusterRequestFailoverAuth(void) {
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  uint32_t totlen;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
  if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  hdr->totlen = htonl(totlen);
  clusterBroadcastMessage(buf, totlen);
}
void clusterSendFailoverAuth(clusterNode *node) {
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  uint32_t totlen;
  if (!node->link) return;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  hdr->totlen = htonl(totlen);
  clusterSendMessage(node->link, (unsigned char *)buf, totlen);
}
void clusterSendMFStart(clusterNode *node) {
  clusterMsg buf[1];
  clusterMsg *hdr = (clusterMsg *)buf;
  uint32_t totlen;
  if (!node->link) return;
  clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_MFSTART);
  totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
  hdr->totlen = htonl(totlen);
  clusterSendMessage(node->link, (unsigned char *)buf, totlen);
}
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
  clusterNode *master = node->slaveof;
  uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
  uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
  unsigned char *claimed_slots = request->myslots;
  int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
  int j;
  if (nodeIsSlave(myself) || myself->numslots == 0) return;
  if (requestCurrentEpoch < server.cluster->currentEpoch) {
    serverLog(LL_WARNING,
              "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
              node->name, (unsigned long long)requestCurrentEpoch,
              (unsigned long long)server.cluster->currentEpoch);
    return;
  }
  if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
    serverLog(LL_WARNING,
              "Failover auth denied to %.40s: already voted for epoch %llu",
              node->name, (unsigned long long)server.cluster->currentEpoch);
    return;
  }
  if (nodeIsMaster(node) || master == NULL ||
      (!nodeFailed(master) && !force_ack)) {
    if (nodeIsMaster(node)) {
      serverLog(LL_WARNING,
                "Failover auth denied to %.40s: it is a master node",
                node->name);
    } else if (master == NULL) {
      serverLog(LL_WARNING,
                "Failover auth denied to %.40s: I don't know its master",
                node->name);
    } else if (!nodeFailed(master)) {
      serverLog(LL_WARNING, "Failover auth denied to %.40s: its master is up",
                node->name);
    }
    return;
  }
  if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2) {
    serverLog(LL_WARNING,
              "Failover auth denied to %.40s: "
              "can't vote about this master before %lld milliseconds",
              node->name,
              (long long)((server.cluster_node_timeout * 2) -
                          (mstime() - node->slaveof->voted_time)));
    return;
  }
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (bitmapTestBit(claimed_slots, j) == 0) continue;
    if (server.cluster->slots[j] == NULL ||
        server.cluster->slots[j]->configEpoch <= requestConfigEpoch) {
      continue;
    }
    serverLog(LL_WARNING,
              "Failover auth denied to %.40s: "
              "slot %d epoch (%llu) > reqEpoch (%llu)",
              node->name, j,
              (unsigned long long)server.cluster->slots[j]->configEpoch,
              (unsigned long long)requestConfigEpoch);
    return;
  }
  server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
  node->slaveof->voted_time = mstime();
  clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
  clusterSendFailoverAuth(node);
  serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
            node->name, (unsigned long long)server.cluster->currentEpoch);
}
int clusterGetSlaveRank(void) {
  long long myoffset;
  int j, rank = 0;
  clusterNode *master;
  serverAssert(nodeIsSlave(myself));
  master = myself->slaveof;
  if (master == NULL) return 0;
  myoffset = replicationGetSlaveOffset();
  for (j = 0; j < master->numslaves; j++)
    if (master->slaves[j] != myself && !nodeCantFailover(master->slaves[j]) &&
        master->slaves[j]->repl_offset > myoffset)
      rank++;
  return rank;
}
void clusterLogCantFailover(int reason) {
  char *msg;
  static time_t lastlog_time = 0;
  mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;
  if (reason == server.cluster->cant_failover_reason &&
      time(NULL) - lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
    return;
  server.cluster->cant_failover_reason = reason;
  if (myself->slaveof && nodeFailed(myself->slaveof) &&
      (mstime() - myself->slaveof->fail_time) < nolog_fail_time)
    return;
  switch (reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
      msg =
          "Disconnected from master for longer than allowed. "
          "Please check the 'cluster-replica-validity-factor' configuration "
          "option.";
      break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
      msg = "Waiting the delay before I can start a new failover.";
      break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
      msg = "Failover attempt expired.";
      break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
      msg = "Waiting for votes, but majority still not reached.";
      break;
    default:
      msg = "Unknown reason code.";
      break;
  }
  lastlog_time = time(NULL);
  serverLog(LL_WARNING, "Currently unable to failover: %s", msg);
}
void clusterFailoverReplaceYourMaster(void) {
  int j;
  clusterNode *oldmaster = myself->slaveof;
  if (nodeIsMaster(myself) || oldmaster == NULL) return;
  clusterSetNodeAsMaster(myself);
  replicationUnsetMaster();
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (clusterNodeGetSlotBit(oldmaster, j)) {
      clusterDelSlot(j);
      clusterAddSlot(myself, j);
    }
  }
  clusterUpdateState();
  clusterSaveConfigOrDie(1);
  clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
  resetManualFailover();
}
void clusterHandleSlaveFailover(void) {
  mstime_t data_age;
  mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
  int needed_quorum = (server.cluster->size / 2) + 1;
  int manual_failover =
      server.cluster->mf_end != 0 && server.cluster->mf_can_start;
  mstime_t auth_timeout, auth_retry_time;
  server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;
  auth_timeout = server.cluster_node_timeout * 2;
  if (auth_timeout < 2000) auth_timeout = 2000;
  auth_retry_time = auth_timeout * 2;
  if (nodeIsMaster(myself) || myself->slaveof == NULL ||
      (!nodeFailed(myself->slaveof) && !manual_failover) ||
      (server.cluster_slave_no_failover && !manual_failover) ||
      myself->slaveof->numslots == 0) {
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    return;
  }
  if (server.repl_state == REPL_STATE_CONNECTED) {
    data_age =
        (mstime_t)(server.unixtime - server.master->lastinteraction) * 1000;
  } else {
    data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
  }
  if (data_age > server.cluster_node_timeout)
    data_age -= server.cluster_node_timeout;
  if (server.cluster_slave_validity_factor &&
      data_age > (((mstime_t)server.repl_ping_slave_period * 1000) +
                  (server.cluster_node_timeout *
                   server.cluster_slave_validity_factor))) {
    if (!manual_failover) {
      clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
      return;
    }
  }
  if (auth_age > auth_retry_time) {
    server.cluster->failover_auth_time =
        mstime() +
        500 +
        random() % 500;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_sent = 0;
    server.cluster->failover_auth_rank = clusterGetSlaveRank();
    server.cluster->failover_auth_time +=
        server.cluster->failover_auth_rank * 1000;
    if (server.cluster->mf_end) {
      server.cluster->failover_auth_time = mstime();
      server.cluster->failover_auth_rank = 0;
      clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
    }
    serverLog(LL_WARNING,
              "Start of election delayed for %lld milliseconds "
              "(rank #%d, offset %lld).",
              server.cluster->failover_auth_time - mstime(),
              server.cluster->failover_auth_rank, replicationGetSlaveOffset());
    clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
    return;
  }
  if (server.cluster->failover_auth_sent == 0 && server.cluster->mf_end == 0) {
    int newrank = clusterGetSlaveRank();
    if (newrank > server.cluster->failover_auth_rank) {
      long long added_delay =
          (newrank - server.cluster->failover_auth_rank) * 1000;
      server.cluster->failover_auth_time += added_delay;
      server.cluster->failover_auth_rank = newrank;
      serverLog(
          LL_WARNING,
          "Replica rank updated to #%d, added %lld milliseconds of delay.",
          newrank, added_delay);
    }
  }
  if (mstime() < server.cluster->failover_auth_time) {
    clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
    return;
  }
  if (auth_age > auth_timeout) {
    clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
    return;
  }
  if (server.cluster->failover_auth_sent == 0) {
    server.cluster->currentEpoch++;
    server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
    serverLog(LL_WARNING, "Starting a failover election for epoch %llu.",
              (unsigned long long)server.cluster->currentEpoch);
    clusterRequestFailoverAuth();
    server.cluster->failover_auth_sent = 1;
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE |
                         CLUSTER_TODO_FSYNC_CONFIG);
    return;
  }
  if (server.cluster->failover_auth_count >= needed_quorum) {
    serverLog(LL_WARNING, "Failover election won: I'm the new master.");
    if (myself->configEpoch < server.cluster->failover_auth_epoch) {
      myself->configEpoch = server.cluster->failover_auth_epoch;
      serverLog(LL_WARNING, "configEpoch set to %llu after successful failover",
                (unsigned long long)myself->configEpoch);
    }
    clusterFailoverReplaceYourMaster();
  } else {
    clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
  }
}
void clusterHandleSlaveMigration(int max_slaves) {
  int j, okslaves = 0;
  clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
  dictIterator *di;
  dictEntry *de;
  if (server.cluster->state != CLUSTER_OK) return;
  if (mymaster == NULL) return;
  for (j = 0; j < mymaster->numslaves; j++)
    if (!nodeFailed(mymaster->slaves[j]) && !nodeTimedOut(mymaster->slaves[j]))
      okslaves++;
  if (okslaves <= server.cluster_migration_barrier) return;
  candidate = myself;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    int okslaves = 0, is_orphaned = 1;
    if (nodeIsSlave(node) || nodeFailed(node)) is_orphaned = 0;
    if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;
    if (nodeIsMaster(node)) okslaves = clusterCountNonFailingSlaves(node);
    if (okslaves > 0) is_orphaned = 0;
    if (is_orphaned) {
      if (!target && node->numslots > 0) target = node;
      if (!node->orphaned_time) node->orphaned_time = mstime();
    } else {
      node->orphaned_time = 0;
    }
    if (okslaves == max_slaves) {
      for (j = 0; j < node->numslaves; j++) {
        if (memcmp(node->slaves[j]->name, candidate->name, CLUSTER_NAMELEN) <
            0) {
          candidate = node->slaves[j];
        }
      }
    }
  }
  dictReleaseIterator(di);
  if (target && candidate == myself &&
      (mstime() - target->orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY &&
      !(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER)) {
    serverLog(LL_WARNING, "Migrating to orphaned master %.40s", target->name);
    clusterSetMaster(target);
  }
}
void resetManualFailover(void) {
  if (server.cluster->mf_end && clientsArePaused()) {
    server.clients_pause_end_time = 0;
    clientsArePaused();
  }
  server.cluster->mf_end = 0;
  server.cluster->mf_can_start = 0;
  server.cluster->mf_slave = NULL;
  server.cluster->mf_master_offset = 0;
}
void manualFailoverCheckTimeout(void) {
  if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
    serverLog(LL_WARNING, "Manual failover timed out.");
    resetManualFailover();
  }
}
void clusterHandleManualFailover(void) {
  if (server.cluster->mf_end == 0) return;
  if (server.cluster->mf_can_start) return;
  if (server.cluster->mf_master_offset == 0) return;
  if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
    server.cluster->mf_can_start = 1;
    serverLog(LL_WARNING,
              "All master replication stream processed, "
              "manual failover can start.");
  }
}
void clusterCron(void) {
  dictIterator *di;
  dictEntry *de;
  int update_state = 0;
  int orphaned_masters;
  int max_slaves;
  int this_slaves;
  mstime_t min_pong = 0, now = mstime();
  clusterNode *min_pong_node = NULL;
  static unsigned long long iteration = 0;
  mstime_t handshake_timeout;
  iteration++;
  {
    static char *prev_ip = NULL;
    char *curr_ip = server.cluster_announce_ip;
    int changed = 0;
    if (prev_ip == NULL && curr_ip != NULL)
      changed = 1;
    else if (prev_ip != NULL && curr_ip == NULL)
      changed = 1;
    else if (prev_ip && curr_ip && strcmp(prev_ip, curr_ip))
      changed = 1;
    if (changed) {
      if (prev_ip) zfree(prev_ip);
      prev_ip = curr_ip;
      if (curr_ip) {
        prev_ip = zstrdup(prev_ip);
        strncpy(myself->ip, server.cluster_announce_ip, NET_IP_STR_LEN);
        myself->ip[NET_IP_STR_LEN - 1] = '\0';
      } else {
        myself->ip[0] = '\0';
      }
    }
  }
  handshake_timeout = server.cluster_node_timeout;
  if (handshake_timeout < 1000) handshake_timeout = 1000;
  clusterUpdateMyselfFlags();
  di = dictGetSafeIterator(server.cluster->nodes);
  server.cluster->stats_pfail_nodes = 0;
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_NOADDR)) continue;
    if (node->flags & CLUSTER_NODE_PFAIL) server.cluster->stats_pfail_nodes++;
    if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
      clusterDelNode(node);
      continue;
    }
    if (node->link == NULL) {
      clusterLink *link = createClusterLink(node);
      link->conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
      connSetPrivateData(link->conn, link);
      if (connConnect(link->conn, node->ip, node->cport, NET_FIRST_BIND_ADDR,
                      clusterLinkConnectHandler) == -1) {
        if (node->ping_sent == 0) node->ping_sent = mstime();
        serverLog(LL_DEBUG,
                  "Unable to connect to "
                  "Cluster Node [%s]:%d -> %s",
                  node->ip, node->cport, server.neterr);
        freeClusterLink(link);
        continue;
      }
      node->link = link;
    }
  }
  dictReleaseIterator(di);
  if (!(iteration % 10)) {
    int j;
    for (j = 0; j < 5; j++) {
      de = dictGetRandomKey(server.cluster->nodes);
      clusterNode *this = dictGetVal(de);
      if (this->link == NULL || this->ping_sent != 0) continue;
      if (this->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_HANDSHAKE))
        continue;
      if (min_pong_node == NULL || min_pong > this->pong_received) {
        min_pong_node = this;
        min_pong = this->pong_received;
      }
    }
    if (min_pong_node) {
      serverLog(LL_DEBUG, "Pinging node %.40s", min_pong_node->name);
      clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
    }
  }
  orphaned_masters = 0;
  max_slaves = 0;
  this_slaves = 0;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    now = mstime();
    mstime_t delay;
    if (node->flags &
        (CLUSTER_NODE_MYSELF | CLUSTER_NODE_NOADDR | CLUSTER_NODE_HANDSHAKE))
      continue;
    if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
      int okslaves = clusterCountNonFailingSlaves(node);
      if (okslaves == 0 && node->numslots > 0 &&
          node->flags & CLUSTER_NODE_MIGRATE_TO) {
        orphaned_masters++;
      }
      if (okslaves > max_slaves) max_slaves = okslaves;
      if (nodeIsSlave(myself) && myself->slaveof == node)
        this_slaves = okslaves;
    }
    if (node->link &&
        now - node->link->ctime >
            server.cluster_node_timeout &&
        node->ping_sent &&
        node->pong_received < node->ping_sent &&
        now - node->ping_sent > server.cluster_node_timeout / 2) {
      freeClusterLink(node->link);
    }
    if (node->link && node->ping_sent == 0 &&
        (now - node->pong_received) > server.cluster_node_timeout / 2) {
      clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
      continue;
    }
    if (server.cluster->mf_end && nodeIsMaster(myself) &&
        server.cluster->mf_slave == node && node->link) {
      clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
      continue;
    }
    if (node->ping_sent == 0) continue;
    delay = now - node->ping_sent;
    if (delay > server.cluster_node_timeout) {
      if (!(node->flags & (CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL))) {
        serverLog(LL_DEBUG, "*** NODE %.40s possibly failing", node->name);
        node->flags |= CLUSTER_NODE_PFAIL;
        update_state = 1;
      }
    }
  }
  dictReleaseIterator(di);
  if (nodeIsSlave(myself) && server.masterhost == NULL && myself->slaveof &&
      nodeHasAddr(myself->slaveof)) {
    replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
  }
  manualFailoverCheckTimeout();
  if (nodeIsSlave(myself)) {
    clusterHandleManualFailover();
    if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
      clusterHandleSlaveFailover();
    if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
      clusterHandleSlaveMigration(max_slaves);
  }
  if (update_state || server.cluster->state == CLUSTER_FAIL)
    clusterUpdateState();
}
void clusterBeforeSleep(void) {
  if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
    clusterHandleSlaveFailover();
  if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
    clusterUpdateState();
  if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
    int fsync = server.cluster->todo_before_sleep & CLUSTER_TODO_FSYNC_CONFIG;
    clusterSaveConfigOrDie(fsync);
  }
  server.cluster->todo_before_sleep = 0;
}
void clusterDoBeforeSleep(int flags) {
  server.cluster->todo_before_sleep |= flags;
}
int bitmapTestBit(unsigned char *bitmap, int pos) {
  off_t byte = pos / 8;
  int bit = pos & 7;
  return (bitmap[byte] & (1 << bit)) != 0;
}
void bitmapSetBit(unsigned char *bitmap, int pos) {
  off_t byte = pos / 8;
  int bit = pos & 7;
  bitmap[byte] |= 1 << bit;
}
void bitmapClearBit(unsigned char *bitmap, int pos) {
  off_t byte = pos / 8;
  int bit = pos & 7;
  bitmap[byte] &= ~(1 << bit);
}
int clusterMastersHaveSlaves(void) {
  dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
  dictEntry *de;
  int slaves = 0;
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (nodeIsSlave(node)) continue;
    slaves += node->numslaves;
  }
  dictReleaseIterator(di);
  return slaves != 0;
}
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
  int old = bitmapTestBit(n->slots, slot);
  bitmapSetBit(n->slots, slot);
  if (!old) {
    n->numslots++;
    if (n->numslots == 1 && clusterMastersHaveSlaves())
      n->flags |= CLUSTER_NODE_MIGRATE_TO;
  }
  return old;
}
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
  int old = bitmapTestBit(n->slots, slot);
  bitmapClearBit(n->slots, slot);
  if (old) n->numslots--;
  return old;
}
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
  return bitmapTestBit(n->slots, slot);
}
int clusterAddSlot(clusterNode *n, int slot) {
  if (server.cluster->slots[slot]) return C_ERR;
  clusterNodeSetSlotBit(n, slot);
  server.cluster->slots[slot] = n;
  return C_OK;
}
int clusterDelSlot(int slot) {
  clusterNode *n = server.cluster->slots[slot];
  if (!n) return C_ERR;
  serverAssert(clusterNodeClearSlotBit(n, slot) == 1);
  server.cluster->slots[slot] = NULL;
  return C_OK;
}
int clusterDelNodeSlots(clusterNode *node) {
  int deleted = 0, j;
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (clusterNodeGetSlotBit(node, j)) {
      clusterDelSlot(j);
      deleted++;
    }
  }
  return deleted;
}
void clusterCloseAllSlots(void) {
  memset(server.cluster->migrating_slots_to, 0,
         sizeof(server.cluster->migrating_slots_to));
  memset(server.cluster->importing_slots_from, 0,
         sizeof(server.cluster->importing_slots_from));
}
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000
void clusterUpdateState(void) {
  int j, new_state;
  int reachable_masters = 0;
  static mstime_t among_minority_time;
  static mstime_t first_call_time = 0;
  server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;
  if (first_call_time == 0) first_call_time = mstime();
  if (nodeIsMaster(myself) && server.cluster->state == CLUSTER_FAIL &&
      mstime() - first_call_time < CLUSTER_WRITABLE_DELAY)
    return;
  new_state = CLUSTER_OK;
  if (server.cluster_require_full_coverage) {
    for (j = 0; j < CLUSTER_SLOTS; j++) {
      if (server.cluster->slots[j] == NULL ||
          server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL)) {
        new_state = CLUSTER_FAIL;
        break;
      }
    }
  }
  {
    dictIterator *di;
    dictEntry *de;
    server.cluster->size = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
      clusterNode *node = dictGetVal(de);
      if (nodeIsMaster(node) && node->numslots) {
        server.cluster->size++;
        if ((node->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) == 0)
          reachable_masters++;
      }
    }
    dictReleaseIterator(di);
  }
  {
    int needed_quorum = (server.cluster->size / 2) + 1;
    if (reachable_masters < needed_quorum) {
      new_state = CLUSTER_FAIL;
      among_minority_time = mstime();
    }
  }
  if (new_state != server.cluster->state) {
    mstime_t rejoin_delay = server.cluster_node_timeout;
    if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
      rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
    if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
      rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;
    if (new_state == CLUSTER_OK && nodeIsMaster(myself) &&
        mstime() - among_minority_time < rejoin_delay) {
      return;
    }
    serverLog(LL_WARNING, "Cluster state changed: %s",
              new_state == CLUSTER_OK ? "ok" : "fail");
    server.cluster->state = new_state;
  }
}
int verifyClusterConfigWithData(void) {
  int j;
  int update_config = 0;
  if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
    return C_OK;
  if (nodeIsSlave(myself)) return C_OK;
  for (j = 1; j < server.dbnum; j++) {
    if (dictSize(server.db[j].dict)) return C_ERR;
  }
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    if (!countKeysInSlot(j)) continue;
    if (server.cluster->slots[j] == myself ||
        server.cluster->importing_slots_from[j] != NULL)
      continue;
    update_config++;
    if (server.cluster->slots[j] == NULL) {
      serverLog(LL_WARNING,
                "I have keys for unassigned slot %d. "
                "Taking responsibility for it.",
                j);
      clusterAddSlot(myself, j);
    } else {
      serverLog(LL_WARNING,
                "I have keys for slot %d, but the slot is "
                "assigned to another node. "
                "Setting it to importing state.",
                j);
      server.cluster->importing_slots_from[j] = server.cluster->slots[j];
    }
  }
  if (update_config) clusterSaveConfigOrDie(1);
  return C_OK;
}
void clusterSetMaster(clusterNode *n) {
  serverAssert(n != myself);
  serverAssert(myself->numslots == 0);
  if (nodeIsMaster(myself)) {
    myself->flags &= ~(CLUSTER_NODE_MASTER | CLUSTER_NODE_MIGRATE_TO);
    myself->flags |= CLUSTER_NODE_SLAVE;
    clusterCloseAllSlots();
  } else {
    if (myself->slaveof) clusterNodeRemoveSlave(myself->slaveof, myself);
  }
  myself->slaveof = n;
  clusterNodeAddSlave(n, myself);
  replicationSetMaster(n->ip, n->port);
  resetManualFailover();
}
struct redisNodeFlags {
  uint16_t flag;
  char *name;
};
static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF, "myself,"}, {CLUSTER_NODE_MASTER, "master,"},
    {CLUSTER_NODE_SLAVE, "slave,"}, {CLUSTER_NODE_PFAIL, "fail?,"},
    {CLUSTER_NODE_FAIL, "fail,"}, {CLUSTER_NODE_HANDSHAKE, "handshake,"},
    {CLUSTER_NODE_NOADDR, "noaddr,"}, {CLUSTER_NODE_NOFAILOVER, "nofailover,"}};
sds representClusterNodeFlags(sds ci, uint16_t flags) {
  size_t orig_len = sdslen(ci);
  int i, size = sizeof(redisNodeFlagsTable) / sizeof(struct redisNodeFlags);
  for (i = 0; i < size; i++) {
    struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
    if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
  }
  if (sdslen(ci) == orig_len) ci = sdscat(ci, "noflags,");
  sdsIncrLen(ci, -1);
  return ci;
}
sds clusterGenNodeDescription(clusterNode *node) {
  int j, start;
  sds ci;
  ci = sdscatprintf(sdsempty(), "%.40s %s:%d@%d ", node->name, node->ip,
                    node->port, node->cport);
  ci = representClusterNodeFlags(ci, node->flags);
  if (node->slaveof)
    ci = sdscatprintf(ci, " %.40s ", node->slaveof->name);
  else
    ci = sdscatlen(ci, " - ", 3);
  ci = sdscatprintf(
      ci, "%lld %lld %llu %s", (long long)node->ping_sent,
      (long long)node->pong_received, (unsigned long long)node->configEpoch,
      (node->link || node->flags & CLUSTER_NODE_MYSELF) ? "connected"
                                                        : "disconnected");
  start = -1;
  for (j = 0; j < CLUSTER_SLOTS; j++) {
    int bit;
    if ((bit = clusterNodeGetSlotBit(node, j)) != 0) {
      if (start == -1) start = j;
    }
    if (start != -1 && (!bit || j == CLUSTER_SLOTS - 1)) {
      if (bit && j == CLUSTER_SLOTS - 1) j++;
      if (start == j - 1) {
        ci = sdscatprintf(ci, " %d", start);
      } else {
        ci = sdscatprintf(ci, " %d-%d", start, j - 1);
      }
      start = -1;
    }
  }
  if (node->flags & CLUSTER_NODE_MYSELF) {
    for (j = 0; j < CLUSTER_SLOTS; j++) {
      if (server.cluster->migrating_slots_to[j]) {
        ci = sdscatprintf(ci, " [%d->-%.40s]", j,
                          server.cluster->migrating_slots_to[j]->name);
      } else if (server.cluster->importing_slots_from[j]) {
        ci = sdscatprintf(ci, " [%d-<-%.40s]", j,
                          server.cluster->importing_slots_from[j]->name);
      }
    }
  }
  return ci;
}
sds clusterGenNodesDescription(int filter) {
  sds ci = sdsempty(), ni;
  dictIterator *di;
  dictEntry *de;
  di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    if (node->flags & filter) continue;
    ni = clusterGenNodeDescription(node);
    ci = sdscatsds(ci, ni);
    sdsfree(ni);
    ci = sdscatlen(ci, "\n", 1);
  }
  dictReleaseIterator(di);
  return ci;
}
const char *clusterGetMessageTypeString(int type) {
  switch (type) {
    case CLUSTERMSG_TYPE_PING:
      return "ping";
    case CLUSTERMSG_TYPE_PONG:
      return "pong";
    case CLUSTERMSG_TYPE_MEET:
      return "meet";
    case CLUSTERMSG_TYPE_FAIL:
      return "fail";
    case CLUSTERMSG_TYPE_PUBLISH:
      return "publish";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST:
      return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK:
      return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE:
      return "update";
    case CLUSTERMSG_TYPE_MFSTART:
      return "mfstart";
    case CLUSTERMSG_TYPE_MODULE:
      return "module";
  }
  return "unknown";
}
int getSlotOrReply(client *c, robj *o) {
  long long slot;
  if (getLongLongFromObject(o, &slot) != C_OK || slot < 0 ||
      slot >= CLUSTER_SLOTS) {
    addReplyError(c, "Invalid or out of range slot");
    return -1;
  }
  return (int)slot;
}
void clusterReplyMultiBulkSlots(client *c) {
  int num_masters = 0;
  void *slot_replylen = addReplyDeferredLen(c);
  dictEntry *de;
  dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
  while ((de = dictNext(di)) != NULL) {
    clusterNode *node = dictGetVal(de);
    int j = 0, start = -1;
    if (!nodeIsMaster(node) || node->numslots == 0) continue;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
      int bit, i;
      if ((bit = clusterNodeGetSlotBit(node, j)) != 0) {
        if (start == -1) start = j;
      }
      if (start != -1 && (!bit || j == CLUSTER_SLOTS - 1)) {
        int nested_elements = 3;
        void *nested_replylen = addReplyDeferredLen(c);
        if (bit && j == CLUSTER_SLOTS - 1) j++;
        if (start == j - 1) {
          addReplyLongLong(c, start);
          addReplyLongLong(c, start);
        } else {
          addReplyLongLong(c, start);
          addReplyLongLong(c, j - 1);
        }
        start = -1;
        addReplyArrayLen(c, 3);
        addReplyBulkCString(c, node->ip);
        addReplyLongLong(c, node->port);
        addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);
        for (i = 0; i < node->numslaves; i++) {
          if (nodeFailed(node->slaves[i])) continue;
          addReplyArrayLen(c, 3);
          addReplyBulkCString(c, node->slaves[i]->ip);
          addReplyLongLong(c, node->slaves[i]->port);
          addReplyBulkCBuffer(c, node->slaves[i]->name, CLUSTER_NAMELEN);
          nested_elements++;
        }
        setDeferredArrayLen(c, nested_replylen, nested_elements);
        num_masters++;
      }
    }
  }
  dictReleaseIterator(di);
  setDeferredArrayLen(c, slot_replylen, num_masters);
}
void clusterCommand(client *c) {
  if (server.cluster_enabled == 0) {
    addReplyError(c, "This instance has cluster support disabled");
    return;
  }
  if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
    const char *help[] = {
        "ADDSLOTS <slot> [slot ...] -- Assign slots to current node.",
        "BUMPEPOCH -- Advance the cluster config epoch.",
        "COUNT-failure-reports <node-id> -- Return number of failure reports "
        "for <node-id>.",
        "COUNTKEYSINSLOT <slot> - Return the number of keys in <slot>.",
        "DELSLOTS <slot> [slot ...] -- Delete slots information from current "
        "node.",
        "FAILOVER [force|takeover] -- Promote current replica node to being a "
        "master.",
        "FORGET <node-id> -- Remove a node from the cluster.",
        "GETKEYSINSLOT <slot> <count> -- Return key names stored by current "
        "node in a slot.",
        "FLUSHSLOTS -- Delete current node own slots information.",
        "INFO - Return onformation about the cluster.",
        "KEYSLOT <key> -- Return the hash slot for <key>.",
        "MEET <ip> <port> [bus-port] -- Connect nodes into a working cluster.",
        "MYID -- Return the node id.",
        "NODES -- Return cluster configuration seen by node. Output format:",
        "    <id> <ip:port> <flags> <master> <pings> <pongs> <epoch> <link> "
        "<slot> ... <slot>",
        "REPLICATE <node-id> -- Configure current node as replica to "
        "<node-id>.",
        "RESET [hard|soft] -- Reset current node (default: soft).",
        "SET-config-epoch <epoch> - Set config epoch of current node.",
        "SETSLOT <slot> (importing|migrating|stable|node <node-id>) -- Set "
        "slot state.",
        "REPLICAS <node-id> -- Return <node-id> replicas.",
        "SLOTS -- Return information about slots range mappings. Each range is "
        "made of:",
        "    start, end, master and replicas IP addresses, ports and ids",
        NULL};
    addReplyHelp(c, help);
  } else if (!strcasecmp(c->argv[1]->ptr, "meet") &&
             (c->argc == 4 || c->argc == 5)) {
    long long port, cport;
    if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
      addReplyErrorFormat(c, "Invalid TCP base port specified: %s",
                          (char *)c->argv[3]->ptr);
      return;
    }
    if (c->argc == 5) {
      if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
        addReplyErrorFormat(c, "Invalid TCP bus port specified: %s",
                            (char *)c->argv[4]->ptr);
        return;
      }
    } else {
      cport = port + CLUSTER_PORT_INCR;
    }
    if (clusterStartHandshake(c->argv[2]->ptr, port, cport) == 0 &&
        errno == EINVAL) {
      addReplyErrorFormat(c, "Invalid node address specified: %s:%s",
                          (char *)c->argv[2]->ptr, (char *)c->argv[3]->ptr);
    } else {
      addReply(c, shared.ok);
    }
  } else if (!strcasecmp(c->argv[1]->ptr, "nodes") && c->argc == 2) {
    sds nodes = clusterGenNodesDescription(0);
    addReplyVerbatim(c, nodes, sdslen(nodes), "txt");
    sdsfree(nodes);
  } else if (!strcasecmp(c->argv[1]->ptr, "myid") && c->argc == 2) {
    addReplyBulkCBuffer(c, myself->name, CLUSTER_NAMELEN);
  } else if (!strcasecmp(c->argv[1]->ptr, "slots") && c->argc == 2) {
    clusterReplyMultiBulkSlots(c);
  } else if (!strcasecmp(c->argv[1]->ptr, "flushslots") && c->argc == 2) {
    if (dictSize(server.db[0].dict) != 0) {
      addReplyError(c, "DB must be empty to perform CLUSTER FLUSHSLOTS.");
      return;
    }
    clusterDelNodeSlots(myself);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    addReply(c, shared.ok);
  } else if ((!strcasecmp(c->argv[1]->ptr, "addslots") ||
              !strcasecmp(c->argv[1]->ptr, "delslots")) &&
             c->argc >= 3) {
    int j, slot;
    unsigned char *slots = zmalloc(CLUSTER_SLOTS);
    int del = !strcasecmp(c->argv[1]->ptr, "delslots");
    memset(slots, 0, CLUSTER_SLOTS);
    for (j = 2; j < c->argc; j++) {
      if ((slot = getSlotOrReply(c, c->argv[j])) == -1) {
        zfree(slots);
        return;
      }
      if (del && server.cluster->slots[slot] == NULL) {
        addReplyErrorFormat(c, "Slot %d is already unassigned", slot);
        zfree(slots);
        return;
      } else if (!del && server.cluster->slots[slot]) {
        addReplyErrorFormat(c, "Slot %d is already busy", slot);
        zfree(slots);
        return;
      }
      if (slots[slot]++ == 1) {
        addReplyErrorFormat(c, "Slot %d specified multiple times", (int)slot);
        zfree(slots);
        return;
      }
    }
    for (j = 0; j < CLUSTER_SLOTS; j++) {
      if (slots[j]) {
        int retval;
        if (server.cluster->importing_slots_from[j])
          server.cluster->importing_slots_from[j] = NULL;
        retval = del ? clusterDelSlot(j) : clusterAddSlot(myself, j);
        serverAssertWithInfo(c, NULL, retval == C_OK);
      }
    }
    zfree(slots);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    addReply(c, shared.ok);
  } else if (!strcasecmp(c->argv[1]->ptr, "setslot") && c->argc >= 4) {
    int slot;
    clusterNode *n;
    if (nodeIsSlave(myself)) {
      addReplyError(c, "Please use SETSLOT only with masters.");
      return;
    }
    if ((slot = getSlotOrReply(c, c->argv[2])) == -1) return;
    if (!strcasecmp(c->argv[3]->ptr, "migrating") && c->argc == 5) {
      if (server.cluster->slots[slot] != myself) {
        addReplyErrorFormat(c, "I'm not the owner of hash slot %u", slot);
        return;
      }
      if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
        addReplyErrorFormat(c, "I don't know about node %s",
                            (char *)c->argv[4]->ptr);
        return;
      }
      server.cluster->migrating_slots_to[slot] = n;
    } else if (!strcasecmp(c->argv[3]->ptr, "importing") && c->argc == 5) {
      if (server.cluster->slots[slot] == myself) {
        addReplyErrorFormat(c, "I'm already the owner of hash slot %u", slot);
        return;
      }
      if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
        addReplyErrorFormat(c, "I don't know about node %s",
                            (char *)c->argv[4]->ptr);
        return;
      }
      server.cluster->importing_slots_from[slot] = n;
    } else if (!strcasecmp(c->argv[3]->ptr, "stable") && c->argc == 4) {
      server.cluster->importing_slots_from[slot] = NULL;
      server.cluster->migrating_slots_to[slot] = NULL;
    } else if (!strcasecmp(c->argv[3]->ptr, "node") && c->argc == 5) {
      clusterNode *n = clusterLookupNode(c->argv[4]->ptr);
      if (!n) {
        addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[4]->ptr);
        return;
      }
      if (server.cluster->slots[slot] == myself && n != myself) {
        if (countKeysInSlot(slot) != 0) {
          addReplyErrorFormat(c,
                              "Can't assign hashslot %d to a different node "
                              "while I still hold keys for this hash slot.",
                              slot);
          return;
        }
      }
      if (countKeysInSlot(slot) == 0 &&
          server.cluster->migrating_slots_to[slot])
        server.cluster->migrating_slots_to[slot] = NULL;
      if (n == myself && server.cluster->importing_slots_from[slot]) {
        if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
          serverLog(LL_WARNING, "configEpoch updated after importing slot %d",
                    slot);
        }
        server.cluster->importing_slots_from[slot] = NULL;
      }
      clusterDelSlot(slot);
      clusterAddSlot(n, slot);
    } else {
      addReplyError(c,
                    "Invalid CLUSTER SETSLOT action or number of arguments. "
                    "Try CLUSTER HELP");
      return;
    }
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
    addReply(c, shared.ok);
  } else if (!strcasecmp(c->argv[1]->ptr, "bumpepoch") && c->argc == 2) {
    int retval = clusterBumpConfigEpochWithoutConsensus();
    sds reply = sdscatprintf(sdsempty(), "+%s %llu\r\n",
                             (retval == C_OK) ? "BUMPED" : "STILL",
                             (unsigned long long)myself->configEpoch);
    addReplySds(c, reply);
  } else if (!strcasecmp(c->argv[1]->ptr, "info") && c->argc == 2) {
    char *statestr[] = {"ok", "fail", "needhelp"};
    int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
    uint64_t myepoch;
    int j;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
      clusterNode *n = server.cluster->slots[j];
      if (n == NULL) continue;
      slots_assigned++;
      if (nodeFailed(n)) {
        slots_fail++;
      } else if (nodeTimedOut(n)) {
        slots_pfail++;
      } else {
        slots_ok++;
      }
    }
    myepoch = (nodeIsSlave(myself) && myself->slaveof)
                  ? myself->slaveof->configEpoch
                  : myself->configEpoch;
    sds info = sdscatprintf(
        sdsempty(),
        "cluster_state:%s\r\n"
        "cluster_slots_assigned:%d\r\n"
        "cluster_slots_ok:%d\r\n"
        "cluster_slots_pfail:%d\r\n"
        "cluster_slots_fail:%d\r\n"
        "cluster_known_nodes:%lu\r\n"
        "cluster_size:%d\r\n"
        "cluster_current_epoch:%llu\r\n"
        "cluster_my_epoch:%llu\r\n",
        statestr[server.cluster->state], slots_assigned, slots_ok, slots_pfail,
        slots_fail, dictSize(server.cluster->nodes), server.cluster->size,
        (unsigned long long)server.cluster->currentEpoch,
        (unsigned long long)myepoch);
    long long tot_msg_sent = 0;
    long long tot_msg_received = 0;
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
      if (server.cluster->stats_bus_messages_sent[i] == 0) continue;
      tot_msg_sent += server.cluster->stats_bus_messages_sent[i];
      info = sdscatprintf(info, "cluster_stats_messages_%s_sent:%lld\r\n",
                          clusterGetMessageTypeString(i),
                          server.cluster->stats_bus_messages_sent[i]);
    }
    info = sdscatprintf(info, "cluster_stats_messages_sent:%lld\r\n",
                        tot_msg_sent);
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
      if (server.cluster->stats_bus_messages_received[i] == 0) continue;
      tot_msg_received += server.cluster->stats_bus_messages_received[i];
      info = sdscatprintf(info, "cluster_stats_messages_%s_received:%lld\r\n",
                          clusterGetMessageTypeString(i),
                          server.cluster->stats_bus_messages_received[i]);
    }
    info = sdscatprintf(info, "cluster_stats_messages_received:%lld\r\n",
                        tot_msg_received);
    addReplyVerbatim(c, info, sdslen(info), "txt");
    sdsfree(info);
  } else if (!strcasecmp(c->argv[1]->ptr, "saveconfig") && c->argc == 2) {
    int retval = clusterSaveConfig(1);
    if (retval == 0)
      addReply(c, shared.ok);
    else
      addReplyErrorFormat(c, "error saving the cluster node config: %s",
                          strerror(errno));
  } else if (!strcasecmp(c->argv[1]->ptr, "keyslot") && c->argc == 3) {
    sds key = c->argv[2]->ptr;
    addReplyLongLong(c, keyHashSlot(key, sdslen(key)));
  } else if (!strcasecmp(c->argv[1]->ptr, "countkeysinslot") && c->argc == 3) {
    long long slot;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &slot, NULL) != C_OK)
      return;
    if (slot < 0 || slot >= CLUSTER_SLOTS) {
      addReplyError(c, "Invalid slot");
      return;
    }
    addReplyLongLong(c, countKeysInSlot(slot));
  } else if (!strcasecmp(c->argv[1]->ptr, "getkeysinslot") && c->argc == 4) {
    long long maxkeys, slot;
    unsigned int numkeys, j;
    robj **keys;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &slot, NULL) != C_OK)
      return;
    if (getLongLongFromObjectOrReply(c, c->argv[3], &maxkeys, NULL) != C_OK)
      return;
    if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
      addReplyError(c, "Invalid slot or number of keys");
      return;
    }
    unsigned int keys_in_slot = countKeysInSlot(slot);
    if (maxkeys > keys_in_slot) maxkeys = keys_in_slot;
    keys = zmalloc(sizeof(robj *) * maxkeys);
    numkeys = getKeysInSlot(slot, keys, maxkeys);
    addReplyArrayLen(c, numkeys);
    for (j = 0; j < numkeys; j++) {
      addReplyBulk(c, keys[j]);
      decrRefCount(keys[j]);
    }
    zfree(keys);
  } else if (!strcasecmp(c->argv[1]->ptr, "forget") && c->argc == 3) {
    clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
    if (!n) {
      addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
      return;
    } else if (n == myself) {
      addReplyError(c, "I tried hard but I can't forget myself...");
      return;
    } else if (nodeIsSlave(myself) && myself->slaveof == n) {
      addReplyError(c, "Can't forget my master!");
      return;
    }
    clusterBlacklistAddNode(n);
    clusterDelNode(n);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    addReply(c, shared.ok);
  } else if (!strcasecmp(c->argv[1]->ptr, "replicate") && c->argc == 3) {
    clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
    if (!n) {
      addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
      return;
    }
    if (n == myself) {
      addReplyError(c, "Can't replicate myself");
      return;
    }
    if (nodeIsSlave(n)) {
      addReplyError(c, "I can only replicate a master, not a replica.");
      return;
    }
    if (nodeIsMaster(myself) &&
        (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
      addReplyError(c,
                    "To set a master the node must be empty and "
                    "without assigned slots.");
      return;
    }
    clusterSetMaster(n);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    addReply(c, shared.ok);
  } else if ((!strcasecmp(c->argv[1]->ptr, "slaves") ||
              !strcasecmp(c->argv[1]->ptr, "replicas")) &&
             c->argc == 3) {
    clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
    int j;
    if (!n) {
      addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
      return;
    }
    if (nodeIsSlave(n)) {
      addReplyError(c, "The specified node is not a master");
      return;
    }
    addReplyArrayLen(c, n->numslaves);
    for (j = 0; j < n->numslaves; j++) {
      sds ni = clusterGenNodeDescription(n->slaves[j]);
      addReplyBulkCString(c, ni);
      sdsfree(ni);
    }
  } else if (!strcasecmp(c->argv[1]->ptr, "count-failure-reports") &&
             c->argc == 3) {
    clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
    if (!n) {
      addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
      return;
    } else {
      addReplyLongLong(c, clusterNodeFailureReportsCount(n));
    }
  } else if (!strcasecmp(c->argv[1]->ptr, "failover") &&
             (c->argc == 2 || c->argc == 3)) {
    int force = 0, takeover = 0;
    if (c->argc == 3) {
      if (!strcasecmp(c->argv[2]->ptr, "force")) {
        force = 1;
      } else if (!strcasecmp(c->argv[2]->ptr, "takeover")) {
        takeover = 1;
        force = 1;
      } else {
        addReply(c, shared.syntaxerr);
        return;
      }
    }
    if (nodeIsMaster(myself)) {
      addReplyError(c, "You should send CLUSTER FAILOVER to a replica");
      return;
    } else if (myself->slaveof == NULL) {
      addReplyError(c, "I'm a replica but my master is unknown to me");
      return;
    } else if (!force &&
               (nodeFailed(myself->slaveof) || myself->slaveof->link == NULL)) {
      addReplyError(c,
                    "Master is down or failed, "
                    "please use CLUSTER FAILOVER FORCE");
      return;
    }
    resetManualFailover();
    server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
    if (takeover) {
      serverLog(LL_WARNING, "Taking over the master (user request).");
      clusterBumpConfigEpochWithoutConsensus();
      clusterFailoverReplaceYourMaster();
    } else if (force) {
      serverLog(LL_WARNING, "Forced failover user request accepted.");
      server.cluster->mf_can_start = 1;
    } else {
      serverLog(LL_WARNING, "Manual failover user request accepted.");
      clusterSendMFStart(myself->slaveof);
    }
    addReply(c, shared.ok);
  } else if (!strcasecmp(c->argv[1]->ptr, "set-config-epoch") && c->argc == 3) {
    long long epoch;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &epoch, NULL) != C_OK)
      return;
    if (epoch < 0) {
      addReplyErrorFormat(c, "Invalid config epoch specified: %lld", epoch);
    } else if (dictSize(server.cluster->nodes) > 1) {
      addReplyError(c,
                    "The user can assign a config epoch only when the "
                    "node does not know any other node.");
    } else if (myself->configEpoch != 0) {
      addReplyError(c, "Node config epoch is already non-zero");
    } else {
      myself->configEpoch = epoch;
      serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long)myself->configEpoch);
      if (server.cluster->currentEpoch < (uint64_t)epoch)
        server.cluster->currentEpoch = epoch;
      clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE |
                           CLUSTER_TODO_SAVE_CONFIG);
      addReply(c, shared.ok);
    }
  } else if (!strcasecmp(c->argv[1]->ptr, "reset") &&
             (c->argc == 2 || c->argc == 3)) {
    int hard = 0;
    if (c->argc == 3) {
      if (!strcasecmp(c->argv[2]->ptr, "hard")) {
        hard = 1;
      } else if (!strcasecmp(c->argv[2]->ptr, "soft")) {
        hard = 0;
      } else {
        addReply(c, shared.syntaxerr);
        return;
      }
    }
    if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
      addReplyError(c,
                    "CLUSTER RESET can't be called with "
                    "master nodes containing keys");
      return;
    }
    clusterReset(hard);
    addReply(c, shared.ok);
  } else {
    addReplySubcommandSyntaxError(c);
    return;
  }
}
void createDumpPayload(rio *payload, robj *o, robj *key) {
  unsigned char buf[2];
  uint64_t crc;
  rioInitWithBuffer(payload, sdsempty());
  serverAssert(rdbSaveObjectType(payload, o));
  serverAssert(rdbSaveObject(payload, o, key));
  buf[0] = RDB_VERSION & 0xff;
  buf[1] = (RDB_VERSION >> 8) & 0xff;
  payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr, buf, 2);
  crc = crc64(0, (unsigned char *)payload->io.buffer.ptr,
              sdslen(payload->io.buffer.ptr));
  memrev64ifbe(&crc);
  payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr, &crc, 8);
}
int verifyDumpPayload(unsigned char *p, size_t len) {
  unsigned char *footer;
  uint16_t rdbver;
  uint64_t crc;
  if (len < 10) return C_ERR;
  footer = p + (len - 10);
  rdbver = (footer[1] << 8) | footer[0];
  if (rdbver > RDB_VERSION) return C_ERR;
  crc = crc64(0, p, len - 8);
  memrev64ifbe(&crc);
  return (memcmp(&crc, footer + 2, 8) == 0) ? C_OK : C_ERR;
}
void dumpCommand(client *c) {
  robj *o;
  rio payload;
  if ((o = lookupKeyRead(c->db, c->argv[1])) == NULL) {
    addReplyNull(c);
    return;
  }
  createDumpPayload(&payload, o, c->argv[1]);
  addReplyBulkSds(c, payload.io.buffer.ptr);
  return;
}
void restoreCommand(client *c) {
  long long ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
  rio payload;
  int j, type, replace = 0, absttl = 0;
  robj *obj;
  for (j = 4; j < c->argc; j++) {
    int additional = c->argc - j - 1;
    if (!strcasecmp(c->argv[j]->ptr, "replace")) {
      replace = 1;
    } else if (!strcasecmp(c->argv[j]->ptr, "absttl")) {
      absttl = 1;
    } else if (!strcasecmp(c->argv[j]->ptr, "idletime") && additional >= 1 &&
               lfu_freq == -1) {
      if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &lru_idle, NULL) !=
          C_OK)
        return;
      if (lru_idle < 0) {
        addReplyError(c, "Invalid IDLETIME value, must be >= 0");
        return;
      }
      lru_clock = LRU_CLOCK();
      j++;
    } else if (!strcasecmp(c->argv[j]->ptr, "freq") && additional >= 1 &&
               lru_idle == -1) {
      if (getLongLongFromObjectOrReply(c, c->argv[j + 1], &lfu_freq, NULL) !=
          C_OK)
        return;
      if (lfu_freq < 0 || lfu_freq > 255) {
        addReplyError(c, "Invalid FREQ value, must be >= 0 and <= 255");
        return;
      }
      j++;
    } else {
      addReply(c, shared.syntaxerr);
      return;
    }
  }
  if (!replace && lookupKeyWrite(c->db, c->argv[1]) != NULL) {
    addReply(c, shared.busykeyerr);
    return;
  }
  if (getLongLongFromObjectOrReply(c, c->argv[2], &ttl, NULL) != C_OK) {
    return;
  } else if (ttl < 0) {
    addReplyError(c, "Invalid TTL value, must be >= 0");
    return;
  }
  if (verifyDumpPayload(c->argv[3]->ptr, sdslen(c->argv[3]->ptr)) == C_ERR) {
    addReplyError(c, "DUMP payload version or checksum are wrong");
    return;
  }
  rioInitWithBuffer(&payload, c->argv[3]->ptr);
  if (((type = rdbLoadObjectType(&payload)) == -1) ||
      ((obj = rdbLoadObject(type, &payload, c->argv[1])) == NULL)) {
    addReplyError(c, "Bad data format");
    return;
  }
  if (replace) dbDelete(c->db, c->argv[1]);
  dbAdd(c->db, c->argv[1], obj);
  if (ttl) {
    if (!absttl) ttl += mstime();
    setExpire(c, c->db, c->argv[1], ttl);
  }
  objectSetLRUOrLFU(obj, lfu_freq, lru_idle, lru_clock);
  signalModifiedKey(c->db, c->argv[1]);
  addReply(c, shared.ok);
  server.dirty++;
}
#define MIGRATE_SOCKET_CACHE_ITEMS 64
#define MIGRATE_SOCKET_CACHE_TTL 10
typedef struct migrateCachedSocket {
  connection *conn;
  long last_dbid;
  time_t last_use_time;
} migrateCachedSocket;
migrateCachedSocket *migrateGetSocket(client *c, robj *host, robj *port,
                                      long timeout) {
  connection *conn;
  sds name = sdsempty();
  migrateCachedSocket *cs;
  name = sdscatlen(name, host->ptr, sdslen(host->ptr));
  name = sdscatlen(name, ":", 1);
  name = sdscatlen(name, port->ptr, sdslen(port->ptr));
  cs = dictFetchValue(server.migrate_cached_sockets, name);
  if (cs) {
    sdsfree(name);
    cs->last_use_time = server.unixtime;
    return cs;
  }
  if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
    dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
    cs = dictGetVal(de);
    connClose(cs->conn);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets, dictGetKey(de));
  }
  conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
  if (connBlockingConnect(conn, c->argv[1]->ptr, atoi(c->argv[2]->ptr),
                          timeout) != C_OK) {
    addReplySds(c,
                sdsnew("-IOERR error or timeout connecting to the client\r\n"));
    connClose(conn);
    sdsfree(name);
    return NULL;
  }
  connEnableTcpNoDelay(conn);
  cs = zmalloc(sizeof(*cs));
  cs->conn = conn;
  cs->last_dbid = -1;
  cs->last_use_time = server.unixtime;
  dictAdd(server.migrate_cached_sockets, name, cs);
  return cs;
}
void migrateCloseSocket(robj *host, robj *port) {
  sds name = sdsempty();
  migrateCachedSocket *cs;
  name = sdscatlen(name, host->ptr, sdslen(host->ptr));
  name = sdscatlen(name, ":", 1);
  name = sdscatlen(name, port->ptr, sdslen(port->ptr));
  cs = dictFetchValue(server.migrate_cached_sockets, name);
  if (!cs) {
    sdsfree(name);
    return;
  }
  connClose(cs->conn);
  zfree(cs);
  dictDelete(server.migrate_cached_sockets, name);
  sdsfree(name);
}
void migrateCloseTimedoutSockets(void) {
  dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
  dictEntry *de;
  while ((de = dictNext(di)) != NULL) {
    migrateCachedSocket *cs = dictGetVal(de);
    if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
      connClose(cs->conn);
      zfree(cs);
      dictDelete(server.migrate_cached_sockets, dictGetKey(de));
    }
  }
  dictReleaseIterator(di);
}
void migrateCommand(client *c) {
  migrateCachedSocket *cs;
  int copy = 0, replace = 0, j;
  char *password = NULL;
  long timeout;
  long dbid;
  robj **ov = NULL;
  robj **kv = NULL;
  robj **newargv = NULL;
  rio cmd, payload;
  int may_retry = 1;
  int write_error = 0;
  int argv_rewritten = 0;
  int first_key = 3;
  int num_keys = 1;
  for (j = 6; j < c->argc; j++) {
    int moreargs = j < c->argc - 1;
    if (!strcasecmp(c->argv[j]->ptr, "copy")) {
      copy = 1;
    } else if (!strcasecmp(c->argv[j]->ptr, "replace")) {
      replace = 1;
    } else if (!strcasecmp(c->argv[j]->ptr, "auth")) {
      if (!moreargs) {
        addReply(c, shared.syntaxerr);
        return;
      }
      j++;
      password = c->argv[j]->ptr;
    } else if (!strcasecmp(c->argv[j]->ptr, "keys")) {
      if (sdslen(c->argv[3]->ptr) != 0) {
        addReplyError(c,
                      "When using MIGRATE KEYS option, the key argument"
                      " must be set to the empty string");
        return;
      }
      first_key = j + 1;
      num_keys = c->argc - j - 1;
      break;
    } else {
      addReply(c, shared.syntaxerr);
      return;
    }
  }
  if (getLongFromObjectOrReply(c, c->argv[5], &timeout, NULL) != C_OK ||
      getLongFromObjectOrReply(c, c->argv[4], &dbid, NULL) != C_OK) {
    return;
  }
  if (timeout <= 0) timeout = 1000;
  ov = zrealloc(ov, sizeof(robj *) * num_keys);
  kv = zrealloc(kv, sizeof(robj *) * num_keys);
  int oi = 0;
  for (j = 0; j < num_keys; j++) {
    if ((ov[oi] = lookupKeyRead(c->db, c->argv[first_key + j])) != NULL) {
      kv[oi] = c->argv[first_key + j];
      oi++;
    }
  }
  num_keys = oi;
  if (num_keys == 0) {
    zfree(ov);
    zfree(kv);
    addReplySds(c, sdsnew("+NOKEY\r\n"));
    return;
  }
try_again:
  write_error = 0;
  cs = migrateGetSocket(c, c->argv[1], c->argv[2], timeout);
  if (cs == NULL) {
    zfree(ov);
    zfree(kv);
    return;
  }
  rioInitWithBuffer(&cmd, sdsempty());
  if (password) {
    serverAssertWithInfo(c, NULL, rioWriteBulkCount(&cmd, '*', 2));
    serverAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "AUTH", 4));
    serverAssertWithInfo(c, NULL,
                         rioWriteBulkString(&cmd, password, sdslen(password)));
  }
  int select = cs->last_dbid != dbid;
  if (select) {
    serverAssertWithInfo(c, NULL, rioWriteBulkCount(&cmd, '*', 2));
    serverAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "SELECT", 6));
    serverAssertWithInfo(c, NULL, rioWriteBulkLongLong(&cmd, dbid));
  }
  int non_expired = 0;
  for (j = 0; j < num_keys; j++) {
    long long ttl = 0;
    long long expireat = getExpire(c->db, kv[j]);
    if (expireat != -1) {
      ttl = expireat - mstime();
      if (ttl < 0) {
        continue;
      }
      if (ttl < 1) ttl = 1;
    }
    kv[non_expired++] = kv[j];
    serverAssertWithInfo(c, NULL,
                         rioWriteBulkCount(&cmd, '*', replace ? 5 : 4));
    if (server.cluster_enabled)
      serverAssertWithInfo(c, NULL,
                           rioWriteBulkString(&cmd, "RESTORE-ASKING", 14));
    else
      serverAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "RESTORE", 7));
    serverAssertWithInfo(c, NULL, sdsEncodedObject(kv[j]));
    serverAssertWithInfo(
        c, NULL, rioWriteBulkString(&cmd, kv[j]->ptr, sdslen(kv[j]->ptr)));
    serverAssertWithInfo(c, NULL, rioWriteBulkLongLong(&cmd, ttl));
    createDumpPayload(&payload, ov[j], kv[j]);
    serverAssertWithInfo(c, NULL,
                         rioWriteBulkString(&cmd, payload.io.buffer.ptr,
                                            sdslen(payload.io.buffer.ptr)));
    sdsfree(payload.io.buffer.ptr);
    if (replace)
      serverAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "REPLACE", 7));
  }
  num_keys = non_expired;
  errno = 0;
  {
    sds buf = cmd.io.buffer.ptr;
    size_t pos = 0, towrite;
    int nwritten = 0;
    while ((towrite = sdslen(buf) - pos) > 0) {
      towrite = (towrite > (64 * 1024) ? (64 * 1024) : towrite);
      nwritten = connSyncWrite(cs->conn, buf + pos, towrite, timeout);
      if (nwritten != (signed)towrite) {
        write_error = 1;
        goto socket_err;
      }
      pos += nwritten;
    }
  }
  char buf0[1024];
  char buf1[1024];
  char buf2[1024];
  if (password && connSyncReadLine(cs->conn, buf0, sizeof(buf0), timeout) <= 0)
    goto socket_err;
  if (select && connSyncReadLine(cs->conn, buf1, sizeof(buf1), timeout) <= 0)
    goto socket_err;
  int error_from_target = 0;
  int socket_error = 0;
  int del_idx = 1;
  if (!copy) newargv = zmalloc(sizeof(robj *) * (num_keys + 1));
  for (j = 0; j < num_keys; j++) {
    if (connSyncReadLine(cs->conn, buf2, sizeof(buf2), timeout) <= 0) {
      socket_error = 1;
      break;
    }
    if ((password && buf0[0] == '-') || (select && buf1[0] == '-') ||
        buf2[0] == '-') {
      if (!error_from_target) {
        cs->last_dbid = -1;
        char *errbuf;
        if (password && buf0[0] == '-')
          errbuf = buf0;
        else if (select && buf1[0] == '-')
          errbuf = buf1;
        else
          errbuf = buf2;
        error_from_target = 1;
        addReplyErrorFormat(c, "Target instance replied with error: %s",
                            errbuf + 1);
      }
    } else {
      if (!copy) {
        dbDelete(c->db, kv[j]);
        signalModifiedKey(c->db, kv[j]);
        server.dirty++;
        newargv[del_idx++] = kv[j];
        incrRefCount(kv[j]);
      }
    }
  }
  if (!error_from_target && socket_error && j == 0 && may_retry &&
      errno != ETIMEDOUT) {
    goto socket_err;
  }
  if (socket_error) migrateCloseSocket(c->argv[1], c->argv[2]);
  if (!copy) {
    if (del_idx > 1) {
      newargv[0] = createStringObject("DEL", 3);
      replaceClientCommandVector(c, del_idx, newargv);
      argv_rewritten = 1;
    } else {
      zfree(newargv);
    }
    newargv = NULL;
  }
  if (!error_from_target && socket_error) {
    may_retry = 0;
    goto socket_err;
  }
  if (!error_from_target) {
    cs->last_dbid = dbid;
    addReply(c, shared.ok);
  } else {
  }
  sdsfree(cmd.io.buffer.ptr);
  zfree(ov);
  zfree(kv);
  zfree(newargv);
  return;
socket_err:
  sdsfree(cmd.io.buffer.ptr);
  if (!argv_rewritten) migrateCloseSocket(c->argv[1], c->argv[2]);
  zfree(newargv);
  newargv = NULL;
  if (errno != ETIMEDOUT && may_retry) {
    may_retry = 0;
    goto try_again;
  }
  zfree(ov);
  zfree(kv);
  addReplySds(c,
              sdscatprintf(sdsempty(),
                           "-IOERR error or timeout %s to target instance\r\n",
                           write_error ? "writing" : "reading"));
  return;
}
void askingCommand(client *c) {
  if (server.cluster_enabled == 0) {
    addReplyError(c, "This instance has cluster support disabled");
    return;
  }
  c->flags |= CLIENT_ASKING;
  addReply(c, shared.ok);
}
void readonlyCommand(client *c) {
  if (server.cluster_enabled == 0) {
    addReplyError(c, "This instance has cluster support disabled");
    return;
  }
  c->flags |= CLIENT_READONLY;
  addReply(c, shared.ok);
}
void readwriteCommand(client *c) {
  c->flags &= ~CLIENT_READONLY;
  addReply(c, shared.ok);
}
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv,
                            int argc, int *hashslot, int *error_code) {
  clusterNode *n = NULL;
  robj *firstkey = NULL;
  int multiple_keys = 0;
  multiState *ms, _ms;
  multiCmd mc;
  int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;
  if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
    return myself;
  if (error_code) *error_code = CLUSTER_REDIR_NONE;
  if (cmd->proc == execCommand) {
    if (!(c->flags & CLIENT_MULTI)) return myself;
    ms = &c->mstate;
  } else {
    ms = &_ms;
    _ms.commands = &mc;
    _ms.count = 1;
    mc.argv = argv;
    mc.argc = argc;
    mc.cmd = cmd;
  }
  for (i = 0; i < ms->count; i++) {
    struct redisCommand *mcmd;
    robj **margv;
    int margc, *keyindex, numkeys, j;
    mcmd = ms->commands[i].cmd;
    margc = ms->commands[i].argc;
    margv = ms->commands[i].argv;
    keyindex = getKeysFromCommand(mcmd, margv, margc, &numkeys);
    for (j = 0; j < numkeys; j++) {
      robj *thiskey = margv[keyindex[j]];
      int thisslot = keyHashSlot((char *)thiskey->ptr, sdslen(thiskey->ptr));
      if (firstkey == NULL) {
        firstkey = thiskey;
        slot = thisslot;
        n = server.cluster->slots[slot];
        if (n == NULL) {
          getKeysFreeResult(keyindex);
          if (error_code) *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
          return NULL;
        }
        if (n == myself && server.cluster->migrating_slots_to[slot] != NULL) {
          migrating_slot = 1;
        } else if (server.cluster->importing_slots_from[slot] != NULL) {
          importing_slot = 1;
        }
      } else {
        if (!equalStringObjects(firstkey, thiskey)) {
          if (slot != thisslot) {
            getKeysFreeResult(keyindex);
            if (error_code) *error_code = CLUSTER_REDIR_CROSS_SLOT;
            return NULL;
          } else {
            multiple_keys = 1;
          }
        }
      }
      if ((migrating_slot || importing_slot) &&
          lookupKeyRead(&server.db[0], thiskey) == NULL) {
        missing_keys++;
      }
    }
    getKeysFreeResult(keyindex);
  }
  if (n == NULL) return myself;
  if (server.cluster->state != CLUSTER_OK) {
    if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
    return NULL;
  }
  if (hashslot) *hashslot = slot;
  if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
    return myself;
  if (migrating_slot && missing_keys) {
    if (error_code) *error_code = CLUSTER_REDIR_ASK;
    return server.cluster->migrating_slots_to[slot];
  }
  if (importing_slot && (c->flags & CLIENT_ASKING || cmd->flags & CMD_ASKING)) {
    if (multiple_keys && missing_keys) {
      if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
      return NULL;
    } else {
      return myself;
    }
  }
  if (c->flags & CLIENT_READONLY &&
      (cmd->flags & CMD_READONLY || cmd->proc == evalCommand ||
       cmd->proc == evalShaCommand) &&
      nodeIsSlave(myself) && myself->slaveof == n) {
    return myself;
  }
  if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
  return n;
}
void clusterRedirectClient(client *c, clusterNode *n, int hashslot,
                           int error_code) {
  if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
    addReplySds(
        c,
        sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
  } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
    addReplySds(
        c,
        sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
  } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
    addReplySds(c, sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
  } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
    addReplySds(c, sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
  } else if (error_code == CLUSTER_REDIR_MOVED ||
             error_code == CLUSTER_REDIR_ASK) {
    addReplySds(
        c, sdscatprintf(sdsempty(), "-%s %d %s:%d\r\n",
                        (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
                        hashslot, n->ip, n->port));
  } else {
    serverPanic("getNodeByQuery() unknown error.");
  }
}
int clusterRedirectBlockedClientIfNeeded(client *c) {
  if (c->flags & CLIENT_BLOCKED &&
      (c->btype == BLOCKED_LIST || c->btype == BLOCKED_ZSET ||
       c->btype == BLOCKED_STREAM)) {
    dictEntry *de;
    dictIterator *di;
    if (server.cluster->state == CLUSTER_FAIL) {
      clusterRedirectClient(c, NULL, 0, CLUSTER_REDIR_DOWN_STATE);
      return 1;
    }
    di = dictGetIterator(c->bpop.keys);
    if ((de = dictNext(di)) != NULL) {
      robj *key = dictGetKey(de);
      int slot = keyHashSlot((char *)key->ptr, sdslen(key->ptr));
      clusterNode *node = server.cluster->slots[slot];
      if (node != myself &&
          server.cluster->importing_slots_from[slot] == NULL) {
        if (node == NULL) {
          clusterRedirectClient(c, NULL, 0, CLUSTER_REDIR_DOWN_UNBOUND);
        } else {
          clusterRedirectClient(c, node, slot, CLUSTER_REDIR_MOVED);
        }
        dictReleaseIterator(di);
        return 1;
      }
    }
    dictReleaseIterator(di);
  }
  return 0;
}
