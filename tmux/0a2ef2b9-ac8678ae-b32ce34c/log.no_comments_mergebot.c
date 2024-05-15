#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vis.h>
#include "tmux.h"
static FILE *log_file;
static int log_level;
static void log_event_cb(int, const char *);
static void log_vwrite(const char *, va_list);
static void log_event_cb(__unused int severity, const char *msg) {
  log_debug("%s", msg);
}
voidlog_add_level(void) { log_level++; }
intlog_get_level(void) { return (log_level); }
void log_open(const char *name) {
  char *path;
  if (log_level == 0) return;
  if (log_file != NULL) fclose(log_file);
  xasprintf(&path, "tmux-%s-%ld.log", name, (long)getpid());
  log_file = fopen(path, "w");
  free(path);
  if (log_file == NULL) return;
  setvbuf(log_file, NULL, _IOLBF, 0);
  event_set_log_callback(log_event_cb);
}
void log_close(void) {
  if (log_file != NULL) fclose(log_file);
  log_file = NULL;
  event_set_log_callback(NULL);
}
static void log_vwrite(const char *msg, va_list ap) {
  char *fmt, *out;
  struct timeval tv;
  if (log_file == NULL) return;
  if (vasprintf(&fmt, msg, ap) == -1) exit(1);
  if (stravis(&out, fmt, VIS_OCTAL | VIS_CSTYLE | VIS_TAB | VIS_NL) == -1)
    exit(1);
  gettimeofday(&tv, NULL);
  if (fprintf(log_file, "%lld.%06d %s\n", (long long)tv.tv_sec, (int)tv.tv_usec,
              out) == -1)
    exit(1);
  fflush(log_file);
  free(out);
  free(fmt);
}
void log_debug(const char *msg, ...) {
  va_list ap;
  va_start(ap, msg);
  log_vwrite(msg, ap);
  va_end(ap);
}
__dead void fatal(const char *msg, ...) {
  char *fmt;
  va_list ap;
  va_start(ap, msg);
  if (asprintf(&fmt, "fatal: %s: %s", msg, strerror(errno)) == -1) exit(1);
  log_vwrite(fmt, ap);
  exit(1);
}
__dead void fatalx(const char *msg, ...) {
  char *fmt;
  va_list ap;
  va_start(ap, msg);
  if (asprintf(&fmt, "fatal: %s", msg) == -1) exit(1);
  log_vwrite(fmt, ap);
  exit(1);
}
