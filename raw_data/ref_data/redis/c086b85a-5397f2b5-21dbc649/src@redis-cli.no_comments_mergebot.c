#include "fmacros.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "anet.h"
#include <sys/time.h>
#include "adlist.h"
#include "hiredis.h"
#include "help.h"
#include "sds.h"
#include "zmalloc.h"
#include "linenoise.h"
#define REDIS_NOTUSED(V) ((void)V)
static redisContext *context;
static struct config {
  char *hostip;
  int hostport;
  char *hostsocket;
  long repeat;
  int dbnum;
  int interactive;
  int shutdown;
  int monitor_mode;
  int pubsub_mode;
  int raw_output;
  int tty;
  int stdinarg;
  char mb_sep;
  char *auth;
  char *historyfile;
} config;
static void usage();
char *redisGitSHA1(void);
static long long mstime(void) {
  struct timeval tv;
  long long mst;
  gettimeofday(&tv, NULL);
  mst = ((long)tv.tv_sec) * 1000;
  mst += tv.tv_usec / 1000;
  return mst;
}
static int cliAuth() {
  redisReply *reply;
  if (config.auth == NULL) return REDIS_OK;
  reply = redisCommand(context, "AUTH %s", config.auth);
  if (reply != NULL) {
    freeReplyObject(reply);
    return REDIS_OK;
  }
  return REDIS_ERR;
}
static int cliSelect() {
  redisReply *reply;
  char dbnum[16];
  if (config.dbnum == 0) return REDIS_OK;
  snprintf(dbnum, sizeof(dbnum), "%d", config.dbnum);
  reply = redisCommand(context, "SELECT %s", dbnum);
  if (reply != NULL) {
    freeReplyObject(reply);
    return REDIS_OK;
  }
  return REDIS_ERR;
}
static int cliConnect(int force) {
  if (context == NULL || force) {
    if (context != NULL) redisFree(context);
    if (config.hostsocket == NULL) {
      context = redisConnect(config.hostip, config.hostport);
    } else {
      context = redisConnectUnix(config.hostsocket);
    }
    if (context->err) {
      fprintf(stderr, "Could not connect to Redis at ");
      if (config.hostsocket == NULL)
        fprintf(stderr, "%s:%d: %s\n", config.hostip, config.hostport,
                context->errstr);
      else
        fprintf(stderr, "%s: %s\n", config.hostsocket, context->errstr);
      redisFree(context);
      context = NULL;
      return REDIS_ERR;
    }
    if (cliAuth() != REDIS_OK) return REDIS_ERR;
    if (cliSelect() != REDIS_OK) return REDIS_ERR;
  }
  return REDIS_OK;
}
static void cliPrintContextErrorAndExit() {
  if (context == NULL) return;
  fprintf(stderr, "Error: %s\n", context->errstr);
  exit(1);
}
static sds cliFormatReply(redisReply *r, char *prefix) {
  sds out = sdsempty();
  switch (r->type) {
    case REDIS_REPLY_ERROR:
      if (config.tty) out = sdscat(out, "(error) ");
      out = sdscatprintf(out, "%s\n", r->str);
      break;
    case REDIS_REPLY_STATUS:
      out = sdscat(out, r->str);
      out = sdscat(out, "\n");
      break;
    case REDIS_REPLY_INTEGER:
      if (config.tty) out = sdscat(out, "(integer) ");
      out = sdscatprintf(out, "%lld\n", r->integer);
      break;
    case REDIS_REPLY_STRING:
      if (config.raw_output || !config.tty) {
        out = sdscatlen(out, r->str, r->len);
      } else {
        out = sdscatrepr(out, r->str, r->len);
        out = sdscat(out, "\n");
      }
      break;
    case REDIS_REPLY_NIL:
      out = sdscat(out, "(nil)\n");
      break;
    case REDIS_REPLY_ARRAY:
      if (r->elements == 0) {
        out = sdscat(out, "(empty list or set)\n");
      } else {
        unsigned int i, idxlen = 0;
        char _prefixlen[16];
        char _prefixfmt[16];
        sds _prefix;
        sds tmp;
        i = r->elements;
        do {
          idxlen++;
          i /= 10;
        } while (i);
        memset(_prefixlen, ' ', idxlen + 2);
        _prefixlen[idxlen + 2] = '\0';
        _prefix = sdscat(sdsnew(prefix), _prefixlen);
        snprintf(_prefixfmt, sizeof(_prefixfmt), "%%s%%%dd) ", idxlen);
        for (i = 0; i < r->elements; i++) {
          out = sdscatprintf(out, _prefixfmt, i == 0 ? "" : prefix, i + 1);
          tmp = cliFormatReply(r->element[i], _prefix);
          out = sdscatlen(out, tmp, sdslen(tmp));
          sdsfree(tmp);
        }
        sdsfree(_prefix);
      }
      break;
    default:
      fprintf(stderr, "Unknown reply type: %d\n", r->type);
      exit(1);
  }
  return out;
}
static int cliReadReply() {
  redisReply *reply;
  sds out;
  if (redisGetReply(context, (void **)&reply) != REDIS_OK) {
    if (config.shutdown) return REDIS_OK;
    if (config.interactive) {
      if (context->err == REDIS_ERR_IO && errno == ECONNRESET) return REDIS_ERR;
      if (context->err == REDIS_ERR_EOF) return REDIS_ERR;
    }
    cliPrintContextErrorAndExit();
    return REDIS_ERR;
  }
  out = cliFormatReply(reply, "");
  freeReplyObject(reply);
  fwrite(out, sdslen(out), 1, stdout);
  sdsfree(out);
  return REDIS_OK;
}
static int cliSendCommand(int argc, char **argv, int repeat) {
  char *command = argv[0];
  size_t *argvlen;
  int j;
  config.raw_output = !strcasecmp(command, "info");
  if (!strcasecmp(command, "help")) {
<<<<<<< HEAD
    showInteractiveHelp();
    return REDIS_OK;
|||||||
    showInteractiveHelp();
    return 0;
=======
    output_help(--argc, ++argv);
    return 0;
>>>>>>> 5397f2b596b5189edbed3e45a42d18f3c99341d6
  }
  if (!strcasecmp(command, "shutdown")) config.shutdown = 1;
  if (!strcasecmp(command, "monitor")) config.monitor_mode = 1;
  if (!strcasecmp(command, "subscribe") || !strcasecmp(command, "psubscribe"))
    config.pubsub_mode = 1;
  argvlen = malloc(argc * sizeof(size_t));
  for (j = 0; j < argc; j++) argvlen[j] = sdslen(argv[j]);
  while (repeat--) {
    redisAppendCommandArgv(context, argc, (const char **)argv, argvlen);
    while (config.monitor_mode) {
      if (cliReadReply() != REDIS_OK) exit(1);
      fflush(stdout);
    }
    if (config.pubsub_mode) {
      printf("Reading messages... (press Ctrl-C to quit)\n");
      while (1) {
        if (cliReadReply() != REDIS_OK) exit(1);
      }
    }
    if (cliReadReply() != REDIS_OK) return REDIS_ERR;
  }
  return REDIS_OK;
}
static int parseOptions(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++) {
    int lastarg = i == argc - 1;
    if (!strcmp(argv[i], "-h") && !lastarg) {
      config.hostip = argv[i + 1];
      i++;
    } else if (!strcmp(argv[i], "-h") && lastarg) {
      usage();
    } else if (!strcmp(argv[i], "-x")) {
      config.stdinarg = 1;
    } else if (!strcmp(argv[i], "-p") && !lastarg) {
      config.hostport = atoi(argv[i + 1]);
      i++;
    } else if (!strcmp(argv[i], "-s") && !lastarg) {
      config.hostsocket = argv[i + 1];
      i++;
    } else if (!strcmp(argv[i], "-r") && !lastarg) {
      config.repeat = strtoll(argv[i + 1], NULL, 10);
      i++;
    } else if (!strcmp(argv[i], "-n") && !lastarg) {
      config.dbnum = atoi(argv[i + 1]);
      i++;
    } else if (!strcmp(argv[i], "-a") && !lastarg) {
      config.auth = argv[i + 1];
      i++;
    } else if (!strcmp(argv[i], "-i")) {
      fprintf(stderr,
              "Starting interactive mode using -i is deprecated. Interactive "
              "mode is started\n"
              "by default when redis-cli is executed without a command to "
              "execute.\n");
    } else if (!strcmp(argv[i], "-c")) {
      fprintf(
          stderr,
          "Reading last argument from standard input using -c is deprecated.\n"
          "When standard input is connected to a pipe or regular file, it is\n"
          "automatically used as last argument.\n");
    } else if (!strcmp(argv[i], "-v")) {
      printf("redis-cli shipped with Redis version %s (%s)\n", REDIS_VERSION,
             redisGitSHA1());
      exit(0);
    } else {
      break;
    }
  }
  return i;
}
static sds readArgFromStdin(void) {
  char buf[1024];
  sds arg = sdsempty();
  while (1) {
    int nread = read(fileno(stdin), buf, 1024);
    if (nread == 0)
      break;
    else if (nread == -1) {
      perror("Reading from standard input");
      exit(1);
    }
    arg = sdscatlen(arg, buf, nread);
  }
  return arg;
}
static void usage() {
  fprintf(
      stderr,
      "usage: redis-cli [-iv] [-h host] [-p port] [-s /path/to/socket] [-a "
      "authpw] [-r repeat_times] [-n db_num] cmd arg1 arg2 arg3 ... argN\n");
  fprintf(stderr,
          "usage: echo \"argN\" | redis-cli -x [options] cmd arg1 arg2 ... "
          "arg(N-1)\n\n");
  fprintf(stderr, "example: cat /etc/passwd | redis-cli -x set my_passwd\n");
  fprintf(stderr, "example: redis-cli get my_passwd\n");
  fprintf(stderr, "example: redis-cli -r 100 lpush mylist x\n");
  fprintf(stderr,
          "\nRun in interactive mode: redis-cli -i or just don't pass any "
          "command\n");
  exit(1);
}
static char **convertToSds(int count, char **args) {
  int j;
  char **sds = zmalloc(sizeof(char *) * count);
  for (j = 0; j < count; j++) sds[j] = sdsnew(args[j]);
  return sds;
}
#define LINE_BUFLEN 4096
static void repl() {
  int argc, j;
  char *line;
  sds *argv;
  config.interactive = 1;
  while ((line = linenoise("redis> ")) != NULL) {
    if (line[0] != '\0') {
      argv = sdssplitargs(line, &argc);
      linenoiseHistoryAdd(line);
      if (config.historyfile) linenoiseHistorySave(config.historyfile);
      if (argv == NULL) {
        printf("Invalid argument(s)\n");
        continue;
      } else if (argc > 0) {
        if (strcasecmp(argv[0], "quit") == 0 ||
            strcasecmp(argv[0], "exit") == 0) {
          exit(0);
        } else {
          long long start_time = mstime(), elapsed;
          if (cliSendCommand(argc, argv, 1) != REDIS_OK) {
            printf("Reconnecting... ");
            fflush(stdout);
            if (cliConnect(1) != REDIS_OK) exit(1);
            printf("OK\n");
            if (cliSendCommand(argc, argv, 1) != REDIS_OK)
              cliPrintContextErrorAndExit();
          }
          elapsed = mstime() - start_time;
          if (elapsed >= 500) {
            printf("(%.2fs)\n", (double)elapsed / 1000);
          }
        }
      }
      for (j = 0; j < argc; j++) sdsfree(argv[j]);
      zfree(argv);
    }
    free(line);
  }
  exit(0);
}
static int noninteractive(int argc, char **argv) {
  int retval = 0;
  if (config.stdinarg) {
    argv = zrealloc(argv, (argc + 1) * sizeof(char *));
    argv[argc] = readArgFromStdin();
    retval = cliSendCommand(argc + 1, argv, config.repeat);
  } else {
    retval = cliSendCommand(argc, argv, config.repeat);
  }
  return retval;
}
int main(int argc, char **argv) {
  int firstarg;
  config.hostip = "127.0.0.1";
  config.hostport = 6379;
  config.hostsocket = NULL;
  config.repeat = 1;
  config.dbnum = 0;
  config.interactive = 0;
  config.shutdown = 0;
  config.monitor_mode = 0;
  config.pubsub_mode = 0;
  config.raw_output = 0;
  config.stdinarg = 0;
  config.auth = NULL;
  config.historyfile = NULL;
  config.tty = isatty(fileno(stdout)) || (getenv("FAKETTY") != NULL);
  config.mb_sep = '\n';
  if (getenv("HOME") != NULL) {
    config.historyfile = malloc(256);
    snprintf(config.historyfile, 256, "%s/.rediscli_history", getenv("HOME"));
    linenoiseHistoryLoad(config.historyfile);
  }
  firstarg = parseOptions(argc, argv);
  argc -= firstarg;
  argv += firstarg;
  if (cliConnect(0) != REDIS_OK) exit(1);
  if (argc == 0) repl();
  return noninteractive(argc, convertToSds(argc, argv));
}
