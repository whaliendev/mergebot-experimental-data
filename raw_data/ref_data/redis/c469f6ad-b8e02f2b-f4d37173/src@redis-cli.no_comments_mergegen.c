#include "fmacros.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <hiredis.h>
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <hiredis_ssl.h>
#endif
#include <sds.h>
#include "dict.h"
#include "adlist.h"
#include "zmalloc.h"
#include "linenoise.h"
#include "help.h"
#include "anet.h"
#include "ae.h"
#define UNUSED(V) ((void) V)
#define OUTPUT_STANDARD 0
#define OUTPUT_RAW 1
#define OUTPUT_CSV 2
#define REDIS_CLI_KEEPALIVE_INTERVAL 15
#define REDIS_CLI_DEFAULT_PIPE_TIMEOUT 30
#define REDIS_CLI_HISTFILE_ENV "REDISCLI_HISTFILE"
#define REDIS_CLI_HISTFILE_DEFAULT ".rediscli_history"
#define REDIS_CLI_RCFILE_ENV "REDISCLI_RCFILE"
#define REDIS_CLI_RCFILE_DEFAULT ".redisclirc"
#define REDIS_CLI_AUTH_ENV "REDISCLI_AUTH"
#define CLUSTER_MANAGER_SLOTS 16384
#define CLUSTER_MANAGER_MIGRATE_TIMEOUT 60000
#define CLUSTER_MANAGER_MIGRATE_PIPELINE 10
#define CLUSTER_MANAGER_REBALANCE_THRESHOLD 2
#define CLUSTER_MANAGER_INVALID_HOST_ARG \
    "[ERR] Invalid arguments: you need to pass either a valid " \
    "address (ie. 120.0.0.1:7000) or space separated IP " \
    "and port (ie. 120.0.0.1 7000)\n"
#define CLUSTER_MANAGER_MODE() (config.cluster_manager_command.name != NULL)
#define CLUSTER_MANAGER_MASTERS_COUNT(nodes,replicas) (nodes/(replicas + 1))
#define CLUSTER_MANAGER_COMMAND(n,...) \
        (redisCommand(n->context, __VA_ARGS__))
#define CLUSTER_MANAGER_NODE_ARRAY_FREE(array) zfree(array->alloc)
#define CLUSTER_MANAGER_PRINT_REPLY_ERROR(n,err) \
    clusterManagerLogErr("Node %s:%d replied with error:\n%s\n", \
                         n->ip, n->port, err);
#define clusterManagerLogInfo(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_INFO,__VA_ARGS__)
#define clusterManagerLogErr(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_ERR,__VA_ARGS__)
#define clusterManagerLogWarn(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_WARN,__VA_ARGS__)
#define clusterManagerLogOk(...) \
    clusterManagerLog(CLUSTER_MANAGER_LOG_LVL_SUCCESS,__VA_ARGS__)
#define CLUSTER_MANAGER_FLAG_MYSELF 1 << 0
#define CLUSTER_MANAGER_FLAG_SLAVE 1 << 1
#define CLUSTER_MANAGER_FLAG_FRIEND 1 << 2
#define CLUSTER_MANAGER_FLAG_NOADDR 1 << 3
#define CLUSTER_MANAGER_FLAG_DISCONNECT 1 << 4
#define CLUSTER_MANAGER_FLAG_FAIL 1 << 5
#define CLUSTER_MANAGER_CMD_FLAG_FIX 1 << 0
#define CLUSTER_MANAGER_CMD_FLAG_SLAVE 1 << 1
#define CLUSTER_MANAGER_CMD_FLAG_YES 1 << 2
#define CLUSTER_MANAGER_CMD_FLAG_AUTOWEIGHTS 1 << 3
#define CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER 1 << 4
#define CLUSTER_MANAGER_CMD_FLAG_SIMULATE 1 << 5
#define CLUSTER_MANAGER_CMD_FLAG_REPLACE 1 << 6
#define CLUSTER_MANAGER_CMD_FLAG_COPY 1 << 7
#define CLUSTER_MANAGER_CMD_FLAG_COLOR 1 << 8
#define CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS 1 << 9
#define CLUSTER_MANAGER_OPT_GETFRIENDS 1 << 0
#define CLUSTER_MANAGER_OPT_COLD 1 << 1
#define CLUSTER_MANAGER_OPT_UPDATE 1 << 2
#define CLUSTER_MANAGER_OPT_QUIET 1 << 6
#define CLUSTER_MANAGER_OPT_VERBOSE 1 << 7
#define CLUSTER_MANAGER_LOG_LVL_INFO 1
#define CLUSTER_MANAGER_LOG_LVL_WARN 2
#define CLUSTER_MANAGER_LOG_LVL_ERR 3
#define CLUSTER_MANAGER_LOG_LVL_SUCCESS 4
#define CLUSTER_JOIN_CHECK_AFTER 20
#define LOG_COLOR_BOLD "29;1m"
#define LOG_COLOR_RED "31;1m"
#define LOG_COLOR_GREEN "32;1m"
#define LOG_COLOR_YELLOW "33;1m"
#define LOG_COLOR_RESET "0m"
#define CC_FORCE (1<<0)
#define CC_QUIET (1<<1)
int spectrum_palette_color_size = 19;
int spectrum_palette_color[] = {0,233,234,235,237,239,241,243,245,247,144,143,142,184,226,214,208,202,196};
int spectrum_palette_mono_size = 13;
int spectrum_palette_mono[] = {0,233,234,235,237,239,241,243,245,247,249,251,253};
int *spectrum_palette;
int spectrum_palette_size;
static uint64_t dictSdsHash(const void *key);
static int dictSdsKeyCompare(void *privdata, const void *key1,
    const void *key2);
static void dictSdsDestructor(void *privdata, void *val);
static void dictListDestructor(void *privdata, void *val);
typedef struct clusterManagerCommand {
    char *name;
    int argc;
    char **argv;
    int flags;
    int replicas;
    char *from;
    char *to;
    char **weight;
    int weight_argc;
    char *master_id;
    int slots;
    int timeout;
    int pipeline;
    float threshold;
    char *backup_dir;
} clusterManagerCommand;
static void createClusterManagerCommand(char *cmdname, int argc, char **argv);
static redisContext *context;
static struct config {
    char *hostip;
    int hostport;
    char *hostsocket;
    int tls;
    char *sni;
    char *cacert;
    char *cacertdir;
    char *cert;
    char *key;
    long repeat;
    long interval;
    int dbnum;
    int interactive;
    int shutdown;
    int monitor_mode;
    int pubsub_mode;
    int latency_mode;
    int latency_dist_mode;
    int latency_history;
    int lru_test_mode;
    long long lru_test_sample_size;
    int cluster_mode;
    int cluster_reissue_command;
    int slave_mode;
    int pipe_mode;
    int pipe_timeout;
    int getrdb_mode;
    int stat_mode;
    int scan_mode;
    int intrinsic_latency_mode;
    int intrinsic_latency_duration;
    char *pattern;
    char *rdb_filename;
    int bigkeys;
    int memkeys;
    unsigned memkeys_samples;
    int hotkeys;
    int stdinarg;
    char *auth;
    char *user;
    int output;
    sds mb_delim;
    char prompt[128];
    char *eval;
    int eval_ldb;
    int eval_ldb_sync;
    int eval_ldb_end;
    int enable_ldb_on_eval;
    int last_cmd_type;
    int verbose;
    clusterManagerCommand cluster_manager_command;
    int no_auth_warning;
    int resp3;
} config;
static struct pref {
    int hints;
} pref;
static volatile sig_atomic_t force_cancel_loop = 0;
static void usage(void);
static void slaveMode(void);
char *redisGitSHA1(void);
char *redisGitDirty(void);
static int cliConnect(int force);
static char *getInfoField(char *info, char *field);
static long getLongInfoField(char *info, char *field);
uint16_t crc16(const char *buf, int len);
static long long ustime(void) {
    struct timeval tv;
    long long ust;
    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}
static long long mstime(void) {
    return ustime()/1000;
}
static void cliRefreshPrompt(void) {
    if (config.eval_ldb) return;
    sds prompt = sdsempty();
    if (config.hostsocket != NULL) {
        prompt = sdscatfmt(prompt,"redis %s",config.hostsocket);
    } else {
        char addr[256];
        anetFormatAddr(addr, sizeof(addr), config.hostip, config.hostport);
        prompt = sdscatlen(prompt,addr,strlen(addr));
    }
    if (config.dbnum != 0)
        prompt = sdscatfmt(prompt,"[%i]",config.dbnum);
    prompt = sdscatlen(prompt,"> ",2);
    snprintf(config.prompt,sizeof(config.prompt),"%s",prompt);
    sdsfree(prompt);
}
static sds getDotfilePath(char *envoverride, char *dotfilename) {
    char *path = NULL;
    sds dotPath = NULL;
    path = getenv(envoverride);
    if (path != NULL && *path != '\0') {
        if (!strcmp("/dev/null", path)) {
            return NULL;
        }
        dotPath = sdsnew(path);
    } else {
        char *home = getenv("HOME");
        if (home != NULL && *home != '\0') {
            dotPath = sdscatprintf(sdsempty(), "%s/%s", home, dotfilename);
        }
    }
    return dotPath;
}
#define isHexChar(c) (isdigit(c) || (c >= 'a' && c <= 'f'))
#define decodeHexChar(c) (isdigit(c) ? c - '0' : c - 'a' + 10)
#define decodeHex(h,l) ((decodeHexChar(h) << 4) + decodeHexChar(l))
static sds percentDecode(const char *pe, size_t len) {
    const char *end = pe + len;
    sds ret = sdsempty();
    const char *curr = pe;
    while (curr < end) {
        if (*curr == '%') {
            if ((end - curr) < 2) {
                fprintf(stderr, "Incomplete URI encoding\n");
                exit(1);
            }
            char h = tolower(*(++curr));
            char l = tolower(*(++curr));
            if (!isHexChar(h) || !isHexChar(l)) {
                fprintf(stderr, "Illegal character in URI encoding\n");
                exit(1);
            }
            char c = decodeHex(h, l);
            ret = sdscatlen(ret, &c, 1);
            curr++;
        } else {
            ret = sdscatlen(ret, curr++, 1);
        }
    }
    return ret;
}
static void parseRedisUri(const char *uri) {
    const char *scheme = "redis://";
    const char *curr = uri;
    const char *end = uri + strlen(uri);
    const char *userinfo, *username, *port, *host, *path;
    if (strncasecmp(scheme, curr, strlen(scheme))) {
        fprintf(stderr,"Invalid URI scheme\n");
        exit(1);
    }
    curr += strlen(scheme);
    if (curr == end) return;
    if ((userinfo = strchr(curr,'@'))) {
        if ((username = strchr(curr, ':')) && username < userinfo) {
            curr = username + 1;
        }
        config.auth = percentDecode(curr, userinfo - curr);
        curr = userinfo + 1;
    }
    if (curr == end) return;
    path = strchr(curr, '/');
    if (*curr != '/') {
        host = path ? path - 1 : end;
        if ((port = strchr(curr, ':'))) {
            config.hostport = atoi(port + 1);
            host = port - 1;
        }
        config.hostip = sdsnewlen(curr, host - curr + 1);
    }
    curr = path ? path + 1 : end;
    if (curr == end) return;
    config.dbnum = atoi(curr);
}
static uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}
static int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}
static void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    sdsfree(val);
}
void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}
void _serverAssert(const char *estr, const char *file, int line) {
    fprintf(stderr, "=== ASSERTION FAILED ===");
    fprintf(stderr, "==> %s:%d '%s' is not true",file,line,estr);
    *((char*)-1) = 'x';
}
#define CLI_HELP_COMMAND 1
#define CLI_HELP_GROUP 2
typedef struct {
    int type;
    int argc;
    sds *argv;
    sds full;
    struct commandHelp *org;
} helpEntry;
static helpEntry *helpEntries;
static int helpEntriesLen;
static sds cliVersion(void) {
    sds version;
    version = sdscatprintf(sdsempty(), "%s", REDIS_VERSION);
    if (strtoll(redisGitSHA1(),NULL,16)) {
        version = sdscatprintf(version, " (git:%s", redisGitSHA1());
        if (strtoll(redisGitDirty(),NULL,10))
            version = sdscatprintf(version, "-dirty");
        version = sdscat(version, ")");
    }
    return version;
}
static void cliInitHelp(void) {
    int commandslen = sizeof(commandHelp)/sizeof(struct commandHelp);
    int groupslen = sizeof(commandGroups)/sizeof(char*);
    int i, len, pos = 0;
    helpEntry tmp;
    helpEntriesLen = len = commandslen+groupslen;
    helpEntries = zmalloc(sizeof(helpEntry)*len);
    for (i = 0; i < groupslen; i++) {
        tmp.argc = 1;
        tmp.argv = zmalloc(sizeof(sds));
        tmp.argv[0] = sdscatprintf(sdsempty(),"@%s",commandGroups[i]);
        tmp.full = tmp.argv[0];
        tmp.type = CLI_HELP_GROUP;
        tmp.org = NULL;
        helpEntries[pos++] = tmp;
    }
    for (i = 0; i < commandslen; i++) {
        tmp.argv = sdssplitargs(commandHelp[i].name,&tmp.argc);
        tmp.full = sdsnew(commandHelp[i].name);
        tmp.type = CLI_HELP_COMMAND;
        tmp.org = &commandHelp[i];
        helpEntries[pos++] = tmp;
    }
}
static void cliIntegrateHelp(void) {
    if (cliConnect(CC_QUIET) == REDIS_ERR) return;
    redisReply *reply = redisCommand(context, "COMMAND");
    if(reply == NULL || reply->type != REDIS_REPLY_ARRAY) return;
    for (size_t j = 0; j < reply->elements; j++) {
        redisReply *entry = reply->element[j];
        if (entry->type != REDIS_REPLY_ARRAY || entry->elements < 4 ||
            entry->element[0]->type != REDIS_REPLY_STRING ||
            entry->element[1]->type != REDIS_REPLY_INTEGER ||
            entry->element[3]->type != REDIS_REPLY_INTEGER) return;
        char *cmdname = entry->element[0]->str;
        int i;
        for (i = 0; i < helpEntriesLen; i++) {
            helpEntry *he = helpEntries+i;
            if (!strcasecmp(he->argv[0],cmdname))
                break;
        }
        if (i != helpEntriesLen) continue;
        helpEntriesLen++;
        helpEntries = zrealloc(helpEntries,sizeof(helpEntry)*helpEntriesLen);
        helpEntry *new = helpEntries+(helpEntriesLen-1);
        new->argc = 1;
        new->argv = zmalloc(sizeof(sds));
        new->argv[0] = sdsnew(cmdname);
        new->full = new->argv[0];
        new->type = CLI_HELP_COMMAND;
        sdstoupper(new->argv[0]);
        struct commandHelp *ch = zmalloc(sizeof(*ch));
        ch->name = new->argv[0];
        ch->params = sdsempty();
        int args = llabs(entry->element[1]->integer);
        args--;
        if (entry->element[3]->integer == 1) {
            ch->params = sdscat(ch->params,"key ");
            args--;
        }
        while(args-- > 0) ch->params = sdscat(ch->params,"arg ");
        if (entry->element[1]->integer < 0)
            ch->params = sdscat(ch->params,"...options...");
        ch->summary = "Help not available";
        ch->group = 0;
        ch->since = "not known";
        new->org = ch;
    }
    freeReplyObject(reply);
}
static void cliOutputCommandHelp(struct commandHelp *help, int group) {
    printf("\r\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\r\n", help->name, help->params);
    printf("  \x1b[33msummary:\x1b[0m %s\r\n", help->summary);
    printf("  \x1b[33msince:\x1b[0m %s\r\n", help->since);
    if (group) {
        printf("  \x1b[33mgroup:\x1b[0m %s\r\n", commandGroups[help->group]);
    }
}
static void cliOutputGenericHelp(void) {
    sds version = cliVersion();
    printf(
        "redis-cli %s\n"
        "To get help about Redis commands type:\n"
        "      \"help @<group>\" to get a list of commands in <group>\n"
        "      \"help <command>\" for help on <command>\n"
        "      \"help <tab>\" to get a list of possible help topics\n"
        "      \"quit\" to exit\n"
        "\n"
        "To set redis-cli preferences:\n"
        "      \":set hints\" enable online hints\n"
        "      \":set nohints\" disable online hints\n"
        "Set your preferences in ~/.redisclirc\n",
        version
    );
    sdsfree(version);
}
static void cliOutputHelp(int argc, char **argv) {
    int i, j, len;
    int group = -1;
    helpEntry *entry;
    struct commandHelp *help;
    if (argc == 0) {
        cliOutputGenericHelp();
        return;
    } else if (argc > 0 && argv[0][0] == '@') {
        len = sizeof(commandGroups)/sizeof(char*);
        for (i = 0; i < len; i++) {
            if (strcasecmp(argv[0]+1,commandGroups[i]) == 0) {
                group = i;
                break;
            }
        }
    }
    assert(argc > 0);
    for (i = 0; i < helpEntriesLen; i++) {
        entry = &helpEntries[i];
        if (entry->type != CLI_HELP_COMMAND) continue;
        help = entry->org;
        if (group == -1) {
            if (argc == entry->argc) {
                for (j = 0; j < argc; j++) {
                    if (strcasecmp(argv[j],entry->argv[j]) != 0) break;
                }
                if (j == argc) {
                    cliOutputCommandHelp(help,1);
                }
            }
        } else {
            if (group == help->group) {
                cliOutputCommandHelp(help,0);
            }
        }
    }
    printf("\r\n");
}
static void completionCallback(const char *buf, linenoiseCompletions *lc) {
    size_t startpos = 0;
    int mask;
    int i;
    size_t matchlen;
    sds tmp;
    if (strncasecmp(buf,"help ",5) == 0) {
        startpos = 5;
        while (isspace(buf[startpos])) startpos++;
        mask = CLI_HELP_COMMAND | CLI_HELP_GROUP;
    } else {
        mask = CLI_HELP_COMMAND;
    }
    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & mask)) continue;
        matchlen = strlen(buf+startpos);
        if (strncasecmp(buf+startpos,helpEntries[i].full,matchlen) == 0) {
            tmp = sdsnewlen(buf,startpos);
            tmp = sdscat(tmp,helpEntries[i].full);
            linenoiseAddCompletion(lc,tmp);
            sdsfree(tmp);
        }
    }
}
static char *hintsCallback(const char *buf, int *color, int *bold) {
    if (!pref.hints) return NULL;
    int i, argc, buflen = strlen(buf);
    sds *argv = sdssplitargs(buf,&argc);
    int endspace = buflen && isspace(buf[buflen-1]);
    if (argc == 0) {
        sdsfreesplitres(argv,argc);
        return NULL;
    }
    for (i = 0; i < helpEntriesLen; i++) {
        if (!(helpEntries[i].type & CLI_HELP_COMMAND)) continue;
        if (strcasecmp(argv[0],helpEntries[i].full) == 0)
        {
            *color = 90;
            *bold = 0;
            sds hint = sdsnew(helpEntries[i].org->params);
            int toremove = argc-1;
            while(toremove > 0 && sdslen(hint)) {
                if (hint[0] == '[') break;
                if (hint[0] == ' ') toremove--;
                sdsrange(hint,1,-1);
            }
            if (!endspace) {
                sds newhint = sdsnewlen(" ",1);
                newhint = sdscatsds(newhint,hint);
                sdsfree(hint);
                hint = newhint;
            }
            sdsfreesplitres(argv,argc);
            return hint;
        }
    }
    sdsfreesplitres(argv,argc);
    return NULL;
}
static void freeHintsCallback(void *ptr) {
    sdsfree(ptr);
}
static int cliAuth(void) {
    redisReply *reply;
    if (config.auth == NULL) return REDIS_OK;
    if (config.user == NULL)
        reply = redisCommand(context,"AUTH %s",config.auth);
    else
        reply = redisCommand(context,"AUTH %s %s",config.user,config.auth);
    if (reply != NULL) {
        if (reply->type == REDIS_REPLY_ERROR)
            fprintf(stderr,"Warning: AUTH failed\n");
        freeReplyObject(reply);
        return REDIS_OK;
    }
    return REDIS_ERR;
}
static int cliSelect(void) {
    redisReply *reply;
    if (config.dbnum == 0) return REDIS_OK;
    reply = redisCommand(context,"SELECT %d",config.dbnum);
    if (reply != NULL) {
        int result = REDIS_OK;
        if (reply->type == REDIS_REPLY_ERROR) result = REDIS_ERR;
        freeReplyObject(reply);
        return result;
    }
    return REDIS_ERR;
}
                                                                                                                 static int cliSecureConnection(redisContext *c, const char **err) { #ifdef USE_OPENSSL static SSL_CTX *ssl_ctx = NULL; if (!ssl_ctx) { ssl_ctx = SSL_CTX_new(SSLv23_client_method()); if (!ssl_ctx) { *err = "Failed to create SSL_CTX"; goto error; } SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3); SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL); if (config.cacert || config.cacertdir) { if (!SSL_CTX_load_verify_locations(ssl_ctx, config.cacert, config.cacertdir)) { *err = "Invalid CA Certificate File/Directory"; goto error; } } else { if (!SSL_CTX_set_default_verify_paths(ssl_ctx)) { *err = "Failed to use default CA paths"; goto error; } } if (config.cert && !SSL_CTX_use_certificate_chain_file(ssl_ctx, config.cert)) { *err = "Invalid client certificate"; goto error; } } if (config.key && !SSL_CTX_use_PrivateKey_file(ssl_ctx, config.key, SSL_FILETYPE_PEM)) { *err = "Invalid private key"; goto error; } } return redisInitiateSSL(c, ssl); error: SSL_CTX_free(ssl_ctx); return REDIS_ERR; } return redisInitiateSSL(c, ssl); error: SSL_CTX_free(ssl_ctx); ssl_ctx = NULL; return REDIS_ERR
static int cliConnect(int flags) {
    if (context == NULL || flags & CC_FORCE) {
        if (context != NULL) {
            redisFree(context);
        }
        if (config.hostsocket == NULL) {
            context = redisConnect(config.hostip,config.hostport);
        } else {
            context = redisConnectUnix(config.hostsocket);
        }
        if (!context->err && config.tls) {
            const char *err = NULL;
            if (cliSecureConnection(context, &err) == REDIS_ERR && err) {
                fprintf(stderr, "Could not negotiate a TLS connection: %s\n", err);
                context = NULL;
                redisFree(context);
                return REDIS_ERR;
            }
        }
        if (context->err) {
            if (!(flags & CC_QUIET)) {
                fprintf(stderr,"Could not connect to Redis at ");
                if (config.hostsocket == NULL)
                    fprintf(stderr,"%s:%d: %s\n",
                        config.hostip,config.hostport,context->errstr);
                else
                    fprintf(stderr,"%s: %s\n",
                        config.hostsocket,context->errstr);
            }
            redisFree(context);
            context = NULL;
            return REDIS_ERR;
        }
        anetKeepAlive(NULL, context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);
        if (cliAuth() != REDIS_OK)
            return REDIS_ERR;
        if (cliSelect() != REDIS_OK)
            return REDIS_ERR;
        if (cliSwitchProto() != REDIS_OK)
            return REDIS_ERR;
    }
    return REDIS_OK;
}
static void cliPrintContextError(void) {
    if (context == NULL) return;
    fprintf(stderr,"Error: %s\n",context->errstr);
}
static sds cliFormatReplyTTY(redisReply *r, char *prefix) {
    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscatprintf(out,"(error) %s\n", r->str);
    break;
    case REDIS_REPLY_STATUS:
        out = sdscat(out,r->str);
        out = sdscat(out,"\n");
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"(integer) %lld\n",r->integer);
    break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"(double) %s\n",r->str);
    break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        if (r->type == REDIS_REPLY_STRING) {
            out = sdscatrepr(out,r->str,r->len);
            out = sdscat(out,"\n");
        } else {
            out = sdscatlen(out,r->str,r->len);
            out = sdscat(out,"\n");
        }
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"(nil)\n");
    break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "(true)\n" : "(false)\n");
    break;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_SET:
        if (r->elements == 0) {
            if (r->type == REDIS_REPLY_ARRAY)
                out = sdscat(out,"(empty array)\n");
            else if (r->type == REDIS_REPLY_MAP)
                out = sdscat(out,"(empty hash)\n");
            else if (r->type == REDIS_REPLY_SET)
                out = sdscat(out,"(empty set)\n");
            else
                out = sdscat(out,"(empty aggregate type)\n");
        } else {
            unsigned int i, idxlen = 0;
            char _prefixlen[16];
            char _prefixfmt[16];
            sds _prefix;
            sds tmp;
            i = r->elements;
            if (r->type == REDIS_REPLY_MAP) i /= 2;
            do {
                idxlen++;
                i /= 10;
            } while(i);
            memset(_prefixlen,' ',idxlen+2);
            _prefixlen[idxlen+2] = '\0';
            _prefix = sdscat(sdsnew(prefix),_prefixlen);
            char numsep;
            if (r->type == REDIS_REPLY_SET) numsep = '~';
            else if (r->type == REDIS_REPLY_MAP) numsep = '#';
            else numsep = ')';
            snprintf(_prefixfmt,sizeof(_prefixfmt),"%%s%%%ud%c ",idxlen,numsep);
            for (i = 0; i < r->elements; i++) {
                unsigned int human_idx = (r->type == REDIS_REPLY_MAP) ?
                                         i/2 : i;
                human_idx++;
                out = sdscatprintf(out,_prefixfmt,i == 0 ? "" : prefix,human_idx);
                tmp = cliFormatReplyTTY(r->element[i],_prefix);
                out = sdscatlen(out,tmp,sdslen(tmp));
                sdsfree(tmp);
                if (r->type == REDIS_REPLY_MAP) {
                    i++;
                    sdsrange(out,0,-2);
                    out = sdscat(out," => ");
                    tmp = cliFormatReplyTTY(r->element[i],_prefix);
                    out = sdscatlen(out,tmp,sdslen(tmp));
                    sdsfree(tmp);
                }
            }
            sdsfree(_prefix);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}
int isColorTerm(void) {
    char *t = getenv("TERM");
    return t != NULL && strstr(t,"xterm") != NULL;
}
sds sdscatcolor(sds o, char *s, size_t len, char *color) {
    if (!isColorTerm()) return sdscatlen(o,s,len);
    int bold = strstr(color,"bold") != NULL;
    int ccode = 37;
    if (strstr(color,"red")) ccode = 31;
    else if (strstr(color,"green")) ccode = 32;
    else if (strstr(color,"yellow")) ccode = 33;
    else if (strstr(color,"blue")) ccode = 34;
    else if (strstr(color,"magenta")) ccode = 35;
    else if (strstr(color,"cyan")) ccode = 36;
    else if (strstr(color,"white")) ccode = 37;
    o = sdscatfmt(o,"\033[%i;%i;49m",bold,ccode);
    o = sdscatlen(o,s,len);
    o = sdscat(o,"\033[0m");
    return o;
}
sds sdsCatColorizedLdbReply(sds o, char *s, size_t len) {
    char *color = "white";
    if (strstr(s,"<debug>")) color = "bold";
    if (strstr(s,"<redis>")) color = "green";
    if (strstr(s,"<reply>")) color = "cyan";
    if (strstr(s,"<error>")) color = "red";
    if (strstr(s,"<hint>")) color = "bold";
    if (strstr(s,"<value>") || strstr(s,"<retval>")) color = "magenta";
    if (len > 4 && isdigit(s[3])) {
        if (s[1] == '>') color = "yellow";
        else if (s[2] == '#') color = "bold";
    }
    return sdscatcolor(o,s,len,color);
}
static sds cliFormatReplyRaw(redisReply *r) {
    sds out = sdsempty(), tmp;
    size_t i;
    switch (r->type) {
    case REDIS_REPLY_NIL:
        break;
    case REDIS_REPLY_ERROR:
        out = sdscatlen(out,r->str,r->len);
        out = sdscatlen(out,"\n",1);
        break;
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        if (r->type == REDIS_REPLY_STATUS && config.eval_ldb) {
            if (strstr(r->str,"<endsession>") == r->str) {
                config.enable_ldb_on_eval = 0;
                config.eval_ldb = 0;
                config.eval_ldb_end = 1;
                config.output = OUTPUT_STANDARD;
                cliRefreshPrompt();
            } else {
                out = sdsCatColorizedLdbReply(out,r->str,r->len);
            }
        } else {
            out = sdscatlen(out,r->str,r->len);
        }
        break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "(true)" : "(false)");
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
        break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"%s",r->str);
        break;
    case REDIS_REPLY_ARRAY:
        for (i = 0; i < r->elements; i++) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
        }
        break;
    case REDIS_REPLY_MAP:
        for (i = 0; i < r->elements; i += 2) {
            if (i > 0) out = sdscat(out,config.mb_delim);
            tmp = cliFormatReplyRaw(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
            out = sdscatlen(out," ",1);
            tmp = cliFormatReplyRaw(r->element[i+1]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            sdsfree(tmp);
        }
        break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}
static sds cliFormatReplyCSV(redisReply *r) {
    unsigned int i;
    sds out = sdsempty();
    switch (r->type) {
    case REDIS_REPLY_ERROR:
        out = sdscat(out,"ERROR,");
        out = sdscatrepr(out,r->str,strlen(r->str));
    break;
    case REDIS_REPLY_STATUS:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_INTEGER:
        out = sdscatprintf(out,"%lld",r->integer);
    break;
    case REDIS_REPLY_DOUBLE:
        out = sdscatprintf(out,"%s",r->str);
        break;
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        out = sdscatrepr(out,r->str,r->len);
    break;
    case REDIS_REPLY_NIL:
        out = sdscat(out,"NULL");
    break;
    case REDIS_REPLY_BOOL:
        out = sdscat(out,r->integer ? "true" : "false");
    break;
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
        for (i = 0; i < r->elements; i++) {
            sds tmp = cliFormatReplyCSV(r->element[i]);
            out = sdscatlen(out,tmp,sdslen(tmp));
            if (i != r->elements-1) out = sdscat(out,",");
            sdsfree(tmp);
        }
    break;
    default:
        fprintf(stderr,"Unknown reply type: %d\n", r->type);
        exit(1);
    }
    return out;
}
static int cliReadReply(int output_raw_strings) {
    void *_reply;
    redisReply *reply;
    sds out = NULL;
    int output = 1;
    if (redisGetReply(context,&_reply) != REDIS_OK) {
        if (config.shutdown) {
            redisFree(context);
            context = NULL;
            return REDIS_OK;
        }
        if (config.interactive) {
            if (context->err == REDIS_ERR_IO &&
                (errno == ECONNRESET || errno == EPIPE))
                return REDIS_ERR;
            if (context->err == REDIS_ERR_EOF)
                return REDIS_ERR;
        }
        cliPrintContextError();
        exit(1);
        return REDIS_ERR;
    }
    reply = (redisReply*)_reply;
    config.last_cmd_type = reply->type;
    if (config.cluster_mode && reply->type == REDIS_REPLY_ERROR &&
        (!strncmp(reply->str,"MOVED",5) || !strcmp(reply->str,"ASK")))
    {
        char *p = reply->str, *s;
        int slot;
        output = 0;
        s = strchr(p,' ');
        p = strchr(s+1,' ');
        *p = '\0';
        slot = atoi(s+1);
        s = strrchr(p+1,':');
        *s = '\0';
        sdsfree(config.hostip);
        config.hostip = sdsnew(p+1);
        config.hostport = atoi(s+1);
        if (config.interactive)
            printf("-> Redirected to slot [%d] located at %s:%d\n",
                slot, config.hostip, config.hostport);
        config.cluster_reissue_command = 1;
        cliRefreshPrompt();
    }
    if (output) {
        if (output_raw_strings) {
            out = cliFormatReplyRaw(reply);
        } else {
            if (config.output == OUTPUT_RAW) {
                out = cliFormatReplyRaw(reply);
                out = sdscat(out,"\n");
            } else if (config.output == OUTPUT_STANDARD) {
                out = cliFormatReplyTTY(reply,"");
            } else if (config.output == OUTPUT_CSV) {
                out = cliFormatReplyCSV(reply);
                out = sdscat(out,"\n");
            }
        }
        fwrite(out,sdslen(out),1,stdout);
        sdsfree(out);
    }
    freeReplyObject(reply);
    return REDIS_OK;
}
static int cliSendCommand(int argc, char **argv, long repeat) {
    char *command = argv[0];
    size_t *argvlen;
    int j, output_raw;
    if (!config.eval_ldb &&
        (!strcasecmp(command,"help") || !strcasecmp(command,"?"))) {
        cliOutputHelp(--argc, ++argv);
        return REDIS_OK;
    }
    if (context == NULL) return REDIS_ERR;
    output_raw = 0;
    if (!strcasecmp(command,"info") ||
        !strcasecmp(command,"lolwut") ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                       !strcasecmp(argv[1],"htstats")) ||
        (argc >= 2 && !strcasecmp(command,"debug") &&
                       !strcasecmp(argv[1],"htstats-key")) ||
        (argc >= 2 && !strcasecmp(command,"memory") &&
                      (!strcasecmp(argv[1],"malloc-stats") ||
                       !strcasecmp(argv[1],"doctor"))) ||
        (argc == 2 && !strcasecmp(command,"cluster") &&
                      (!strcasecmp(argv[1],"nodes") ||
                       !strcasecmp(argv[1],"info"))) ||
        (argc >= 2 && !strcasecmp(command,"client") &&
                       !strcasecmp(argv[1],"list")) ||
        (argc == 3 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"graph")) ||
        (argc == 2 && !strcasecmp(command,"latency") &&
                       !strcasecmp(argv[1],"doctor")))
    {
        output_raw = 1;
    }
    if (!strcasecmp(command,"shutdown")) config.shutdown = 1;
    if (!strcasecmp(command,"monitor")) config.monitor_mode = 1;
    if (!strcasecmp(command,"subscribe") ||
        !strcasecmp(command,"psubscribe")) config.pubsub_mode = 1;
    if (!strcasecmp(command,"sync") ||
        !strcasecmp(command,"psync")) config.slave_mode = 1;
    if (argc == 3 && !strcasecmp(argv[0],"script") &&
                     !strcasecmp(argv[1],"debug"))
    {
        if (!strcasecmp(argv[2],"yes") || !strcasecmp(argv[2],"sync")) {
            config.enable_ldb_on_eval = 1;
        } else {
            config.enable_ldb_on_eval = 0;
        }
    }
    if (!strcasecmp(command,"eval") && config.enable_ldb_on_eval) {
        config.eval_ldb = 1;
        config.output = OUTPUT_RAW;
    }
    argvlen = zmalloc(argc*sizeof(size_t));
    for (j = 0; j < argc; j++)
        argvlen[j] = sdslen(argv[j]);
    while(repeat < 0 || repeat-- > 0) {
        redisAppendCommandArgv(context,argc,(const char**)argv,argvlen);
        while (config.monitor_mode) {
            if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            fflush(stdout);
        }
        if (config.pubsub_mode) {
            if (config.output != OUTPUT_RAW)
                printf("Reading messages... (press Ctrl-C to quit)\n");
            while (1) {
                if (cliReadReply(output_raw) != REDIS_OK) exit(1);
            }
        }
        if (config.slave_mode) {
            printf("Entering replica output mode...  (press Ctrl-C to quit)\n");
            slaveMode();
            config.slave_mode = 0;
            zfree(argvlen);
            return REDIS_ERR;
        }
        if (cliReadReply(output_raw) != REDIS_OK) {
            zfree(argvlen);
            return REDIS_ERR;
        } else {
            if (!strcasecmp(command,"select") && argc == 2 && config.last_cmd_type != REDIS_REPLY_ERROR) {
                config.dbnum = atoi(argv[1]);
                cliRefreshPrompt();
            } else if (!strcasecmp(command,"auth") && (argc == 2 || argc == 3))
            {
                cliSelect();
            }
        }
        if (config.cluster_reissue_command){
            break;
        }
        if (config.interval) usleep(config.interval);
        fflush(stdout);
    }
    zfree(argvlen);
    return REDIS_OK;
}
static redisReply *reconnectingRedisCommand(redisContext *c, const char *fmt, ...) {
    redisReply *reply = NULL;
    int tries = 0;
    va_list ap;
    assert(!c->err);
    while(reply == NULL) {
        while (c->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            printf("\r\x1b[0K");
            printf("Reconnecting... %d\r", ++tries);
            fflush(stdout);
            redisFree(c);
            c = redisConnect(config.hostip,config.hostport);
            if (!c->err && config.tls) {
                const char *err = NULL;
                if (cliSecureConnection(c, &err) == REDIS_ERR && err) {
                    fprintf(stderr, "TLS Error: %s\n", err);
                    exit(1);
                }
            }
            usleep(1000000);
        }
        va_start(ap,fmt);
        reply = redisvCommand(c,fmt,ap);
        va_end(ap);
        if (c->err && !(c->err & (REDIS_ERR_IO | REDIS_ERR_EOF))) {
            fprintf(stderr, "Error: %s\n", c->errstr);
            exit(1);
        } else if (tries > 0) {
            printf("\r\x1b[0K");
        }
    }
    context = c;
    return reply;
}
static int parseOptions(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;
        if (!strcmp(argv[i],"-h") && !lastarg) {
            sdsfree(config.hostip);
            config.hostip = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"--help")) {
            usage();
        } else if (!strcmp(argv[i],"-x")) {
            config.stdinarg = 1;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-s") && !lastarg) {
            config.hostsocket = argv[++i];
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"-i") && !lastarg) {
            double seconds = atof(argv[++i]);
            config.interval = seconds*1000000;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.dbnum = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-auth-warning")) {
            config.no_auth_warning = 1;
        } else if ((!strcmp(argv[i],"-a") || !strcmp(argv[i],"--pass"))
                   && !lastarg)
        {
            config.auth = argv[++i];
        } else if (!strcmp(argv[i],"--user") && !lastarg) {
            config.user = argv[++i];
        } else if (!strcmp(argv[i],"-u") && !lastarg) {
            parseRedisUri(argv[++i]);
        } else if (!strcmp(argv[i],"--raw")) {
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--no-raw")) {
            config.output = OUTPUT_STANDARD;
        } else if (!strcmp(argv[i],"--csv")) {
            config.output = OUTPUT_CSV;
        } else if (!strcmp(argv[i],"--latency")) {
            config.latency_mode = 1;
        } else if (!strcmp(argv[i],"--latency-dist")) {
            config.latency_dist_mode = 1;
        } else if (!strcmp(argv[i],"--mono")) {
            spectrum_palette = spectrum_palette_mono;
            spectrum_palette_size = spectrum_palette_mono_size;
        } else if (!strcmp(argv[i],"--latency-history")) {
            config.latency_mode = 1;
            config.latency_history = 1;
        } else if (!strcmp(argv[i],"--lru-test") && !lastarg) {
            config.lru_test_mode = 1;
            config.lru_test_sample_size = strtoll(argv[++i],NULL,10);
        } else if (!strcmp(argv[i],"--slave")) {
            config.slave_mode = 1;
        } else if (!strcmp(argv[i],"--replica")) {
            config.slave_mode = 1;
        } else if (!strcmp(argv[i],"--stat")) {
            config.stat_mode = 1;
        } else if (!strcmp(argv[i],"--scan")) {
            config.scan_mode = 1;
        } else if (!strcmp(argv[i],"--pattern") && !lastarg) {
            config.pattern = argv[++i];
        } else if (!strcmp(argv[i],"--intrinsic-latency") && !lastarg) {
            config.intrinsic_latency_mode = 1;
            config.intrinsic_latency_duration = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--rdb") && !lastarg) {
            config.getrdb_mode = 1;
            config.rdb_filename = argv[++i];
        } else if (!strcmp(argv[i],"--pipe")) {
            config.pipe_mode = 1;
        } else if (!strcmp(argv[i],"--pipe-timeout") && !lastarg) {
            config.pipe_timeout = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--bigkeys")) {
            config.bigkeys = 1;
        } else if (!strcmp(argv[i],"--memkeys")) {
            config.memkeys = 1;
            config.memkeys_samples = 0;
        } else if (!strcmp(argv[i],"--memkeys-samples")) {
            config.memkeys = 1;
            config.memkeys_samples = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--hotkeys")) {
            config.hotkeys = 1;
        } else if (!strcmp(argv[i],"--eval") && !lastarg) {
            config.eval = argv[++i];
        } else if (!strcmp(argv[i],"--ldb")) {
            config.eval_ldb = 1;
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"--ldb-sync-mode")) {
            config.eval_ldb = 1;
            config.eval_ldb_sync = 1;
            config.output = OUTPUT_RAW;
        } else if (!strcmp(argv[i],"-c")) {
            config.cluster_mode = 1;
        } else if (!strcmp(argv[i],"-d") && !lastarg) {
            sdsfree(config.mb_delim);
            config.mb_delim = sdsnew(argv[++i]);
        } else if (!strcmp(argv[i],"--verbose")) {
            config.verbose = 1;
        } else if (!strcmp(argv[i],"--cluster") && !lastarg) {
            if (CLUSTER_MANAGER_MODE()) usage();
            char *cmd = argv[++i];
            int j = i;
            while (j < argc && argv[j][0] != '-') j++;
            if (j > i) j--;
            createClusterManagerCommand(cmd, j - i, argv + i + 1);
            i = j;
        } else if (!strcmp(argv[i],"--cluster") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"--cluster-replicas") && !lastarg) {
            config.cluster_manager_command.replicas = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-master-id") && !lastarg) {
            config.cluster_manager_command.master_id = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-from") && !lastarg) {
            config.cluster_manager_command.from = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-to") && !lastarg) {
            config.cluster_manager_command.to = argv[++i];
        } else if (!strcmp(argv[i],"--cluster-weight") && !lastarg) {
            if (config.cluster_manager_command.weight != NULL) {
                fprintf(stderr, "WARNING: you cannot use --cluster-weight "
                                "more than once.\n"
                                "You can set more weights by adding them "
                                "as a space-separated list, ie:\n"
                                "--cluster-weight n1=w n2=w\n");
                exit(1);
            }
            int widx = i + 1;
            char **weight = argv + widx;
            int wargc = 0;
            for (; widx < argc; widx++) {
                if (strstr(argv[widx], "--") == argv[widx]) break;
                if (strchr(argv[widx], '=') == NULL) break;
                wargc++;
            }
            if (wargc > 0) {
                config.cluster_manager_command.weight = weight;
                config.cluster_manager_command.weight_argc = wargc;
                i += wargc;
            }
        } else if (!strcmp(argv[i],"--cluster-slots") && !lastarg) {
            config.cluster_manager_command.slots = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-timeout") && !lastarg) {
            config.cluster_manager_command.timeout = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-pipeline") && !lastarg) {
            config.cluster_manager_command.pipeline = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-threshold") && !lastarg) {
            config.cluster_manager_command.threshold = atof(argv[++i]);
        } else if (!strcmp(argv[i],"--cluster-yes")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_YES;
        } else if (!strcmp(argv[i],"--cluster-simulate")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_SIMULATE;
        } else if (!strcmp(argv[i],"--cluster-replace")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_REPLACE;
        } else if (!strcmp(argv[i],"--cluster-copy")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_COPY;
        } else if (!strcmp(argv[i],"--cluster-slave")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_SLAVE;
        } else if (!strcmp(argv[i],"--cluster-use-empty-masters")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER;
        } else if (!strcmp(argv[i],"--cluster-search-multiple-owners")) {
            config.cluster_manager_command.flags |=
                CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS;
#ifdef USE_OPENSSL
        } else if (!strcmp(argv[i],"--tls")) {
            config.tls = 1;
        } else if (!strcmp(argv[i],"--sni")) {
            config.sni = argv[++i];
        } else if (!strcmp(argv[i],"--cacertdir")) {
            config.cacertdir = argv[++i];
        } else if (!strcmp(argv[i],"--cacert")) {
            config.cacert = argv[++i];
        } else if (!strcmp(argv[i],"--cert")) {
            config.cert = argv[++i];
        } else if (!strcmp(argv[i],"--key")) {
            config.key = argv[++i];
#endif
        } else if (!strcmp(argv[i],"-v") || !strcmp(argv[i], "--version")) {
            sds version = cliVersion();
            printf("redis-cli %s\n", version);
            sdsfree(version);
            exit(0);
        } else if (!strcmp(argv[i],"-3")) {
            config.resp3 = 1;
        } else if (CLUSTER_MANAGER_MODE() && argv[i][0] != '-') {
            if (config.cluster_manager_command.argc == 0) {
                int j = i + 1;
                while (j < argc && argv[j][0] != '-') j++;
                int cmd_argc = j - i;
                config.cluster_manager_command.argc = cmd_argc;
                config.cluster_manager_command.argv = argv + i;
                if (cmd_argc > 1) i = j - 1;
            }
        } else {
            if (argv[i][0] == '-') {
                fprintf(stderr,
                    "Unrecognized option or bad number of args for: '%s'\n",
                    argv[i]);
                exit(1);
            } else {
                break;
            }
        }
    }
    if (config.eval_ldb && config.eval == NULL) {
        fprintf(stderr,"Options --ldb and --ldb-sync-mode require --eval.\n");
        fprintf(stderr,"Try %s --help for more information.\n", argv[0]);
        exit(1);
    }
    if (!config.no_auth_warning && config.auth != NULL) {
        fputs("Warning: Using a password with '-a' or '-u' option on the command"
              " line interface may not be safe.\n", stderr);
    }
    return i;
}
static void parseEnv() {
    char *auth = getenv(REDIS_CLI_AUTH_ENV);
    if (auth != NULL && config.auth == NULL) {
        config.auth = auth;
    }
}
static sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();
    while(1) {
        int nread = read(fileno(stdin),buf,1024);
        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}
static void usage(void) {
    sds version = cliVersion();
    fprintf(stderr,
"redis-cli %s\n"
"\n"
"Usage: redis-cli [OPTIONS] [cmd [arg [arg ...]]]\n"
"  -h <hostname>      Server hostname (default: 127.0.0.1).\n"
"  -p <port>          Server port (default: 6379).\n"
"  -s <socket>        Server socket (overrides hostname and port).\n"
"  -a <password>      Password to use when connecting to the server.\n"
"                     You can also use the " REDIS_CLI_AUTH_ENV " environment\n"
"                     variable to pass this password more safely\n"
"                     (if both are used, this argument takes predecence).\n"
"  -user <username>   Used to send ACL style 'AUTH username pass'. Needs -a.\n"
"  -pass <password>   Alias of -a for consistency with the new --user option.\n"
"  -u <uri>           Server URI.\n"
"  -r <repeat>        Execute specified command N times.\n"
"  -i <interval>      When -r is used, waits <interval> seconds per command.\n"
"                     It is possible to specify sub-second times like -i 0.1.\n"
"  -n <db>            Database number.\n"
"  -3                 Start session in RESP3 protocol mode.\n"
"  -x                 Read last argument from STDIN.\n"
"  -d <delimiter>     Multi-bulk delimiter in for raw formatting (default: \\n).\n"
"  -c                 Enable cluster mode (follow -ASK and -MOVED redirections).\n"
#ifdef USE_OPENSSL
"  --tls              Establish a secure TLS connection.\n"
"  --cacert           CA Certificate file to verify with.\n"
"  --cacertdir        Directory where trusted CA certificates are stored.\n"
"                     If neither cacert nor cacertdir are specified, the default\n"
"                     system-wide trusted root certs configuration will apply.\n"
"  --cert             Client certificate to authenticate with.\n"
"  --key              Private key file to authenticate with.\n"
#endif
"  --raw              Use raw formatting for replies (default when STDOUT is\n"
"                     not a tty).\n"
"  --no-raw           Force formatted output even when STDOUT is not a tty.\n"
"  --csv              Output in CSV format.\n"
"  --stat             Print rolling stats about server: mem, clients, ...\n"
"  --latency          Enter a special mode continuously sampling latency.\n"
"                     If you use this mode in an interactive session it runs\n"
"                     forever displaying real-time stats. Otherwise if --raw or\n"
"                     --csv is specified, or if you redirect the output to a non\n"
"                     TTY, it samples the latency for 1 second (you can use\n"
"                     -i to change the interval), then produces a single output\n"
"                     and exits.\n",version);
    fprintf(stderr,
"  --latency-history  Like --latency but tracking latency changes over time.\n"
"                     Default time interval is 15 sec. Change it using -i.\n"
"  --latency-dist     Shows latency as a spectrum, requires xterm 256 colors.\n"
"                     Default time interval is 1 sec. Change it using -i.\n"
"  --lru-test <keys>  Simulate a cache workload with an 80-20 distribution.\n"
"  --replica          Simulate a replica showing commands received from the master.\n"
"  --rdb <filename>   Transfer an RDB dump from remote server to local file.\n"
"  --pipe             Transfer raw Redis protocol from stdin to server.\n"
"  --pipe-timeout <n> In --pipe mode, abort with error if after sending all data.\n"
"                     no reply is received within <n> seconds.\n"
"                     Default timeout: %d. Use 0 to wait forever.\n",
    version, REDIS_CLI_DEFAULT_PIPE_TIMEOUT);
    fprintf(stderr,
"  --bigkeys          Sample Redis keys looking for keys with many elements (complexity).\n"
"  --memkeys          Sample Redis keys looking for keys consuming a lot of memory.\n"
"  --memkeys-samples <n> Sample Redis keys looking for keys consuming a lot of memory.\n"
"                     And define number of key elements to sample\n"
"  --hotkeys          Sample Redis keys looking for hot keys.\n"
"                     only works when maxmemory-policy is *lfu.\n"
"  --scan             List all keys using the SCAN command.\n"
"  --pattern <pat>    Useful with --scan to specify a SCAN pattern.\n"
"  --intrinsic-latency <sec> Run a test to measure intrinsic system latency.\n"
"                     The test will run for the specified amount of seconds.\n"
"  --eval <file>      Send an EVAL command using the Lua script at <file>.\n"
"  --ldb              Used with --eval enable the Redis Lua debugger.\n"
"  --ldb-sync-mode    Like --ldb but uses the synchronous Lua debugger, in\n"
"                     this mode the server is blocked and script changes are\n"
"                     not rolled back from the server memory.\n"
"  --cluster <command> [args...] [opts...]\n"
"                     Cluster Manager command and arguments (see below).\n"
"  --verbose          Verbose mode.\n"
"  --no-auth-warning  Don't show warning message when using password on command\n"
"                     line interface.\n"
"  --help             Output this help and exit.\n"
"  --version          Output version and exit.\n"
" --version Output version and exit.\n""\n");
    fprintf(stderr,
"Cluster Manager Commands:\n"
"  Use --cluster help to list all available cluster manager commands.\n"
"\n"
"Examples:\n"
"  cat /etc/passwd | redis-cli -x set mypasswd\n"
"  redis-cli get mypasswd\n"
"  redis-cli -r 100 lpush mylist x\n"
"  redis-cli -r 100 -i 1 info | grep used_memory_human:\n"
"  redis-cli --eval myscript.lua key1 key2 , arg1 arg2 arg3\n"
"  redis-cli --scan --pattern '*:12345*'\n"
"\n"
"  (Note: when using --eval the comma separates KEYS[] from ARGV[] items)\n"
"\n"
"When no command is given, redis-cli starts in interactive mode.\n"
"Type \"help\" in interactive mode for information on available commands\n"
"and settings.\n"
"\n");
    sdsfree(version);
    exit(1);
}
static int confirmWithYes(char *msg) {
    printf("%s (type 'yes' to accept): ", msg);
    fflush(stdout);
    char buf[4];
    int nread = read(fileno(stdin),buf,4);
    buf[3] = '\0';
    return (nread != 0 && !strcmp("yes", buf));
}
static char **convertToSds(int count, char** args) {
  int j;
  char **sds = zmalloc(sizeof(char*)*count);
  for(j = 0; j < count; j++)
    sds[j] = sdsnew(args[j]);
  return sds;
}
static int issueCommandRepeat(int argc, char **argv, long repeat) {
    while (1) {
        config.cluster_reissue_command = 0;
        if (cliSendCommand(argc,argv,repeat) != REDIS_OK) {
            cliConnect(CC_FORCE);
            if (cliSendCommand(argc,argv,repeat) != REDIS_OK) {
                cliPrintContextError();
                return REDIS_ERR;
            }
        }
        if (config.cluster_mode && config.cluster_reissue_command) {
            cliConnect(CC_FORCE);
        } else {
            break;
        }
    }
    return REDIS_OK;
}
static int issueCommand(int argc, char **argv) {
    return issueCommandRepeat(argc, argv, config.repeat);
}
static sds *cliSplitArgs(char *line, int *argc) {
    if (config.eval_ldb && (strstr(line,"eval ") == line ||
                            strstr(line,"e ") == line))
    {
        sds *argv = sds_malloc(sizeof(sds)*2);
        *argc = 2;
        int len = strlen(line);
        int elen = line[1] == ' ' ? 2 : 5;
        argv[0] = sdsnewlen(line,elen-1);
        argv[1] = sdsnewlen(line+elen,len-elen);
        return argv;
    } else {
        return sdssplitargs(line,argc);
    }
}
void cliSetPreferences(char **argv, int argc, int interactive) {
    if (!strcasecmp(argv[0],":set") && argc >= 2) {
        if (!strcasecmp(argv[1],"hints")) pref.hints = 1;
        else if (!strcasecmp(argv[1],"nohints")) pref.hints = 0;
        else {
            printf("%sunknown redis-cli preference '%s'\n",
                interactive ? "" : ".redisclirc: ",
                argv[1]);
        }
    } else {
        printf("%sunknown redis-cli internal command '%s'\n",
            interactive ? "" : ".redisclirc: ",
            argv[0]);
    }
}
void cliLoadPreferences(void) {
    sds rcfile = getDotfilePath(REDIS_CLI_RCFILE_ENV,REDIS_CLI_RCFILE_DEFAULT);
    if (rcfile == NULL) return;
    FILE *fp = fopen(rcfile,"r");
    char buf[1024];
    if (fp) {
        while(fgets(buf,sizeof(buf),fp) != NULL) {
            sds *argv;
            int argc;
            argv = sdssplitargs(buf,&argc);
            if (argc > 0) cliSetPreferences(argv,argc,0);
            sdsfreesplitres(argv,argc);
        }
        fclose(fp);
    }
    sdsfree(rcfile);
}
static void repl(void) {
    sds historyfile = NULL;
    int history = 0;
    char *line;
    int argc;
    sds *argv;
    cliInitHelp();
    cliIntegrateHelp();
    config.interactive = 1;
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(completionCallback);
    linenoiseSetHintsCallback(hintsCallback);
    linenoiseSetFreeHintsCallback(freeHintsCallback);
    if (isatty(fileno(stdin))) {
        historyfile = getDotfilePath(REDIS_CLI_HISTFILE_ENV,REDIS_CLI_HISTFILE_DEFAULT);
        history = 1;
        if (historyfile != NULL) {
            linenoiseHistoryLoad(historyfile);
        }
        cliLoadPreferences();
    }
    cliRefreshPrompt();
    while((line = linenoise(context ? config.prompt : "not connected> ")) != NULL) {
        if (line[0] != '\0') {
            long repeat = 1;
            int skipargs = 0;
            char *endptr = NULL;
            argv = cliSplitArgs(line,&argc);
            if (argv && argc > 0) {
                errno = 0;
                repeat = strtol(argv[0], &endptr, 10);
                if (argc > 1 && *endptr == '\0') {
                    if (errno == ERANGE || errno == EINVAL || repeat <= 0) {
                        fputs("Invalid redis-cli repeat command option value.\n", stdout);
                        sdsfreesplitres(argv, argc);
                        linenoiseFree(line);
                        continue;
                    }
                    skipargs = 1;
                } else {
                    repeat = 1;
                }
            }
            if (!(argv && argc > 0 && !strcasecmp(argv[0+skipargs], "auth"))) {
                if (history) linenoiseHistoryAdd(line);
                if (historyfile) linenoiseHistorySave(historyfile);
            }
            if (argv == NULL) {
                printf("Invalid argument(s)\n");
                linenoiseFree(line);
                continue;
            } else if (argc > 0) {
                if (strcasecmp(argv[0],"quit") == 0 ||
                    strcasecmp(argv[0],"exit") == 0)
                {
                    exit(0);
                } else if (argv[0][0] == ':') {
                    cliSetPreferences(argv,argc,1);
                    sdsfreesplitres(argv,argc);
                    linenoiseFree(line);
                    continue;
                } else if (strcasecmp(argv[0],"restart") == 0) {
                    if (config.eval) {
                        config.eval_ldb = 1;
                        config.output = OUTPUT_RAW;
                        return;
                    } else {
                        printf("Use 'restart' only in Lua debugging mode.");
                    }
                } else if (argc == 3 && !strcasecmp(argv[0],"connect")) {
                    sdsfree(config.hostip);
                    config.hostip = sdsnew(argv[1]);
                    config.hostport = atoi(argv[2]);
                    cliRefreshPrompt();
                    cliConnect(CC_FORCE);
                } else if (argc == 1 && !strcasecmp(argv[0],"clear")) {
                    linenoiseClearScreen();
                } else {
                    long long start_time = mstime(), elapsed;
                    issueCommandRepeat(argc-skipargs, argv+skipargs, repeat);
                    if (config.eval_ldb_end) {
                        config.eval_ldb_end = 0;
                        cliReadReply(0);
                        printf("\n(Lua debugging session ended%s)\n\n",
                            config.eval_ldb_sync ? "" :
                            " -- dataset changes rolled back");
                    }
                    elapsed = mstime()-start_time;
                    if (elapsed >= 500 &&
                        config.output == OUTPUT_STANDARD)
                    {
                        printf("(%.2fs)\n",(double)elapsed/1000);
                    }
                }
            }
            sdsfreesplitres(argv,argc);
        }
        linenoiseFree(line);
    }
    exit(0);
}
static int noninteractive(int argc, char **argv) {
    int retval = 0;
    if (config.stdinarg) {
        argv = zrealloc(argv, (argc+1)*sizeof(char*));
        argv[argc] = readArgFromStdin();
        retval = issueCommand(argc+1, argv);
    } else {
        retval = issueCommand(argc, argv);
    }
    return retval;
}
static int evalMode(int argc, char **argv) {
    sds script = NULL;
    FILE *fp;
    char buf[1024];
    size_t nread;
    char **argv2;
    int j, got_comma, keys;
    int retval = REDIS_OK;
    while(1) {
        if (config.eval_ldb) {
            printf(
            "Lua debugging session started, please use:\n"
            "quit    -- End the session.\n"
            "restart -- Restart the script in debug mode again.\n"
            "help    -- Show Lua script debugging commands.\n\n"
            );
        }
        sdsfree(script);
        script = sdsempty();
        got_comma = 0;
        keys = 0;
        fp = fopen(config.eval,"r");
        if (!fp) {
            fprintf(stderr,
                "Can't open file '%s': %s\n", config.eval, strerror(errno));
            exit(1);
        }
        while((nread = fread(buf,1,sizeof(buf),fp)) != 0) {
            script = sdscatlen(script,buf,nread);
        }
        fclose(fp);
        if (config.eval_ldb) {
            redisReply *reply = redisCommand(context,
                    config.eval_ldb_sync ?
                    "SCRIPT DEBUG sync": "SCRIPT DEBUG yes");
            if (reply) freeReplyObject(reply);
        }
        argv2 = zmalloc(sizeof(sds)*(argc+3));
        argv2[0] = sdsnew("EVAL");
        argv2[1] = script;
        for (j = 0; j < argc; j++) {
            if (!got_comma && argv[j][0] == ',' && argv[j][1] == 0) {
                got_comma = 1;
                continue;
            }
            argv2[j+3-got_comma] = sdsnew(argv[j]);
            if (!got_comma) keys++;
        }
        argv2[2] = sdscatprintf(sdsempty(),"%d",keys);
        int eval_ldb = config.eval_ldb;
        retval = issueCommand(argc+3-got_comma, argv2);
        if (eval_ldb) {
            if (!config.eval_ldb) {
                printf("Eval debugging session can't start:\n");
                cliReadReply(0);
                break;
            } else {
                strncpy(config.prompt,"lua debugger> ",sizeof(config.prompt));
                repl();
                cliConnect(CC_FORCE);
                printf("\n");
            }
        } else {
            break;
        }
    }
    return retval;
}
static struct clusterManager {
    list *nodes;
    list *errors;
} cluster_manager;
dict *clusterManagerUncoveredSlots = NULL;
typedef struct clusterManagerNode {
    redisContext *context;
    sds name;
    char *ip;
    int port;
    uint64_t current_epoch;
    time_t ping_sent;
    time_t ping_recv;
    int flags;
    list *flags_str;
    sds replicate;
    int dirty;
    uint8_t slots[CLUSTER_MANAGER_SLOTS];
    int slots_count;
    int replicas_count;
    list *friends;
    sds *migrating;
    sds *importing;
    int migrating_count;
    int importing_count;
    float weight;
    int balance;
} clusterManagerNode;
typedef struct clusterManagerNodeArray {
    clusterManagerNode **nodes;
    clusterManagerNode **alloc;
    int len;
    int count;
} clusterManagerNodeArray;
typedef struct clusterManagerReshardTableItem {
    clusterManagerNode *source;
    int slot;
} clusterManagerReshardTableItem;
typedef struct clusterManagerLink {
    sds node_name;
    sds node_addr;
    int connected;
    int handshaking;
} clusterManagerLink;
static dictType clusterManagerDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    NULL,
    dictSdsDestructor
};
static dictType clusterManagerLinkDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    dictSdsDestructor,
    dictListDestructor
};
typedef int clusterManagerCommandProc(int argc, char **argv);
typedef int (*clusterManagerOnReplyError)(redisReply *reply,
    clusterManagerNode *n, int bulk_idx);
static clusterManagerNode *clusterManagerNewNode(char *ip, int port);
static clusterManagerNode *clusterManagerNodeByName(const char *name);
static clusterManagerNode *clusterManagerNodeByAbbreviatedName(const char *n);
static void clusterManagerNodeResetSlots(clusterManagerNode *node);
static int clusterManagerNodeIsCluster(clusterManagerNode *node, char **err);
static void clusterManagerPrintNotClusterNodeError(clusterManagerNode *node,
                                                   char *err);
static int clusterManagerNodeLoadInfo(clusterManagerNode *node, int opts,
                                      char **err);
static int clusterManagerLoadInfoFromNode(clusterManagerNode *node, int opts);
static int clusterManagerNodeIsEmpty(clusterManagerNode *node, char **err);
static int clusterManagerGetAntiAffinityScore(clusterManagerNodeArray *ipnodes,
    int ip_count, clusterManagerNode ***offending, int *offending_len);
static void clusterManagerOptimizeAntiAffinity(clusterManagerNodeArray *ipnodes,
    int ip_count);
static sds clusterManagerNodeInfo(clusterManagerNode *node, int indent);
static void clusterManagerShowNodes(void);
static void clusterManagerShowClusterInfo(void);
static int clusterManagerFlushNodeConfig(clusterManagerNode *node, char **err);
static void clusterManagerWaitForClusterJoin(void);
static int clusterManagerCheckCluster(int quiet);
static void clusterManagerLog(int level, const char* fmt, ...);
static int clusterManagerIsConfigConsistent(void);
static dict *clusterManagerGetLinkStatus(void);
static void clusterManagerOnError(sds err);
static void clusterManagerNodeArrayInit(clusterManagerNodeArray *array,
                                        int len);
static void clusterManagerNodeArrayReset(clusterManagerNodeArray *array);
static void clusterManagerNodeArrayShift(clusterManagerNodeArray *array,
                                         clusterManagerNode **nodeptr);
static void clusterManagerNodeArrayAdd(clusterManagerNodeArray *array,
                                       clusterManagerNode *node);
static int clusterManagerCommandCreate(int argc, char **argv);
static int clusterManagerCommandAddNode(int argc, char **argv);
static int clusterManagerCommandDeleteNode(int argc, char **argv);
static int clusterManagerCommandInfo(int argc, char **argv);
static int clusterManagerCommandCheck(int argc, char **argv);
static int clusterManagerCommandFix(int argc, char **argv);
static int clusterManagerCommandReshard(int argc, char **argv);
static int clusterManagerCommandRebalance(int argc, char **argv);
static int clusterManagerCommandSetTimeout(int argc, char **argv);
static int clusterManagerCommandImport(int argc, char **argv);
static int clusterManagerCommandCall(int argc, char **argv);
static int clusterManagerCommandHelp(int argc, char **argv);
static int clusterManagerCommandBackup(int argc, char **argv);
typedef struct clusterManagerCommandDef {
    char *name;
    clusterManagerCommandProc *proc;
    int arity;
    char *args;
    char *options;
} clusterManagerCommandDef;
clusterManagerCommandDef clusterManagerCommands[] = {
    {"create", clusterManagerCommandCreate, -2, "host1:port1 ... hostN:portN",
     "replicas <arg>"},
    {"check", clusterManagerCommandCheck, -1, "host:port",
     "search-multiple-owners"},
    {"info", clusterManagerCommandInfo, -1, "host:port", NULL},
    {"fix", clusterManagerCommandFix, -1, "host:port",
     "search-multiple-owners"},
    {"reshard", clusterManagerCommandReshard, -1, "host:port",
     "from <arg>,to <arg>,slots <arg>,yes,timeout <arg>,pipeline <arg>,"
     "replace"},
    {"rebalance", clusterManagerCommandRebalance, -1, "host:port",
     "weight <node1=w1...nodeN=wN>,use-empty-masters,"
     "timeout <arg>,simulate,pipeline <arg>,threshold <arg>,replace"},
    {"add-node", clusterManagerCommandAddNode, 2,
     "new_host:new_port existing_host:existing_port", "slave,master-id <arg>"},
    {"del-node", clusterManagerCommandDeleteNode, 2, "host:port node_id",NULL},
    {"call", clusterManagerCommandCall, -2,
        "host:port command arg arg .. arg", NULL},
    {"set-timeout", clusterManagerCommandSetTimeout, 2,
     "host:port milliseconds", NULL},
    {"import", clusterManagerCommandImport, 1, "host:port",
     "from <arg>,copy,replace"},
    {"backup", clusterManagerCommandBackup, 2, "host:port backup_directory",
     NULL},
    {"help", clusterManagerCommandHelp, 0, NULL, NULL}
};
static void getRDB(clusterManagerNode *node);
static void createClusterManagerCommand(char *cmdname, int argc, char **argv) {
    clusterManagerCommand *cmd = &config.cluster_manager_command;
    cmd->name = cmdname;
    cmd->argc = argc;
    cmd->argv = argc ? argv : NULL;
    if (isColorTerm()) cmd->flags |= CLUSTER_MANAGER_CMD_FLAG_COLOR;
}
static clusterManagerCommandProc *validateClusterManagerCommand(void) {
    int i, commands_count = sizeof(clusterManagerCommands) /
                            sizeof(clusterManagerCommandDef);
    clusterManagerCommandProc *proc = NULL;
    char *cmdname = config.cluster_manager_command.name;
    int argc = config.cluster_manager_command.argc;
    for (i = 0; i < commands_count; i++) {
        clusterManagerCommandDef cmddef = clusterManagerCommands[i];
        if (!strcmp(cmddef.name, cmdname)) {
            if ((cmddef.arity > 0 && argc != cmddef.arity) ||
                (cmddef.arity < 0 && argc < (cmddef.arity * -1))) {
                fprintf(stderr, "[ERR] Wrong number of arguments for "
                                "specified --cluster sub command\n");
                return NULL;
            }
            proc = cmddef.proc;
        }
    }
    if (!proc) fprintf(stderr, "Unknown --cluster subcommand\n");
    return proc;
}
static int parseClusterNodeAddress(char *addr, char **ip_ptr, int *port_ptr,
                                   int *bus_port_ptr)
{
    char *c = strrchr(addr, '@');
    if (c != NULL) {
        *c = '\0';
        if (bus_port_ptr != NULL)
            *bus_port_ptr = atoi(c + 1);
    }
    c = strrchr(addr, ':');
    if (c != NULL) {
        *c = '\0';
        *ip_ptr = addr;
        *port_ptr = atoi(++c);
    } else return 0;
    return 1;
}
static int getClusterHostFromCmdArgs(int argc, char **argv,
                                     char **ip_ptr, int *port_ptr) {
    int port = 0;
    char *ip = NULL;
    if (argc == 1) {
        char *addr = argv[0];
        if (!parseClusterNodeAddress(addr, &ip, &port, NULL)) return 0;
    } else {
        ip = argv[0];
        port = atoi(argv[1]);
    }
    if (!ip || !port) return 0;
    else {
        *ip_ptr = ip;
        *port_ptr = port;
    }
    return 1;
}
static void freeClusterManagerNodeFlags(list *flags) {
    listIter li;
    listNode *ln;
    listRewind(flags, &li);
    while ((ln = listNext(&li)) != NULL) {
        sds flag = ln->value;
        sdsfree(flag);
    }
    listRelease(flags);
}
static void freeClusterManagerNode(clusterManagerNode *node) {
    if (node->context != NULL) redisFree(node->context);
    if (node->friends != NULL) {
        listIter li;
        listNode *ln;
        listRewind(node->friends,&li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *fn = ln->value;
            freeClusterManagerNode(fn);
        }
        listRelease(node->friends);
        node->friends = NULL;
    }
    if (node->name != NULL) sdsfree(node->name);
    if (node->replicate != NULL) sdsfree(node->replicate);
    if ((node->flags & CLUSTER_MANAGER_FLAG_FRIEND) && node->ip)
        sdsfree(node->ip);
    int i;
    if (node->migrating != NULL) {
        for (i = 0; i < node->migrating_count; i++) sdsfree(node->migrating[i]);
        zfree(node->migrating);
    }
    if (node->importing != NULL) {
        for (i = 0; i < node->importing_count; i++) sdsfree(node->importing[i]);
        zfree(node->importing);
    }
    if (node->flags_str != NULL) {
        freeClusterManagerNodeFlags(node->flags_str);
        node->flags_str = NULL;
    }
    zfree(node);
}
static void freeClusterManager(void) {
    listIter li;
    listNode *ln;
    if (cluster_manager.nodes != NULL) {
        listRewind(cluster_manager.nodes,&li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            freeClusterManagerNode(n);
        }
        listRelease(cluster_manager.nodes);
        cluster_manager.nodes = NULL;
    }
    if (cluster_manager.errors != NULL) {
        listRewind(cluster_manager.errors,&li);
        while ((ln = listNext(&li)) != NULL) {
            sds err = ln->value;
            sdsfree(err);
        }
        listRelease(cluster_manager.errors);
        cluster_manager.errors = NULL;
    }
    if (clusterManagerUncoveredSlots != NULL)
        dictRelease(clusterManagerUncoveredSlots);
}
static clusterManagerNode *clusterManagerNewNode(char *ip, int port) {
    clusterManagerNode *node = zmalloc(sizeof(*node));
    node->context = NULL;
    node->name = NULL;
    node->ip = ip;
    node->port = port;
    node->current_epoch = 0;
    node->ping_sent = 0;
    node->ping_recv = 0;
    node->flags = 0;
    node->flags_str = NULL;
    node->replicate = NULL;
    node->dirty = 0;
    node->friends = NULL;
    node->migrating = NULL;
    node->importing = NULL;
    node->migrating_count = 0;
    node->importing_count = 0;
    node->replicas_count = 0;
    node->weight = 1.0f;
    node->balance = 0;
    clusterManagerNodeResetSlots(node);
    return node;
}
static sds clusterManagerGetNodeRDBFilename(clusterManagerNode *node) {
    assert(config.cluster_manager_command.backup_dir);
    sds filename = sdsnew(config.cluster_manager_command.backup_dir);
    if (filename[sdslen(filename) - 1] != '/')
        filename = sdscat(filename, "/");
    filename = sdscatprintf(filename, "redis-node-%s-%d-%s.rdb", node->ip,
                            node->port, node->name);
    return filename;
}
static int clusterManagerCheckRedisReply(clusterManagerNode *n,
                                         redisReply *r, char **err)
{
    int is_err = 0;
    if (!r || (is_err = (r->type == REDIS_REPLY_ERROR))) {
        if (is_err) {
            if (err != NULL) {
                *err = zmalloc((r->len + 1) * sizeof(char));
                strcpy(*err, r->str);
            } else CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, r->str);
        }
        return 0;
    }
    return 1;
}
static int clusterManagerStartTransaction(clusterManagerNode *node) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "MULTI");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerExecTransaction(clusterManagerNode *node,
                                         clusterManagerOnReplyError onerror)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "EXEC");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (success) {
        if (reply->type != REDIS_REPLY_ARRAY) {
            success = 0;
            goto cleanup;
        }
        size_t i;
        for (i = 0; i < reply->elements; i++) {
            redisReply *r = reply->element[i];
            char *err = NULL;
            success = clusterManagerCheckRedisReply(node, r, &err);
            if (!success && onerror) success = onerror(r, node, i);
            if (err) {
                if (!success)
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                zfree(err);
            }
            if (!success) break;
        }
    }
cleanup:
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerNodeConnect(clusterManagerNode *node) {
    if (node->context) redisFree(node->context);
    node->context = redisConnect(node->ip, node->port);
    if (!node->context->err && config.tls) {
        const char *err = NULL;
        if (cliSecureConnection(node->context, &err) == REDIS_ERR && err) {
            fprintf(stderr,"TLS Error: %s\n", err);
            redisFree(node->context);
            node->context = NULL;
            return 0;
        }
    }
    if (node->context->err) {
        fprintf(stderr,"Could not connect to Redis at ");
        fprintf(stderr,"%s:%d: %s\n", node->ip, node->port,
                node->context->errstr);
        redisFree(node->context);
        node->context = NULL;
        return 0;
    }
    anetKeepAlive(NULL, node->context->fd, REDIS_CLI_KEEPALIVE_INTERVAL);
    if (config.auth) {
        redisReply *reply;
        if (config.user == NULL)
            reply = redisCommand(node->context,"AUTH %s", config.auth);
        else
            reply = redisCommand(node->context,"AUTH %s %s",
                                 config.user,config.auth);
        int ok = clusterManagerCheckRedisReply(node, reply, NULL);
        if (reply != NULL) freeReplyObject(reply);
        if (!ok) return 0;
    }
    return 1;
}
static void clusterManagerRemoveNodeFromList(list *nodelist,
                                             clusterManagerNode *node) {
    listIter li;
    listNode *ln;
    listRewind(nodelist, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (node == ln->value) {
            listDelNode(nodelist, ln);
            break;
        }
    }
}
static clusterManagerNode *clusterManagerNodeByName(const char *name) {
    if (cluster_manager.nodes == NULL) return NULL;
    clusterManagerNode *found = NULL;
    sds lcname = sdsempty();
    lcname = sdscpy(lcname, name);
    sdstolower(lcname);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->name && !sdscmp(n->name, lcname)) {
            found = n;
            break;
        }
    }
    sdsfree(lcname);
    return found;
}
static clusterManagerNode *clusterManagerNodeByAbbreviatedName(const char*name)
{
    if (cluster_manager.nodes == NULL) return NULL;
    clusterManagerNode *found = NULL;
    sds lcname = sdsempty();
    lcname = sdscpy(lcname, name);
    sdstolower(lcname);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->name &&
            strstr(n->name, lcname) == n->name) {
            found = n;
            break;
        }
    }
    sdsfree(lcname);
    return found;
}
static void clusterManagerNodeResetSlots(clusterManagerNode *node) {
    memset(node->slots, 0, sizeof(node->slots));
    node->slots_count = 0;
}
static redisReply *clusterManagerGetNodeRedisInfo(clusterManagerNode *node,
                                                  char **err)
{
    redisReply *info = CLUSTER_MANAGER_COMMAND(node, "INFO");
    if (err != NULL) *err = NULL;
    if (info == NULL) return NULL;
    if (info->type == REDIS_REPLY_ERROR) {
        if (err != NULL) {
            *err = zmalloc((info->len + 1) * sizeof(char));
            strcpy(*err, info->str);
        }
        freeReplyObject(info);
        return NULL;
    }
    return info;
}
static int clusterManagerNodeIsCluster(clusterManagerNode *node, char **err) {
    redisReply *info = clusterManagerGetNodeRedisInfo(node, err);
    if (info == NULL) return 0;
    int is_cluster = (int) getLongInfoField(info->str, "cluster_enabled");
    freeReplyObject(info);
    return is_cluster;
}
static int clusterManagerNodeIsEmpty(clusterManagerNode *node, char **err) {
    redisReply *info = clusterManagerGetNodeRedisInfo(node, err);
    int is_empty = 1;
    if (info == NULL) return 0;
    if (strstr(info->str, "db0:") != NULL) {
        is_empty = 0;
        goto result;
    }
    freeReplyObject(info);
    info = CLUSTER_MANAGER_COMMAND(node, "CLUSTER INFO");
    if (err != NULL) *err = NULL;
    if (!clusterManagerCheckRedisReply(node, info, err)) {
        is_empty = 0;
        goto result;
    }
    long known_nodes = getLongInfoField(info->str, "cluster_known_nodes");
    is_empty = (known_nodes == 1);
result:
    freeReplyObject(info);
    return is_empty;
}
static int clusterManagerGetAntiAffinityScore(clusterManagerNodeArray *ipnodes,
    int ip_count, clusterManagerNode ***offending, int *offending_len)
{
    int score = 0, i, j;
    int node_len = cluster_manager.nodes->len;
    clusterManagerNode **offending_p = NULL;
    if (offending != NULL) {
        *offending = zcalloc(node_len * sizeof(clusterManagerNode*));
        offending_p = *offending;
    }
    for (i = 0; i < ip_count; i++) {
        clusterManagerNodeArray *node_array = &(ipnodes[i]);
        dict *related = dictCreate(&clusterManagerDictType, NULL);
        char *ip = NULL;
        for (j = 0; j < node_array->len; j++) {
            clusterManagerNode *node = node_array->nodes[j];
            if (node == NULL) continue;
            if (!ip) ip = node->ip;
            sds types;
            sds key = (!node->replicate ? node->name : node->replicate);
            assert(key != NULL);
            dictEntry *entry = dictFind(related, key);
            if (entry) types = sdsdup((sds) dictGetVal(entry));
            else types = sdsempty();
            if (!node->replicate) types = sdscatprintf(types, "m%s", types);
            else types = sdscat(types, "s");
            dictReplace(related, key, types);
        }
        dictIterator *iter = dictGetIterator(related);
        dictEntry *entry;
        while ((entry = dictNext(iter)) != NULL) {
            sds types = (sds) dictGetVal(entry);
            sds name = (sds) dictGetKey(entry);
            int typeslen = sdslen(types);
            if (typeslen < 2) continue;
            if (types[0] == 'm') score += (10000 * (typeslen - 1));
            else score += (1 * typeslen);
            if (offending == NULL) continue;
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->replicate == NULL) continue;
                if (!strcmp(n->replicate, name) && !strcmp(n->ip, ip)) {
                    *(offending_p++) = n;
                    if (offending_len != NULL) (*offending_len)++;
                    break;
                }
            }
        }
        dictReleaseIterator(iter);
        dictRelease(related);
    }
    return score;
}
static void clusterManagerOptimizeAntiAffinity(clusterManagerNodeArray *ipnodes,
    int ip_count)
{
    clusterManagerNode **offenders = NULL;
    int score = clusterManagerGetAntiAffinityScore(ipnodes, ip_count,
                                                   NULL, NULL);
    if (score == 0) goto cleanup;
    clusterManagerLogInfo(">>> Trying to optimize slaves allocation "
                          "for anti-affinity\n");
    int node_len = cluster_manager.nodes->len;
    int maxiter = 500 * node_len;
    srand(time(NULL));
    while (maxiter > 0) {
        int offending_len = 0;
        if (offenders != NULL) {
            zfree(offenders);
            offenders = NULL;
        }
        score = clusterManagerGetAntiAffinityScore(ipnodes,
                                                   ip_count,
                                                   &offenders,
                                                   &offending_len);
        if (score == 0) break;
        int rand_idx = rand() % offending_len;
        clusterManagerNode *first = offenders[rand_idx],
                           *second = NULL;
        clusterManagerNode **other_replicas = zcalloc((node_len - 1) *
                                                      sizeof(*other_replicas));
        int other_replicas_count = 0;
        listIter li;
        listNode *ln;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n != first && n->replicate != NULL)
                other_replicas[other_replicas_count++] = n;
        }
        if (other_replicas_count == 0) {
            zfree(other_replicas);
            break;
        }
        rand_idx = rand() % other_replicas_count;
        second = other_replicas[rand_idx];
        char *first_master = first->replicate,
             *second_master = second->replicate;
        first->replicate = second_master, first->dirty = 1;
        second->replicate = first_master, second->dirty = 1;
        int new_score = clusterManagerGetAntiAffinityScore(ipnodes,
                                                           ip_count,
                                                           NULL, NULL);
        if (new_score > score) {
            first->replicate = first_master;
            second->replicate = second_master;
        }
        zfree(other_replicas);
        maxiter--;
    }
    score = clusterManagerGetAntiAffinityScore(ipnodes, ip_count, NULL, NULL);
    char *msg;
    int perfect = (score == 0);
    int log_level = (perfect ? CLUSTER_MANAGER_LOG_LVL_SUCCESS :
                               CLUSTER_MANAGER_LOG_LVL_WARN);
    if (perfect) msg = "[OK] Perfect anti-affinity obtained!";
    else if (score >= 10000)
        msg = ("[WARNING] Some slaves are in the same host as their master");
    else
        msg=("[WARNING] Some slaves of the same master are in the same host");
    clusterManagerLog(log_level, "%s\n", msg);
cleanup:
    zfree(offenders);
}
static sds clusterManagerNodeFlagString(clusterManagerNode *node) {
    sds flags = sdsempty();
    if (!node->flags_str) return flags;
    int empty = 1;
    listIter li;
    listNode *ln;
    listRewind(node->flags_str, &li);
    while ((ln = listNext(&li)) != NULL) {
        sds flag = ln->value;
        if (strcmp(flag, "myself") == 0) continue;
        if (!empty) flags = sdscat(flags, ",");
        flags = sdscatfmt(flags, "%S", flag);
        empty = 0;
    }
    return flags;
}
static sds clusterManagerNodeSlotsString(clusterManagerNode *node) {
    sds slots = sdsempty();
    int first_range_idx = -1, last_slot_idx = -1, i;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        int has_slot = node->slots[i];
        if (has_slot) {
            if (first_range_idx == -1) {
                if (sdslen(slots)) slots = sdscat(slots, ",");
                first_range_idx = i;
                slots = sdscatfmt(slots, "[%u", i);
            }
            last_slot_idx = i;
        } else {
            if (last_slot_idx >= 0) {
                if (first_range_idx == last_slot_idx)
                    slots = sdscat(slots, "]");
                else slots = sdscatfmt(slots, "-%u]", last_slot_idx);
            }
            last_slot_idx = -1;
            first_range_idx = -1;
        }
    }
    if (last_slot_idx >= 0) {
        if (first_range_idx == last_slot_idx) slots = sdscat(slots, "]");
        else slots = sdscatfmt(slots, "-%u]", last_slot_idx);
    }
    return slots;
}
static sds clusterManagerNodeGetJSON(clusterManagerNode *node,
                                     unsigned long error_count)
{
    sds json = sdsempty();
    sds replicate = sdsempty();
    if (node->replicate)
        replicate = sdscatprintf(replicate, "\"%s\"", node->replicate);
    else
        replicate = sdscat(replicate, "null");
    sds slots = clusterManagerNodeSlotsString(node);
    sds flags = clusterManagerNodeFlagString(node);
    char *p = slots;
    while ((p = strchr(p, '-')) != NULL)
        *(p++) = ',';
    json = sdscatprintf(json,
        "  {\n"
        "    \"name\": \"%s\",\n"
        "    \"host\": \"%s\",\n"
        "    \"port\": %d,\n"
        "    \"replicate\": %s,\n"
        "    \"slots\": [%s],\n"
        "    \"slots_count\": %d,\n"
        "    \"flags\": \"%s\",\n"
        "    \"current_epoch\": %llu",
        node->name,
        node->ip,
        node->port,
        replicate,
        slots,
        node->slots_count,
        flags,
        (unsigned long long)node->current_epoch
    );
    if (error_count > 0) {
        json = sdscatprintf(json, ",\n    \"cluster_errors\": %lu",
                            error_count);
    }
    if (node->migrating_count > 0 && node->migrating != NULL) {
        int i = 0;
        sds migrating = sdsempty();
        for (; i < node->migrating_count; i += 2) {
            sds slot = node->migrating[i];
            sds dest = node->migrating[i + 1];
            if (slot && dest) {
                if (sdslen(migrating) > 0) migrating = sdscat(migrating, ",");
                migrating = sdscatfmt(migrating, "\"%S\": \"%S\"", slot, dest);
            }
        }
        if (sdslen(migrating) > 0)
            json = sdscatfmt(json, ",\n    \"migrating\": {%S}", migrating);
        sdsfree(migrating);
    }
    if (node->importing_count > 0 && node->importing != NULL) {
        int i = 0;
        sds importing = sdsempty();
        for (; i < node->importing_count; i += 2) {
            sds slot = node->importing[i];
            sds from = node->importing[i + 1];
            if (slot && from) {
                if (sdslen(importing) > 0) importing = sdscat(importing, ",");
                importing = sdscatfmt(importing, "\"%S\": \"%S\"", slot, from);
            }
        }
        if (sdslen(importing) > 0)
            json = sdscatfmt(json, ",\n    \"importing\": {%S}", importing);
        sdsfree(importing);
    }
    json = sdscat(json, "\n  }");
    sdsfree(replicate);
    sdsfree(slots);
    sdsfree(flags);
    return json;
}
static unsigned int clusterManagerKeyHashSlot(char *key, int keylen) {
    int s, e;
    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}
static sds clusterManagerNodeInfo(clusterManagerNode *node, int indent) {
    sds info = sdsempty();
    sds spaces = sdsempty();
    int i;
    for (i = 0; i < indent; i++) spaces = sdscat(spaces, " ");
    if (indent) info = sdscat(info, spaces);
    int is_master = !(node->flags & CLUSTER_MANAGER_FLAG_SLAVE);
    char *role = (is_master ? "M" : "S");
    sds slots = NULL;
    if (node->dirty && node->replicate != NULL)
        info = sdscatfmt(info, "S: %S %s:%u", node->name, node->ip, node->port);
    else {
        slots = clusterManagerNodeSlotsString(node);
        sds flags = clusterManagerNodeFlagString(node);
        info = sdscatfmt(info, "%s: %S %s:%u\n"
                               "%s   slots:%S (%u slots) "
                               "%S",
                               role, node->name, node->ip, node->port, spaces,
                               slots, node->slots_count, flags);
        sdsfree(slots);
        sdsfree(flags);
    }
    if (node->replicate != NULL)
        info = sdscatfmt(info, "\n%s   replicates %S", spaces, node->replicate);
    else if (node->replicas_count)
        info = sdscatfmt(info, "\n%s   %U additional replica(s)",
                         spaces, node->replicas_count);
    sdsfree(spaces);
    return info;
}
static void clusterManagerShowNodes(void) {
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        sds info = clusterManagerNodeInfo(node, 0);
        printf("%s\n", (char *) info);
        sdsfree(info);
    }
}
static void clusterManagerShowClusterInfo(void) {
    int masters = 0;
    int keys = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        if (!(node->flags & CLUSTER_MANAGER_FLAG_SLAVE)) {
            if (!node->name) continue;
            int replicas = 0;
            int dbsize = -1;
            char name[9];
            memcpy(name, node->name, 8);
            name[8] = '\0';
            listIter ri;
            listNode *rn;
            listRewind(cluster_manager.nodes, &ri);
            while ((rn = listNext(&ri)) != NULL) {
                clusterManagerNode *n = rn->value;
                if (n == node || !(n->flags & CLUSTER_MANAGER_FLAG_SLAVE))
                    continue;
                if (n->replicate && !strcmp(n->replicate, node->name))
                    replicas++;
            }
            redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "DBSIZE");
            if (reply != NULL && reply->type == REDIS_REPLY_INTEGER)
                dbsize = reply->integer;
            if (dbsize < 0) {
                char *err = "";
                if (reply != NULL && reply->type == REDIS_REPLY_ERROR)
                    err = reply->str;
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                if (reply != NULL) freeReplyObject(reply);
                return;
            };
            if (reply != NULL) freeReplyObject(reply);
            printf("%s:%d (%s...) -> %d keys | %d slots | %d slaves.\n",
                   node->ip, node->port, name, dbsize,
                   node->slots_count, replicas);
            masters++;
            keys += dbsize;
        }
    }
    clusterManagerLogOk("[OK] %d keys in %d masters.\n", keys, masters);
    float keys_per_slot = keys / (float) CLUSTER_MANAGER_SLOTS;
    printf("%.2f keys per slot on average.\n", keys_per_slot);
}
static int clusterManagerAddSlots(clusterManagerNode *node, char**err)
{
    redisReply *reply = NULL;
    void *_reply = NULL;
    int success = 1;
    int argc = node->slots_count + 2;
    sds *argv = zmalloc(argc * sizeof(*argv));
    size_t *argvlen = zmalloc(argc * sizeof(*argvlen));
    argv[0] = "CLUSTER";
    argv[1] = "ADDSLOTS";
    argvlen[0] = 7;
    argvlen[1] = 8;
    *err = NULL;
    int i, argv_idx = 2;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        if (argv_idx >= argc) break;
        if (node->slots[i]) {
            argv[argv_idx] = sdsfromlonglong((long long) i);
            argvlen[argv_idx] = sdslen(argv[argv_idx]);
            argv_idx++;
        }
    }
    if (!argv_idx) {
        success = 0;
        goto cleanup;
    }
    redisAppendCommandArgv(node->context,argc,(const char**)argv,argvlen);
    if (redisGetReply(node->context, &_reply) != REDIS_OK) {
        success = 0;
        goto cleanup;
    }
    reply = (redisReply*) _reply;
    success = clusterManagerCheckRedisReply(node, reply, err);
cleanup:
    zfree(argvlen);
    if (argv != NULL) {
        for (i = 2; i < argc; i++) sdsfree(argv[i]);
        zfree(argv);
    }
    if (reply != NULL) freeReplyObject(reply);
    return success;
}
static clusterManagerNode *clusterManagerGetSlotOwner(clusterManagerNode *n,
                                                      int slot, char **err)
{
    assert(slot >= 0 && slot < CLUSTER_MANAGER_SLOTS);
    clusterManagerNode *owner = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(n, "CLUSTER SLOTS");
    if (clusterManagerCheckRedisReply(n, reply, err)) {
        assert(reply->type == REDIS_REPLY_ARRAY);
        size_t i;
        for (i = 0; i < reply->elements; i++) {
            redisReply *r = reply->element[i];
            assert(r->type == REDIS_REPLY_ARRAY && r->elements >= 3);
            int from, to;
            from = r->element[0]->integer;
            to = r->element[1]->integer;
            if (slot < from || slot > to) continue;
            redisReply *nr = r->element[2];
            assert(nr->type == REDIS_REPLY_ARRAY && nr->elements >= 2);
            char *name = NULL;
            if (nr->elements >= 3)
                name = nr->element[2]->str;
            if (name != NULL)
                owner = clusterManagerNodeByName(name);
            else {
                char *ip = nr->element[0]->str;
                assert(ip != NULL);
                int port = (int) nr->element[1]->integer;
                listIter li;
                listNode *ln;
                listRewind(cluster_manager.nodes, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerNode *nd = ln->value;
                    if (strcmp(nd->ip, ip) == 0 && port == nd->port) {
                        owner = nd;
                        break;
                    }
                }
            }
            if (owner) break;
        }
    }
    if (reply) freeReplyObject(reply);
    return owner;
}
static int clusterManagerSetSlot(clusterManagerNode *node1,
                                 clusterManagerNode *node2,
                                 int slot, const char *status, char **err) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node1, "CLUSTER "
                                                "SETSLOT %d %s %s",
                                                slot, status,
                                                (char *) node2->name);
    if (err != NULL) *err = NULL;
    if (!reply) return 0;
    int success = 1;
    if (reply->type == REDIS_REPLY_ERROR) {
        success = 0;
        if (err != NULL) {
            *err = zmalloc((reply->len + 1) * sizeof(char));
            strcpy(*err, reply->str);
        } else CLUSTER_MANAGER_PRINT_REPLY_ERROR(node1, reply->str);
        goto cleanup;
    }
cleanup:
    freeReplyObject(reply);
    return success;
}
static int clusterManagerClearSlotStatus(clusterManagerNode *node, int slot) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER SETSLOT %d %s", slot, "STABLE");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerDelSlot(clusterManagerNode *node, int slot,
                                 int ignore_unassigned_err)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER DELSLOTS %d", slot);
    char *err = NULL;
    int success = clusterManagerCheckRedisReply(node, reply, &err);
    if (!success && reply && reply->type == REDIS_REPLY_ERROR &&
        ignore_unassigned_err)
    {
        char *get_owner_err = NULL;
        clusterManagerNode *assigned_to =
            clusterManagerGetSlotOwner(node, slot, &get_owner_err);
        if (!assigned_to) {
            if (get_owner_err == NULL) success = 1;
            else {
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, get_owner_err);
                zfree(get_owner_err);
            }
        }
    }
    if (!success && err != NULL) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
        zfree(err);
    }
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerAddSlot(clusterManagerNode *node, int slot) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER ADDSLOTS %d", slot);
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}
static signed int clusterManagerCountKeysInSlot(clusterManagerNode *node,
                                                int slot)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node,
        "CLUSTER COUNTKEYSINSLOT %d", slot);
    int count = -1;
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (success && reply->type == REDIS_REPLY_INTEGER) count = reply->integer;
    if (reply) freeReplyObject(reply);
    return count;
}
static int clusterManagerBumpEpoch(clusterManagerNode *node) {
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER BUMPEPOCH");
    int success = clusterManagerCheckRedisReply(node, reply, NULL);
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerOnSetOwnerErr(redisReply *reply,
    clusterManagerNode *n, int bulk_idx)
{
    UNUSED(reply);
    UNUSED(n);
    return (bulk_idx != 1);
}
static int clusterManagerSetSlotOwner(clusterManagerNode *owner,
                                      int slot,
                                      int do_clear)
{
    int success = clusterManagerStartTransaction(owner);
    if (!success) return 0;
    clusterManagerDelSlot(owner, slot, 1);
    clusterManagerAddSlot(owner, slot);
    if (do_clear) clusterManagerClearSlotStatus(owner, slot);
    clusterManagerBumpEpoch(owner);
    success = clusterManagerExecTransaction(owner, clusterManagerOnSetOwnerErr);
    return success;
}
static int clusterManagerCompareKeysValues(clusterManagerNode *n1,
                                          clusterManagerNode *n2,
                                          redisReply *keys_reply,
                                          list *diffs)
{
    size_t i, argc = keys_reply->elements + 2;
    static const char *hash_zero = "0000000000000000000000000000000000000000";
    char **argv = zcalloc(argc * sizeof(char *));
    size_t *argv_len = zcalloc(argc * sizeof(size_t));
    argv[0] = "DEBUG";
    argv_len[0] = 5;
    argv[1] = "DIGEST-VALUE";
    argv_len[1] = 12;
    for (i = 0; i < keys_reply->elements; i++) {
        redisReply *entry = keys_reply->element[i];
        int idx = i + 2;
        argv[idx] = entry->str;
        argv_len[idx] = entry->len;
    }
    int success = 0;
    void *_reply1 = NULL, *_reply2 = NULL;
    redisReply *r1 = NULL, *r2 = NULL;
    redisAppendCommandArgv(n1->context,argc, (const char**)argv,argv_len);
    success = (redisGetReply(n1->context, &_reply1) == REDIS_OK);
    if (!success) goto cleanup;
    r1 = (redisReply *) _reply1;
    redisAppendCommandArgv(n2->context,argc, (const char**)argv,argv_len);
    success = (redisGetReply(n2->context, &_reply2) == REDIS_OK);
    if (!success) goto cleanup;
    r2 = (redisReply *) _reply2;
    success = (r1->type != REDIS_REPLY_ERROR && r2->type != REDIS_REPLY_ERROR);
    if (r1->type == REDIS_REPLY_ERROR) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(n1, r1->str);
        success = 0;
    }
    if (r2->type == REDIS_REPLY_ERROR) {
        CLUSTER_MANAGER_PRINT_REPLY_ERROR(n2, r2->str);
        success = 0;
    }
    if (!success) goto cleanup;
    assert(keys_reply->elements == r1->elements &&
           keys_reply->elements == r2->elements);
    for (i = 0; i < keys_reply->elements; i++) {
        char *key = keys_reply->element[i]->str;
        char *hash1 = r1->element[i]->str;
        char *hash2 = r2->element[i]->str;
        if (strcmp(hash1, hash_zero) == 0 || strcmp(hash2, hash_zero) == 0)
            continue;
        if (strcmp(hash1, hash2) != 0) listAddNodeTail(diffs, key);
    }
cleanup:
    if (r1) freeReplyObject(r1);
    if (r2) freeReplyObject(r2);
    zfree(argv);
    zfree(argv_len);
    return success;
}
static redisReply *clusterManagerMigrateKeysInReply(clusterManagerNode *source,
                                                    clusterManagerNode *target,
                                                    redisReply *reply,
                                                    int replace, int timeout,
                                                    char *dots)
{
    redisReply *migrate_reply = NULL;
    char **argv = NULL;
    size_t *argv_len = NULL;
    int c = (replace ? 8 : 7);
    if (config.auth) c += 2;
    size_t argc = c + reply->elements;
    size_t i, offset = 6;
    argv = zcalloc(argc * sizeof(char *));
    argv_len = zcalloc(argc * sizeof(size_t));
    char portstr[255];
    char timeoutstr[255];
    snprintf(portstr, 10, "%d", target->port);
    snprintf(timeoutstr, 10, "%d", timeout);
    argv[0] = "MIGRATE";
    argv_len[0] = 7;
    argv[1] = target->ip;
    argv_len[1] = strlen(target->ip);
    argv[2] = portstr;
    argv_len[2] = strlen(portstr);
    argv[3] = "";
    argv_len[3] = 0;
    argv[4] = "0";
    argv_len[4] = 1;
    argv[5] = timeoutstr;
    argv_len[5] = strlen(timeoutstr);
    if (replace) {
        argv[offset] = "REPLACE";
        argv_len[offset] = 7;
        offset++;
    }
    if (config.auth) {
        argv[offset] = "AUTH";
        argv_len[offset] = 4;
        offset++;
        argv[offset] = config.auth;
        argv_len[offset] = strlen(config.auth);
        offset++;
    }
    argv[offset] = "KEYS";
    argv_len[offset] = 4;
    offset++;
    for (i = 0; i < reply->elements; i++) {
        redisReply *entry = reply->element[i];
        size_t idx = i + offset;
        assert(entry->type == REDIS_REPLY_STRING);
        argv[idx] = (char *) sdsnewlen(entry->str, entry->len);
        argv_len[idx] = entry->len;
        if (dots) dots[i] = '.';
    }
    if (dots) dots[reply->elements] = '\0';
    void *_reply = NULL;
    redisAppendCommandArgv(source->context,argc,
                           (const char**)argv,argv_len);
    int success = (redisGetReply(source->context, &_reply) == REDIS_OK);
    for (i = 0; i < reply->elements; i++) sdsfree(argv[i + offset]);
    if (!success) goto cleanup;
    migrate_reply = (redisReply *) _reply;
cleanup:
    zfree(argv);
    zfree(argv_len);
    return migrate_reply;
}
static int clusterManagerMigrateKeysInSlot(clusterManagerNode *source,
                                           clusterManagerNode *target,
                                           int slot, int timeout,
                                           int pipeline, int verbose,
                                           char **err)
{
    int success = 1;
    int do_fix = config.cluster_manager_command.flags &
                 CLUSTER_MANAGER_CMD_FLAG_FIX;
    int do_replace = config.cluster_manager_command.flags &
                     CLUSTER_MANAGER_CMD_FLAG_REPLACE;
    while (1) {
        char *dots = NULL;
        redisReply *reply = NULL, *migrate_reply = NULL;
        reply = CLUSTER_MANAGER_COMMAND(source, "CLUSTER "
                                        "GETKEYSINSLOT %d %d", slot,
                                        pipeline);
        success = (reply != NULL);
        if (!success) return 0;
        if (reply->type == REDIS_REPLY_ERROR) {
            success = 0;
            if (err != NULL) {
                *err = zmalloc((reply->len + 1) * sizeof(char));
                strcpy(*err, reply->str);
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(source, *err);
            }
            goto next;
        }
        assert(reply->type == REDIS_REPLY_ARRAY);
        size_t count = reply->elements;
        if (count == 0) {
            freeReplyObject(reply);
            break;
        }
        if (verbose) dots = zmalloc((count+1) * sizeof(char));
        migrate_reply = clusterManagerMigrateKeysInReply(source, target,
                                                         reply, 0, timeout,
                                                         dots);
        if (migrate_reply == NULL) goto next;
        if (migrate_reply->type == REDIS_REPLY_ERROR) {
            int is_busy = strstr(migrate_reply->str, "BUSYKEY") != NULL;
            int not_served = 0;
            if (!is_busy) {
                char *get_owner_err = NULL;
                clusterManagerNode *served_by =
                    clusterManagerGetSlotOwner(source, slot, &get_owner_err);
                if (!served_by) {
                    if (get_owner_err == NULL) not_served = 1;
                    else {
                        CLUSTER_MANAGER_PRINT_REPLY_ERROR(source,
                                                          get_owner_err);
                        zfree(get_owner_err);
                    }
                }
            }
            if (is_busy || not_served) {
                if (do_fix && not_served) {
                    clusterManagerLogWarn("*** Slot was not served, setting "
                                          "owner to node %s:%d.\n",
                                          target->ip, target->port);
                    clusterManagerSetSlot(source, target, slot, "node", NULL);
                }
                if (is_busy) {
                    clusterManagerLogWarn("\n*** Target key exists\n");
                    if (!do_replace) {
                        clusterManagerLogWarn("*** Checking key values on "
                                              "both nodes...\n");
                        list *diffs = listCreate();
                        success = clusterManagerCompareKeysValues(source,
                            target, reply, diffs);
                        if (!success) {
                            clusterManagerLogErr("*** Value check failed!\n");
                            listRelease(diffs);
                            goto next;
                        }
                        if (listLength(diffs) > 0) {
                            success = 0;
                            clusterManagerLogErr(
                                "*** Found %d key(s) in both source node and "
                                "target node having different values.\n"
                                "    Source node: %s:%d\n"
                                "    Target node: %s:%d\n"
                                "    Keys(s):\n",
                                listLength(diffs),
                                source->ip, source->port,
                                target->ip, target->port);
                            listIter dli;
                            listNode *dln;
                            listRewind(diffs, &dli);
                            while((dln = listNext(&dli)) != NULL) {
                                char *k = dln->value;
                                clusterManagerLogErr("    - %s\n", k);
                            }
                            clusterManagerLogErr("Please fix the above key(s) "
                                                 "manually and try again "
                                                 "or relaunch the command \n"
                                                 "with --cluster-replace "
                                                 "option to force key "
                                                 "overriding.\n");
                            listRelease(diffs);
                            goto next;
                        }
                        listRelease(diffs);
                    }
                    clusterManagerLogWarn("*** Replacing target keys...\n");
                }
                freeReplyObject(migrate_reply);
                migrate_reply = clusterManagerMigrateKeysInReply(source,
                                                                 target,
                                                                 reply,
                                                                 is_busy,
                                                                 timeout,
                                                                 NULL);
                success = (migrate_reply != NULL &&
                           migrate_reply->type != REDIS_REPLY_ERROR);
            } else success = 0;
            if (!success) {
                if (migrate_reply != NULL) {
                    if (err) {
                        *err = zmalloc((migrate_reply->len + 1) * sizeof(char));
                        strcpy(*err, migrate_reply->str);
                    }
                    printf("\n");
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(source,
                                                      migrate_reply->str);
                }
                goto next;
            }
        }
        if (verbose) {
            printf("%s", dots);
            fflush(stdout);
        }
next:
        if (reply != NULL) freeReplyObject(reply);
        if (migrate_reply != NULL) freeReplyObject(migrate_reply);
        if (dots) zfree(dots);
        if (!success) break;
    }
    return success;
}
static int clusterManagerMoveSlot(clusterManagerNode *source,
                                  clusterManagerNode *target,
                                  int slot, int opts, char**err)
{
    if (!(opts & CLUSTER_MANAGER_OPT_QUIET)) {
        printf("Moving slot %d from %s:%d to %s:%d: ", slot, source->ip,
               source->port, target->ip, target->port);
        fflush(stdout);
    }
    if (err != NULL) *err = NULL;
    int pipeline = config.cluster_manager_command.pipeline,
        timeout = config.cluster_manager_command.timeout,
        print_dots = (opts & CLUSTER_MANAGER_OPT_VERBOSE),
        option_cold = (opts & CLUSTER_MANAGER_OPT_COLD),
        success = 1;
    if (!option_cold) {
        success = clusterManagerSetSlot(target, source, slot,
                                        "importing", err);
        if (!success) return 0;
        success = clusterManagerSetSlot(source, target, slot,
                                        "migrating", err);
        if (!success) return 0;
    }
    success = clusterManagerMigrateKeysInSlot(source, target, slot, timeout,
                                              pipeline, print_dots, err);
    if (!(opts & CLUSTER_MANAGER_OPT_QUIET)) printf("\n");
    if (!success) return 0;
    if (!option_cold) {
        listIter li;
        listNode *ln;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER "
                                                    "SETSLOT %d %s %s",
                                                    slot, "node",
                                                    target->name);
            success = (r != NULL);
            if (!success) return 0;
            if (r->type == REDIS_REPLY_ERROR) {
                success = 0;
                if (err != NULL) {
                    *err = zmalloc((r->len + 1) * sizeof(char));
                    strcpy(*err, r->str);
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, *err);
                }
            }
            freeReplyObject(r);
            if (!success) return 0;
        }
    }
    if (opts & CLUSTER_MANAGER_OPT_UPDATE) {
        source->slots[slot] = 0;
        target->slots[slot] = 1;
    }
    return 1;
}
static int clusterManagerFlushNodeConfig(clusterManagerNode *node, char **err) {
    if (!node->dirty) return 0;
    redisReply *reply = NULL;
    int is_err = 0, success = 1;
    if (err != NULL) *err = NULL;
    if (node->replicate != NULL) {
        reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER REPLICATE %s",
                                        node->replicate);
        if (reply == NULL || (is_err = (reply->type == REDIS_REPLY_ERROR))) {
            if (is_err && err != NULL) {
                *err = zmalloc((reply->len + 1) * sizeof(char));
                strcpy(*err, reply->str);
            }
            success = 0;
            goto cleanup;
        }
    } else {
        int added = clusterManagerAddSlots(node, err);
        if (!added || *err != NULL) success = 0;
    }
    node->dirty = 0;
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    return success;
}
static void clusterManagerWaitForClusterJoin(void) {
    printf("Waiting for the cluster to join\n");
    int counter = 0,
        check_after = CLUSTER_JOIN_CHECK_AFTER +
                      (int)(listLength(cluster_manager.nodes) * 0.15f);
    while(!clusterManagerIsConfigConsistent()) {
        printf(".");
        fflush(stdout);
        sleep(1);
        if (++counter > check_after) {
            dict *status = clusterManagerGetLinkStatus();
            dictIterator *iter = NULL;
            if (status != NULL && dictSize(status) > 0) {
                printf("\n");
                clusterManagerLogErr("Warning: %d node(s) may "
                                     "be unreachable\n", dictSize(status));
                iter = dictGetIterator(status);
                dictEntry *entry;
                while ((entry = dictNext(iter)) != NULL) {
                    sds nodeaddr = (sds) dictGetKey(entry);
                    char *node_ip = NULL;
                    int node_port = 0, node_bus_port = 0;
                    list *from = (list *) dictGetVal(entry);
                    if (parseClusterNodeAddress(nodeaddr, &node_ip,
                        &node_port, &node_bus_port) && node_bus_port) {
                        clusterManagerLogErr(" - The port %d of node %s may "
                                             "be unreachable from:\n",
                                             node_bus_port, node_ip);
                    } else {
                        clusterManagerLogErr(" - Node %s may be unreachable "
                                             "from:\n", nodeaddr);
                    }
                    listIter li;
                    listNode *ln;
                    listRewind(from, &li);
                    while ((ln = listNext(&li)) != NULL) {
                        sds from_addr = ln->value;
                        clusterManagerLogErr("   %s\n", from_addr);
                        sdsfree(from_addr);
                    }
                    clusterManagerLogErr("Cluster bus ports must be reachable "
                                         "by every node.\nRemember that "
                                         "cluster bus ports are different "
                                         "from standard instance ports.\n");
                    listEmpty(from);
                }
            }
            if (iter != NULL) dictReleaseIterator(iter);
            if (status != NULL) dictRelease(status);
            counter = 0;
        }
    }
    printf("\n");
}
static int clusterManagerNodeLoadInfo(clusterManagerNode *node, int opts,
                                      char **err)
{
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    int success = 1;
    *err = NULL;
    if (!clusterManagerCheckRedisReply(node, reply, err)) {
        success = 0;
        goto cleanup;
    }
    int getfriends = (opts & CLUSTER_MANAGER_OPT_GETFRIENDS);
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *name = NULL, *addr = NULL, *flags = NULL, *master_id = NULL,
             *ping_sent = NULL, *ping_recv = NULL, *config_epoch = NULL,
             *link_status = NULL;
        UNUSED(link_status);
        int i = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            switch(i++){
            case 0: name = token; break;
            case 1: addr = token; break;
            case 2: flags = token; break;
            case 3: master_id = token; break;
            case 4: ping_sent = token; break;
            case 5: ping_recv = token; break;
            case 6: config_epoch = token; break;
            case 7: link_status = token; break;
            }
            if (i == 8) break;
        }
        if (!flags) {
            success = 0;
            goto cleanup;
        }
        int myself = (strstr(flags, "myself") != NULL);
        clusterManagerNode *currentNode = NULL;
        if (myself) {
            node->flags |= CLUSTER_MANAGER_FLAG_MYSELF;
            currentNode = node;
            clusterManagerNodeResetSlots(node);
            if (i == 8) {
                int remaining = strlen(line);
                while (remaining > 0) {
                    p = strchr(line, ' ');
                    if (p == NULL) p = line + remaining;
                    remaining -= (p - line);
                    char *slotsdef = line;
                    *p = '\0';
                    if (remaining) {
                        line = p + 1;
                        remaining--;
                    } else line = p;
                    char *dash = NULL;
                    if (slotsdef[0] == '[') {
                        slotsdef++;
                        if ((p = strstr(slotsdef, "->-"))) {
                            *p = '\0';
                            p += 3;
                            char *closing_bracket = strchr(p, ']');
                            if (closing_bracket) *closing_bracket = '\0';
                            sds slot = sdsnew(slotsdef);
                            sds dst = sdsnew(p);
                            node->migrating_count += 2;
                            node->migrating = zrealloc(node->migrating,
                                (node->migrating_count * sizeof(sds)));
                            node->migrating[node->migrating_count - 2] =
                                slot;
                            node->migrating[node->migrating_count - 1] =
                                dst;
                        } else if ((p = strstr(slotsdef, "-<-"))) {
                            *p = '\0';
                            p += 3;
                            char *closing_bracket = strchr(p, ']');
                            if (closing_bracket) *closing_bracket = '\0';
                            sds slot = sdsnew(slotsdef);
                            sds src = sdsnew(p);
                            node->importing_count += 2;
                            node->importing = zrealloc(node->importing,
                                (node->importing_count * sizeof(sds)));
                            node->importing[node->importing_count - 2] =
                                slot;
                            node->importing[node->importing_count - 1] =
                                src;
                        }
                    } else if ((dash = strchr(slotsdef, '-')) != NULL) {
                        p = dash;
                        int start, stop;
                        *p = '\0';
                        start = atoi(slotsdef);
                        stop = atoi(p + 1);
                        node->slots_count += (stop - (start - 1));
                        while (start <= stop) node->slots[start++] = 1;
                    } else if (p > slotsdef) {
                        node->slots[atoi(slotsdef)] = 1;
                        node->slots_count++;
                    }
                }
            }
            node->dirty = 0;
        } else if (!getfriends) {
            if (!(node->flags & CLUSTER_MANAGER_FLAG_MYSELF)) continue;
            else break;
        } else {
            if (addr == NULL) {
                fprintf(stderr, "Error: invalid CLUSTER NODES reply\n");
                success = 0;
                goto cleanup;
            }
            char *c = strrchr(addr, '@');
            if (c != NULL) *c = '\0';
            c = strrchr(addr, ':');
            if (c == NULL) {
                fprintf(stderr, "Error: invalid CLUSTER NODES reply\n");
                success = 0;
                goto cleanup;
            }
            *c = '\0';
            int port = atoi(++c);
            currentNode = clusterManagerNewNode(sdsnew(addr), port);
            currentNode->flags |= CLUSTER_MANAGER_FLAG_FRIEND;
            if (node->friends == NULL) node->friends = listCreate();
            listAddNodeTail(node->friends, currentNode);
        }
        if (name != NULL) {
            if (currentNode->name) sdsfree(currentNode->name);
            currentNode->name = sdsnew(name);
        }
        if (currentNode->flags_str != NULL)
            freeClusterManagerNodeFlags(currentNode->flags_str);
        currentNode->flags_str = listCreate();
        int flag_len;
        while ((flag_len = strlen(flags)) > 0) {
            sds flag = NULL;
            char *fp = strchr(flags, ',');
            if (fp) {
                *fp = '\0';
                flag = sdsnew(flags);
                flags = fp + 1;
            } else {
                flag = sdsnew(flags);
                flags += flag_len;
            }
            if (strcmp(flag, "noaddr") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_NOADDR;
            else if (strcmp(flag, "disconnected") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_DISCONNECT;
            else if (strcmp(flag, "fail") == 0)
                currentNode->flags |= CLUSTER_MANAGER_FLAG_FAIL;
            else if (strcmp(flag, "slave") == 0) {
                currentNode->flags |= CLUSTER_MANAGER_FLAG_SLAVE;
                if (master_id != NULL) {
                    if (currentNode->replicate) sdsfree(currentNode->replicate);
                    currentNode->replicate = sdsnew(master_id);
                }
            }
            listAddNodeTail(currentNode->flags_str, flag);
        }
        if (config_epoch != NULL)
            currentNode->current_epoch = atoll(config_epoch);
        if (ping_sent != NULL) currentNode->ping_sent = atoll(ping_sent);
        if (ping_recv != NULL) currentNode->ping_recv = atoll(ping_recv);
        if (!getfriends && myself) break;
    }
cleanup:
    if (reply) freeReplyObject(reply);
    return success;
}
static int clusterManagerLoadInfoFromNode(clusterManagerNode *node, int opts) {
    if (node->context == NULL && !clusterManagerNodeConnect(node)) {
        freeClusterManagerNode(node);
        return 0;
    }
    opts |= CLUSTER_MANAGER_OPT_GETFRIENDS;
    char *e = NULL;
    if (!clusterManagerNodeIsCluster(node, &e)) {
        clusterManagerPrintNotClusterNodeError(node, e);
        if (e) zfree(e);
        freeClusterManagerNode(node);
        return 0;
    }
    e = NULL;
    if (!clusterManagerNodeLoadInfo(node, opts, &e)) {
        if (e) {
            CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, e);
            zfree(e);
        }
        freeClusterManagerNode(node);
        return 0;
    }
    listIter li;
    listNode *ln;
    if (cluster_manager.nodes != NULL) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL)
            freeClusterManagerNode((clusterManagerNode *) ln->value);
        listRelease(cluster_manager.nodes);
    }
    cluster_manager.nodes = listCreate();
    listAddNodeTail(cluster_manager.nodes, node);
    if (node->friends != NULL) {
        listRewind(node->friends, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *friend = ln->value;
            if (!friend->ip || !friend->port) goto invalid_friend;
            if (!friend->context && !clusterManagerNodeConnect(friend))
                goto invalid_friend;
            e = NULL;
            if (clusterManagerNodeLoadInfo(friend, 0, &e)) {
                if (friend->flags & (CLUSTER_MANAGER_FLAG_NOADDR |
                                     CLUSTER_MANAGER_FLAG_DISCONNECT |
                                     CLUSTER_MANAGER_FLAG_FAIL))
                    goto invalid_friend;
                listAddNodeTail(cluster_manager.nodes, friend);
            } else {
                clusterManagerLogErr("[ERR] Unable to load info for "
                                     "node %s:%d\n",
                                     friend->ip, friend->port);
                goto invalid_friend;
            }
            continue;
invalid_friend:
            freeClusterManagerNode(friend);
        }
        listRelease(node->friends);
        node->friends = NULL;
    }
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->replicate != NULL) {
            clusterManagerNode *master = clusterManagerNodeByName(n->replicate);
            if (master == NULL) {
                clusterManagerLogWarn("*** WARNING: %s:%d claims to be "
                                      "slave of unknown node ID %s.\n",
                                      n->ip, n->port, n->replicate);
            } else master->replicas_count++;
        }
    }
    return 1;
}
int clusterManagerSlotCompare(const void *slot1, const void *slot2) {
    const char **i1 = (const char **)slot1;
    const char **i2 = (const char **)slot2;
    return strcmp(*i1, *i2);
}
int clusterManagerSlotCountCompareDesc(const void *n1, const void *n2) {
    clusterManagerNode *node1 = *((clusterManagerNode **) n1);
    clusterManagerNode *node2 = *((clusterManagerNode **) n2);
    return node2->slots_count - node1->slots_count;
}
int clusterManagerCompareNodeBalance(const void *n1, const void *n2) {
    clusterManagerNode *node1 = *((clusterManagerNode **) n1);
    clusterManagerNode *node2 = *((clusterManagerNode **) n2);
    return node1->balance - node2->balance;
}
static sds clusterManagerGetConfigSignature(clusterManagerNode *node) {
    sds signature = NULL;
    int node_count = 0, i = 0, name_len = 0;
    char **node_configs = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    if (reply == NULL || reply->type == REDIS_REPLY_ERROR)
        goto cleanup;
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        i = 0;
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *nodename = NULL;
        int tot_size = 0;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            if (i == 0) {
                nodename = token;
                tot_size = (p - token);
                name_len = tot_size++;
            }
            if (++i == 8) break;
        }
        if (i != 8) continue;
        if (nodename == NULL) continue;
        int remaining = strlen(line);
        if (remaining == 0) continue;
        char **slots = NULL;
        int c = 0;
        while (remaining > 0) {
            p = strchr(line, ' ');
            if (p == NULL) p = line + remaining;
            int size = (p - line);
            remaining -= size;
            tot_size += size;
            char *slotsdef = line;
            *p = '\0';
            if (remaining) {
                line = p + 1;
                remaining--;
            } else line = p;
            if (slotsdef[0] != '[') {
                c++;
                slots = zrealloc(slots, (c * sizeof(char *)));
                slots[c - 1] = slotsdef;
            }
        }
        if (c > 0) {
            if (c > 1)
                qsort(slots, c, sizeof(char *), clusterManagerSlotCompare);
            node_count++;
            node_configs =
                zrealloc(node_configs, (node_count * sizeof(char *)));
            tot_size += (sizeof(char) * (c - 1));
            char *cfg = zmalloc((sizeof(char) * tot_size) + 1);
            memcpy(cfg, nodename, name_len);
            char *sp = cfg + name_len;
            *(sp++) = ':';
            for (i = 0; i < c; i++) {
                if (i > 0) *(sp++) = ',';
                int slen = strlen(slots[i]);
                memcpy(sp, slots[i], slen);
                sp += slen;
            }
            *(sp++) = '\0';
            node_configs[node_count - 1] = cfg;
        }
        zfree(slots);
    }
    if (node_count > 0) {
        if (node_count > 1) {
            qsort(node_configs, node_count, sizeof(char *),
                  clusterManagerSlotCompare);
        }
        signature = sdsempty();
        for (i = 0; i < node_count; i++) {
            if (i > 0) signature = sdscatprintf(signature, "%c", '|');
            signature = sdscatfmt(signature, "%s", node_configs[i]);
        }
    }
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    if (node_configs != NULL) {
        for (i = 0; i < node_count; i++) zfree(node_configs[i]);
        zfree(node_configs);
    }
    return signature;
}
static int clusterManagerIsConfigConsistent(void) {
    if (cluster_manager.nodes == NULL) return 0;
    int consistent = (listLength(cluster_manager.nodes) <= 1);
    if (consistent) return 1;
    sds first_cfg = NULL;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        sds cfg = clusterManagerGetConfigSignature(node);
        if (cfg == NULL) {
            consistent = 0;
            break;
        }
        if (first_cfg == NULL) first_cfg = cfg;
        else {
            consistent = !sdscmp(first_cfg, cfg);
            sdsfree(cfg);
            if (!consistent) break;
        }
    }
    if (first_cfg != NULL) sdsfree(first_cfg);
    return consistent;
}
static list *clusterManagerGetDisconnectedLinks(clusterManagerNode *node) {
    list *links = NULL;
    redisReply *reply = CLUSTER_MANAGER_COMMAND(node, "CLUSTER NODES");
    if (!clusterManagerCheckRedisReply(node, reply, NULL)) goto cleanup;
    links = listCreate();
    char *lines = reply->str, *p, *line;
    while ((p = strstr(lines, "\n")) != NULL) {
        int i = 0;
        *p = '\0';
        line = lines;
        lines = p + 1;
        char *nodename = NULL, *addr = NULL, *flags = NULL, *link_status = NULL;
        while ((p = strchr(line, ' ')) != NULL) {
            *p = '\0';
            char *token = line;
            line = p + 1;
            if (i == 0) nodename = token;
            else if (i == 1) addr = token;
            else if (i == 2) flags = token;
            else if (i == 7) link_status = token;
            else if (i == 8) break;
            i++;
        }
        if (i == 7) link_status = line;
        if (nodename == NULL || addr == NULL || flags == NULL ||
            link_status == NULL) continue;
        if (strstr(flags, "myself") != NULL) continue;
        int disconnected = ((strstr(flags, "disconnected") != NULL) ||
                            (strstr(link_status, "disconnected")));
        int handshaking = (strstr(flags, "handshake") != NULL);
        if (disconnected || handshaking) {
            clusterManagerLink *link = zmalloc(sizeof(*link));
            link->node_name = sdsnew(nodename);
            link->node_addr = sdsnew(addr);
            link->connected = 0;
            link->handshaking = handshaking;
            listAddNodeTail(links, link);
        }
    }
cleanup:
    if (reply != NULL) freeReplyObject(reply);
    return links;
}
static dict *clusterManagerGetLinkStatus(void) {
    if (cluster_manager.nodes == NULL) return NULL;
    dict *status = dictCreate(&clusterManagerLinkDictType, NULL);
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        list *links = clusterManagerGetDisconnectedLinks(node);
        if (links) {
            listIter lli;
            listNode *lln;
            listRewind(links, &lli);
            while ((lln = listNext(&lli)) != NULL) {
                clusterManagerLink *link = lln->value;
                list *from = NULL;
                dictEntry *entry = dictFind(status, link->node_addr);
                if (entry) from = dictGetVal(entry);
                else {
                    from = listCreate();
                    dictAdd(status, sdsdup(link->node_addr), from);
                }
                sds myaddr = sdsempty();
                myaddr = sdscatfmt(myaddr, "%s:%u", node->ip, node->port);
                listAddNodeTail(from, myaddr);
                sdsfree(link->node_name);
                sdsfree(link->node_addr);
                zfree(link);
            }
            listRelease(links);
        }
    }
    return status;
}
static void clusterManagerOnError(sds err) {
    if (cluster_manager.errors == NULL)
        cluster_manager.errors = listCreate();
    listAddNodeTail(cluster_manager.errors, err);
    clusterManagerLogErr("%s\n", (char *) err);
}
static int clusterManagerGetCoveredSlots(char *all_slots) {
    if (cluster_manager.nodes == NULL) return 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    int totslots = 0, i;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
            if (node->slots[i] && !all_slots[i]) {
                all_slots[i] = 1;
                totslots++;
            }
        }
    }
    return totslots;
}
static void clusterManagerPrintSlotsList(list *slots) {
    listIter li;
    listNode *ln;
    listRewind(slots, &li);
    sds first = NULL;
    while ((ln = listNext(&li)) != NULL) {
        sds slot = ln->value;
        if (!first) first = slot;
        else printf(", ");
        printf("%s", slot);
    }
    printf("\n");
}
static clusterManagerNode * clusterManagerGetNodeWithMostKeysInSlot(list *nodes,
                                                                    int slot,
                                                                    char **err)
{
    clusterManagerNode *node = NULL;
    int numkeys = 0;
    listIter li;
    listNode *ln;
    listRewind(nodes, &li);
    if (err) *err = NULL;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
            continue;
        redisReply *r =
            CLUSTER_MANAGER_COMMAND(n, "CLUSTER COUNTKEYSINSLOT %d", slot);
        int success = clusterManagerCheckRedisReply(n, r, err);
        if (success) {
            if (r->integer > numkeys || node == NULL) {
                numkeys = r->integer;
                node = n;
            }
        }
        if (r != NULL) freeReplyObject(r);
        if (!success) {
            if (err != NULL && *err != NULL)
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(n, err);
            node = NULL;
            break;
        }
    }
    return node;
}
static clusterManagerNode *clusterManagerNodeWithLeastReplicas() {
    clusterManagerNode *node = NULL;
    int lowest_count = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (node == NULL || n->replicas_count < lowest_count) {
            node = n;
            lowest_count = n->replicas_count;
        }
    }
    return node;
}
static clusterManagerNode *clusterManagerNodeMasterRandom() {
    int master_count = 0;
    int idx;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        master_count++;
    }
    srand(time(NULL));
    idx = rand() % master_count;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (!idx--) {
            return n;
        }
    }
    return NULL;
}
static int clusterManagerFixSlotsCoverage(char *all_slots) {
    int i, fixed = 0;
    list *none = NULL, *single = NULL, *multi = NULL;
    clusterManagerLogInfo(">>> Fixing slots coverage...\n");
    printf("List of not covered slots: \n");
    int uncovered_count = 0;
    sds log = sdsempty();
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        int covered = all_slots[i];
        if (!covered) {
            sds key = sdsfromlonglong((long long) i);
            if (uncovered_count++ > 0) printf(",");
            printf("%s", (char *) key);
            list *slot_nodes = listCreate();
            sds slot_nodes_str = sdsempty();
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
                    continue;
                redisReply *reply = CLUSTER_MANAGER_COMMAND(n,
                    "CLUSTER GETKEYSINSLOT %d %d", i, 1);
                if (!clusterManagerCheckRedisReply(n, reply, NULL)) {
                    fixed = -1;
                    if (reply) freeReplyObject(reply);
                    goto cleanup;
                }
                assert(reply->type == REDIS_REPLY_ARRAY);
                if (reply->elements > 0) {
                    listAddNodeTail(slot_nodes, n);
                    if (listLength(slot_nodes) > 1)
                        slot_nodes_str = sdscat(slot_nodes_str, ", ");
                    slot_nodes_str = sdscatfmt(slot_nodes_str,
                                               "%s:%u", n->ip, n->port);
                }
                freeReplyObject(reply);
            }
            log = sdscatfmt(log, "\nSlot %S has keys in %u nodes: %S",
                            key, listLength(slot_nodes), slot_nodes_str);
            sdsfree(slot_nodes_str);
            dictAdd(clusterManagerUncoveredSlots, key, slot_nodes);
        }
    }
    printf("\n%s\n", log);
    none = listCreate();
    single = listCreate();
    multi = listCreate();
    dictIterator *iter = dictGetIterator(clusterManagerUncoveredSlots);
    dictEntry *entry;
    while ((entry = dictNext(iter)) != NULL) {
        sds slot = (sds) dictGetKey(entry);
        list *nodes = (list *) dictGetVal(entry);
        switch (listLength(nodes)){
        case 0: listAddNodeTail(none, slot); break;
        case 1: listAddNodeTail(single, slot); break;
        default: listAddNodeTail(multi, slot); break;
        }
    }
    dictReleaseIterator(iter);
    if (listLength(none) > 0) {
        printf("The following uncovered slots have no keys "
               "across the cluster:\n");
        clusterManagerPrintSlotsList(none);
        if (confirmWithYes("Fix these slots by covering with a random node?")){
            listIter li;
            listNode *ln;
            listRewind(none, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                int s = atoi(slot);
                clusterManagerNode *n = clusterManagerNodeMasterRandom();
                clusterManagerLogInfo(">>> Covering slot %s with %s:%d\n",
                                      slot, n->ip, n->port);
                if (!clusterManagerSetSlotOwner(n, s, 0)) {
                    fixed = -1;
                    goto cleanup;
                }
                n->slots[s] = 1;
                fixed++;
            }
        }
    }
    if (listLength(single) > 0) {
        printf("The following uncovered slots have keys in just one node:\n");
        clusterManagerPrintSlotsList(single);
        if (confirmWithYes("Fix these slots by covering with those nodes?")){
            listIter li;
            listNode *ln;
            listRewind(single, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                int s = atoi(slot);
                dictEntry *entry = dictFind(clusterManagerUncoveredSlots, slot);
                assert(entry != NULL);
                list *nodes = (list *) dictGetVal(entry);
                listNode *fn = listFirst(nodes);
                assert(fn != NULL);
                clusterManagerNode *n = fn->value;
                clusterManagerLogInfo(">>> Covering slot %s with %s:%d\n",
                                      slot, n->ip, n->port);
                if (!clusterManagerSetSlotOwner(n, s, 0)) {
                    fixed = -1;
                    goto cleanup;
                }
                n->slots[atoi(slot)] = 1;
                fixed++;
            }
        }
    }
    if (listLength(multi) > 0) {
        printf("The following uncovered slots have keys in multiple nodes:\n");
        clusterManagerPrintSlotsList(multi);
        if (confirmWithYes("Fix these slots by moving keys "
                           "into a single node?")) {
            listIter li;
            listNode *ln;
            listRewind(multi, &li);
            while ((ln = listNext(&li)) != NULL) {
                sds slot = ln->value;
                dictEntry *entry = dictFind(clusterManagerUncoveredSlots, slot);
                assert(entry != NULL);
                list *nodes = (list *) dictGetVal(entry);
                int s = atoi(slot);
                clusterManagerNode *target =
                    clusterManagerGetNodeWithMostKeysInSlot(nodes, s, NULL);
                if (target == NULL) {
                    fixed = -1;
                    goto cleanup;
                }
                clusterManagerLogInfo(">>> Covering slot %s moving keys "
                                      "to %s:%d\n", slot,
                                      target->ip, target->port);
                if (!clusterManagerSetSlotOwner(target, s, 1)) {
                    fixed = -1;
                    goto cleanup;
                }
                target->slots[atoi(slot)] = 1;
                listIter nli;
                listNode *nln;
                listRewind(nodes, &nli);
                while ((nln = listNext(&nli)) != NULL) {
                    clusterManagerNode *src = nln->value;
                    if (src == target) continue;
                    if (!clusterManagerSetSlot(src, target, s, "NODE", NULL))
                        fixed = -1;
                    if (fixed < 0) goto cleanup;
                    if (!clusterManagerSetSlot(src, target, s,
                                               "IMPORTING", NULL)) fixed = -1;
                    if (fixed < 0) goto cleanup;
                    int opts = CLUSTER_MANAGER_OPT_VERBOSE |
                               CLUSTER_MANAGER_OPT_COLD;
                    if (!clusterManagerMoveSlot(src, target, s, opts, NULL)) {
                        fixed = -1;
                        goto cleanup;
                    }
                    if (!clusterManagerClearSlotStatus(src, s))
                        fixed = -1;
                    if (fixed < 0) goto cleanup;
                }
                fixed++;
            }
        }
    }
cleanup:
    sdsfree(log);
    if (none) listRelease(none);
    if (single) listRelease(single);
    if (multi) listRelease(multi);
    return fixed;
}
static int clusterManagerFixOpenSlot(int slot) {
    clusterManagerLogInfo(">>> Fixing open slot %d\n", slot);
    int success = 1;
    list *owners = listCreate();
    list *migrating = listCreate();
    list *importing = listCreate();
    sds migrating_str = sdsempty();
    sds importing_str = sdsempty();
    clusterManagerNode *owner = NULL;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        if (n->slots[slot]) listAddNodeTail(owners, n);
        else {
            redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                "CLUSTER COUNTKEYSINSLOT %d", slot);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (success && r->integer > 0) {
                clusterManagerLogWarn("*** Found keys about slot %d "
                                      "in non-owner node %s:%d!\n", slot,
                                      n->ip, n->port);
                listAddNodeTail(owners, n);
            }
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        }
    }
    if (listLength(owners) == 1) owner = listFirst(owners)->value;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        int is_migrating = 0, is_importing = 0;
        if (n->migrating) {
            for (int i = 0; i < n->migrating_count; i += 2) {
                sds migrating_slot = n->migrating[i];
                if (atoi(migrating_slot) == slot) {
                    char *sep = (listLength(migrating) == 0 ? "" : ",");
                    migrating_str = sdscatfmt(migrating_str, "%s%s:%u",
                                              sep, n->ip, n->port);
                    listAddNodeTail(migrating, n);
                    is_migrating = 1;
                    break;
                }
            }
        }
        if (!is_migrating && n->importing) {
            for (int i = 0; i < n->importing_count; i += 2) {
                sds importing_slot = n->importing[i];
                if (atoi(importing_slot) == slot) {
                    char *sep = (listLength(importing) == 0 ? "" : ",");
                    importing_str = sdscatfmt(importing_str, "%s%s:%u",
                                              sep, n->ip, n->port);
                    listAddNodeTail(importing, n);
                    is_importing = 1;
                    break;
                }
            }
        }
        if (!is_migrating && !is_importing && n != owner) {
            redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                "CLUSTER COUNTKEYSINSLOT %d", slot);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (success && r->integer > 0) {
                clusterManagerLogWarn("*** Found keys about slot %d "
                                      "in node %s:%d!\n", slot, n->ip,
                                      n->port);
                char *sep = (listLength(importing) == 0 ? "" : ",");
                importing_str = sdscatfmt(importing_str, "%s%S:%u",
                                          sep, n->ip, n->port);
                listAddNodeTail(importing, n);
            }
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        }
    }
    if (sdslen(migrating_str) > 0)
        printf("Set as migrating in: %s\n", migrating_str);
    if (sdslen(importing_str) > 0)
        printf("Set as importing in: %s\n", importing_str);
    if (owner == NULL) {
        clusterManagerLogInfo(">>> Nobody claims ownership, "
                              "selecting an owner...\n");
        owner = clusterManagerGetNodeWithMostKeysInSlot(cluster_manager.nodes,
                                                        slot, NULL);
        if (owner == NULL) {
            clusterManagerLogErr("[ERR] Can't select a slot owner. "
                                 "Impossible to fix.\n");
            success = 0;
            goto cleanup;
        }
        clusterManagerLogWarn("*** Configuring %s:%d as the slot owner\n",
                              owner->ip, owner->port);
        success = clusterManagerClearSlotStatus(owner, slot);
        if (!success) goto cleanup;
        success = clusterManagerSetSlotOwner(owner, slot, 0);
        if (!success) goto cleanup;
        owner->slots[slot] = 1;
        success = clusterManagerBumpEpoch(owner);
        if (!success) goto cleanup;
        clusterManagerRemoveNodeFromList(migrating, owner);
        clusterManagerRemoveNodeFromList(importing, owner);
    }
    if (listLength(owners) > 1) {
        assert(owner != NULL);
        listRewind(owners, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            success = clusterManagerDelSlot(n, slot, 1);
            if (!success) goto cleanup;
            n->slots[slot] = 0;
            success = clusterManagerSetSlot(n, owner, slot, "node", NULL);
            if (!success) goto cleanup;
            success = clusterManagerSetSlot(n, owner, slot, "importing", NULL);
            if (!success) goto cleanup;
            clusterManagerRemoveNodeFromList(importing, n);
            listAddNodeTail(importing, n);
            clusterManagerRemoveNodeFromList(migrating, n);
        }
    }
    int move_opts = CLUSTER_MANAGER_OPT_VERBOSE;
    if (listLength(migrating) == 1 && listLength(importing) == 1) {
        clusterManagerNode *src = listFirst(migrating)->value;
        clusterManagerNode *dst = listFirst(importing)->value;
        clusterManagerLogInfo(">>> Case 1: Moving slot %d from "
                              "%s:%d to %s:%d\n", slot,
                              src->ip, src->port, dst->ip, dst->port);
        move_opts |= CLUSTER_MANAGER_OPT_UPDATE;
        success = clusterManagerMoveSlot(src, dst, slot, move_opts, NULL);
    }
    else if (listLength(migrating) == 0 && listLength(importing) > 0) {
        clusterManagerLogInfo(">>> Case 2: Moving all the %d slot keys to its "
                              "owner %s:%d\n", slot, owner->ip, owner->port);
        move_opts |= CLUSTER_MANAGER_OPT_COLD;
        listRewind(importing, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            success = clusterManagerMoveSlot(n, owner, slot, move_opts, NULL);
            if (!success) goto cleanup;
            clusterManagerLogInfo(">>> Setting %d as STABLE in "
                                  "%s:%d\n", slot, n->ip, n->port);
            success = clusterManagerClearSlotStatus(n, slot);
            if (!success) goto cleanup;
        }
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n == owner) continue;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            success = clusterManagerSetSlot(n, owner, slot, "NODE", NULL);
            if (!success) goto cleanup;
        }
    }
    else if (listLength(migrating) == 1 && listLength(importing) > 1) {
        int try_to_fix = 1;
        clusterManagerNode *src = listFirst(migrating)->value;
        clusterManagerNode *dst = NULL;
        sds target_id = NULL;
        for (int i = 0; i < src->migrating_count; i += 2) {
            sds migrating_slot = src->migrating[i];
            if (atoi(migrating_slot) == slot) {
                target_id = src->migrating[i + 1];
                break;
            }
        }
        assert(target_id != NULL);
        listIter li;
        listNode *ln;
        listRewind(importing, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            int count = clusterManagerCountKeysInSlot(n, slot);
            if (count > 0) {
                try_to_fix = 0;
                break;
            }
            if (strcmp(n->name, target_id) == 0) dst = n;
        }
        if (!try_to_fix) goto unhandled_case;
        if (dst != NULL) {
            clusterManagerLogInfo(">>> Case 3: Moving slot %d from %s:%d to "
                                  "%s:%d and closing it on all the other "
                                  "importing nodes.\n",
                                  slot, src->ip, src->port,
                                  dst->ip, dst->port);
            success = clusterManagerMoveSlot(src, dst, slot, move_opts, NULL);
            if (!success) goto cleanup;
            listRewind(importing, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (dst == n) continue;
                success = clusterManagerClearSlotStatus(n, slot);
                if (!success) goto cleanup;
            }
        } else {
            clusterManagerLogInfo(">>> Case 3: Closing slot %d on both "
                                  "migrating and importing nodes.\n", slot);
            success = clusterManagerClearSlotStatus(src, slot);
            if (!success) goto cleanup;
            listRewind(importing, &li);
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                success = clusterManagerClearSlotStatus(n, slot);
                if (!success) goto cleanup;
            }
        }
    } else {
        int try_to_close_slot = (listLength(importing) == 0 &&
                                 listLength(migrating) == 1);
        if (try_to_close_slot) {
            clusterManagerNode *n = listFirst(migrating)->value;
            if (!owner || owner != n) {
                redisReply *r = CLUSTER_MANAGER_COMMAND(n,
                    "CLUSTER GETKEYSINSLOT %d %d", slot, 10);
                success = clusterManagerCheckRedisReply(n, r, NULL);
                if (r) {
                    if (success) try_to_close_slot = (r->elements == 0);
                    freeReplyObject(r);
                }
                if (!success) goto cleanup;
            }
        }
        if (try_to_close_slot) {
            clusterManagerNode *n = listFirst(migrating)->value;
            clusterManagerLogInfo(">>> Case 4: Closing slot %d on %s:%d\n",
                                  slot, n->ip, n->port);
            redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER SETSLOT %d %s",
                                                    slot, "STABLE");
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (r) freeReplyObject(r);
            if (!success) goto cleanup;
        } else {
unhandled_case:
            success = 0;
            clusterManagerLogErr("[ERR] Sorry, redis-cli can't fix this slot "
                                 "yet (work in progress). Slot is set as "
                                 "migrating in %s, as importing in %s, "
                                 "owner is %s:%d\n", migrating_str,
                                 importing_str, owner->ip, owner->port);
        }
    }
cleanup:
    listRelease(owners);
    listRelease(migrating);
    listRelease(importing);
    sdsfree(migrating_str);
    sdsfree(importing_str);
    return success;
}
static int clusterManagerFixMultipleSlotOwners(int slot, list *owners) {
    clusterManagerLogInfo(">>> Fixing multiple owners for slot %d...\n", slot);
    int success = 0;
    assert(listLength(owners) > 1);
    clusterManagerNode *owner = clusterManagerGetNodeWithMostKeysInSlot(owners,
                                                                        slot,
                                                                        NULL);
    if (!owner) owner = listFirst(owners)->value;
    clusterManagerLogInfo(">>> Setting slot %d owner: %s:%d\n",
                          slot, owner->ip, owner->port);
    if (!clusterManagerSetSlotOwner(owner, slot, 0)) return 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n == owner) continue;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
        int count = clusterManagerCountKeysInSlot(n, slot);
        success = (count >= 0);
        if (!success) break;
        clusterManagerDelSlot(n, slot, 1);
        if (!clusterManagerSetSlot(n, owner, slot, "node", NULL)) return 0;
        if (count > 0) {
            int opts = CLUSTER_MANAGER_OPT_VERBOSE |
                       CLUSTER_MANAGER_OPT_COLD;
            success = clusterManagerMoveSlot(n, owner, slot, opts, NULL);
            if (!success) break;
        }
    }
    return success;
}
static int clusterManagerCheckCluster(int quiet) {
    listNode *ln = listFirst(cluster_manager.nodes);
    if (!ln) return 0;
    clusterManagerNode *node = ln->value;
    clusterManagerLogInfo(">>> Performing Cluster Check (using node %s:%d)\n",
                          node->ip, node->port);
    int result = 1, consistent = 0;
    int do_fix = config.cluster_manager_command.flags &
                 CLUSTER_MANAGER_CMD_FLAG_FIX;
    if (!quiet) clusterManagerShowNodes();
    consistent = clusterManagerIsConfigConsistent();
    if (!consistent) {
        sds err = sdsnew("[ERR] Nodes don't agree about configuration!");
        clusterManagerOnError(err);
        result = 0;
    } else {
        clusterManagerLogOk("[OK] All nodes agree about slots "
                            "configuration.\n");
    }
    clusterManagerLogInfo(">>> Check for open slots...\n");
    listIter li;
    listRewind(cluster_manager.nodes, &li);
    int i;
    dict *open_slots = NULL;
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->migrating != NULL) {
            if (open_slots == NULL)
                open_slots = dictCreate(&clusterManagerDictType, NULL);
            sds errstr = sdsempty();
            errstr = sdscatprintf(errstr,
                                "[WARNING] Node %s:%d has slots in "
                                "migrating state ",
                                n->ip,
                                n->port);
            for (i = 0; i < n->migrating_count; i += 2) {
                sds slot = n->migrating[i];
                dictReplace(open_slots, slot, sdsdup(n->migrating[i + 1]));
                char *fmt = (i > 0 ? ",%S" : "%S");
                errstr = sdscatfmt(errstr, fmt, slot);
            }
            errstr = sdscat(errstr, ".");
            clusterManagerOnError(errstr);
        }
        if (n->importing != NULL) {
            if (open_slots == NULL)
                open_slots = dictCreate(&clusterManagerDictType, NULL);
            sds errstr = sdsempty();
            errstr = sdscatprintf(errstr,
                                "[WARNING] Node %s:%d has slots in "
                                "importing state ",
                                n->ip,
                                n->port);
            for (i = 0; i < n->importing_count; i += 2) {
                sds slot = n->importing[i];
                dictReplace(open_slots, slot, sdsdup(n->importing[i + 1]));
                char *fmt = (i > 0 ? ",%S" : "%S");
                errstr = sdscatfmt(errstr, fmt, slot);
            }
            errstr = sdscat(errstr, ".");
            clusterManagerOnError(errstr);
        }
    }
    if (open_slots != NULL) {
        result = 0;
        dictIterator *iter = dictGetIterator(open_slots);
        dictEntry *entry;
        sds errstr = sdsnew("[WARNING] The following slots are open: ");
        i = 0;
        while ((entry = dictNext(iter)) != NULL) {
            sds slot = (sds) dictGetKey(entry);
            char *fmt = (i++ > 0 ? ",%S" : "%S");
            errstr = sdscatfmt(errstr, fmt, slot);
        }
        clusterManagerLogErr("%s.\n", (char *) errstr);
        sdsfree(errstr);
        if (do_fix) {
            dictReleaseIterator(iter);
            iter = dictGetIterator(open_slots);
            while ((entry = dictNext(iter)) != NULL) {
                sds slot = (sds) dictGetKey(entry);
                result = clusterManagerFixOpenSlot(atoi(slot));
                if (!result) break;
            }
        }
        dictReleaseIterator(iter);
        dictRelease(open_slots);
    }
    clusterManagerLogInfo(">>> Check slots coverage...\n");
    char slots[CLUSTER_MANAGER_SLOTS];
    memset(slots, 0, CLUSTER_MANAGER_SLOTS);
    int coverage = clusterManagerGetCoveredSlots(slots);
    if (coverage == CLUSTER_MANAGER_SLOTS) {
        clusterManagerLogOk("[OK] All %d slots covered.\n",
                            CLUSTER_MANAGER_SLOTS);
    } else {
        sds err = sdsempty();
        err = sdscatprintf(err, "[ERR] Not all %d slots are "
                                "covered by nodes.\n",
                                CLUSTER_MANAGER_SLOTS);
        clusterManagerOnError(err);
        result = 0;
        if (do_fix ) {
            dictType dtype = clusterManagerDictType;
            dtype.keyDestructor = dictSdsDestructor;
            dtype.valDestructor = dictListDestructor;
            clusterManagerUncoveredSlots = dictCreate(&dtype, NULL);
            int fixed = clusterManagerFixSlotsCoverage(slots);
            if (fixed > 0) result = 1;
        }
    }
    int search_multiple_owners = config.cluster_manager_command.flags &
                                 CLUSTER_MANAGER_CMD_FLAG_CHECK_OWNERS;
    if (search_multiple_owners) {
        clusterManagerLogInfo(">>> Check for multiple slot owners...\n");
        int slot = 0, slots_with_multiple_owners = 0;
        for (; slot < CLUSTER_MANAGER_SLOTS; slot++) {
            listIter li;
            listNode *ln;
            listRewind(cluster_manager.nodes, &li);
            list *owners = listCreate();
            while ((ln = listNext(&li)) != NULL) {
                clusterManagerNode *n = ln->value;
                if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
                if (n->slots[slot]) listAddNodeTail(owners, n);
                else {
                    int count = clusterManagerCountKeysInSlot(n, slot);
                    if (count > 0) listAddNodeTail(owners, n);
                }
            }
            if (listLength(owners) > 1) {
                result = 0;
                clusterManagerLogErr("[WARNING] Slot %d has %d owners:\n",
                                     slot, listLength(owners));
                listRewind(owners, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerNode *n = ln->value;
                    clusterManagerLogErr("    %s:%d\n", n->ip, n->port);
                }
                slots_with_multiple_owners++;
                if (do_fix) {
                    result = clusterManagerFixMultipleSlotOwners(slot, owners);
                    if (!result) {
                        clusterManagerLogErr("Failed to fix multiple owners "
                                             "for slot %d\n", slot);
                        listRelease(owners);
                        break;
                    } else slots_with_multiple_owners--;
                }
            }
            listRelease(owners);
        }
        if (slots_with_multiple_owners == 0)
            clusterManagerLogOk("[OK] No multiple owners found.\n");
    }
    return result;
}
static clusterManagerNode *clusterNodeForResharding(char *id,
                                                    clusterManagerNode *target,
                                                    int *raise_err)
{
    clusterManagerNode *node = NULL;
    const char *invalid_node_msg = "*** The specified node (%s) is not known "
                                   "or not a master, please retry.\n";
    node = clusterManagerNodeByName(id);
    *raise_err = 0;
    if (!node || node->flags & CLUSTER_MANAGER_FLAG_SLAVE) {
        clusterManagerLogErr(invalid_node_msg, id);
        *raise_err = 1;
        return NULL;
    } else if (node != NULL && target != NULL) {
        if (!strcmp(node->name, target->name)) {
            clusterManagerLogErr( "*** It is not possible to use "
                                  "the target node as "
                                  "source node.\n");
            return NULL;
        }
    }
    return node;
}
static list *clusterManagerComputeReshardTable(list *sources, int numslots) {
    list *moved = listCreate();
    int src_count = listLength(sources), i = 0, tot_slots = 0, j;
    clusterManagerNode **sorted = zmalloc(src_count * sizeof(*sorted));
    listIter li;
    listNode *ln;
    listRewind(sources, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *node = ln->value;
        tot_slots += node->slots_count;
        sorted[i++] = node;
    }
    qsort(sorted, src_count, sizeof(clusterManagerNode *),
          clusterManagerSlotCountCompareDesc);
    for (i = 0; i < src_count; i++) {
        clusterManagerNode *node = sorted[i];
        float n = ((float) numslots / tot_slots * node->slots_count);
        if (i == 0) n = ceil(n);
        else n = floor(n);
        int max = (int) n, count = 0;
        for (j = 0; j < CLUSTER_MANAGER_SLOTS; j++) {
            int slot = node->slots[j];
            if (!slot) continue;
            if (count >= max || (int)listLength(moved) >= numslots) break;
            clusterManagerReshardTableItem *item = zmalloc(sizeof(*item));
            item->source = node;
            item->slot = j;
            listAddNodeTail(moved, item);
            count++;
        }
    }
    zfree(sorted);
    return moved;
}
static void clusterManagerShowReshardTable(list *table) {
    listIter li;
    listNode *ln;
    listRewind(table, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerReshardTableItem *item = ln->value;
        clusterManagerNode *n = item->source;
        printf("    Moving slot %d from %s\n", item->slot, (char *) n->name);
    }
}
static void clusterManagerReleaseReshardTable(list *table) {
    if (table != NULL) {
        listIter li;
        listNode *ln;
        listRewind(table, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerReshardTableItem *item = ln->value;
            zfree(item);
        }
        listRelease(table);
    }
}
static void clusterManagerLog(int level, const char* fmt, ...) {
    int use_colors =
        (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_COLOR);
    if (use_colors) {
        printf("\033[");
        switch (level) {
        case CLUSTER_MANAGER_LOG_LVL_INFO: printf(LOG_COLOR_BOLD); break;
        case CLUSTER_MANAGER_LOG_LVL_WARN: printf(LOG_COLOR_YELLOW); break;
        case CLUSTER_MANAGER_LOG_LVL_ERR: printf(LOG_COLOR_RED); break;
        case CLUSTER_MANAGER_LOG_LVL_SUCCESS: printf(LOG_COLOR_GREEN); break;
        default: printf(LOG_COLOR_RESET); break;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (use_colors) printf("\033[" LOG_COLOR_RESET);
}
static void clusterManagerNodeArrayInit(clusterManagerNodeArray *array,
                                        int alloc_len)
{
    array->nodes = zcalloc(alloc_len * sizeof(clusterManagerNode*));
    array->alloc = array->nodes;
    array->len = alloc_len;
    array->count = 0;
}
static void clusterManagerNodeArrayReset(clusterManagerNodeArray *array) {
    if (array->nodes > array->alloc) {
        array->len = array->nodes - array->alloc;
        array->nodes = array->alloc;
        array->count = 0;
        int i = 0;
        for(; i < array->len; i++) {
            if (array->nodes[i] != NULL) array->count++;
        }
    }
}
static void clusterManagerNodeArrayShift(clusterManagerNodeArray *array,
                                         clusterManagerNode **nodeptr)
{
    assert(array->nodes < (array->nodes + array->len));
    if (*array->nodes != NULL) array->count--;
    *nodeptr = *array->nodes;
    array->nodes++;
    array->len--;
}
static void clusterManagerNodeArrayAdd(clusterManagerNodeArray *array,
                                       clusterManagerNode *node)
{
    assert(array->nodes < (array->nodes + array->len));
    assert(node != NULL);
    assert(array->count < array->len);
    array->nodes[array->count++] = node;
}
static void clusterManagerPrintNotEmptyNodeError(clusterManagerNode *node,
                                                 char *err)
{
    char *msg;
    if (err) msg = err;
    else {
        msg = "is not empty. Either the node already knows other "
              "nodes (check with CLUSTER NODES) or contains some "
              "key in database 0.";
    }
    clusterManagerLogErr("[ERR] Node %s:%d %s\n", node->ip, node->port, msg);
}
static void clusterManagerPrintNotClusterNodeError(clusterManagerNode *node,
                                                   char *err)
{
    char *msg = (err ? err : "is not configured as a cluster node.");
    clusterManagerLogErr("[ERR] Node %s:%d %s\n", node->ip, node->port, msg);
}
static void clusterManagerMode(clusterManagerCommandProc *proc) {
    int argc = config.cluster_manager_command.argc;
    char **argv = config.cluster_manager_command.argv;
    cluster_manager.nodes = NULL;
    if (!proc(argc, argv)) goto cluster_manager_err;
    freeClusterManager();
    exit(0);
cluster_manager_err:
    freeClusterManager();
    sdsfree(config.hostip);
    sdsfree(config.mb_delim);
    exit(1);
}
static int clusterManagerCommandCreate(int argc, char **argv) {
    int i, j, success = 1;
    cluster_manager.nodes = listCreate();
    for (i = 0; i < argc; i++) {
        char *addr = argv[i];
        char *c = strrchr(addr, '@');
        if (c != NULL) *c = '\0';
        c = strrchr(addr, ':');
        if (c == NULL) {
            fprintf(stderr, "Invalid address format: %s\n", addr);
            return 0;
        }
        *c = '\0';
        char *ip = addr;
        int port = atoi(++c);
        clusterManagerNode *node = clusterManagerNewNode(ip, port);
        if (!clusterManagerNodeConnect(node)) {
            freeClusterManagerNode(node);
            return 0;
        }
        char *err = NULL;
        if (!clusterManagerNodeIsCluster(node, &err)) {
            clusterManagerPrintNotClusterNodeError(node, err);
            if (err) zfree(err);
            freeClusterManagerNode(node);
            return 0;
        }
        err = NULL;
        if (!clusterManagerNodeLoadInfo(node, 0, &err)) {
            if (err) {
                CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                zfree(err);
            }
            freeClusterManagerNode(node);
            return 0;
        }
        err = NULL;
        if (!clusterManagerNodeIsEmpty(node, &err)) {
            clusterManagerPrintNotEmptyNodeError(node, err);
            if (err) zfree(err);
            freeClusterManagerNode(node);
            return 0;
        }
        listAddNodeTail(cluster_manager.nodes, node);
    }
    int node_len = cluster_manager.nodes->len;
    int replicas = config.cluster_manager_command.replicas;
    int masters_count = CLUSTER_MANAGER_MASTERS_COUNT(node_len, replicas);
    if (masters_count < 3) {
        clusterManagerLogErr(
            "*** ERROR: Invalid configuration for cluster creation.\n"
            "*** Redis Cluster requires at least 3 master nodes.\n"
            "*** This is not possible with %d nodes and %d replicas per node.",
            node_len, replicas);
        clusterManagerLogErr("\n*** At least %d nodes are required.\n",
                             3 * (replicas + 1));
        return 0;
    }
    clusterManagerLogInfo(">>> Performing hash slots allocation "
                          "on %d nodes...\n", node_len);
    int interleaved_len = 0, ip_count = 0;
    clusterManagerNode **interleaved = zcalloc(node_len*sizeof(**interleaved));
    char **ips = zcalloc(node_len * sizeof(char*));
    clusterManagerNodeArray *ip_nodes = zcalloc(node_len * sizeof(*ip_nodes));
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        int found = 0;
        for (i = 0; i < ip_count; i++) {
            char *ip = ips[i];
            if (!strcmp(ip, n->ip)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            ips[ip_count++] = n->ip;
        }
        clusterManagerNodeArray *node_array = &(ip_nodes[i]);
        if (node_array->nodes == NULL)
            clusterManagerNodeArrayInit(node_array, node_len);
        clusterManagerNodeArrayAdd(node_array, n);
    }
    while (interleaved_len < node_len) {
        for (i = 0; i < ip_count; i++) {
            clusterManagerNodeArray *node_array = &(ip_nodes[i]);
            if (node_array->count > 0) {
                clusterManagerNode *n = NULL;
                clusterManagerNodeArrayShift(node_array, &n);
                interleaved[interleaved_len++] = n;
            }
        }
    }
    clusterManagerNode **masters = interleaved;
    interleaved += masters_count;
    interleaved_len -= masters_count;
    float slots_per_node = CLUSTER_MANAGER_SLOTS / (float) masters_count;
    long first = 0;
    float cursor = 0.0f;
    for (i = 0; i < masters_count; i++) {
        clusterManagerNode *master = masters[i];
        long last = lround(cursor + slots_per_node - 1);
        if (last > CLUSTER_MANAGER_SLOTS || i == (masters_count - 1))
            last = CLUSTER_MANAGER_SLOTS - 1;
        if (last < first) last = first;
        printf("Master[%d] -> Slots %lu - %lu\n", i, first, last);
        master->slots_count = 0;
        for (j = first; j <= last; j++) {
            master->slots[j] = 1;
            master->slots_count++;
        }
        master->dirty = 1;
        first = last + 1;
        cursor += slots_per_node;
    }
    clusterManagerNode *first_node = interleaved[0];
    for (i = 0; i < (interleaved_len - 1); i++)
        interleaved[i] = interleaved[i + 1];
    interleaved[interleaved_len - 1] = first_node;
    int assign_unused = 0, available_count = interleaved_len;
assign_replicas:
    for (i = 0; i < masters_count; i++) {
        clusterManagerNode *master = masters[i];
        int assigned_replicas = 0;
        while (assigned_replicas < replicas) {
            if (available_count == 0) break;
            clusterManagerNode *found = NULL, *slave = NULL;
            int firstNodeIdx = -1;
            for (j = 0; j < interleaved_len; j++) {
                clusterManagerNode *n = interleaved[j];
                if (n == NULL) continue;
                if (strcmp(n->ip, master->ip)) {
                    found = n;
                    interleaved[j] = NULL;
                    break;
                }
                if (firstNodeIdx < 0) firstNodeIdx = j;
            }
            if (found) slave = found;
            else if (firstNodeIdx >= 0) {
                slave = interleaved[firstNodeIdx];
                interleaved_len -= (interleaved - (interleaved + firstNodeIdx));
                interleaved += (firstNodeIdx + 1);
            }
            if (slave != NULL) {
                assigned_replicas++;
                available_count--;
                if (slave->replicate) sdsfree(slave->replicate);
                slave->replicate = sdsnew(master->name);
                slave->dirty = 1;
            } else break;
            printf("Adding replica %s:%d to %s:%d\n", slave->ip, slave->port,
                   master->ip, master->port);
            if (assign_unused) break;
        }
    }
    if (!assign_unused && available_count > 0) {
        assign_unused = 1;
        printf("Adding extra replicas...\n");
        goto assign_replicas;
    }
    for (i = 0; i < ip_count; i++) {
        clusterManagerNodeArray *node_array = ip_nodes + i;
        clusterManagerNodeArrayReset(node_array);
    }
    clusterManagerOptimizeAntiAffinity(ip_nodes, ip_count);
    clusterManagerShowNodes();
    if (confirmWithYes("Can I set the above configuration?")) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            char *err = NULL;
            int flushed = clusterManagerFlushNodeConfig(node, &err);
            if (!flushed && node->dirty && !node->replicate) {
                if (err != NULL) {
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                    zfree(err);
                }
                success = 0;
                goto cleanup;
            } else if (err != NULL) zfree(err);
        }
        clusterManagerLogInfo(">>> Nodes configuration updated\n");
        clusterManagerLogInfo(">>> Assign a different config epoch to "
                              "each node\n");
        int config_epoch = 1;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            redisReply *reply = NULL;
            reply = CLUSTER_MANAGER_COMMAND(node,
                                            "cluster set-config-epoch %d",
                                            config_epoch++);
            if (reply != NULL) freeReplyObject(reply);
        }
        clusterManagerLogInfo(">>> Sending CLUSTER MEET messages to join "
                              "the cluster\n");
        clusterManagerNode *first = NULL;
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (first == NULL) {
                first = node;
                continue;
            }
            redisReply *reply = NULL;
            reply = CLUSTER_MANAGER_COMMAND(node, "cluster meet %s %d",
                                            first->ip, first->port);
            int is_err = 0;
            if (reply != NULL) {
                if ((is_err = reply->type == REDIS_REPLY_ERROR))
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, reply->str);
                freeReplyObject(reply);
            } else {
                is_err = 1;
                fprintf(stderr, "Failed to send CLUSTER MEET command.\n");
            }
            if (is_err) {
                success = 0;
                goto cleanup;
            }
        }
        sleep(1);
        clusterManagerWaitForClusterJoin();
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (!node->dirty) continue;
            char *err = NULL;
            int flushed = clusterManagerFlushNodeConfig(node, &err);
            if (!flushed && !node->replicate) {
                if (err != NULL) {
                    CLUSTER_MANAGER_PRINT_REPLY_ERROR(node, err);
                    zfree(err);
                }
                success = 0;
                goto cleanup;
            }
        }
        listRewind(cluster_manager.nodes, &li);
        clusterManagerNode *first_node = NULL;
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *node = ln->value;
            if (!first_node) first_node = node;
            else freeClusterManagerNode(node);
        }
        listEmpty(cluster_manager.nodes);
        if (!clusterManagerLoadInfoFromNode(first_node, 0)) {
            success = 0;
            goto cleanup;
        }
        clusterManagerCheckCluster(0);
    }
cleanup:
    zfree(masters);
    zfree(ips);
    for (i = 0; i < node_len; i++) {
        clusterManagerNodeArray *node_array = ip_nodes + i;
        CLUSTER_MANAGER_NODE_ARRAY_FREE(node_array);
    }
    zfree(ip_nodes);
    return success;
}
static int clusterManagerCommandAddNode(int argc, char **argv) {
    int success = 1;
    redisReply *reply = NULL;
    char *ref_ip = NULL, *ip = NULL;
    int ref_port = 0, port = 0;
    if (!getClusterHostFromCmdArgs(argc - 1, argv + 1, &ref_ip, &ref_port))
        goto invalid_args;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port))
        goto invalid_args;
    clusterManagerLogInfo(">>> Adding node %s:%d to cluster %s:%d\n", ip, port,
                          ref_ip, ref_port);
    clusterManagerNode *refnode = clusterManagerNewNode(ref_ip, ref_port);
    if (!clusterManagerLoadInfoFromNode(refnode, 0)) return 0;
    if (!clusterManagerCheckCluster(0)) return 0;
    clusterManagerNode *master_node = NULL;
    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_SLAVE) {
        char *master_id = config.cluster_manager_command.master_id;
        if (master_id != NULL) {
            master_node = clusterManagerNodeByName(master_id);
            if (master_node == NULL) {
                clusterManagerLogErr("[ERR] No such master ID %s\n", master_id);
                return 0;
            }
        } else {
            master_node = clusterManagerNodeWithLeastReplicas();
            assert(master_node != NULL);
            printf("Automatically selected master %s:%d\n", master_node->ip,
                   master_node->port);
        }
    }
    clusterManagerNode *new_node = clusterManagerNewNode(ip, port);
    int added = 0;
    if (!clusterManagerNodeConnect(new_node)) {
        clusterManagerLogErr("[ERR] Sorry, can't connect to node %s:%d\n",
                             ip, port);
        success = 0;
        goto cleanup;
    }
    char *err = NULL;
    if (!(success = clusterManagerNodeIsCluster(new_node, &err))) {
        clusterManagerPrintNotClusterNodeError(new_node, err);
        if (err) zfree(err);
        goto cleanup;
    }
    if (!clusterManagerNodeLoadInfo(new_node, 0, &err)) {
        if (err) {
            CLUSTER_MANAGER_PRINT_REPLY_ERROR(new_node, err);
            zfree(err);
        }
        success = 0;
        goto cleanup;
    }
    if (!(success = clusterManagerNodeIsEmpty(new_node, &err))) {
        clusterManagerPrintNotEmptyNodeError(new_node, err);
        if (err) zfree(err);
        goto cleanup;
    }
    clusterManagerNode *first = listFirst(cluster_manager.nodes)->value;
    listAddNodeTail(cluster_manager.nodes, new_node);
    added = 1;
    clusterManagerLogInfo(">>> Send CLUSTER MEET to node %s:%d to make it "
                          "join the cluster.\n", ip, port);
    reply = CLUSTER_MANAGER_COMMAND(new_node, "CLUSTER MEET %s %d",
                                    first->ip, first->port);
    if (!(success = clusterManagerCheckRedisReply(new_node, reply, NULL)))
        goto cleanup;
    if (master_node) {
        sleep(1);
        clusterManagerWaitForClusterJoin();
        clusterManagerLogInfo(">>> Configure node as replica of %s:%d.\n",
                              master_node->ip, master_node->port);
        freeReplyObject(reply);
        reply = CLUSTER_MANAGER_COMMAND(new_node, "CLUSTER REPLICATE %s",
                                        master_node->name);
        if (!(success = clusterManagerCheckRedisReply(new_node, reply, NULL)))
            goto cleanup;
    }
    clusterManagerLogOk("[OK] New node added correctly.\n");
cleanup:
    if (!added && new_node) freeClusterManagerNode(new_node);
    if (reply) freeReplyObject(reply);
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandDeleteNode(int argc, char **argv) {
    UNUSED(argc);
    int success = 1;
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    char *node_id = argv[1];
    clusterManagerLogInfo(">>> Removing node %s from cluster %s:%d\n",
                          node_id, ip, port);
    clusterManagerNode *ref_node = clusterManagerNewNode(ip, port);
    clusterManagerNode *node = NULL;
    if (!clusterManagerLoadInfoFromNode(ref_node, 0)) return 0;
    node = clusterManagerNodeByName(node_id);
    if (node == NULL) {
        clusterManagerLogErr("[ERR] No such node ID %s\n", node_id);
        return 0;
    }
    if (node->slots_count != 0) {
        clusterManagerLogErr("[ERR] Node %s:%d is not empty! Reshard data "
                             "away and try again.\n", node->ip, node->port);
        return 0;
    }
    clusterManagerLogInfo(">>> Sending CLUSTER FORGET messages to the "
                          "cluster...\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n == node) continue;
        if (n->replicate && !strcasecmp(n->replicate, node_id)) {
            clusterManagerNode *master = clusterManagerNodeWithLeastReplicas();
            assert(master != NULL);
            clusterManagerLogInfo(">>> %s:%d as replica of %s:%d\n",
                                  n->ip, n->port, master->ip, master->port);
            redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER REPLICATE %s",
                                                    master->name);
            success = clusterManagerCheckRedisReply(n, r, NULL);
            if (r) freeReplyObject(r);
            if (!success) return 0;
        }
        redisReply *r = CLUSTER_MANAGER_COMMAND(n, "CLUSTER FORGET %s",
                                                node_id);
        success = clusterManagerCheckRedisReply(n, r, NULL);
        if (r) freeReplyObject(r);
        if (!success) return 0;
    }
    clusterManagerLogInfo(">>> Sending CLUSTER RESET SOFT to the "
                          "deleted node.\n");
    redisReply *r = redisCommand(node->context, "CLUSTER RESET %s", "SOFT");
    success = clusterManagerCheckRedisReply(node, r, NULL);
    if (r) freeReplyObject(r);
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandInfo(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(node, 0)) return 0;
    clusterManagerShowClusterInfo();
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandCheck(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(node, 0)) return 0;
    clusterManagerShowClusterInfo();
    return clusterManagerCheckCluster(0);
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandFix(int argc, char **argv) {
    config.cluster_manager_command.flags |= CLUSTER_MANAGER_CMD_FLAG_FIX;
    return clusterManagerCommandCheck(argc, argv);
}
static int clusterManagerCommandReshard(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(node, 0)) return 0;
    clusterManagerCheckCluster(0);
    if (cluster_manager.errors && listLength(cluster_manager.errors) > 0) {
        fflush(stdout);
        fprintf(stderr,
                "*** Please fix your cluster problems before resharding\n");
        return 0;
    }
    int slots = config.cluster_manager_command.slots;
    if (!slots) {
        while (slots <= 0 || slots > CLUSTER_MANAGER_SLOTS) {
            printf("How many slots do you want to move (from 1 to %d)? ",
                   CLUSTER_MANAGER_SLOTS);
            fflush(stdout);
            char buf[6];
            int nread = read(fileno(stdin),buf,6);
            if (nread <= 0) continue;
            int last_idx = nread - 1;
            if (buf[last_idx] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
            }
            buf[last_idx] = '\0';
            slots = atoi(buf);
        }
    }
    char buf[255];
    char *to = config.cluster_manager_command.to,
         *from = config.cluster_manager_command.from;
    while (to == NULL) {
        printf("What is the receiving node ID? ");
        fflush(stdout);
        int nread = read(fileno(stdin),buf,255);
        if (nread <= 0) continue;
        int last_idx = nread - 1;
        if (buf[last_idx] != '\n') {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
        }
        buf[last_idx] = '\0';
        if (strlen(buf) > 0) to = buf;
    }
    int raise_err = 0;
    clusterManagerNode *target = clusterNodeForResharding(to, NULL, &raise_err);
    if (target == NULL) return 0;
    list *sources = listCreate();
    list *table = NULL;
    int all = 0, result = 1;
    if (from == NULL) {
        printf("Please enter all the source node IDs.\n");
        printf("  Type 'all' to use all the nodes as source nodes for "
               "the hash slots.\n");
        printf("  Type 'done' once you entered all the source nodes IDs.\n");
        while (1) {
            printf("Source node #%lu: ", listLength(sources) + 1);
            fflush(stdout);
            int nread = read(fileno(stdin),buf,255);
            if (nread <= 0) continue;
            int last_idx = nread - 1;
            if (buf[last_idx] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
            }
            buf[last_idx] = '\0';
            if (!strcmp(buf, "done")) break;
            else if (!strcmp(buf, "all")) {
                all = 1;
                break;
            } else {
                clusterManagerNode *src =
                    clusterNodeForResharding(buf, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
        }
    } else {
        char *p;
        while((p = strchr(from, ',')) != NULL) {
            *p = '\0';
            if (!strcmp(from, "all")) {
                all = 1;
                break;
            } else {
                clusterManagerNode *src =
                    clusterNodeForResharding(from, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
            from = p + 1;
        }
        if (!all && strlen(from) > 0) {
            if (!strcmp(from, "all")) all = 1;
            if (!all) {
                clusterManagerNode *src =
                    clusterNodeForResharding(from, target, &raise_err);
                if (src != NULL) listAddNodeTail(sources, src);
                else if (raise_err) {
                    result = 0;
                    goto cleanup;
                }
            }
        }
    }
    listIter li;
    listNode *ln;
    if (all) {
        listEmpty(sources);
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
                continue;
            if (!sdscmp(n->name, target->name)) continue;
            listAddNodeTail(sources, n);
        }
    }
    if (listLength(sources) == 0) {
        fprintf(stderr, "*** No source nodes given, operation aborted.\n");
        result = 0;
        goto cleanup;
    }
    printf("\nReady to move %d slots.\n", slots);
    printf("  Source nodes:\n");
    listRewind(sources, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *src = ln->value;
        sds info = clusterManagerNodeInfo(src, 4);
        printf("%s\n", info);
        sdsfree(info);
    }
    printf("  Destination node:\n");
    sds info = clusterManagerNodeInfo(target, 4);
    printf("%s\n", info);
    sdsfree(info);
    table = clusterManagerComputeReshardTable(sources, slots);
    printf("  Resharding plan:\n");
    clusterManagerShowReshardTable(table);
    if (!(config.cluster_manager_command.flags &
          CLUSTER_MANAGER_CMD_FLAG_YES))
    {
        printf("Do you want to proceed with the proposed "
               "reshard plan (yes/no)? ");
        fflush(stdout);
        char buf[4];
        int nread = read(fileno(stdin),buf,4);
        buf[3] = '\0';
        if (nread <= 0 || strcmp("yes", buf) != 0) {
            result = 0;
            goto cleanup;
        }
    }
    int opts = CLUSTER_MANAGER_OPT_VERBOSE;
    listRewind(table, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerReshardTableItem *item = ln->value;
        char *err = NULL;
        result = clusterManagerMoveSlot(item->source, target, item->slot,
                                        opts, &err);
        if (!result) {
            if (err != NULL) {
                zfree(err);
            }
            goto cleanup;
        }
    }
cleanup:
    listRelease(sources);
    clusterManagerReleaseReshardTable(table);
    return result;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandRebalance(int argc, char **argv) {
    int port = 0;
    char *ip = NULL;
    clusterManagerNode **weightedNodes = NULL;
    list *involved = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *node = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(node, 0)) return 0;
    int result = 1, i;
    if (config.cluster_manager_command.weight != NULL) {
        for (i = 0; i < config.cluster_manager_command.weight_argc; i++) {
            char *name = config.cluster_manager_command.weight[i];
            char *p = strchr(name, '=');
            if (p == NULL) {
                result = 0;
                goto cleanup;
            }
            *p = '\0';
            float w = atof(++p);
            clusterManagerNode *n = clusterManagerNodeByAbbreviatedName(name);
            if (n == NULL) {
                clusterManagerLogErr("*** No such master node %s\n", name);
                result = 0;
                goto cleanup;
            }
            n->weight = w;
        }
    }
    float total_weight = 0;
    int nodes_involved = 0;
    int use_empty = config.cluster_manager_command.flags &
                    CLUSTER_MANAGER_CMD_FLAG_EMPTYMASTER;
    involved = listCreate();
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE || n->replicate)
            continue;
        if (!use_empty && n->slots_count == 0) {
            n->weight = 0;
            continue;
        }
        total_weight += n->weight;
        nodes_involved++;
        listAddNodeTail(involved, n);
    }
    weightedNodes = zmalloc(nodes_involved * sizeof(clusterManagerNode *));
    if (weightedNodes == NULL) goto cleanup;
    clusterManagerCheckCluster(1);
    if (cluster_manager.errors && listLength(cluster_manager.errors) > 0) {
        clusterManagerLogErr("*** Please fix your cluster problems "
                             "before rebalancing\n");
        result = 0;
        goto cleanup;
    }
    int threshold_reached = 0, total_balance = 0;
    float threshold = config.cluster_manager_command.threshold;
    i = 0;
    listRewind(involved, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        weightedNodes[i++] = n;
        int expected = (int) (((float)CLUSTER_MANAGER_SLOTS / total_weight) *
                        n->weight);
        n->balance = n->slots_count - expected;
        total_balance += n->balance;
        int over_threshold = 0;
        if (threshold > 0) {
            if (n->slots_count > 0) {
                float err_perc = fabs((100-(100.0*expected/n->slots_count)));
                if (err_perc > threshold) over_threshold = 1;
            } else if (expected > 1) {
                over_threshold = 1;
            }
        }
        if (over_threshold) threshold_reached = 1;
    }
    if (!threshold_reached) {
        clusterManagerLogWarn("*** No rebalancing needed! "
                             "All nodes are within the %.2f%% threshold.\n",
                             config.cluster_manager_command.threshold);
        goto cleanup;
    }
    while (total_balance > 0) {
        listRewind(involved, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->balance <= 0 && total_balance > 0) {
                n->balance--;
                total_balance--;
            }
        }
    }
    qsort(weightedNodes, nodes_involved, sizeof(clusterManagerNode *),
          clusterManagerCompareNodeBalance);
    clusterManagerLogInfo(">>> Rebalancing across %d nodes. "
                          "Total weight = %.2f\n",
                          nodes_involved, total_weight);
    if (config.verbose) {
        for (i = 0; i < nodes_involved; i++) {
            clusterManagerNode *n = weightedNodes[i];
            printf("%s:%d balance is %d slots\n", n->ip, n->port, n->balance);
        }
    }
    int dst_idx = 0;
    int src_idx = nodes_involved - 1;
    int simulate = config.cluster_manager_command.flags &
                   CLUSTER_MANAGER_CMD_FLAG_SIMULATE;
    while (dst_idx < src_idx) {
        clusterManagerNode *dst = weightedNodes[dst_idx];
        clusterManagerNode *src = weightedNodes[src_idx];
        int db = abs(dst->balance);
        int sb = abs(src->balance);
        int numslots = (db < sb ? db : sb);
        if (numslots > 0) {
            printf("Moving %d slots from %s:%d to %s:%d\n", numslots,
                                                            src->ip,
                                                            src->port,
                                                            dst->ip,
                                                            dst->port);
            list *lsrc = listCreate(), *table = NULL;
            listAddNodeTail(lsrc, src);
            table = clusterManagerComputeReshardTable(lsrc, numslots);
            listRelease(lsrc);
            int table_len = (int) listLength(table);
            if (!table || table_len != numslots) {
                clusterManagerLogErr("*** Assertion failed: Reshard table "
                                     "!= number of slots");
                result = 0;
                goto end_move;
            }
            if (simulate) {
                for (i = 0; i < table_len; i++) printf("#");
            } else {
                int opts = CLUSTER_MANAGER_OPT_QUIET |
                           CLUSTER_MANAGER_OPT_UPDATE;
                listRewind(table, &li);
                while ((ln = listNext(&li)) != NULL) {
                    clusterManagerReshardTableItem *item = ln->value;
                    result = clusterManagerMoveSlot(item->source,
                                                    dst,
                                                    item->slot,
                                                    opts, NULL);
                    if (!result) goto end_move;
                    printf("#");
                    fflush(stdout);
                }
            }
            printf("\n");
end_move:
            clusterManagerReleaseReshardTable(table);
            if (!result) goto cleanup;
        }
        dst->balance += numslots;
        src->balance -= numslots;
        if (dst->balance == 0) dst_idx++;
        if (src->balance == 0) src_idx --;
    }
cleanup:
    if (involved != NULL) listRelease(involved);
    if (weightedNodes != NULL) zfree(weightedNodes);
    return result;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandSetTimeout(int argc, char **argv) {
    UNUSED(argc);
    int port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    int timeout = atoi(argv[1]);
    if (timeout < 100) {
        fprintf(stderr, "Setting a node timeout of less than 100 "
                "milliseconds is a bad idea.\n");
        return 0;
    }
    clusterManagerNode *node = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(node, 0)) return 0;
    int ok_count = 0, err_count = 0;
    clusterManagerLogInfo(">>> Reconfiguring node timeout in every "
                          "cluster node...\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        char *err = NULL;
        redisReply *reply = CLUSTER_MANAGER_COMMAND(n, "CONFIG %s %s %d",
                                                    "SET",
                                                    "cluster-node-timeout",
                                                    timeout);
        if (reply == NULL) goto reply_err;
        int ok = clusterManagerCheckRedisReply(n, reply, &err);
        freeReplyObject(reply);
        if (!ok) goto reply_err;
        reply = CLUSTER_MANAGER_COMMAND(n, "CONFIG %s", "REWRITE");
        if (reply == NULL) goto reply_err;
        ok = clusterManagerCheckRedisReply(n, reply, &err);
        freeReplyObject(reply);
        if (!ok) goto reply_err;
        clusterManagerLogWarn("*** New timeout set for %s:%d\n", n->ip,
                              n->port);
        ok_count++;
        continue;
reply_err:;
        int need_free = 0;
        if (err == NULL) err = "";
        else need_free = 1;
        clusterManagerLogErr("ERR setting node-timeot for %s:%d: %s\n", n->ip,
                             n->port, err);
        if (need_free) zfree(err);
        err_count++;
    }
    clusterManagerLogInfo(">>> New node timeout set. %d OK, %d ERR.\n",
                          ok_count, err_count);
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandImport(int argc, char **argv) {
    int success = 1;
    int port = 0, src_port = 0;
    char *ip = NULL, *src_ip = NULL;
    char *invalid_args_msg = NULL;
    if (!getClusterHostFromCmdArgs(argc, argv, &ip, &port)) {
        invalid_args_msg = CLUSTER_MANAGER_INVALID_HOST_ARG;
        goto invalid_args;
    }
    if (config.cluster_manager_command.from == NULL) {
        invalid_args_msg = "[ERR] Option '--cluster-from' is required for "
                           "subcommand 'import'.\n";
        goto invalid_args;
    }
    char *src_host[] = {config.cluster_manager_command.from};
    if (!getClusterHostFromCmdArgs(1, src_host, &src_ip, &src_port)) {
        invalid_args_msg = "[ERR] Invalid --cluster-from host. You need to "
                           "pass a valid address (ie. 120.0.0.1:7000).\n";
        goto invalid_args;
    }
    clusterManagerLogInfo(">>> Importing data from %s:%d to cluster %s:%d\n",
                          src_ip, src_port, ip, port);
    clusterManagerNode *refnode = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(refnode, 0)) return 0;
    if (!clusterManagerCheckCluster(0)) return 0;
    char *reply_err = NULL;
    redisReply *src_reply = NULL;
    redisContext *src_ctx = redisConnect(src_ip, src_port);
    if (src_ctx->err) {
        success = 0;
        fprintf(stderr,"Could not connect to Redis at %s:%d: %s.\n", src_ip,
                src_port, src_ctx->errstr);
        goto cleanup;
    }
    src_reply = reconnectingRedisCommand(src_ctx, "INFO");
    if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
        if (src_reply && src_reply->str) reply_err = src_reply->str;
        success = 0;
        goto cleanup;
    }
    if (getLongInfoField(src_reply->str, "cluster_enabled")) {
        clusterManagerLogErr("[ERR] The source node should not be a "
                             "cluster node.\n");
        success = 0;
        goto cleanup;
    }
    freeReplyObject(src_reply);
    src_reply = reconnectingRedisCommand(src_ctx, "DBSIZE");
    if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
        if (src_reply && src_reply->str) reply_err = src_reply->str;
        success = 0;
        goto cleanup;
    }
    int size = src_reply->integer, i;
    clusterManagerLogWarn("*** Importing %d keys from DB 0\n", size);
    clusterManagerNode *slots_map[CLUSTER_MANAGER_SLOTS];
    memset(slots_map, 0, sizeof(slots_map));
    listIter li;
    listNode *ln;
    for (i = 0; i < CLUSTER_MANAGER_SLOTS; i++) {
        listRewind(cluster_manager.nodes, &li);
        while ((ln = listNext(&li)) != NULL) {
            clusterManagerNode *n = ln->value;
            if (n->flags & CLUSTER_MANAGER_FLAG_SLAVE) continue;
            if (n->slots_count == 0) continue;
            if (n->slots[i]) {
                slots_map[i] = n;
                break;
            }
        }
    }
    char cmdfmt[50] = "MIGRATE %s %d %s %d %d";
    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_COPY)
        strcat(cmdfmt, " %s");
    if (config.cluster_manager_command.flags & CLUSTER_MANAGER_CMD_FLAG_REPLACE)
        strcat(cmdfmt, " %s");
    int cursor = -999, timeout = config.cluster_manager_command.timeout;
    while (cursor != 0) {
        if (cursor < 0) cursor = 0;
        freeReplyObject(src_reply);
        src_reply = reconnectingRedisCommand(src_ctx, "SCAN %d COUNT %d",
                                             cursor, 1000);
        if (!src_reply || src_reply->type == REDIS_REPLY_ERROR) {
            if (src_reply && src_reply->str) reply_err = src_reply->str;
            success = 0;
            goto cleanup;
        }
        assert(src_reply->type == REDIS_REPLY_ARRAY);
        assert(src_reply->elements >= 2);
        assert(src_reply->element[1]->type == REDIS_REPLY_ARRAY);
        if (src_reply->element[0]->type == REDIS_REPLY_STRING)
            cursor = atoi(src_reply->element[0]->str);
        else if (src_reply->element[0]->type == REDIS_REPLY_INTEGER)
            cursor = src_reply->element[0]->integer;
        int keycount = src_reply->element[1]->elements;
        for (i = 0; i < keycount; i++) {
            redisReply *kr = src_reply->element[1]->element[i];
            assert(kr->type == REDIS_REPLY_STRING);
            char *key = kr->str;
            uint16_t slot = clusterManagerKeyHashSlot(key, kr->len);
            clusterManagerNode *target = slots_map[slot];
            printf("Migrating %s to %s:%d: ", key, target->ip, target->port);
            redisReply *r = reconnectingRedisCommand(src_ctx, cmdfmt,
                                                     target->ip, target->port,
                                                     key, 0, timeout,
                                                     "COPY", "REPLACE");
            if (!r || r->type == REDIS_REPLY_ERROR) {
                if (r && r->str) {
                    clusterManagerLogErr("Source %s:%d replied with "
                                         "error:\n%s\n", src_ip, src_port,
                                         r->str);
                }
                success = 0;
            }
            freeReplyObject(r);
            if (!success) goto cleanup;
            clusterManagerLogOk("OK\n");
        }
    }
cleanup:
    if (reply_err)
        clusterManagerLogErr("Source %s:%d replied with error:\n%s\n",
                             src_ip, src_port, reply_err);
    if (src_ctx) redisFree(src_ctx);
    if (src_reply) freeReplyObject(src_reply);
    return success;
invalid_args:
    fprintf(stderr, "%s", invalid_args_msg);
    return 0;
}
static int clusterManagerCommandCall(int argc, char **argv) {
    int port = 0, i;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *refnode = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(refnode, 0)) return 0;
    argc--;
    argv++;
    size_t *argvlen = zmalloc(argc*sizeof(size_t));
    clusterManagerLogInfo(">>> Calling");
    for (i = 0; i < argc; i++) {
        argvlen[i] = strlen(argv[i]);
        printf(" %s", argv[i]);
    }
    printf("\n");
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterManagerNode *n = ln->value;
        if (!n->context && !clusterManagerNodeConnect(n)) continue;
        redisReply *reply = NULL;
        redisAppendCommandArgv(n->context, argc, (const char **) argv, argvlen);
        int status = redisGetReply(n->context, (void **)(&reply));
        if (status != REDIS_OK || reply == NULL )
            printf("%s:%d: Failed!\n", n->ip, n->port);
        else {
            sds formatted_reply = cliFormatReplyRaw(reply);
            printf("%s:%d: %s\n", n->ip, n->port, (char *) formatted_reply);
            sdsfree(formatted_reply);
        }
        if (reply != NULL) freeReplyObject(reply);
    }
    zfree(argvlen);
    return 1;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandBackup(int argc, char **argv) {
    UNUSED(argc);
    int success = 1, port = 0;
    char *ip = NULL;
    if (!getClusterHostFromCmdArgs(1, argv, &ip, &port)) goto invalid_args;
    clusterManagerNode *refnode = clusterManagerNewNode(ip, port);
    if (!clusterManagerLoadInfoFromNode(refnode, 0)) return 0;
    int no_issues = clusterManagerCheckCluster(0);
    int cluster_errors_count = (no_issues ? 0 :
                                listLength(cluster_manager.errors));
    config.cluster_manager_command.backup_dir = argv[1];
    sds json = sdsnew("[\n");
    int first_node = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster_manager.nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        if (!first_node) first_node = 1;
        else json = sdscat(json, ",\n");
        clusterManagerNode *node = ln->value;
        sds node_json = clusterManagerNodeGetJSON(node, cluster_errors_count);
        json = sdscat(json, node_json);
        sdsfree(node_json);
        if (node->replicate)
            continue;
        clusterManagerLogInfo(">>> Node %s:%d -> Saving RDB...\n",
                              node->ip, node->port);
        fflush(stdout);
        getRDB(node);
    }
    json = sdscat(json, "\n]");
    sds jsonpath = sdsnew(config.cluster_manager_command.backup_dir);
    if (jsonpath[sdslen(jsonpath) - 1] != '/')
        jsonpath = sdscat(jsonpath, "/");
    jsonpath = sdscat(jsonpath, "nodes.json");
    fflush(stdout);
    clusterManagerLogInfo("Saving cluster configuration to: %s\n", jsonpath);
    FILE *out = fopen(jsonpath, "w+");
    if (!out) {
        clusterManagerLogErr("Could not save nodes to: %s\n", jsonpath);
        success = 0;
        goto cleanup;
    }
    fputs(json, out);
    fclose(out);
cleanup:
    sdsfree(json);
    sdsfree(jsonpath);
    if (success) {
        if (!no_issues) {
            clusterManagerLogWarn("*** Cluster seems to have some problems, "
                                  "please be aware of it if you're going "
                                  "to restore this backup.\n");
        }
        clusterManagerLogOk("[OK] Backup created into: %s\n",
                            config.cluster_manager_command.backup_dir);
    } else clusterManagerLogOk("[ERR] Failed to back cluster!\n");
    return success;
invalid_args:
    fprintf(stderr, CLUSTER_MANAGER_INVALID_HOST_ARG);
    return 0;
}
static int clusterManagerCommandHelp(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);
    int commands_count = sizeof(clusterManagerCommands) /
                         sizeof(clusterManagerCommandDef);
    int i = 0, j;
    fprintf(stderr, "Cluster Manager Commands:\n");
    int padding = 15;
    for (; i < commands_count; i++) {
        clusterManagerCommandDef *def = &(clusterManagerCommands[i]);
        int namelen = strlen(def->name), padlen = padding - namelen;
        fprintf(stderr, "  %s", def->name);
        for (j = 0; j < padlen; j++) fprintf(stderr, " ");
        fprintf(stderr, "%s\n", (def->args ? def->args : ""));
        if (def->options != NULL) {
            int optslen = strlen(def->options);
            char *p = def->options, *eos = p + optslen;
            char *comma = NULL;
            while ((comma = strchr(p, ',')) != NULL) {
                int deflen = (int)(comma - p);
                char buf[255];
                memcpy(buf, p, deflen);
                buf[deflen] = '\0';
                for (j = 0; j < padding; j++) fprintf(stderr, " ");
                fprintf(stderr, "  --cluster-%s\n", buf);
                p = comma + 1;
                if (p >= eos) break;
            }
            if (p < eos) {
                for (j = 0; j < padding; j++) fprintf(stderr, " ");
                fprintf(stderr, "  --cluster-%s\n", p);
            }
        }
    }
    fprintf(stderr, "\nFor check, fix, reshard, del-node, set-timeout you "
                    "can specify the host and port of any working node in "
                    "the cluster.\n\n");
    return 0;
}
static void latencyModePrint(long long min, long long max, double avg, long long count) {
    if (config.output == OUTPUT_STANDARD) {
        printf("min: %lld, max: %lld, avg: %.2f (%lld samples)",
                min, max, avg, count);
        fflush(stdout);
    } else if (config.output == OUTPUT_CSV) {
        printf("%lld,%lld,%.2f,%lld\n", min, max, avg, count);
    } else if (config.output == OUTPUT_RAW) {
        printf("%lld %lld %.2f %lld\n", min, max, avg, count);
    }
}
#define LATENCY_SAMPLE_RATE 10
#define LATENCY_HISTORY_DEFAULT_INTERVAL 15000
static void latencyMode(void) {
    redisReply *reply;
    long long start, latency, min = 0, max = 0, tot = 0, count = 0;
    long long history_interval =
        config.interval ? config.interval/1000 :
                          LATENCY_HISTORY_DEFAULT_INTERVAL;
    double avg;
    long long history_start = mstime();
    if (config.interval == 0) {
        config.interval = 1000;
    } else {
        config.interval /= 1000;
    }
    if (!context) exit(1);
    while(1) {
        start = mstime();
        reply = reconnectingRedisCommand(context,"PING");
        if (reply == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        }
        latency = mstime()-start;
        freeReplyObject(reply);
        count++;
        if (count == 1) {
            min = max = tot = latency;
            avg = (double) latency;
        } else {
            if (latency < min) min = latency;
            if (latency > max) max = latency;
            tot += latency;
            avg = (double) tot/count;
        }
        if (config.output == OUTPUT_STANDARD) {
            printf("\x1b[0G\x1b[2K");
            latencyModePrint(min,max,avg,count);
        } else {
            if (config.latency_history) {
                latencyModePrint(min,max,avg,count);
            } else if (mstime()-history_start > config.interval) {
                latencyModePrint(min,max,avg,count);
                exit(0);
            }
        }
        if (config.latency_history && mstime()-history_start > history_interval)
        {
            printf(" -- %.2f seconds range\n", (float)(mstime()-history_start)/1000);
            history_start = mstime();
            min = max = tot = count = 0;
        }
        usleep(LATENCY_SAMPLE_RATE * 1000);
    }
}
#define LATENCY_DIST_DEFAULT_INTERVAL 1000
struct distsamples {
    long long max;
    long long count;
    int character;
};
void showLatencyDistSamples(struct distsamples *samples, long long tot) {
    int j;
    printf("\033[38;5;0m");
    for (j = 0; ; j++) {
        int coloridx =
            ceil((float) samples[j].count / tot * (spectrum_palette_size-1));
        int color = spectrum_palette[coloridx];
        printf("\033[48;5;%dm%c", (int)color, samples[j].character);
        samples[j].count = 0;
        if (samples[j].max == 0) break;
    }
    printf("\033[0m\n");
    fflush(stdout);
}
void showLatencyDistLegend(void) {
    int j;
    printf("---------------------------------------------\n");
    printf(". - * #          .01 .125 .25 .5 milliseconds\n");
    printf("1,2,3,...,9      from 1 to 9     milliseconds\n");
    printf("A,B,C,D,E        10,20,30,40,50  milliseconds\n");
    printf("F,G,H,I,J        .1,.2,.3,.4,.5       seconds\n");
    printf("K,L,M,N,O,P,Q,?  1,2,4,8,16,30,60,>60 seconds\n");
    printf("From 0 to 100%%: ");
    for (j = 0; j < spectrum_palette_size; j++) {
        printf("\033[48;5;%dm ", spectrum_palette[j]);
    }
    printf("\033[0m\n");
    printf("---------------------------------------------\n");
}
static void latencyDistMode(void) {
    redisReply *reply;
    long long start, latency, count = 0;
    long long history_interval =
        config.interval ? config.interval/1000 :
                          LATENCY_DIST_DEFAULT_INTERVAL;
    long long history_start = ustime();
    int j, outputs = 0;
    struct distsamples samples[] = {
        {10,0,'.'},
        {125,0,'-'},
        {250,0,'*'},
        {500,0,'#'},
        {1000,0,'1'},
        {2000,0,'2'},
        {3000,0,'3'},
        {4000,0,'4'},
        {5000,0,'5'},
        {6000,0,'6'},
        {7000,0,'7'},
        {8000,0,'8'},
        {9000,0,'9'},
        {10000,0,'A'},
        {20000,0,'B'},
        {30000,0,'C'},
        {40000,0,'D'},
        {50000,0,'E'},
        {100000,0,'F'},
        {200000,0,'G'},
        {300000,0,'H'},
        {400000,0,'I'},
        {500000,0,'J'},
        {1000000,0,'K'},
        {2000000,0,'L'},
        {4000000,0,'M'},
        {8000000,0,'N'},
        {16000000,0,'O'},
        {30000000,0,'P'},
        {60000000,0,'Q'},
        {0,0,'?'},
    };
    if (!context) exit(1);
    while(1) {
        start = ustime();
        reply = reconnectingRedisCommand(context,"PING");
        if (reply == NULL) {
            fprintf(stderr,"\nI/O error\n");
            exit(1);
        }
        latency = ustime()-start;
        freeReplyObject(reply);
        count++;
        for (j = 0; ; j++) {
            if (samples[j].max == 0 || latency <= samples[j].max) {
                samples[j].count++;
                break;
            }
        }
        if (count && (ustime()-history_start)/1000 > history_interval) {
            if ((outputs++ % 20) == 0)
                showLatencyDistLegend();
            showLatencyDistSamples(samples,count);
            history_start = ustime();
            count = 0;
        }
        usleep(LATENCY_SAMPLE_RATE * 1000);
    }
}
#define RDB_EOF_MARK_SIZE 40
void sendReplconf(const char* arg1, const char* arg2) {
    printf("sending REPLCONF %s %s\n", arg1, arg2);
    redisReply *reply = redisCommand(context, "REPLCONF %s %s", arg1, arg2);
    if(reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if(reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "REPLCONF %s error: %s\n", arg1, reply->str);
    }
    freeReplyObject(reply);
}
void sendCapa() {
    sendReplconf("capa", "eof");
}
unsigned long long sendSync(int fd, char *out_eof) {
    char buf[4096], *p;
    ssize_t nread;
    if (write(fd,"SYNC\r\n",6) != 6) {
        fprintf(stderr,"Error writing to master\n");
        exit(1);
    }
    p = buf;
    while(1) {
        nread = read(fd,p,1);
        if (nread <= 0) {
            fprintf(stderr,"Error reading bulk length while SYNCing\n");
            exit(1);
        }
        if (*p == '\n' && p != buf) break;
        if (*p != '\n') p++;
    }
    *p = '\0';
    if (buf[0] == '-') {
        printf("SYNC with master failed: %s\n", buf);
        exit(1);
    }
    if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= RDB_EOF_MARK_SIZE) {
        memcpy(out_eof, buf+5, RDB_EOF_MARK_SIZE);
        return 0;
    }
    return strtoull(buf+1,NULL,10);
}
static void slaveMode(void) {
    int fd = context->fd;
    static char eofmark[RDB_EOF_MARK_SIZE];
    static char lastbytes[RDB_EOF_MARK_SIZE];
    static int usemark = 0;
    unsigned long long payload = sendSync(fd, eofmark);
    char buf[1024];
    int original_output = config.output;
    if (payload == 0) {
        payload = ULLONG_MAX;
        memset(lastbytes,0,RDB_EOF_MARK_SIZE);
        usemark = 1;
        fprintf(stderr,"SYNC with master, discarding "
                       "bytes of bulk transfer until EOF marker...\n");
    } else {
        fprintf(stderr,"SYNC with master, discarding %llu "
                       "bytes of bulk transfer...\n", payload);
    }
    while(payload) {
        ssize_t nread;
        nread = read(fd,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"Error reading RDB payload while SYNCing\n");
            exit(1);
        }
        payload -= nread;
        if (usemark) {
            if (nread >= RDB_EOF_MARK_SIZE) {
                memcpy(lastbytes,buf+nread-RDB_EOF_MARK_SIZE,RDB_EOF_MARK_SIZE);
            } else {
                int rem = RDB_EOF_MARK_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,RDB_EOF_MARK_SIZE) == 0)
                break;
        }
    }
    if (usemark) {
        unsigned long long offset = ULLONG_MAX - payload;
        fprintf(stderr,"SYNC done after %llu bytes. Logging commands from master.\n", offset);
        sleep(1);
        sendReplconf("ACK", "0");
    } else
        fprintf(stderr,"SYNC done. Logging commands from master.\n");
    config.output = OUTPUT_CSV;
    while (cliReadReply(0) == REDIS_OK);
    config.output = original_output;
}
static void getRDB(clusterManagerNode *node) {
    int s, fd;
    char *filename;
    if (node != NULL) {
        assert(node->context);
        s = node->context->fd;
        filename = clusterManagerGetNodeRDBFilename(node);
    } else {
        s = context->fd;
        filename = config.rdb_filename;
    }
    static char eofmark[RDB_EOF_MARK_SIZE];
    static char lastbytes[RDB_EOF_MARK_SIZE];
    static int usemark = 0;
    unsigned long long payload = sendSync(s, eofmark);
    char buf[4096];
    if (payload == 0) {
        payload = ULLONG_MAX;
        memset(lastbytes,0,RDB_EOF_MARK_SIZE);
        usemark = 1;
        fprintf(stderr,"SYNC sent to master, writing bytes of bulk transfer "
                "until EOF marker to '%s'\n", filename);
    } else {
        fprintf(stderr,"SYNC sent to master, writing %llu bytes to '%s'\n",
            payload, filename);
    }
    if (!strcmp(filename,"-")) {
        fd = STDOUT_FILENO;
    } else {
        fd = open(filename, O_CREAT|O_WRONLY, 0644);
        if (fd == -1) {
            fprintf(stderr, "Error opening '%s': %s\n", filename,
                strerror(errno));
            exit(1);
        }
    }
    while(payload) {
        ssize_t nread, nwritten;
        nread = read(s,buf,(payload > sizeof(buf)) ? sizeof(buf) : payload);
        if (nread <= 0) {
            fprintf(stderr,"I/O Error reading RDB payload from socket\n");
            exit(1);
        }
        nwritten = write(fd, buf, nread);
        if (nwritten != nread) {
            fprintf(stderr,"Error writing data to file: %s\n",
                (nwritten == -1) ? strerror(errno) : "short write");
            exit(1);
        }
        payload -= nread;
        if (usemark) {
            if (nread >= RDB_EOF_MARK_SIZE) {
                memcpy(lastbytes,buf+nread-RDB_EOF_MARK_SIZE,RDB_EOF_MARK_SIZE);
            } else {
                int rem = RDB_EOF_MARK_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,RDB_EOF_MARK_SIZE) == 0)
                break;
        }
    }
    if (usemark) {
        payload = ULLONG_MAX - payload - RDB_EOF_MARK_SIZE;
        if (ftruncate(fd, payload) == -1)
            fprintf(stderr,"ftruncate failed: %s.\n", strerror(errno));
        fprintf(stderr,"Transfer finished with success after %llu bytes\n", payload);
    } else {
        fprintf(stderr,"Transfer finished with success.\n");
    }
    close(s);
    fsync(fd);
    close(fd);
    fprintf(stderr,"Transfer finished with success.\n");
    if (node) {
        sdsfree(filename);
        return;
    }
    exit(0);
}
#define PIPEMODE_WRITE_LOOP_MAX_BYTES (128*1024)
static void pipeMode(void) {
    int fd = context->fd;
    long long errors = 0, replies = 0, obuf_len = 0, obuf_pos = 0;
    char ibuf[1024*16], obuf[1024*16];
    char aneterr[ANET_ERR_LEN];
    redisReader *reader = redisReaderCreate();
    redisReply *reply;
    int eof = 0;
    int done = 0;
    char magic[20];
    time_t last_read_time = time(NULL);
    srand(time(NULL));
    if (anetNonBlock(aneterr,fd) == ANET_ERR) {
        fprintf(stderr, "Can't set the socket in non blocking mode: %s\n",
            aneterr);
        exit(1);
    }
    while(!done) {
        int mask = AE_READABLE;
        if (!eof || obuf_len != 0) mask |= AE_WRITABLE;
        mask = aeWait(fd,mask,1000);
        if (mask & AE_READABLE) {
            ssize_t nread;
            int read_error = 0;
            do {
                nread = read(fd,ibuf,sizeof(ibuf));
                if (nread == -1 && errno != EAGAIN && errno != EINTR) {
                    fprintf(stderr, "Error reading from the server: %s\n",
                        strerror(errno));
                    read_error = 1;
                    break;
                }
                if (nread > 0) {
                    redisReaderFeed(reader,ibuf,nread);
                    last_read_time = time(NULL);
                }
            } while(nread > 0);
            do {
                if (redisReaderGetReply(reader,(void**)&reply) == REDIS_ERR) {
                    fprintf(stderr, "Error reading replies from server\n");
                    exit(1);
                }
                if (reply) {
                    if (reply->type == REDIS_REPLY_ERROR) {
                        fprintf(stderr,"%s\n", reply->str);
                        errors++;
                    } else if (eof && reply->type == REDIS_REPLY_STRING &&
                                      reply->len == 20) {
                        if (memcmp(reply->str,magic,20) == 0) {
                            printf("Last reply received from server.\n");
                            done = 1;
                            replies--;
                        }
                    }
                    replies++;
                    freeReplyObject(reply);
                }
            } while(reply);
            if (read_error) exit(1);
        }
        if (mask & AE_WRITABLE) {
            ssize_t loop_nwritten = 0;
            while(1) {
                if (obuf_len != 0) {
                    ssize_t nwritten = write(fd,obuf+obuf_pos,obuf_len);
                    if (nwritten == -1) {
                        if (errno != EAGAIN && errno != EINTR) {
                            fprintf(stderr, "Error writing to the server: %s\n",
                                strerror(errno));
                            exit(1);
                        } else {
                            nwritten = 0;
                        }
                    }
                    obuf_len -= nwritten;
                    obuf_pos += nwritten;
                    loop_nwritten += nwritten;
                    if (obuf_len != 0) break;
                }
                if (obuf_len == 0 && !eof) {
                    ssize_t nread = read(STDIN_FILENO,obuf,sizeof(obuf));
                    if (nread == 0) {
                        char echo[] =
                        "\r\n*2\r\n$4\r\nECHO\r\n$20\r\n01234567890123456789\r\n";
                        int j;
                        eof = 1;
                        for (j = 0; j < 20; j++)
                            magic[j] = rand() & 0xff;
                        memcpy(echo+21,magic,20);
                        memcpy(obuf,echo,sizeof(echo)-1);
                        obuf_len = sizeof(echo)-1;
                        obuf_pos = 0;
                        printf("All data transferred. Waiting for the last reply...\n");
                    } else if (nread == -1) {
                        fprintf(stderr, "Error reading from stdin: %s\n",
                            strerror(errno));
                        exit(1);
                    } else {
                        obuf_len = nread;
                        obuf_pos = 0;
                    }
                }
                if ((obuf_len == 0 && eof) ||
                    loop_nwritten > PIPEMODE_WRITE_LOOP_MAX_BYTES) break;
            }
        }
        if (eof && config.pipe_timeout > 0 &&
            time(NULL)-last_read_time > config.pipe_timeout)
        {
            fprintf(stderr,"No replies for %d seconds: exiting.\n",
                config.pipe_timeout);
            errors++;
            break;
        }
    }
    redisReaderFree(reader);
    printf("errors: %lld, replies: %lld\n", errors, replies);
    if (errors)
        exit(1);
    else
        exit(0);
}
static redisReply *sendScan(unsigned long long *it) {
    redisReply *reply = redisCommand(context, "SCAN %llu", *it);
    if(reply == NULL) {
        fprintf(stderr, "\nI/O error\n");
        exit(1);
    } else if(reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "SCAN error: %s\n", reply->str);
        exit(1);
    } else if(reply->type != REDIS_REPLY_ARRAY) {
        fprintf(stderr, "Non ARRAY response from SCAN!\n");
        exit(1);
    } else if(reply->elements != 2) {
        fprintf(stderr, "Invalid element count from SCAN!\n");
        exit(1);
    }
    assert(reply->element[0]->type == REDIS_REPLY_STRING);
    assert(reply->element[1]->type == REDIS_REPLY_ARRAY);
    *it = strtoull(reply->element[0]->str, NULL, 10);
    return reply;
}
static int getDbSize(void) {
    redisReply *reply;
    int size;
    reply = redisCommand(context, "DBSIZE");
    if(reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "Couldn't determine DBSIZE!\n");
        exit(1);
    }
    size = reply->integer;
    freeReplyObject(reply);
    return size;
}
typedef struct {
    char *name;
    char *sizecmd;
    char *sizeunit;
    unsigned long long biggest;
    unsigned long long count;
    unsigned long long totalsize;
    sds biggest_key;
} typeinfo;
typeinfo type_string = { "string", "STRLEN", "bytes" };
typeinfo type_list = { "list", "LLEN", "items" };
typeinfo type_set = { "set", "SCARD", "members" };
typeinfo type_hash = { "hash", "HLEN", "fields" };
typeinfo type_zset = { "zset", "ZCARD", "members" };
typeinfo type_stream = { "stream", "XLEN", "entries" };
typeinfo type_other = { "other", NULL, "?" };
static typeinfo* typeinfo_add(dict *types, char* name, typeinfo* type_template) {
    typeinfo *info = zmalloc(sizeof(typeinfo));
    *info = *type_template;
    info->name = sdsnew(name);
    dictAdd(types, info->name, info);
    return info;
}
void type_free(void* priv_data, void* val) {
    typeinfo *info = val;
    UNUSED(priv_data);
    if (info->biggest_key)
        sdsfree(info->biggest_key);
    sdsfree(info->name);
    zfree(info);
}
static dictType typeinfoDictType = {
    dictSdsHash,
    NULL,
    NULL,
    dictSdsKeyCompare,
    NULL,
    type_free
};
static void getKeyTypes(dict *types_dict, redisReply *keys, typeinfo **types) {
    redisReply *reply;
    unsigned int i;
    for(i=0;i<keys->elements;i++) {
        redisAppendCommand(context, "TYPE %s", keys->element[i]->str);
    }
    for(i=0;i<keys->elements;i++) {
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting type for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_STATUS) {
            if(reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "TYPE returned an error: %s\n", reply->str);
            } else {
                fprintf(stderr,
                    "Invalid reply type (%d) for TYPE on key '%s'!\n",
                    reply->type, keys->element[i]->str);
            }
            exit(1);
        }
        sds typereply = sdsnew(reply->str);
        dictEntry *de = dictFind(types_dict, typereply);
        sdsfree(typereply);
        typeinfo *type = NULL;
        if (de)
            type = dictGetVal(de);
        else if (strcmp(reply->str, "none"))
            type = typeinfo_add(types_dict, reply->str, &type_other);
        types[i] = type;
        freeReplyObject(reply);
    }
}
static void getKeySizes(redisReply *keys, typeinfo **types,
                        unsigned long long *sizes, int memkeys,
                        unsigned memkeys_samples)
{
    redisReply *reply;
    unsigned int i;
    for(i=0;i<keys->elements;i++) {
        if(!types[i] || (!types[i]->sizecmd && !memkeys))
            continue;
        if (!memkeys)
            redisAppendCommand(context, "%s %s",
                types[i]->sizecmd, keys->element[i]->str);
        else if (memkeys_samples==0)
            redisAppendCommand(context, "%s %s %s",
                "MEMORY", "USAGE", keys->element[i]->str);
        else
            redisAppendCommand(context, "%s %s %s SAMPLES %u",
                "MEMORY", "USAGE", keys->element[i]->str, memkeys_samples);
    }
    for(i=0;i<keys->elements;i++) {
        if(!types[i] || (!types[i]->sizecmd && !memkeys)) {
            sizes[i] = 0;
            continue;
        }
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting size for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_INTEGER) {
            fprintf(stderr,
                "Warning:  %s on '%s' failed (may have changed type)\n",
                !memkeys? types[i]->sizecmd: "MEMORY USAGE",
                keys->element[i]->str);
            sizes[i] = 0;
        } else {
            sizes[i] = reply->integer;
        }
        freeReplyObject(reply);
    }
}
static void findBigKeys(int memkeys, unsigned memkeys_samples) {
    unsigned long long sampled = 0, total_keys, totlen=0, *sizes=NULL, it=0;
    redisReply *reply, *keys;
    unsigned int arrsize=0, i;
    dictIterator *di;
    dictEntry *de;
    typeinfo **types = NULL;
    double pct;
    dict *types_dict = dictCreate(&typeinfoDictType, NULL);
    typeinfo_add(types_dict, "string", &type_string);
    typeinfo_add(types_dict, "list", &type_list);
    typeinfo_add(types_dict, "set", &type_set);
    typeinfo_add(types_dict, "hash", &type_hash);
    typeinfo_add(types_dict, "zset", &type_zset);
    typeinfo_add(types_dict, "stream", &type_stream);
    total_keys = getDbSize();
    printf("\n# Scanning the entire keyspace to find biggest keys as well as\n");
    printf("# average sizes per key type.  You can use -i 0.1 to sleep 0.1 sec\n");
    printf("# per 100 SCAN commands (not usually needed).\n\n");
    do {
        pct = 100 * (double)sampled/total_keys;
        reply = sendScan(&it);
        keys = reply->element[1];
        if(keys->elements > arrsize) {
            types = zrealloc(types, sizeof(typeinfo*)*keys->elements);
            sizes = zrealloc(sizes, sizeof(unsigned long long)*keys->elements);
            if(!types || !sizes) {
                fprintf(stderr, "Failed to allocate storage for keys!\n");
                exit(1);
            }
            arrsize = keys->elements;
        }
        getKeyTypes(types_dict, keys, types);
        getKeySizes(keys, types, sizes, memkeys, memkeys_samples);
        for(i=0;i<keys->elements;i++) {
            typeinfo *type = types[i];
            if(!type)
                continue;
            type->totalsize += sizes[i];
            type->count++;
            totlen += keys->element[i]->len;
            sampled++;
            if(type->biggest<sizes[i]) {
                printf(
                   "[%05.2f%%] Biggest %-6s found so far '%s' with %llu %s\n",
                   pct, type->name, keys->element[i]->str, sizes[i],
                   !memkeys? type->sizeunit: "bytes");
                if (type->biggest_key)
                    sdsfree(type->biggest_key);
                type->biggest_key = sdsnew(keys->element[i]->str);
                if(!type->biggest_key) {
                    fprintf(stderr, "Failed to allocate memory for key!\n");
                    exit(1);
                }
                type->biggest = sizes[i];
            }
            if(sampled % 1000000 == 0) {
                printf("[%05.2f%%] Sampled %llu keys so far\n", pct, sampled);
            }
        }
        if(sampled && (sampled %100) == 0 && config.interval) {
            usleep(config.interval);
        }
        freeReplyObject(reply);
    } while(it != 0);
    if(types) zfree(types);
    if(sizes) zfree(sizes);
    printf("\n-------- summary -------\n\n");
    printf("Sampled %llu keys in the keyspace!\n", sampled);
    printf("Total key length in bytes is %llu (avg len %.2f)\n\n",
       totlen, totlen ? (double)totlen/sampled : 0);
    di = dictGetIterator(types_dict);
    while ((de = dictNext(di))) {
        typeinfo *type = dictGetVal(de);
        if(type->biggest_key) {
            printf("Biggest %6s found '%s' has %llu %s\n", type->name, type->biggest_key,
               type->biggest, !memkeys? type->sizeunit: "bytes");
        }
    }
    dictReleaseIterator(di);
    printf("\n");
    di = dictGetIterator(types_dict);
    while ((de = dictNext(di))) {
        typeinfo *type = dictGetVal(de);
        printf("%llu %ss with %llu %s (%05.2f%% of keys, avg size %.2f)\n",
           type->count, type->name, type->totalsize, !memkeys? type->sizeunit: "bytes",
           sampled ? 100 * (double)type->count/sampled : 0,
           type->count ? (double)type->totalsize/type->count : 0);
    }
    dictReleaseIterator(di);
    dictRelease(types_dict);
    exit(0);
}
static void getKeyFreqs(redisReply *keys, unsigned long long *freqs) {
    redisReply *reply;
    unsigned int i;
    for(i=0;i<keys->elements;i++) {
        redisAppendCommand(context, "OBJECT freq %s", keys->element[i]->str);
    }
    for(i=0;i<keys->elements;i++) {
        if(redisGetReply(context, (void**)&reply)!=REDIS_OK) {
            fprintf(stderr, "Error getting freq for key '%s' (%d: %s)\n",
                keys->element[i]->str, context->err, context->errstr);
            exit(1);
        } else if(reply->type != REDIS_REPLY_INTEGER) {
            if(reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
                exit(1);
            } else {
                fprintf(stderr, "Warning: OBJECT freq on '%s' failed (may have been deleted)\n", keys->element[i]->str);
                freqs[i] = 0;
            }
        } else {
            freqs[i] = reply->integer;
        }
        freeReplyObject(reply);
    }
}
#define HOTKEYS_SAMPLE 16
static void findHotKeys(void) {
    redisReply *keys, *reply;
    unsigned long long counters[HOTKEYS_SAMPLE] = {0};
    sds hotkeys[HOTKEYS_SAMPLE] = {NULL};
    unsigned long long sampled = 0, total_keys, *freqs = NULL, it = 0;
    unsigned int arrsize = 0, i, k;
    double pct;
    total_keys = getDbSize();
    printf("\n# Scanning the entire keyspace to find hot keys as well as\n");
    printf("# average sizes per key type.  You can use -i 0.1 to sleep 0.1 sec\n");
    printf("# per 100 SCAN commands (not usually needed).\n\n");
    do {
        pct = 100 * (double)sampled/total_keys;
        reply = sendScan(&it);
        keys = reply->element[1];
        if(keys->elements > arrsize) {
            freqs = zrealloc(freqs, sizeof(unsigned long long)*keys->elements);
            if(!freqs) {
                fprintf(stderr, "Failed to allocate storage for keys!\n");
                exit(1);
            }
            arrsize = keys->elements;
        }
        getKeyFreqs(keys, freqs);
        for(i=0;i<keys->elements;i++) {
            sampled++;
            if(sampled % 1000000 == 0) {
                printf("[%05.2f%%] Sampled %llu keys so far\n", pct, sampled);
            }
            k = 0;
            while (k < HOTKEYS_SAMPLE && freqs[i] > counters[k]) k++;
            if (k == 0) continue;
            k--;
            if (k == 0 || counters[k] == 0) {
                sdsfree(hotkeys[k]);
            } else {
                sdsfree(hotkeys[0]);
                memmove(counters,counters+1,sizeof(counters[0])*k);
                memmove(hotkeys,hotkeys+1,sizeof(hotkeys[0])*k);
            }
            counters[k] = freqs[i];
            hotkeys[k] = sdsnew(keys->element[i]->str);
            printf(
               "[%05.2f%%] Hot key '%s' found so far with counter %llu\n",
               pct, keys->element[i]->str, freqs[i]);
        }
        if(sampled && (sampled %100) == 0 && config.interval) {
            usleep(config.interval);
        }
        freeReplyObject(reply);
    } while(it != 0);
    if (freqs) zfree(freqs);
    printf("\n-------- summary -------\n\n");
    printf("Sampled %llu keys in the keyspace!\n", sampled);
    for (i=1; i<= HOTKEYS_SAMPLE; i++) {
        k = HOTKEYS_SAMPLE - i;
        if(counters[k]>0) {
            printf("hot key found with counter: %llu\tkeyname: %s\n", counters[k], hotkeys[k]);
            sdsfree(hotkeys[k]);
        }
    }
    exit(0);
}
static char *getInfoField(char *info, char *field) {
    char *p = strstr(info,field);
    char *n1, *n2;
    char *result;
    if (!p) return NULL;
    p += strlen(field)+1;
    n1 = strchr(p,'\r');
    n2 = strchr(p,',');
    if (n2 && n2 < n1) n1 = n2;
    result = zmalloc(sizeof(char)*(n1-p)+1);
    memcpy(result,p,(n1-p));
    result[n1-p] = '\0';
    return result;
}
static long getLongInfoField(char *info, char *field) {
    char *value = getInfoField(info,field);
    long l;
    if (!value) return LONG_MIN;
    l = strtol(value,NULL,10);
    zfree(value);
    return l;
}
void bytesToHuman(char *s, long long n) {
    double d;
    if (n < 0) {
        *s = '-';
        s++;
        n = -n;
    }
    if (n < 1024) {
        sprintf(s,"%lldB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}
static void statMode(void) {
    redisReply *reply;
    long aux, requests = 0;
    int i = 0;
    while(1) {
        char buf[64];
        int j;
        reply = reconnectingRedisCommand(context,"INFO");
        if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", reply->str);
            exit(1);
        }
        if ((i++ % 20) == 0) {
            printf(
"------- data ------ --------------------- load -------------------- - child -\n"
"keys       mem      clients blocked requests            connections          \n");
        }
        aux = 0;
        for (j = 0; j < 20; j++) {
            long k;
            sprintf(buf,"db%d:keys",j);
            k = getLongInfoField(reply->str,buf);
            if (k == LONG_MIN) continue;
            aux += k;
        }
        sprintf(buf,"%ld",aux);
        printf("%-11s",buf);
        aux = getLongInfoField(reply->str,"used_memory");
        bytesToHuman(buf,aux);
        printf("%-8s",buf);
        aux = getLongInfoField(reply->str,"connected_clients");
        sprintf(buf,"%ld",aux);
        printf(" %-8s",buf);
        aux = getLongInfoField(reply->str,"blocked_clients");
        sprintf(buf,"%ld",aux);
        printf("%-8s",buf);
        aux = getLongInfoField(reply->str,"total_commands_processed");
        sprintf(buf,"%ld (+%ld)",aux,requests == 0 ? 0 : aux-requests);
        printf("%-19s",buf);
        requests = aux;
        aux = getLongInfoField(reply->str,"total_connections_received");
        sprintf(buf,"%ld",aux);
        printf(" %-12s",buf);
        aux = getLongInfoField(reply->str,"bgsave_in_progress");
        aux |= getLongInfoField(reply->str,"aof_rewrite_in_progress") << 1;
        aux |= getLongInfoField(reply->str,"loading") << 2;
        switch(aux) {
        case 0: break;
        case 1:
            printf("SAVE");
            break;
        case 2:
            printf("AOF");
            break;
        case 3:
            printf("SAVE+AOF");
            break;
        case 4:
            printf("LOAD");
            break;
        }
        printf("\n");
        freeReplyObject(reply);
        usleep(config.interval);
    }
}
static void scanMode(void) {
    redisReply *reply;
    unsigned long long cur = 0;
    do {
        if (config.pattern)
            reply = redisCommand(context,"SCAN %llu MATCH %s",
                cur,config.pattern);
        else
            reply = redisCommand(context,"SCAN %llu",cur);
        if (reply == NULL) {
            printf("I/O error\n");
            exit(1);
        } else if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", reply->str);
            exit(1);
        } else {
            unsigned int j;
            cur = strtoull(reply->element[0]->str,NULL,10);
            for (j = 0; j < reply->element[1]->elements; j++)
                printf("%s\n", reply->element[1]->element[j]->str);
        }
        freeReplyObject(reply);
    } while(cur != 0);
    exit(0);
}
long long powerLawRand(long long min, long long max, double alpha) {
    double pl, r;
    max += 1;
    r = ((double)rand()) / RAND_MAX;
    pl = pow(
        ((pow(max,alpha+1) - pow(min,alpha+1))*r + pow(min,alpha+1)),
        (1.0/(alpha+1)));
    return (max-1-(long long)pl)+min;
}
void LRUTestGenKey(char *buf, size_t buflen) {
    snprintf(buf, buflen, "lru:%lld",
        powerLawRand(1, config.lru_test_sample_size, 6.2));
}
#define LRU_CYCLE_PERIOD 1000
#define LRU_CYCLE_PIPELINE_SIZE 250
static void LRUTestMode(void) {
    redisReply *reply;
    char key[128];
    long long start_cycle;
    int j;
    srand(time(NULL)^getpid());
    while(1) {
        start_cycle = mstime();
        long long hits = 0, misses = 0;
        while(mstime() - start_cycle < 1000) {
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                char val[6];
                val[5] = '\0';
                for (int i = 0; i < 5; i++) val[i] = 'A'+rand()%('z'-'A');
                LRUTestGenKey(key,sizeof(key));
                redisAppendCommand(context, "SET %s %s",key,val);
            }
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++)
                redisGetReply(context, (void**)&reply);
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                LRUTestGenKey(key,sizeof(key));
                redisAppendCommand(context, "GET %s",key);
            }
            for (j = 0; j < LRU_CYCLE_PIPELINE_SIZE; j++) {
                if (redisGetReply(context, (void**)&reply) == REDIS_OK) {
                    switch(reply->type) {
                        case REDIS_REPLY_ERROR:
                            printf("%s\n", reply->str);
                            break;
                        case REDIS_REPLY_NIL:
                            misses++;
                            break;
                        default:
                            hits++;
                            break;
                    }
                }
            }
            if (context->err) {
                fprintf(stderr,"I/O error during LRU test\n");
                exit(1);
            }
        }
        printf(
            "%lld Gets/sec | Hits: %lld (%.2f%%) | Misses: %lld (%.2f%%)\n",
            hits+misses,
            hits, (double)hits/(hits+misses)*100,
            misses, (double)misses/(hits+misses)*100);
    }
    exit(0);
}
unsigned long compute_something_fast(void) {
    unsigned char s[256], i, j, t;
    int count = 1000, k;
    unsigned long output = 0;
    for (k = 0; k < 256; k++) s[k] = k;
    i = 0;
    j = 0;
    while(count--) {
        i++;
        j = j + s[i];
        t = s[i];
        s[i] = s[j];
        s[j] = t;
        output += s[(s[i]+s[j])&255];
    }
    return output;
}
static void intrinsicLatencyModeStop(int s) {
    UNUSED(s);
    force_cancel_loop = 1;
}
static void intrinsicLatencyMode(void) {
    long long test_end, run_time, max_latency = 0, runs = 0;
    run_time = config.intrinsic_latency_duration*1000000;
    test_end = ustime() + run_time;
    signal(SIGINT, intrinsicLatencyModeStop);
    while(1) {
        long long start, end, latency;
        start = ustime();
        compute_something_fast();
        end = ustime();
        latency = end-start;
        runs++;
        if (latency <= 0) continue;
        if (latency > max_latency) {
            max_latency = latency;
            printf("Max latency so far: %lld microseconds.\n", max_latency);
        }
        double avg_us = (double)run_time/runs;
        double avg_ns = avg_us * 1e3;
        if (force_cancel_loop || end > test_end) {
            printf("\n%lld total runs "
                "(avg latency: "
                "%.4f microseconds / %.2f nanoseconds per run).\n",
                runs, avg_us, avg_ns);
            printf("Worst run took %.0fx longer than the average latency.\n",
                max_latency / avg_us);
            exit(0);
        }
    }
}
int main(int argc, char **argv) {
    int firstarg;
    config.hostip = sdsnew("127.0.0.1");
    config.hostport = 6379;
    config.hostsocket = NULL;
    config.repeat = 1;
    config.interval = 0;
    config.dbnum = 0;
    config.interactive = 0;
    config.shutdown = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.latency_mode = 0;
    config.latency_dist_mode = 0;
    config.latency_history = 0;
    config.lru_test_mode = 0;
    config.lru_test_sample_size = 0;
    config.cluster_mode = 0;
    config.slave_mode = 0;
    config.getrdb_mode = 0;
    config.stat_mode = 0;
    config.scan_mode = 0;
    config.intrinsic_latency_mode = 0;
    config.pattern = NULL;
    config.rdb_filename = NULL;
    config.pipe_mode = 0;
    config.pipe_timeout = REDIS_CLI_DEFAULT_PIPE_TIMEOUT;
    config.bigkeys = 0;
    config.hotkeys = 0;
    config.stdinarg = 0;
    config.auth = NULL;
    config.user = NULL;
    config.eval = NULL;
    config.eval_ldb = 0;
    config.eval_ldb_end = 0;
    config.eval_ldb_sync = 0;
    config.enable_ldb_on_eval = 0;
    config.last_cmd_type = -1;
    config.verbose = 0;
    config.no_auth_warning = 0;
    config.cluster_manager_command.name = NULL;
    config.cluster_manager_command.argc = 0;
    config.cluster_manager_command.argv = NULL;
    config.cluster_manager_command.flags = 0;
    config.cluster_manager_command.replicas = 0;
    config.cluster_manager_command.from = NULL;
    config.cluster_manager_command.to = NULL;
    config.cluster_manager_command.weight = NULL;
    config.cluster_manager_command.weight_argc = 0;
    config.cluster_manager_command.slots = 0;
    config.cluster_manager_command.timeout = CLUSTER_MANAGER_MIGRATE_TIMEOUT;
    config.cluster_manager_command.pipeline = CLUSTER_MANAGER_MIGRATE_PIPELINE;
    config.cluster_manager_command.threshold =
        CLUSTER_MANAGER_REBALANCE_THRESHOLD;
    config.cluster_manager_command.backup_dir = NULL;
    pref.hints = 1;
    spectrum_palette = spectrum_palette_color;
    spectrum_palette_size = spectrum_palette_color_size;
    if (!isatty(fileno(stdout)) && (getenv("FAKETTY") == NULL))
        config.output = OUTPUT_RAW;
    else
        config.output = OUTPUT_STANDARD;
    config.mb_delim = sdsnew("\n");
    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;
    parseEnv();
    if (CLUSTER_MANAGER_MODE()) {
        clusterManagerCommandProc *proc = validateClusterManagerCommand();
        if (!proc) {
            sdsfree(config.hostip);
            sdsfree(config.mb_delim);
            exit(1);
        }
        clusterManagerMode(proc);
    }
    if (config.latency_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        latencyMode();
    }
    if (config.latency_dist_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        latencyDistMode();
    }
    if (config.slave_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        sendCapa();
        slaveMode();
    }
    if (config.getrdb_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        sendCapa();
        getRDB(NULL);
    }
    if (config.pipe_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        pipeMode();
    }
    if (config.bigkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys(0, 0);
    }
    if (config.memkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findBigKeys(1, config.memkeys_samples);
    }
    if (config.hotkeys) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        findHotKeys();
    }
    if (config.stat_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        if (config.interval == 0) config.interval = 1000000;
        statMode();
    }
    if (config.scan_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        scanMode();
    }
    if (config.lru_test_mode) {
        if (cliConnect(0) == REDIS_ERR) exit(1);
        LRUTestMode();
    }
    if (config.intrinsic_latency_mode) intrinsicLatencyMode();
    if (argc == 0 && !config.eval) {
        signal(SIGPIPE, SIG_IGN);
        cliConnect(0);
        repl();
    }
    if (cliConnect(0) != REDIS_OK) exit(1);
    if (config.eval) {
        return evalMode(argc,argv);
    } else {
        return noninteractive(argc,convertToSds(argc,argv));
    }
}
