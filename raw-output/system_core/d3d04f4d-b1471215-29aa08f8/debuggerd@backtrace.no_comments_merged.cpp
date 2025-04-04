#define LOG_TAG "DEBUG"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <memory>
#include <string>
#include <backtrace/Backtrace.h>
#include <log/log.h>
#include "backtrace.h"
#include "utility.h"
static void dump_process_header(log_t* log, pid_t pid) {
  char path[PATH_MAX];
  char procnamebuf[1024];
  char* procname = NULL;
  FILE* fp;
  snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  if ((fp = fopen(path, "r"))) {
    procname = fgets(procnamebuf, sizeof(procnamebuf), fp);
    fclose(fp);
  }
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char timestr[64];
  strftime(timestr, sizeof(timestr), "%F %T", &tm);
  _LOG(log, logtype::BACKTRACE, "\n\n----- pid %d at %s -----\n", pid, timestr);
  if (procname) {
    _LOG(log, logtype::BACKTRACE, "Cmd line: %s\n", procname);
  }
  _LOG(log, logtype::BACKTRACE, "ABI: '%s'\n", ABI_STRING);
}
static void dump_process_footer(log_t* log, pid_t pid) {
  _LOG(log, logtype::BACKTRACE, "\n----- end %d -----\n", pid);
}
<<<<<<< HEAD
static void dump_thread(log_t* log, BacktraceMap* map, pid_t pid, pid_t tid) {
||||||| 29aa08f8c
static void dump_thread(
    log_t* log, pid_t tid, bool attached, bool* detach_failed, int* total_sleep_time_usec) {
=======
static void dump_thread(log_t* log, pid_t pid, pid_t tid, bool attached,
                        bool* detach_failed, int* total_sleep_time_usec) {
>>>>>>> b1471215
  char path[PATH_MAX];
  char threadnamebuf[1024];
  char* threadname = NULL;
  FILE* fp;
  snprintf(path, sizeof(path), "/proc/%d/comm", tid);
  if ((fp = fopen(path, "r"))) {
    threadname = fgets(threadnamebuf, sizeof(threadnamebuf), fp);
    fclose(fp);
    if (threadname) {
      size_t len = strlen(threadname);
      if (len && threadname[len - 1] == '\n') {
          threadname[len - 1] = '\0';
      }
    }
  }
  _LOG(log, logtype::BACKTRACE, "\n\"%s\" sysTid=%d\n", threadname ? threadname : "<unknown>", tid);
<<<<<<< HEAD
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, tid, map));
||||||| 29aa08f8c
  if (!attached && ptrace(PTRACE_ATTACH, tid, 0, 0) < 0) {
    _LOG(log, logtype::BACKTRACE, "Could not attach to thread: %s\n", strerror(errno));
    return;
  }
  if (!attached && wait_for_sigstop(tid, total_sleep_time_usec, detach_failed) == -1) {
    return;
  }
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(tid, BACKTRACE_CURRENT_THREAD));
=======
  if (!attached && !ptrace_attach_thread(pid, tid)) {
    _LOG(log, logtype::BACKTRACE, "Could not attach to thread: %s\n", strerror(errno));
    return;
  }
  if (!attached && wait_for_sigstop(tid, total_sleep_time_usec, detach_failed) == -1) {
    return;
  }
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(tid, BACKTRACE_CURRENT_THREAD));
>>>>>>> b1471215
  if (backtrace->Unwind(0)) {
    dump_backtrace_to_log(backtrace.get(), log, "  ");
  } else {
    ALOGE("Unwind failed: tid = %d: %s", tid,
          backtrace->GetErrorString(backtrace->GetError()).c_str());
  }
}
void dump_backtrace(int fd, BacktraceMap* map, pid_t pid, pid_t tid,
                    const std::set<pid_t>& siblings, std::string* amfd_data) {
  log_t log;
  log.tfd = fd;
  log.amfd_data = amfd_data;
  dump_process_header(&log, pid);
<<<<<<< HEAD
  dump_thread(&log, map, pid, tid);
||||||| 29aa08f8c
  dump_thread(&log, tid, true, detach_failed, total_sleep_time_usec);
=======
  dump_thread(&log, pid, tid, true, detach_failed, total_sleep_time_usec);
>>>>>>> b1471215
<<<<<<< HEAD
  for (pid_t sibling : siblings) {
    dump_thread(&log, map, pid, sibling);
||||||| 29aa08f8c
  char task_path[64];
  snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
  DIR* d = opendir(task_path);
  if (d != NULL) {
    struct dirent* de = NULL;
    while ((de = readdir(d)) != NULL) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
        continue;
      }
      char* end;
      pid_t new_tid = strtoul(de->d_name, &end, 10);
      if (*end || new_tid == tid) {
        continue;
      }
      dump_thread(&log, new_tid, false, detach_failed, total_sleep_time_usec);
    }
    closedir(d);
=======
  char task_path[64];
  snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
  DIR* d = opendir(task_path);
  if (d != NULL) {
    struct dirent* de = NULL;
    while ((de = readdir(d)) != NULL) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
        continue;
      }
      char* end;
      pid_t new_tid = strtoul(de->d_name, &end, 10);
      if (*end || new_tid == tid) {
        continue;
      }
      dump_thread(&log, pid, new_tid, false, detach_failed, total_sleep_time_usec);
    }
    closedir(d);
>>>>>>> b1471215
  }
  dump_process_footer(&log, pid);
}
void dump_backtrace_to_log(Backtrace* backtrace, log_t* log, const char* prefix) {
  for (size_t i = 0; i < backtrace->NumFrames(); i++) {
    _LOG(log, logtype::BACKTRACE, "%s%s\n", prefix, backtrace->FormatFrameData(i).c_str());
  }
}
