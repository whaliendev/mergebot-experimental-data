#include "server.h"
#include "cluster.h"
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(connection *conn);
void replicationSendAck(void);
void putSlaveOnline(client *slave);
int cancelReplicationHandshake(void);
char *replicationGetSlaveName(client *c) {
    static char buf[NET_PEER_ID_LEN];
    char ip[NET_IP_STR_LEN];
    ip[0] = '\0';
    buf[0] = '\0';
    if (c->slave_ip[0] != '\0' ||
        connPeerToString(c->conn,ip,sizeof(ip),NULL) != -1)
    {
        if (c->slave_ip[0] != '\0') memcpy(ip,c->slave_ip,sizeof(c->slave_ip));
        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}
void createReplicationBacklog(void) {
    serverAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    server.repl_backlog_off = server.master_repl_offset+1;
}
void resizeReplicationBacklog(long long newsize) {
    if (newsize < CONFIG_REPL_BACKLOG_MIN_SIZE)
        newsize = CONFIG_REPL_BACKLOG_MIN_SIZE;
    if (server.repl_backlog_size == newsize) return;
    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        zfree(server.repl_backlog);
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}
void freeReplicationBacklog(void) {
    serverAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;
    server.master_repl_offset += len;
    while(len) {
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;
        if (thislen > len) thislen = len;
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        server.repl_backlog_idx += thislen;
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;
        len -= thislen;
        p += thislen;
        server.repl_backlog_histlen += thislen;
    }
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;
    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[LONG_STR_SIZE];
    if (server.masterhost != NULL) return;
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;
    serverAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));
    if (server.slaveseldb != dictid) {
        robj *selectcmd;
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;
            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);
        listRewind(slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
            addReply(slave,selectcmd);
        }
        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    server.slaveseldb = dictid;
    if (server.repl_backlog) {
        char aux[LONG_STR_SIZE+3];
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);
        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        addReplyArrayLen(slave,argc);
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}
#include <ctype.h>
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen) {
    listNode *ln;
    listIter li;
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }
    if (server.repl_backlog) feedReplicationBacklog(buf,buflen);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        addReplyProto(slave,buf,buflen);
    }
}
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;
    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & CLIENT_LUA) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(OBJ_STRING,cmdrepr);
    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        client *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long j, skip, len;
    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);
    if (server.repl_backlog_histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }
    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    serverLog(LL_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);
    skip = offset - server.repl_backlog_off;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    serverLog(LL_DEBUG, "[PSYNC] Index of first byte: %lld", j);
    j = (j + skip) % server.repl_backlog_size;
    len = server.repl_backlog_histlen - skip;
    serverLog(LL_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;
        serverLog(LL_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}
long long getPsyncInitialOffset(void) {
    return server.master_repl_offset;
}
int replicationSetupSlaveForFullResync(client *slave, long long offset) {
    char buf[128];
    int buflen;
    slave->psync_initial_offset = offset;
    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    server.slaveseldb = -1;
    if (!(slave->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          server.replid,offset);
        if (connWrite(slave->conn,buf,buflen) != buflen) {
            freeClientAsync(slave);
            return C_ERR;
        }
    }
    return C_OK;
}
int masterTryPartialResynchronization(client *c) {
    long long psync_offset, psync_len;
    char *master_replid = c->argv[1]->ptr;
    char buf[128];
    int buflen;
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       C_OK) goto need_full_resync;
    if (strcasecmp(master_replid, server.replid) &&
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, server.replid) &&
                strcasecmp(master_replid, server.replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, server.replid, server.replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, server.second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", server.replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (connWrite(c->conn,buf,buflen) != buflen) {
        freeClientAsync(c);
        return C_OK;
    }
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    refreshGoodSlavesCount();
    return C_OK;
need_full_resync:
    return C_ERR;
}
int startBgsaveForReplication(int mincapa) {
    int retval;
    int socket_target = server.repl_diskless_sync && (mincapa & SLAVE_CAPA_EOF);
    listIter li;
    listNode *ln;
    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rsiptr) {
        if (socket_target)
            retval = rdbSaveToSlavesSockets(rsiptr);
        else
            retval = rdbSaveBackground(server.rdb_filename,rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                slave->replstate = REPL_STATE_NONE;
                slave->flags &= ~CLIENT_SLAVE;
                listDelNode(server.slaves,ln);
                addReplyError(slave,
                    "BGSAVE failed, replication can't continue");
                slave->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }
    if (!socket_target) {
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                    replicationSetupSlaveForFullResync(slave,
                            getPsyncInitialOffset());
            }
        }
    }
    if (retval == C_OK) replicationScriptCacheFlush();
    return retval;
}
void syncCommand(client *c) {
    if (c->flags & CLIENT_SLAVE) return;
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED) {
        addReplySds(c,sdsnew("-NOMASTERLINK Can't SYNC while not connected with my master\r\n"));
        return;
    }
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }
    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        if (masterTryPartialResynchronization(c) == C_OK) {
            server.stat_sync_partial_ok++;
            return;
        } else {
            char *master_replid = c->argv[1]->ptr;
            if (master_replid[0] != '?') server.stat_sync_partial_err++;
        }
    } else {
        c->flags |= CLIENT_PRE_PSYNC;
    }
    server.stat_sync_full++;
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (server.repl_disable_tcp_nodelay)
        connDisableTcpNoDelay(c->conn);
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(server.slaves,c);
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
    }
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        client *slave;
        listNode *ln;
        listIter li;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa)) {
            copyClientOutputBuffer(c,slave);
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");
    } else {
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            if (server.repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            if (!hasActiveChildProcess()) {
                startBgsaveForReplication(c->slave_capa);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but another BG operation is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}
void replconfCommand(client *c) {
    int j;
    if ((c->argc % 2) == 0) {
        addReply(c,shared.syntaxerr);
        return;
    }
    for (j = 1; j < c->argc; j+=2) {
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;
            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ip-address")) {
            sds ip = c->argv[j+1]->ptr;
            if (sdslen(ip) < sizeof(c->slave_ip)) {
                memcpy(c->slave_ip,ip,sdslen(ip)+1);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(ip));
                return;
            }
        } else if (!strcasecmp(c->argv[j]->ptr,"capa")) {
            if (!strcasecmp(c->argv[j+1]->ptr,"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp(c->argv[j+1]->ptr,"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            long long offset;
            if (!(c->flags & CLIENT_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = server.unixtime;
            if (c->repl_put_online_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                putSlaveOnline(c);
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            if (server.masterhost && server.master) replicationSendAck();
            return;
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}
void putSlaveOnline(client *slave) {
    slave->replstate = SLAVE_STATE_ONLINE;
    slave->repl_put_online_on_ack = 0;
    slave->repl_ack_time = server.unixtime;
    if (connSetWriteHandler(slave->conn, sendReplyToClient) == C_ERR) {
        serverLog(LL_WARNING,"Unable to register writable event for replica bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    refreshGoodSlavesCount();
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(slave));
}
void sendBulkToSlave(connection *conn) {
    client *slave = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;
    if (slave->replpreamble) {
        nwritten = connWrite(conn,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_VERBOSE,"Write error sending RDB preamble to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
            return;
        }
        server.stat_net_output_bytes += nwritten;
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
        } else {
            return;
        }
    }
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = connWrite(conn,buf,buflen)) == -1) {
        if (connGetState(conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
        }
        return;
    }
    slave->repldboff += nwritten;
    server.stat_net_output_bytes += nwritten;
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        connSetWriteHandler(slave->conn,NULL);
        putSlaveOnline(slave);
    }
}
void rdbPipeWriteHandlerConnRemoved(struct connection *conn) {
    if (!connHasWriteHandler(conn))
        return;
    connSetWriteHandler(conn, NULL);
    server.rdb_pipe_numconns_writing--;
    if (server.rdb_pipe_numconns_writing == 0) {
        if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
            serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
        }
    }
}
void rdbPipeWriteHandler(struct connection *conn) {
    serverAssert(server.rdb_pipe_bufflen>0);
    client *slave = connGetPrivateData(conn);
    int nwritten;
    if ((nwritten = connWrite(conn, server.rdb_pipe_buff + slave->repldboff,
                              server.rdb_pipe_bufflen - slave->repldboff)) == -1)
    {
        if (connGetState(conn) == CONN_STATE_CONNECTED)
            return;
        serverLog(LL_WARNING,"Write error sending DB to replica: %s",
            connGetLastError(conn));
        freeClient(slave);
        return;
    } else {
        slave->repldboff += nwritten;
        server.stat_net_output_bytes += nwritten;
        if (slave->repldboff < server.rdb_pipe_bufflen)
            return;
    }
    rdbPipeWriteHandlerConnRemoved(conn);
}
void RdbPipeCleanup() {
    close(server.rdb_pipe_read);
    zfree(server.rdb_pipe_conns);
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    zfree(server.rdb_pipe_buff);
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    checkChildrenDone();
}
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    UNUSED(mask);
    UNUSED(clientData);
    UNUSED(eventLoop);
    int i;
    if (!server.rdb_pipe_buff)
        server.rdb_pipe_buff = zmalloc(PROTO_IOBUF_LEN);
    serverAssert(server.rdb_pipe_numconns_writing==0);
    while (1) {
        server.rdb_pipe_bufflen = read(fd, server.rdb_pipe_buff, PROTO_IOBUF_LEN);
        if (server.rdb_pipe_bufflen < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            serverLog(LL_WARNING,"Diskless rdb transfer, read error sending DB to replicas: %s", strerror(errno));
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                client *slave = connGetPrivateData(conn);
                freeClient(slave);
                server.rdb_pipe_conns[i] = NULL;
            }
            killRDBChild();
            return;
        }
        if (server.rdb_pipe_bufflen == 0) {
            int stillUp = 0;
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            for (i=0; i < server.rdb_pipe_numconns; i++)
            {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                stillUp++;
            }
            serverLog(LL_WARNING,"Diskless rdb transfer, done reading from pipe, %d replicas still up.", stillUp);
            RdbPipeCleanup();
            return;
        }
        int stillAlive = 0;
        for (i=0; i < server.rdb_pipe_numconns; i++)
        {
            int nwritten;
            connection *conn = server.rdb_pipe_conns[i];
            if (!conn)
                continue;
            client *slave = connGetPrivateData(conn);
            if ((nwritten = connWrite(conn, server.rdb_pipe_buff, server.rdb_pipe_bufflen)) == -1) {
                if (connGetState(conn) != CONN_STATE_CONNECTED) {
                    serverLog(LL_WARNING,"Diskless rdb transfer, write error sending DB to replica: %s",
                        connGetLastError(conn));
                    freeClient(slave);
                    server.rdb_pipe_conns[i] = NULL;
                    continue;
                }
                slave->repldboff = 0;
            } else {
                slave->repldboff = nwritten;
                server.stat_net_output_bytes += nwritten;
            }
            if (nwritten != server.rdb_pipe_bufflen) {
                server.rdb_pipe_numconns_writing++;
                connSetWriteHandler(conn, rdbPipeWriteHandler);
            }
            stillAlive++;
        }
        if (stillAlive == 0) {
            serverLog(LL_WARNING,"Diskless rdb transfer, last replica dropped, killing fork child.");
            killRDBChild();
            RdbPipeCleanup();
        }
        if (server.rdb_pipe_numconns_writing || stillAlive == 0) {
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            break;
        }
    }
}
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    int mincapa = -1;
    listIter li;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            startbgsave = 1;
            mincapa = (mincapa == -1) ? slave->slave_capa :
                                        (mincapa & slave->slave_capa);
        } else if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            struct redis_stat buf;
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                slave->replstate = SLAVE_STATE_ONLINE;
                slave->repl_put_online_on_ack = 1;
                slave->repl_ack_time = server.unixtime;
            } else {
                if (bgsaveerr != C_OK) {
                    freeClient(slave);
                    serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                    continue;
                }
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClient(slave);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                slave->repldboff = 0;
                slave->repldbsize = buf.st_size;
                slave->replstate = SLAVE_STATE_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);
                connSetWriteHandler(slave->conn,NULL);
                if (connSetWriteHandler(slave->conn,sendBulkToSlave) == C_ERR) {
                    freeClient(slave);
                    continue;
                }
            }
        }
    }
    if (startbgsave) startBgsaveForReplication(mincapa);
}
void changeReplicationId(void) {
    getRandomHexChars(server.replid,CONFIG_RUN_ID_SIZE);
    server.replid[CONFIG_RUN_ID_SIZE] = '\0';
}
void clearReplicationId2(void) {
    memset(server.replid2,'0',sizeof(server.replid));
    server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
    server.second_replid_offset = -1;
}
void shiftReplicationId(void) {
    memcpy(server.replid2,server.replid,sizeof(server.replid));
    server.second_replid_offset = server.master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", server.replid2, server.second_replid_offset, server.replid);
}
int slaveIsInHandshakeState(void) {
    return server.repl_state >= REPL_STATE_RECEIVE_PONG &&
           server.repl_state <= REPL_STATE_RECEIVE_PSYNC;
}
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        if (server.repl_transfer_s) connWrite(server.repl_transfer_s, "\n", 1);
    }
}
void replicationEmptyDbCallback(void *privdata) {
    UNUSED(privdata);
    replicationSendNewlineToMaster();
}
void replicationCreateMasterClient(connection *conn, int dbid) {
    server.master = createClient(conn);
    if (conn)
        connSetReadHandler(server.master->conn, readQueryFromClient);
    server.master->flags |= CLIENT_MASTER;
    server.master->authenticated = 1;
    server.master->reploff = server.master_initial_offset;
    server.master->read_reploff = server.master->reploff;
    server.master->user = NULL;
    memcpy(server.master->replid, server.master_replid,
        sizeof(server.master_replid));
    if (server.master->reploff == -1)
        server.master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(server.master,dbid);
}
void restartAOFAfterSYNC() {
    unsigned int tries, max_tries = 10;
    for (tries = 0; tries < max_tries; ++tries) {
        if (startAppendOnly() == C_OK) break;
        serverLog(LL_WARNING,
            "Failed enabling the AOF after successful master synchronization! "
            "Trying it again in one second.");
        sleep(1);
    }
    if (tries == max_tries) {
        serverLog(LL_WARNING,
            "FATAL: this replica instance finished the synchronization with "
            "its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}
static int useDisklessLoad() {
    int enabled = server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB ||
           (server.repl_diskless_load == REPL_DISKLESS_LOAD_WHEN_DB_EMPTY && dbTotalServerKeyCount()==0);
    if (enabled && !moduleAllDatatypesHandleErrors()) {
        serverLog(LL_WARNING,
            "Skipping diskless-load because there are modules that don't handle read errors.");
        enabled = 0;
    }
    return enabled;
}
redisDb *disklessLoadMakeBackups(void) {
    redisDb *backups = zmalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        backups[i] = server.db[i];
        server.db[i].dict = dictCreate(&dbDictType,NULL);
        server.db[i].expires = dictCreate(&keyptrDictType,NULL);
    }
    return backups;
}
void disklessLoadRestoreBackups(redisDb *backup, int restore, int empty_db_flags)
{
    if (restore) {
        emptyDbGeneric(server.db,-1,empty_db_flags,replicationEmptyDbCallback);
        for (int i=0; i<server.dbnum; i++) {
            dictRelease(server.db[i].dict);
            dictRelease(server.db[i].expires);
            server.db[i] = backup[i];
        }
    } else {
        emptyDbGeneric(backup,-1,empty_db_flags,replicationEmptyDbCallback);
        for (int i=0; i<server.dbnum; i++) {
            dictRelease(backup[i].dict);
            dictRelease(backup[i].expires);
        }
    }
    zfree(backup);
}
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8)
void readSyncBulkPayload(connection *conn) {
    char buf[4096];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load;
    redisDb *diskless_load_backup = NULL;
    int empty_db_flags = server.repl_slave_lazy_flush ? EMPTYDB_ASYNC :
                                                        EMPTYDB_NO_FLAGS;
    off_t left;
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;
    if (server.repl_transfer_size == -1) {
        if (connSyncReadLine(conn,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }
        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            server.repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master with EOF %s",
                useDisklessLoad()? "to parser":"to disk");
        } else {
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master %s",
                (long long) server.repl_transfer_size,
                useDisklessLoad()? "to parser":"to disk");
        }
        return;
    }
    use_diskless_load = useDisklessLoad();
    if (!use_diskless_load) {
        if (usemark) {
            readlen = sizeof(buf);
        } else {
            left = server.repl_transfer_size - server.repl_transfer_read;
            readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
        }
        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
                (nread == -1) ? strerror(errno) : "connection lost");
            cancelReplicationHandshake();
            return;
        }
        server.stat_net_input_bytes += nread;
        int eof_reached = 0;
        if (usemark) {
            if (nread >= CONFIG_RUN_ID_SIZE) {
                memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,
                       CONFIG_RUN_ID_SIZE);
            } else {
                int rem = CONFIG_RUN_ID_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0)
                eof_reached = 1;
        }
        server.repl_transfer_lastio = server.unixtime;
        if ((nwritten = write(server.repl_transfer_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                "Write error or short write writing to the DB dump file "
                "needed for MASTER <-> REPLICA synchronization: %s",
                (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        server.repl_transfer_read += nread;
        if (usemark && eof_reached) {
            if (ftruncate(server.repl_transfer_fd,
                server.repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
            {
                serverLog(LL_WARNING,
                    "Error truncating the RDB file received from the master "
                    "for SYNC: %s", strerror(errno));
                goto error;
            }
        }
        if (server.repl_transfer_read >=
            server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
        {
            off_t sync_size = server.repl_transfer_read -
                              server.repl_transfer_last_fsync_off;
            rdb_fsync_range(server.repl_transfer_fd,
                server.repl_transfer_last_fsync_off, sync_size);
            server.repl_transfer_last_fsync_off += sync_size;
        }
        if (!usemark) {
            if (server.repl_transfer_read == server.repl_transfer_size)
                eof_reached = 1;
        }
        if (!eof_reached) return;
    }
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Flushing old data");
    if (server.aof_state != AOF_OFF) stopAppendOnly();
    signalFlushedDb(-1);
    if (use_diskless_load &&
        server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB)
    {
        diskless_load_backup = disklessLoadMakeBackups();
    } else {
        emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);
    }
    connSetReadHandler(conn, NULL);
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
    rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
    if (use_diskless_load) {
        rio rdb;
        rioInitWithConn(&rdb,conn,server.repl_transfer_size);
        connBlock(conn);
        connRecvTimeout(conn, server.repl_timeout*1000);
        startLoading(server.repl_transfer_size);
        if (rdbLoadRio(&rdb,&rsi,0) != C_OK) {
            stopLoading();
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization DB "
                "from socket");
            cancelReplicationHandshake();
            rioFreeConn(&rdb, NULL);
            if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
                disklessLoadRestoreBackups(diskless_load_backup,1,
                                           empty_db_flags);
            } else {
                emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);
            }
            return;
        }
        stopLoading();
        if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
            disklessLoadRestoreBackups(diskless_load_backup,0,empty_db_flags);
        }
        if (usemark) {
            if (!rioRead(&rdb,buf,CONFIG_RUN_ID_SIZE) ||
                memcmp(buf,eofmark,CONFIG_RUN_ID_SIZE) != 0)
            {
                serverLog(LL_WARNING,"Replication stream EOF marker is broken");
                cancelReplicationHandshake();
                rioFreeConn(&rdb, NULL);
                return;
            }
        }
        rioFreeConn(&rdb, NULL);
        connNonBlock(conn);
        connRecvTimeout(conn,0);
    } else {
        if (server.rdb_child_pid != -1) {
            serverLog(LL_NOTICE,
                "Replica is about to load the RDB file received from the "
                "master, but there is a pending RDB child running. "
                "Killing process %ld and removing its temp file to avoid "
                "any race",
                    (long) server.rdb_child_pid);
            killRDBChild();
        }
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to rename the temp DB into %s in "
                "MASTER <-> REPLICA synchronization: %s",
                server.rdb_filename, strerror(errno));
            cancelReplicationHandshake();
            return;
        }
        if (rdbLoad(server.rdb_filename,&rsi) != C_OK) {
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization "
                "DB from disk");
            cancelReplicationHandshake();
            return;
        }
        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        server.repl_transfer_fd = -1;
        server.repl_transfer_tmpfile = NULL;
    }
    replicationCreateMasterClient(server.repl_transfer_s,rsi.repl_stream_db);
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;
    memcpy(server.replid,server.master->replid,sizeof(server.replid));
    server.master_repl_offset = server.master->reploff;
    clearReplicationId2();
    if (server.repl_backlog == NULL) createReplicationBacklog();
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");
    if (server.aof_enabled) restartAOFAfterSYNC();
    return;
error:
    cancelReplicationHandshake();
    return;
}
#define SYNC_CMD_READ (1<<0)
#define SYNC_CMD_WRITE (1<<1)
#define SYNC_CMD_FULL (SYNC_CMD_READ|SYNC_CMD_WRITE)
char *sendSynchronousCommand(int flags, connection *conn, ...) {
    if (flags & SYNC_CMD_WRITE) {
        char *arg;
        va_list ap;
        sds cmd = sdsempty();
        sds cmdargs = sdsempty();
        size_t argslen = 0;
        va_start(ap,conn);
        while(1) {
            arg = va_arg(ap, char*);
            if (arg == NULL) break;
            cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
            argslen++;
        }
        va_end(ap);
        cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
        cmd = sdscatsds(cmd,cmdargs);
        sdsfree(cmdargs);
        if (connSyncWrite(conn,cmd,sdslen(cmd),server.repl_syncio_timeout*1000)
            == -1)
        {
            sdsfree(cmd);
            return sdscatprintf(sdsempty(),"-Writing to master: %s",
                    connGetLastError(conn));
        }
        sdsfree(cmd);
    }
    if (flags & SYNC_CMD_READ) {
        char buf[256];
        if (connSyncReadLine(conn,buf,sizeof(buf),server.repl_syncio_timeout*1000)
            == -1)
        {
            return sdscatprintf(sdsempty(),"-Reading from master: %s",
                    strerror(errno));
        }
        server.repl_transfer_lastio = server.unixtime;
        return sdsnew(buf);
    }
    return NULL;
}
#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
int slaveTryPartialResynchronization(connection *conn, int read_reply) {
    char *psync_replid;
    char psync_offset[32];
    sds reply;
    if (!read_reply) {
        server.master_initial_offset = -1;
        if (server.cached_master) {
            psync_replid = server.cached_master->replid;
            snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
        } else {
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }
        reply = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PSYNC",psync_replid,psync_offset,NULL);
        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            connSetReadHandler(conn, NULL);
            return PSYNC_WRITE_ERROR;
        }
        return PSYNC_WAIT_REPLY;
    }
    reply = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
    if (sdslen(reply) == 0) {
        sdsfree(reply);
        return PSYNC_WAIT_REPLY;
    }
    connSetReadHandler(conn, NULL);
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;
        replid = strchr(reply,' ');
        if (replid) {
            replid++;
            offset = strchr(replid,' ');
            if (offset) offset++;
        }
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            memset(server.master_replid,0,CONFIG_RUN_ID_SIZE+1);
        } else {
            memcpy(server.master_replid, replid, offset-replid-1);
            server.master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            server.master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                server.master_replid,
                server.master_initial_offset);
        }
        replicationDiscardCachedMaster();
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }
    if (!strncmp(reply,"+CONTINUE",9)) {
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char new[CONFIG_RUN_ID_SIZE+1];
            memcpy(new,start,CONFIG_RUN_ID_SIZE);
            new[CONFIG_RUN_ID_SIZE] = '\0';
            if (strcmp(new,server.cached_master->replid)) {
                serverLog(LL_WARNING,"Master replication ID changed to %s",new);
                memcpy(server.replid2,server.cached_master->replid,
                    sizeof(server.replid2));
                server.second_replid_offset = server.master_repl_offset+1;
                memcpy(server.replid,new,sizeof(server.replid));
                memcpy(server.cached_master->replid,new,sizeof(server.replid));
                disconnectSlaves();
            }
        }
        sdsfree(reply);
        replicationResurrectCachedMaster(conn);
        if (server.repl_backlog == NULL) createReplicationBacklog();
        return PSYNC_CONTINUE;
    }
    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }
    if (strncmp(reply,"-ERR",4)) {
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();
    return PSYNC_NOT_SUPPORTED;
}
void syncWithMaster(connection *conn) {
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int psync_result;
    if (server.repl_state == REPL_STATE_NONE) {
        connClose(conn);
        return;
    }
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
                connGetLastError(conn));
        goto error;
    }
    if (server.repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        connSetReadHandler(conn, syncWithMaster);
        connSetWriteHandler(conn, NULL);
        server.repl_state = REPL_STATE_RECEIVE_PONG;
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PING",NULL);
        if (err) goto write_error;
        return;
    }
    if (server.repl_state == REPL_STATE_RECEIVE_PONG) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_AUTH;
    }
    if (server.repl_state == REPL_STATE_SEND_AUTH) {
        if (server.masteruser && server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",
                                         server.masteruser,server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else if (server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
        } else {
            server.repl_state = REPL_STATE_SEND_PORT;
        }
    }
    if (server.repl_state == REPL_STATE_RECEIVE_AUTH) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PORT;
    }
    if (server.repl_state == REPL_STATE_SEND_PORT) {
        int port;
        if (server.slave_announce_port) port = server.slave_announce_port;
        else if (server.tls_replication && server.tls_port) port = server.tls_port;
        else port = server.port;
        sds portstr = sdsfromlonglong(port);
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "listening-port",portstr, NULL);
        sdsfree(portstr);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_PORT;
        return;
    }
    if (server.repl_state == REPL_STATE_RECEIVE_PORT) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_IP;
    }
    if (server.repl_state == REPL_STATE_SEND_IP &&
        server.slave_announce_ip == NULL)
    {
            server.repl_state = REPL_STATE_SEND_CAPA;
    }
    if (server.repl_state == REPL_STATE_SEND_IP) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "ip-address",server.slave_announce_ip, NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_IP;
        return;
    }
    if (server.repl_state == REPL_STATE_RECEIVE_IP) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_CAPA;
    }
    if (server.repl_state == REPL_STATE_SEND_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "capa","eof","capa","psync2",NULL);
        if (err) goto write_error;
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_CAPA;
        return;
    }
    if (server.repl_state == REPL_STATE_RECEIVE_CAPA) {
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_SEND_PSYNC;
    }
    if (server.repl_state == REPL_STATE_SEND_PSYNC) {
        if (slaveTryPartialResynchronization(conn,0) == PSYNC_WRITE_ERROR) {
            err = sdsnew("Write error sending the PSYNC command.");
            goto write_error;
        }
        server.repl_state = REPL_STATE_RECEIVE_PSYNC;
        return;
    }
    if (server.repl_state != REPL_STATE_RECEIVE_PSYNC) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             server.repl_state);
        goto error;
    }
    psync_result = slaveTryPartialResynchronization(conn,1);
    if (psync_result == PSYNC_WAIT_REPLY) return;
    if (psync_result == PSYNC_TRY_LATER) goto error;
    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        return;
    }
    disconnectSlaves();
    freeReplicationBacklog();
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        if (connSyncWrite(conn,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }
    if (!useDisklessLoad()) {
        while(maxtries--) {
            snprintf(tmpfile,256,
                "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
            dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
            if (dfd != -1) break;
            sleep(1);
        }
        if (dfd == -1) {
            serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
            goto error;
        }
        server.repl_transfer_tmpfile = zstrdup(tmpfile);
        server.repl_transfer_fd = dfd;
    }
    if (connSetReadHandler(conn, readSyncBulkPayload)
            == C_ERR)
    {
        char conninfo[CONN_INFO_LEN];
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (%s)",
            strerror(errno), connGetInfo(conn, conninfo, sizeof(conninfo)));
        goto error;
    }
    server.repl_state = REPL_STATE_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_lastio = server.unixtime;
    return;
error:
    if (dfd != -1) close(dfd);
    connClose(conn);
    server.repl_transfer_s = NULL;
    if (server.repl_transfer_fd != -1)
        close(server.repl_transfer_fd);
    if (server.repl_transfer_tmpfile)
        zfree(server.repl_transfer_tmpfile);
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_state = REPL_STATE_CONNECT;
    return;
write_error:
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}
int connectWithMaster(void) {
    server.repl_transfer_s = server.tls_replication ? connCreateTLS() : connCreateSocket();
    if (connConnect(server.repl_transfer_s, server.masterhost, server.masterport,
                NET_FIRST_BIND_ADDR, syncWithMaster) == C_ERR) {
        serverLog(LL_WARNING,"Unable to connect to MASTER: %s",
                connGetLastError(server.repl_transfer_s));
        connClose(server.repl_transfer_s);
        server.repl_transfer_s = NULL;
        return C_ERR;
    }
    server.repl_transfer_lastio = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTING;
    return C_OK;
}
void undoConnectWithMaster(void) {
    connClose(server.repl_transfer_s);
    server.repl_transfer_s = NULL;
}
void replicationAbortSyncTransfer(void) {
    serverAssert(server.repl_state == REPL_STATE_TRANSFER);
    undoConnectWithMaster();
    if (server.repl_transfer_fd!=-1) {
        close(server.repl_transfer_fd);
        unlink(server.repl_transfer_tmpfile);
        zfree(server.repl_transfer_tmpfile);
        server.repl_transfer_tmpfile = NULL;
        server.repl_transfer_fd = -1;
    }
}
int cancelReplicationHandshake(void) {
    if (server.repl_state == REPL_STATE_TRANSFER) {
        replicationAbortSyncTransfer();
        server.repl_state = REPL_STATE_CONNECT;
    } else if (server.repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState())
    {
        undoConnectWithMaster();
        server.repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }
    return 1;
}
void replicationSetMaster(char *ip, int port) {
    int was_master = server.masterhost == NULL;
    sdsfree(server.masterhost);
    server.masterhost = sdsnew(ip);
    server.masterport = port;
    if (server.master) {
        freeClient(server.master);
    }
    disconnectAllBlockedClients();
    disconnectSlaves();
    cancelReplicationHandshake();
    if (was_master) {
        replicationDiscardCachedMaster();
        replicationCacheMasterUsingMyself();
    }
    server.repl_state = REPL_STATE_CONNECT;
}
void replicationUnsetMaster(void) {
    if (server.masterhost == NULL) return;
    sdsfree(server.masterhost);
    server.masterhost = NULL;
    shiftReplicationId();
    if (server.master) freeClient(server.master);
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    disconnectSlaves();
    server.repl_state = REPL_STATE_NONE;
    server.slaveseldb = -1;
    server.repl_no_slaves_since = server.unixtime;
}
void replicationHandleMasterDisconnection(void) {
    server.master = NULL;
    server.repl_state = REPL_STATE_CONNECT;
    server.repl_down_since = server.unixtime;
}
void replicaofCommand(client *c) {
    if (server.cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
            if (server.aof_enabled && server.aof_state == AOF_OFF) restartAOFAfterSYNC();
        }
    } else {
        long port;
        if (c->flags & CLIENT_SLAVE)
        {
            addReplyError(c, "Command is not valid when client is a replica.");
            return;
        }
        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != C_OK))
            return;
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization "
                                "with the master we are already connected "
                                "with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified "
                                 "master\r\n"));
            return;
        }
        replicationSetMaster(c->argv[1]->ptr, port);
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            server.masterhost, server.masterport, client);
        sdsfree(client);
    }
    addReply(c,shared.ok);
}
void roleCommand(client *c) {
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;
        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addReplyDeferredLen(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;
            if (slaveip[0] == '\0') {
                if (connPeerToString(slave->conn,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveip = ip;
            }
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyArrayLen(c,3);
            addReplyBulkCString(c,slaveip);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredArrayLen(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;
        addReplyArrayLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        if (slaveIsInHandshakeState()) {
            slavestate = "handshake";
        } else {
            switch(server.repl_state) {
            case REPL_STATE_NONE: slavestate = "none"; break;
            case REPL_STATE_CONNECT: slavestate = "connect"; break;
            case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
            case REPL_STATE_TRANSFER: slavestate = "sync"; break;
            case REPL_STATE_CONNECTED: slavestate = "connected"; break;
            default: slavestate = "unknown"; break;
            }
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}
void replicationSendAck(void) {
    client *c = server.master;
    if (c != NULL) {
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        addReplyArrayLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}
void replicationCacheMaster(client *c) {
    serverAssert(server.master != NULL && server.cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");
    unlinkClient(c);
    sdsclear(server.master->querybuf);
    sdsclear(server.master->pending_querybuf);
    server.master->read_reploff = server.master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);
    server.cached_master = server.master;
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }
    replicationHandleMasterDisconnection();
}
void replicationCacheMasterUsingMyself(void) {
    server.master_initial_offset = server.master_repl_offset;
    replicationCreateMasterClient(NULL,-1);
    memcpy(server.master->replid, server.replid, sizeof(server.replid));
    unlinkClient(server.master);
    server.cached_master = server.master;
    server.master = NULL;
    serverLog(LL_NOTICE,"Before turning into a replica, using my master parameters to synthesize a cached master: I may be able to synchronize with the new master with just a partial transfer.");
}
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;
    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~CLIENT_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}
void replicationResurrectCachedMaster(connection *conn) {
    server.master = server.cached_master;
    server.cached_master = NULL;
    server.master->conn = conn;
    connSetPrivateData(server.master->conn, server.master);
    server.master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;
    linkClient(server.master);
    if (connSetReadHandler(server.master->conn, readQueryFromClient)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master);
    }
    if (clientHasPendingReplies(server.master)) {
        if (connSetWriteHandler(server.master->conn, sendReplyToClient)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master);
        }
    }
}
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;
    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        time_t lag = server.unixtime - slave->repl_ack_time;
        if (slave->replstate == SLAVE_STATE_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    server.repl_good_slaves_count = good;
}
void replicationScriptCacheInit(void) {
    server.repl_scriptcache_size = 10000;
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    server.repl_scriptcache_fifo = listCreate();
}
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);
        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        serverAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    serverAssert(retval == DICT_OK);
}
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate != SLAVE_STATE_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;
    if (server.masterhost) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);
    replicationRequestAckFromSlaves();
}
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;
    listIter li;
    listNode *ln;
    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        client *c = ln->value;
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);
            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}
long long replicationGetSlaveOffset(void) {
    long long offset = 0;
    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    if (offset < 0) offset = 0;
    return offset;
}
void replicationCron(void) {
    static long long replication_cron_loops = 0;
    if (server.masterhost &&
        (server.repl_state == REPL_STATE_CONNECTING ||
         slaveIsInHandshakeState()) &&
         (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
        cancelReplicationHandshake();
    }
    if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        cancelReplicationHandshake();
    }
    if (server.masterhost && server.repl_state == REPL_STATE_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }
    if (server.repl_state == REPL_STATE_CONNECT) {
        serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        if (connectWithMaster() == C_OK) {
            serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
        }
    }
    if (server.masterhost && server.master &&
        !(server.master->flags & CLIENT_PRE_PSYNC))
        replicationSendAck();
    listIter li;
    listNode *ln;
    robj *ping_argv[1];
    if ((replication_cron_loops % server.repl_ping_slave_period) == 0 &&
        listLength(server.slaves))
    {
        int manual_failover_in_progress =
            server.cluster_enabled &&
            server.cluster->mf_end &&
            clientsArePaused();
        if (!manual_failover_in_progress) {
            ping_argv[0] = createStringObject("PING",4);
            replicationFeedSlaves(server.slaves, server.slaveseldb,
                ping_argv, 1);
            decrRefCount(ping_argv[0]);
        }
    }
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        int is_presync =
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
             server.rdb_child_type != RDB_CHILD_TYPE_SOCKET));
        if (is_presync) {
            connWrite(slave->conn, "\n", 1);
        }
    }
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            if (slave->flags & CLIENT_PRE_PSYNC) continue;
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                serverLog(LL_WARNING, "Disconnecting timedout replica: %s",
                    replicationGetSlaveName(slave));
                freeClient(slave);
            }
        }
    }
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog && server.masterhost == NULL)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;
        if (idle > server.repl_backlog_time_limit) {
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) server.repl_backlog_time_limit);
        }
    }
    if (listLength(server.slaves) == 0 &&
        server.aof_state == AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }
    if (!hasActiveChildProcess()) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa = -1;
        listNode *ln;
        listIter li;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                idle = server.unixtime - slave->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
                mincapa = (mincapa == -1) ? slave->slave_capa :
                                            (mincapa & slave->slave_capa);
            }
        }
        if (slaves_waiting &&
            (!server.repl_diskless_sync ||
             max_idle > server.repl_diskless_sync_delay))
        {
            startBgsaveForReplication(mincapa);
        }
    }
    refreshGoodSlavesCount();
    replication_cron_loops++;
}
