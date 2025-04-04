#include "src/core/lib/iomgr/port.h"
#ifdef GRPC_LINUX_EPOLL
#include "src/core/lib/iomgr/ev_epoll1_linux.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <grpc/support/alloc.h>
#include <grpc/support/cpu.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/tls.h>
#include <grpc/support/useful.h>
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/lockfree_event.h"
#include "src/core/lib/iomgr/wakeup_fd_posix.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/support/block_annotate.h"
#include "src/core/lib/support/string.h"
static grpc_wakeup_fd global_wakeup_fd;
#define MAX_EPOLL_EVENTS 100
#define MAX_EPOLL_EVENTS_HANDLED_PER_ITERATION 1
typedef struct epoll_set {
  int epfd;
  struct epoll_event events[MAX_EPOLL_EVENTS];
  gpr_atm num_events;
  gpr_atm cursor;
} epoll_set;
static epoll_set g_epoll_set;
static bool epoll_set_init() {
  g_epoll_set.epfd = epoll_create1(EPOLL_CLOEXEC);
  if (g_epoll_set.epfd < 0) {
    gpr_log(GPR_ERROR, "epoll unavailable");
    return false;
  }
  gpr_log(GPR_INFO, "grpc epoll fd: %d", g_epoll_set.epfd);
  gpr_atm_no_barrier_store(&g_epoll_set.num_events, 0);
  gpr_atm_no_barrier_store(&g_epoll_set.cursor, 0);
  return true;
}
static void epoll_set_shutdown() {
  if (g_epoll_set.epfd >= 0) {
    close(g_epoll_set.epfd);
    g_epoll_set.epfd = -1;
  }
}
struct grpc_fd {
  int fd;
  gpr_atm read_closure;
  gpr_atm write_closure;
  struct grpc_fd *freelist_next;
  gpr_atm read_notifier_pollset;
  grpc_iomgr_object iomgr_object;
};
static void fd_global_init(void);
static void fd_global_shutdown(void);
typedef enum { UNKICKED, KICKED, DESIGNATED_POLLER } kick_state_t;
static const char *kick_state_string(kick_state_t st) {
  switch (st) {
    case UNKICKED:
      return "UNKICKED";
    case KICKED:
      return "KICKED";
    case DESIGNATED_POLLER:
      return "DESIGNATED_POLLER";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}
struct grpc_pollset_worker {
  kick_state_t kick_state;
  int kick_state_mutator;
  bool initialized_cv;
  grpc_pollset_worker *next;
  grpc_pollset_worker *prev;
  gpr_cv cv;
  grpc_closure_list schedule_on_end_work;
};
#define SET_KICK_STATE(worker,state) \
  do { \
    (worker)->kick_state = (state); \
    (worker)->kick_state_mutator = __LINE__; \
  } while (false)
#define MAX_NEIGHBORHOODS 1024
typedef struct pollset_neighborhood {
  gpr_mu mu;
  grpc_pollset *active_root;
  char pad[GPR_CACHELINE_SIZE];
} pollset_neighborhood;
struct grpc_pollset {
  gpr_mu mu;
  pollset_neighborhood *neighborhood;
  bool reassigning_neighborhood;
  grpc_pollset_worker *root_worker;
  bool kicked_without_poller;
  bool seen_inactive;
  bool shutting_down;
  grpc_closure *shutdown_closure;
  int begin_refs;
  grpc_pollset *next;
  grpc_pollset *prev;
};
struct grpc_pollset_set {
  char unused;
};
static bool append_error(grpc_error **composite, grpc_error *error,
                         const char *desc) {
  if (error == GRPC_ERROR_NONE) return true;
  if (*composite == GRPC_ERROR_NONE) {
    *composite = GRPC_ERROR_CREATE_FROM_COPIED_STRING(desc);
  }
  *composite = grpc_error_add_child(*composite, error);
  return false;
}
static grpc_fd *fd_freelist = NULL;
static gpr_mu fd_freelist_mu;
static void fd_global_init(void) { gpr_mu_init(&fd_freelist_mu); }
static void fd_global_shutdown(void) {
  gpr_mu_lock(&fd_freelist_mu);
  gpr_mu_unlock(&fd_freelist_mu);
  while (fd_freelist != NULL) {
    grpc_fd *fd = fd_freelist;
    fd_freelist = fd_freelist->freelist_next;
    gpr_free(fd);
  }
  gpr_mu_destroy(&fd_freelist_mu);
}
static grpc_fd *fd_create(int fd, const char *name) {
  grpc_fd *new_fd = NULL;
  gpr_mu_lock(&fd_freelist_mu);
  if (fd_freelist != NULL) {
    new_fd = fd_freelist;
    fd_freelist = fd_freelist->freelist_next;
  }
  gpr_mu_unlock(&fd_freelist_mu);
  if (new_fd == NULL) {
    new_fd = (grpc_fd *)gpr_malloc(sizeof(grpc_fd));
  }
  new_fd->fd = fd;
  grpc_lfev_init(&new_fd->read_closure);
  grpc_lfev_init(&new_fd->write_closure);
  gpr_atm_no_barrier_store(&new_fd->read_notifier_pollset, (gpr_atm)NULL);
  new_fd->freelist_next = NULL;
  char *fd_name;
  gpr_asprintf(&fd_name, "%s fd=%d", name, fd);
  grpc_iomgr_register_object(&new_fd->iomgr_object, fd_name);
#ifndef NDEBUG
  if (GRPC_TRACER_ON(grpc_trace_fd_refcount)) {
    gpr_log(GPR_DEBUG, "FD %d %p create %s", fd, new_fd, fd_name);
  }
#endif
  gpr_free(fd_name);
  struct epoll_event ev = {.events = (uint32_t)(EPOLLIN | EPOLLOUT | EPOLLET),
                           .data.ptr = new_fd};
  if (epoll_ctl(g_epoll_set.epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    gpr_log(GPR_ERROR, "epoll_ctl failed: %s", strerror(errno));
  }
  return new_fd;
}
static int fd_wrapped_fd(grpc_fd *fd) { return fd->fd; }
static void fd_shutdown_internal(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                                 grpc_error *why, bool releasing_fd) {
  if (grpc_lfev_set_shutdown(exec_ctx, &fd->read_closure,
                             GRPC_ERROR_REF(why))) {
    if (!releasing_fd) {
      shutdown(fd->fd, SHUT_RDWR);
    }
    grpc_lfev_set_shutdown(exec_ctx, &fd->write_closure, GRPC_ERROR_REF(why));
  }
  GRPC_ERROR_UNREF(why);
}
static void fd_shutdown(grpc_exec_ctx *exec_ctx, grpc_fd *fd, grpc_error *why) {
  fd_shutdown_internal(exec_ctx, fd, why, false);
}
static void fd_orphan(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                      grpc_closure *on_done, int *release_fd,
                      bool already_closed, const char *reason) {
  grpc_error *error = GRPC_ERROR_NONE;
  bool is_release_fd = (release_fd != NULL);
  if (!grpc_lfev_is_shutdown(&fd->read_closure)) {
    fd_shutdown_internal(exec_ctx, fd,
                         GRPC_ERROR_CREATE_FROM_COPIED_STRING(reason),
                         is_release_fd);
  }
  if (is_release_fd) {
    *release_fd = fd->fd;
  } else if (!already_closed) {
    close(fd->fd);
  }
  GRPC_CLOSURE_SCHED(exec_ctx, on_done, GRPC_ERROR_REF(error));
  grpc_iomgr_unregister_object(&fd->iomgr_object);
  grpc_lfev_destroy(&fd->read_closure);
  grpc_lfev_destroy(&fd->write_closure);
  gpr_mu_lock(&fd_freelist_mu);
  fd->freelist_next = fd_freelist;
  fd_freelist = fd;
  gpr_mu_unlock(&fd_freelist_mu);
}
static grpc_pollset *fd_get_read_notifier_pollset(grpc_exec_ctx *exec_ctx,
                                                  grpc_fd *fd) {
  gpr_atm notifier = gpr_atm_acq_load(&fd->read_notifier_pollset);
  return (grpc_pollset *)notifier;
}
static bool fd_is_shutdown(grpc_fd *fd) {
  return grpc_lfev_is_shutdown(&fd->read_closure);
}
static void fd_notify_on_read(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                              grpc_closure *closure) {
  grpc_lfev_notify_on(exec_ctx, &fd->read_closure, closure, "read");
}
static void fd_notify_on_write(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                               grpc_closure *closure) {
  grpc_lfev_notify_on(exec_ctx, &fd->write_closure, closure, "write");
}
static void fd_become_readable(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                               grpc_pollset *notifier) {
  grpc_lfev_set_ready(exec_ctx, &fd->read_closure, "read");
  gpr_atm_rel_store(&fd->read_notifier_pollset, (gpr_atm)notifier);
}
static void fd_become_writable(grpc_exec_ctx *exec_ctx, grpc_fd *fd) {
  grpc_lfev_set_ready(exec_ctx, &fd->write_closure, "write");
}
GPR_TLS_DECL(g_current_thread_pollset);
GPR_TLS_DECL(g_current_thread_worker);
static gpr_atm g_active_poller;
static pollset_neighborhood *g_neighborhoods;
static size_t g_num_neighborhoods;
static bool worker_insert(grpc_pollset *pollset, grpc_pollset_worker *worker) {
  if (pollset->root_worker == NULL) {
    pollset->root_worker = worker;
    worker->next = worker->prev = worker;
    return true;
  } else {
    worker->next = pollset->root_worker;
    worker->prev = worker->next->prev;
    worker->next->prev = worker;
    worker->prev->next = worker;
    return false;
  }
}
typedef enum { EMPTIED, NEW_ROOT, REMOVED } worker_remove_result;
static worker_remove_result worker_remove(grpc_pollset *pollset,
                                          grpc_pollset_worker *worker) {
  if (worker == pollset->root_worker) {
    if (worker == worker->next) {
      pollset->root_worker = NULL;
      return EMPTIED;
    } else {
      pollset->root_worker = worker->next;
      worker->prev->next = worker->next;
      worker->next->prev = worker->prev;
      return NEW_ROOT;
    }
  } else {
    worker->prev->next = worker->next;
    worker->next->prev = worker->prev;
    return REMOVED;
  }
}
static size_t choose_neighborhood(void) {
  return (size_t)gpr_cpu_current_cpu() % g_num_neighborhoods;
}
static grpc_error *pollset_global_init(void) {
  gpr_tls_init(&g_current_thread_pollset);
  gpr_tls_init(&g_current_thread_worker);
  gpr_atm_no_barrier_store(&g_active_poller, 0);
  global_wakeup_fd.read_fd = -1;
  grpc_error *err = grpc_wakeup_fd_init(&global_wakeup_fd);
  if (err != GRPC_ERROR_NONE) return err;
  struct epoll_event ev = {.events = (uint32_t)(EPOLLIN | EPOLLET),
                           .data.ptr = &global_wakeup_fd};
  if (epoll_ctl(g_epoll_set.epfd, EPOLL_CTL_ADD, global_wakeup_fd.read_fd,
                &ev) != 0) {
    return GRPC_OS_ERROR(errno, "epoll_ctl");
  }
  g_num_neighborhoods = GPR_CLAMP(gpr_cpu_num_cores(), 1, MAX_NEIGHBORHOODS);
  g_neighborhoods = (pollset_neighborhood *)gpr_zalloc(
      sizeof(*g_neighborhoods) * g_num_neighborhoods);
  for (size_t i = 0; i < g_num_neighborhoods; i++) {
    gpr_mu_init(&g_neighborhoods[i].mu);
  }
  return GRPC_ERROR_NONE;
}
static void pollset_global_shutdown(void) {
  gpr_tls_destroy(&g_current_thread_pollset);
  gpr_tls_destroy(&g_current_thread_worker);
  if (global_wakeup_fd.read_fd != -1) grpc_wakeup_fd_destroy(&global_wakeup_fd);
  for (size_t i = 0; i < g_num_neighborhoods; i++) {
    gpr_mu_destroy(&g_neighborhoods[i].mu);
  }
  gpr_free(g_neighborhoods);
}
static void pollset_init(grpc_pollset *pollset, gpr_mu **mu) {
  gpr_mu_init(&pollset->mu);
  *mu = &pollset->mu;
  pollset->neighborhood = &g_neighborhoods[choose_neighborhood()];
  pollset->reassigning_neighborhood = false;
  pollset->root_worker = NULL;
  pollset->kicked_without_poller = false;
  pollset->seen_inactive = true;
  pollset->shutting_down = false;
  pollset->shutdown_closure = NULL;
  pollset->begin_refs = 0;
  pollset->next = pollset->prev = NULL;
}
static void pollset_destroy(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset) {
  gpr_mu_lock(&pollset->mu);
  if (!pollset->seen_inactive) {
    pollset_neighborhood *neighborhood = pollset->neighborhood;
    gpr_mu_unlock(&pollset->mu);
  retry_lock_neighborhood:
    gpr_mu_lock(&neighborhood->mu);
    gpr_mu_lock(&pollset->mu);
    if (!pollset->seen_inactive) {
      if (pollset->neighborhood != neighborhood) {
        gpr_mu_unlock(&neighborhood->mu);
        neighborhood = pollset->neighborhood;
        gpr_mu_unlock(&pollset->mu);
        goto retry_lock_neighborhood;
      }
      pollset->prev->next = pollset->next;
      pollset->next->prev = pollset->prev;
      if (pollset == pollset->neighborhood->active_root) {
        pollset->neighborhood->active_root =
            pollset->next == pollset ? NULL : pollset->next;
      }
    }
    gpr_mu_unlock(&pollset->neighborhood->mu);
  }
  gpr_mu_unlock(&pollset->mu);
  gpr_mu_destroy(&pollset->mu);
}
static grpc_error *pollset_kick_all(grpc_exec_ctx *exec_ctx,
                                    grpc_pollset *pollset) {
  GPR_TIMER_BEGIN("pollset_kick_all", 0);
  grpc_error *error = GRPC_ERROR_NONE;
  if (pollset->root_worker != NULL) {
    grpc_pollset_worker *worker = pollset->root_worker;
    do {
      GRPC_STATS_INC_POLLSET_KICK(exec_ctx);
      switch (worker->kick_state) {
        case KICKED:
          GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
          break;
        case UNKICKED:
          SET_KICK_STATE(worker, KICKED);
          if (worker->initialized_cv) {
            GRPC_STATS_INC_POLLSET_KICK_WAKEUP_CV(exec_ctx);
            gpr_cv_signal(&worker->cv);
          }
          break;
        case DESIGNATED_POLLER:
          GRPC_STATS_INC_POLLSET_KICK_WAKEUP_FD(exec_ctx);
          SET_KICK_STATE(worker, KICKED);
          append_error(&error, grpc_wakeup_fd_wakeup(&global_wakeup_fd),
                       "pollset_kick_all");
          break;
      }
      worker = worker->next;
    } while (worker != pollset->root_worker);
  }
  GPR_TIMER_END("pollset_kick_all", 0);
  return error;
}
static void pollset_maybe_finish_shutdown(grpc_exec_ctx *exec_ctx,
                                          grpc_pollset *pollset) {
  if (pollset->shutdown_closure != NULL && pollset->root_worker == NULL &&
      pollset->begin_refs == 0) {
    GPR_TIMER_MARK("pollset_finish_shutdown", 0);
    GRPC_CLOSURE_SCHED(exec_ctx, pollset->shutdown_closure, GRPC_ERROR_NONE);
    pollset->shutdown_closure = NULL;
  }
}
static void pollset_shutdown(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                             grpc_closure *closure) {
  GPR_TIMER_BEGIN("pollset_shutdown", 0);
  GPR_ASSERT(pollset->shutdown_closure == NULL);
  GPR_ASSERT(!pollset->shutting_down);
  pollset->shutdown_closure = closure;
  pollset->shutting_down = true;
  GRPC_LOG_IF_ERROR("pollset_shutdown", pollset_kick_all(exec_ctx, pollset));
  pollset_maybe_finish_shutdown(exec_ctx, pollset);
  GPR_TIMER_END("pollset_shutdown", 0);
}
static int poll_deadline_to_millis_timeout(gpr_timespec deadline,
                                           gpr_timespec now) {
  gpr_timespec timeout;
  if (gpr_time_cmp(deadline, gpr_inf_future(deadline.clock_type)) == 0) {
    return -1;
  }
  if (gpr_time_cmp(deadline, now) <= 0) {
    return 0;
  }
  static const gpr_timespec round_up = {
      .clock_type = GPR_TIMESPAN, .tv_sec = 0, .tv_nsec = GPR_NS_PER_MS - 1};
  timeout = gpr_time_sub(deadline, now);
  int millis = gpr_time_to_millis(gpr_time_add(timeout, round_up));
  return millis >= 1 ? millis : 1;
}
static grpc_error *process_epoll_events(grpc_exec_ctx *exec_ctx,
                                        grpc_pollset *pollset) {
  static const char *err_desc = "process_events";
  grpc_error *error = GRPC_ERROR_NONE;
  GPR_TIMER_BEGIN("process_epoll_events", 0);
  long num_events = gpr_atm_acq_load(&g_epoll_set.num_events);
  long cursor = gpr_atm_acq_load(&g_epoll_set.cursor);
  for (int idx = 0;
       (idx < MAX_EPOLL_EVENTS_HANDLED_PER_ITERATION) && cursor != num_events;
       idx++) {
    long c = cursor++;
    struct epoll_event *ev = &g_epoll_set.events[c];
    void *data_ptr = ev->data.ptr;
    if (data_ptr == &global_wakeup_fd) {
      append_error(&error, grpc_wakeup_fd_consume_wakeup(&global_wakeup_fd),
                   err_desc);
    } else {
      grpc_fd *fd = (grpc_fd *)(data_ptr);
      bool cancel = (ev->events & (EPOLLERR | EPOLLHUP)) != 0;
      bool read_ev = (ev->events & (EPOLLIN | EPOLLPRI)) != 0;
      bool write_ev = (ev->events & EPOLLOUT) != 0;
      if (read_ev || cancel) {
        fd_become_readable(exec_ctx, fd, pollset);
      }
      if (write_ev || cancel) {
        fd_become_writable(exec_ctx, fd);
      }
    }
  }
  gpr_atm_rel_store(&g_epoll_set.cursor, cursor);
  GPR_TIMER_END("process_epoll_events", 0);
  return error;
}
static grpc_error *do_epoll_wait(grpc_exec_ctx *exec_ctx, grpc_pollset *ps,
                                 gpr_timespec now, gpr_timespec deadline) {
  GPR_TIMER_BEGIN("do_epoll_wait", 0);
  int r;
  int timeout = poll_deadline_to_millis_timeout(deadline, now);
  if (timeout != 0) {
    GRPC_SCHEDULING_START_BLOCKING_REGION;
  }
  do {
    GRPC_STATS_INC_SYSCALL_POLL(exec_ctx);
    r = epoll_wait(g_epoll_set.epfd, g_epoll_set.events, MAX_EPOLL_EVENTS,
                   timeout);
  } while (r < 0 && errno == EINTR);
  if (timeout != 0) {
    GRPC_SCHEDULING_END_BLOCKING_REGION;
  }
  if (r < 0) return GRPC_OS_ERROR(errno, "epoll_wait");
  GRPC_STATS_INC_POLL_EVENTS_RETURNED(exec_ctx, r);
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "ps: %p poll got %d events", ps, r);
  }
  gpr_atm_rel_store(&g_epoll_set.num_events, r);
  gpr_atm_rel_store(&g_epoll_set.cursor, 0);
  GPR_TIMER_END("do_epoll_wait", 0);
  return GRPC_ERROR_NONE;
}
static bool begin_worker(grpc_pollset *pollset, grpc_pollset_worker *worker,
                         grpc_pollset_worker **worker_hdl, gpr_timespec *now,
                         gpr_timespec deadline) {
  GPR_TIMER_BEGIN("begin_worker", 0);
  if (worker_hdl != NULL) *worker_hdl = worker;
  worker->initialized_cv = false;
  SET_KICK_STATE(worker, UNKICKED);
  worker->schedule_on_end_work = (grpc_closure_list)GRPC_CLOSURE_LIST_INIT;
  pollset->begin_refs++;
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_ERROR, "PS:%p BEGIN_STARTS:%p", pollset, worker);
  }
  if (pollset->seen_inactive) {
    bool is_reassigning = false;
    if (!pollset->reassigning_neighborhood) {
      is_reassigning = true;
      pollset->reassigning_neighborhood = true;
      pollset->neighborhood = &g_neighborhoods[choose_neighborhood()];
    }
    pollset_neighborhood *neighborhood = pollset->neighborhood;
    gpr_mu_unlock(&pollset->mu);
  retry_lock_neighborhood:
    gpr_mu_lock(&neighborhood->mu);
    gpr_mu_lock(&pollset->mu);
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, "PS:%p BEGIN_REORG:%p kick_state=%s is_reassigning=%d",
              pollset, worker, kick_state_string(worker->kick_state),
              is_reassigning);
    }
    if (pollset->seen_inactive) {
      if (neighborhood != pollset->neighborhood) {
        gpr_mu_unlock(&neighborhood->mu);
        neighborhood = pollset->neighborhood;
        gpr_mu_unlock(&pollset->mu);
        goto retry_lock_neighborhood;
      }
      if (worker->kick_state == UNKICKED) {
        pollset->seen_inactive = false;
        if (neighborhood->active_root == NULL) {
          neighborhood->active_root = pollset->next = pollset->prev = pollset;
          if (worker->kick_state == UNKICKED &&
              gpr_atm_no_barrier_cas(&g_active_poller, 0, (gpr_atm)worker)) {
            SET_KICK_STATE(worker, DESIGNATED_POLLER);
          }
        } else {
          pollset->next = neighborhood->active_root;
          pollset->prev = pollset->next->prev;
          pollset->next->prev = pollset->prev->next = pollset;
        }
      }
    }
    if (is_reassigning) {
      GPR_ASSERT(pollset->reassigning_neighborhood);
      pollset->reassigning_neighborhood = false;
    }
    gpr_mu_unlock(&neighborhood->mu);
  }
  worker_insert(pollset, worker);
  pollset->begin_refs--;
  if (worker->kick_state == UNKICKED && !pollset->kicked_without_poller) {
    GPR_ASSERT(gpr_atm_no_barrier_load(&g_active_poller) != (gpr_atm)worker);
    worker->initialized_cv = true;
    gpr_cv_init(&worker->cv);
    while (worker->kick_state == UNKICKED && !pollset->shutting_down) {
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_ERROR, "PS:%p BEGIN_WAIT:%p kick_state=%s shutdown=%d",
                pollset, worker, kick_state_string(worker->kick_state),
                pollset->shutting_down);
      }
      if (gpr_cv_wait(&worker->cv, &pollset->mu, deadline) &&
          worker->kick_state == UNKICKED) {
        SET_KICK_STATE(worker, KICKED);
      }
    }
    *now = gpr_now(now->clock_type);
  }
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_ERROR,
            "PS:%p BEGIN_DONE:%p kick_state=%s shutdown=%d "
            "kicked_without_poller: %d",
            pollset, worker, kick_state_string(worker->kick_state),
            pollset->shutting_down, pollset->kicked_without_poller);
  }
  if (pollset->kicked_without_poller) {
    pollset->kicked_without_poller = false;
    GPR_TIMER_END("begin_worker", 0);
    return false;
  }
  GPR_TIMER_END("begin_worker", 0);
  return worker->kick_state == DESIGNATED_POLLER && !pollset->shutting_down;
}
static bool check_neighborhood_for_available_poller(
    grpc_exec_ctx *exec_ctx, pollset_neighborhood *neighborhood) {
  GPR_TIMER_BEGIN("check_neighborhood_for_available_poller", 0);
  bool found_worker = false;
  do {
    grpc_pollset *inspect = neighborhood->active_root;
    if (inspect == NULL) {
      break;
    }
    gpr_mu_lock(&inspect->mu);
    GPR_ASSERT(!inspect->seen_inactive);
    grpc_pollset_worker *inspect_worker = inspect->root_worker;
    if (inspect_worker != NULL) {
      do {
        switch (inspect_worker->kick_state) {
          case UNKICKED:
            if (gpr_atm_no_barrier_cas(&g_active_poller, 0,
                                       (gpr_atm)inspect_worker)) {
              if (GRPC_TRACER_ON(grpc_polling_trace)) {
                gpr_log(GPR_DEBUG, " .. choose next poller to be %p",
                        inspect_worker);
              }
              SET_KICK_STATE(inspect_worker, DESIGNATED_POLLER);
              if (inspect_worker->initialized_cv) {
                GPR_TIMER_MARK("signal worker", 0);
                GRPC_STATS_INC_POLLSET_KICK_WAKEUP_CV(exec_ctx);
                gpr_cv_signal(&inspect_worker->cv);
              }
            } else {
              if (GRPC_TRACER_ON(grpc_polling_trace)) {
                gpr_log(GPR_DEBUG, " .. beaten to choose next poller");
              }
            }
            found_worker = true;
            break;
          case KICKED:
            break;
          case DESIGNATED_POLLER:
            found_worker = true;
            break;
        }
        inspect_worker = inspect_worker->next;
      } while (!found_worker && inspect_worker != inspect->root_worker);
    }
    if (!found_worker) {
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_DEBUG, " .. mark pollset %p inactive", inspect);
      }
      inspect->seen_inactive = true;
      if (inspect == neighborhood->active_root) {
        neighborhood->active_root =
            inspect->next == inspect ? NULL : inspect->next;
      }
      inspect->next->prev = inspect->prev;
      inspect->prev->next = inspect->next;
      inspect->next = inspect->prev = NULL;
    }
    gpr_mu_unlock(&inspect->mu);
  } while (!found_worker);
  GPR_TIMER_END("check_neighborhood_for_available_poller", 0);
  return found_worker;
}
static void end_worker(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                       grpc_pollset_worker *worker,
                       grpc_pollset_worker **worker_hdl) {
  GPR_TIMER_BEGIN("end_worker", 0);
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "PS:%p END_WORKER:%p", pollset, worker);
  }
  if (worker_hdl != NULL) *worker_hdl = NULL;
  SET_KICK_STATE(worker, KICKED);
  grpc_closure_list_move(&worker->schedule_on_end_work,
                         &exec_ctx->closure_list);
  if (gpr_atm_no_barrier_load(&g_active_poller) == (gpr_atm)worker) {
    if (worker->next != worker && worker->next->kick_state == UNKICKED) {
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_DEBUG, " .. choose next poller to be peer %p", worker);
      }
      GPR_ASSERT(worker->next->initialized_cv);
      gpr_atm_no_barrier_store(&g_active_poller, (gpr_atm)worker->next);
      SET_KICK_STATE(worker->next, DESIGNATED_POLLER);
      gpr_cv_signal(&worker->next->cv);
      if (grpc_exec_ctx_has_work(exec_ctx)) {
        gpr_mu_unlock(&pollset->mu);
        grpc_exec_ctx_flush(exec_ctx);
        gpr_mu_lock(&pollset->mu);
      }
    } else {
      gpr_atm_no_barrier_store(&g_active_poller, 0);
      size_t poller_neighborhood_idx =
          (size_t)(pollset->neighborhood - g_neighborhoods);
      gpr_mu_unlock(&pollset->mu);
      bool found_worker = false;
      bool scan_state[MAX_NEIGHBORHOODS];
      for (size_t i = 0; !found_worker && i < g_num_neighborhoods; i++) {
        pollset_neighborhood *neighborhood =
            &g_neighborhoods[(poller_neighborhood_idx + i) %
                             g_num_neighborhoods];
        if (gpr_mu_trylock(&neighborhood->mu)) {
          found_worker =
              check_neighborhood_for_available_poller(exec_ctx, neighborhood);
          gpr_mu_unlock(&neighborhood->mu);
          scan_state[i] = true;
        } else {
          scan_state[i] = false;
        }
      }
      for (size_t i = 0; !found_worker && i < g_num_neighborhoods; i++) {
        if (scan_state[i]) continue;
        pollset_neighborhood *neighborhood =
            &g_neighborhoods[(poller_neighborhood_idx + i) %
                             g_num_neighborhoods];
        gpr_mu_lock(&neighborhood->mu);
        found_worker =
            check_neighborhood_for_available_poller(exec_ctx, neighborhood);
        gpr_mu_unlock(&neighborhood->mu);
      }
      grpc_exec_ctx_flush(exec_ctx);
      gpr_mu_lock(&pollset->mu);
    }
  } else if (grpc_exec_ctx_has_work(exec_ctx)) {
    gpr_mu_unlock(&pollset->mu);
    grpc_exec_ctx_flush(exec_ctx);
    gpr_mu_lock(&pollset->mu);
  }
  if (worker->initialized_cv) {
    gpr_cv_destroy(&worker->cv);
  }
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, " .. remove worker");
  }
  if (EMPTIED == worker_remove(pollset, worker)) {
    pollset_maybe_finish_shutdown(exec_ctx, pollset);
  }
  GPR_ASSERT(gpr_atm_no_barrier_load(&g_active_poller) != (gpr_atm)worker);
  GPR_TIMER_END("end_worker", 0);
}
static grpc_error *pollset_work(grpc_exec_ctx *exec_ctx, grpc_pollset *ps,
                                grpc_pollset_worker **worker_hdl,
                                gpr_timespec now, gpr_timespec deadline) {
  grpc_pollset_worker worker;
  grpc_error *error = GRPC_ERROR_NONE;
  static const char *err_desc = "pollset_work";
  GPR_TIMER_BEGIN("pollset_work", 0);
  if (ps->kicked_without_poller) {
    ps->kicked_without_poller = false;
    GPR_TIMER_END("pollset_work", 0);
    return GRPC_ERROR_NONE;
  }
  if (begin_worker(ps, &worker, worker_hdl, &now, deadline)) {
    gpr_tls_set(&g_current_thread_pollset, (intptr_t)ps);
    gpr_tls_set(&g_current_thread_worker, (intptr_t)&worker);
    GPR_ASSERT(!ps->shutting_down);
    GPR_ASSERT(!ps->seen_inactive);
    gpr_mu_unlock(&ps->mu);
    if (gpr_atm_acq_load(&g_epoll_set.cursor) ==
        gpr_atm_acq_load(&g_epoll_set.num_events)) {
      append_error(&error, do_epoll_wait(exec_ctx, ps, now, deadline),
                   err_desc);
    }
    append_error(&error, process_epoll_events(exec_ctx, ps), err_desc);
    gpr_mu_lock(&ps->mu);
    gpr_tls_set(&g_current_thread_worker, 0);
  } else {
    gpr_tls_set(&g_current_thread_pollset, (intptr_t)ps);
  }
  end_worker(exec_ctx, ps, &worker, worker_hdl);
  gpr_tls_set(&g_current_thread_pollset, 0);
  GPR_TIMER_END("pollset_work", 0);
  return error;
}
static grpc_error *pollset_kick(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                                grpc_pollset_worker *specific_worker) {
  GPR_TIMER_BEGIN("pollset_kick", 0);
  GRPC_STATS_INC_POLLSET_KICK(exec_ctx);
  grpc_error *ret_err = GRPC_ERROR_NONE;
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_strvec log;
    gpr_strvec_init(&log);
    char *tmp;
    gpr_asprintf(
        &tmp, "PS:%p KICK:%p curps=%p curworker=%p root=%p", pollset,
        specific_worker, (void *)gpr_tls_get(&g_current_thread_pollset),
        (void *)gpr_tls_get(&g_current_thread_worker), pollset->root_worker);
    gpr_strvec_add(&log, tmp);
    if (pollset->root_worker != NULL) {
      gpr_asprintf(&tmp, " {kick_state=%s next=%p {kick_state=%s}}",
                   kick_state_string(pollset->root_worker->kick_state),
                   pollset->root_worker->next,
                   kick_state_string(pollset->root_worker->next->kick_state));
      gpr_strvec_add(&log, tmp);
    }
    if (specific_worker != NULL) {
      gpr_asprintf(&tmp, " worker_kick_state=%s",
                   kick_state_string(specific_worker->kick_state));
      gpr_strvec_add(&log, tmp);
    }
    tmp = gpr_strvec_flatten(&log, NULL);
    gpr_strvec_destroy(&log);
    gpr_log(GPR_ERROR, "%s", tmp);
    gpr_free(tmp);
  }
  if (specific_worker == NULL) {
    if (gpr_tls_get(&g_current_thread_pollset) != (intptr_t)pollset) {
      grpc_pollset_worker *root_worker = pollset->root_worker;
      if (root_worker == NULL) {
        GRPC_STATS_INC_POLLSET_KICKED_WITHOUT_POLLER(exec_ctx);
        pollset->kicked_without_poller = true;
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_ERROR, " .. kicked_without_poller");
        }
        goto done;
      }
      grpc_pollset_worker *next_worker = root_worker->next;
      if (root_worker->kick_state == KICKED) {
        GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_ERROR, " .. already kicked %p", root_worker);
        }
        SET_KICK_STATE(root_worker, KICKED);
        goto done;
      } else if (next_worker->kick_state == KICKED) {
        GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_ERROR, " .. already kicked %p", next_worker);
        }
        SET_KICK_STATE(next_worker, KICKED);
        goto done;
      } else if (root_worker ==
                     next_worker &&
                 root_worker == (grpc_pollset_worker *)gpr_atm_no_barrier_load(
                                    &g_active_poller)) {
        GRPC_STATS_INC_POLLSET_KICK_WAKEUP_FD(exec_ctx);
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_ERROR, " .. kicked %p", root_worker);
        }
        SET_KICK_STATE(root_worker, KICKED);
        ret_err = grpc_wakeup_fd_wakeup(&global_wakeup_fd);
        goto done;
      } else if (next_worker->kick_state == UNKICKED) {
        GRPC_STATS_INC_POLLSET_KICK_WAKEUP_CV(exec_ctx);
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_ERROR, " .. kicked %p", next_worker);
        }
        GPR_ASSERT(next_worker->initialized_cv);
        SET_KICK_STATE(next_worker, KICKED);
        gpr_cv_signal(&next_worker->cv);
        goto done;
      } else if (next_worker->kick_state == DESIGNATED_POLLER) {
        if (root_worker->kick_state != DESIGNATED_POLLER) {
          if (GRPC_TRACER_ON(grpc_polling_trace)) {
            gpr_log(
                GPR_ERROR,
                " .. kicked root non-poller %p (initialized_cv=%d) (poller=%p)",
                root_worker, root_worker->initialized_cv, next_worker);
          }
          SET_KICK_STATE(root_worker, KICKED);
          if (root_worker->initialized_cv) {
            GRPC_STATS_INC_POLLSET_KICK_WAKEUP_CV(exec_ctx);
            gpr_cv_signal(&root_worker->cv);
          }
          goto done;
        } else {
          GRPC_STATS_INC_POLLSET_KICK_WAKEUP_FD(exec_ctx);
          if (GRPC_TRACER_ON(grpc_polling_trace)) {
            gpr_log(GPR_ERROR, " .. non-root poller %p (root=%p)", next_worker,
                    root_worker);
          }
          SET_KICK_STATE(next_worker, KICKED);
          ret_err = grpc_wakeup_fd_wakeup(&global_wakeup_fd);
          goto done;
        }
      } else {
        GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
        GPR_ASSERT(next_worker->kick_state == KICKED);
        SET_KICK_STATE(next_worker, KICKED);
        goto done;
      }
    } else {
      GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_ERROR, " .. kicked while waking up");
      }
      goto done;
    }
    GPR_UNREACHABLE_CODE(goto done);
  }
  if (specific_worker->kick_state == KICKED) {
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, " .. specific worker already kicked");
    }
    goto done;
  } else if (gpr_tls_get(&g_current_thread_worker) ==
             (intptr_t)specific_worker) {
    GRPC_STATS_INC_POLLSET_KICK_OWN_THREAD(exec_ctx);
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, " .. mark %p kicked", specific_worker);
    }
    SET_KICK_STATE(specific_worker, KICKED);
    goto done;
  } else if (specific_worker ==
             (grpc_pollset_worker *)gpr_atm_no_barrier_load(&g_active_poller)) {
    GRPC_STATS_INC_POLLSET_KICK_WAKEUP_FD(exec_ctx);
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, " .. kick active poller");
    }
    SET_KICK_STATE(specific_worker, KICKED);
    ret_err = grpc_wakeup_fd_wakeup(&global_wakeup_fd);
    goto done;
  } else if (specific_worker->initialized_cv) {
    GRPC_STATS_INC_POLLSET_KICK_WAKEUP_CV(exec_ctx);
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, " .. kick waiting worker");
    }
    SET_KICK_STATE(specific_worker, KICKED);
    gpr_cv_signal(&specific_worker->cv);
    goto done;
  } else {
    GRPC_STATS_INC_POLLSET_KICKED_AGAIN(exec_ctx);
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_ERROR, " .. kick non-waiting worker");
    }
    SET_KICK_STATE(specific_worker, KICKED);
    goto done;
  }
done:
  GPR_TIMER_END("pollset_kick", 0);
  return ret_err;
}
static void pollset_add_fd(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                           grpc_fd *fd) {}
static grpc_pollset_set *pollset_set_create(void) {
  return (grpc_pollset_set *)((intptr_t)0xdeafbeef);
}
static void pollset_set_destroy(grpc_exec_ctx *exec_ctx,
                                grpc_pollset_set *pss) {}
static void pollset_set_add_fd(grpc_exec_ctx *exec_ctx, grpc_pollset_set *pss,
                               grpc_fd *fd) {}
static void pollset_set_del_fd(grpc_exec_ctx *exec_ctx, grpc_pollset_set *pss,
                               grpc_fd *fd) {}
static void pollset_set_add_pollset(grpc_exec_ctx *exec_ctx,
                                    grpc_pollset_set *pss, grpc_pollset *ps) {}
static void pollset_set_del_pollset(grpc_exec_ctx *exec_ctx,
                                    grpc_pollset_set *pss, grpc_pollset *ps) {}
static void pollset_set_add_pollset_set(grpc_exec_ctx *exec_ctx,
                                        grpc_pollset_set *bag,
                                        grpc_pollset_set *item) {}
static void pollset_set_del_pollset_set(grpc_exec_ctx *exec_ctx,
                                        grpc_pollset_set *bag,
                                        grpc_pollset_set *item) {}
static void shutdown_engine(void) {
  fd_global_shutdown();
  pollset_global_shutdown();
  epoll_set_shutdown();
}
static const grpc_event_engine_vtable vtable = {
    .pollset_size = sizeof(grpc_pollset),
    .fd_create = fd_create,
    .fd_wrapped_fd = fd_wrapped_fd,
    .fd_orphan = fd_orphan,
    .fd_shutdown = fd_shutdown,
    .fd_is_shutdown = fd_is_shutdown,
    .fd_notify_on_read = fd_notify_on_read,
    .fd_notify_on_write = fd_notify_on_write,
    .fd_get_read_notifier_pollset = fd_get_read_notifier_pollset,
    .pollset_init = pollset_init,
    .pollset_shutdown = pollset_shutdown,
    .pollset_destroy = pollset_destroy,
    .pollset_work = pollset_work,
    .pollset_kick = pollset_kick,
    .pollset_add_fd = pollset_add_fd,
    .pollset_set_create = pollset_set_create,
    .pollset_set_destroy = pollset_set_destroy,
    .pollset_set_add_pollset = pollset_set_add_pollset,
    .pollset_set_del_pollset = pollset_set_del_pollset,
    .pollset_set_add_pollset_set = pollset_set_add_pollset_set,
    .pollset_set_del_pollset_set = pollset_set_del_pollset_set,
    .pollset_set_add_fd = pollset_set_add_fd,
    .pollset_set_del_fd = pollset_set_del_fd,
    .shutdown_engine = shutdown_engine,
};
const grpc_event_engine_vtable *grpc_init_epoll1_linux(bool explicit_request) {
  if (!grpc_has_wakeup_fd()) {
    return NULL;
  }
  if (!epoll_set_init()) {
    return NULL;
  }
  fd_global_init();
  if (!GRPC_LOG_IF_ERROR("pollset_global_init", pollset_global_init())) {
    fd_global_shutdown();
    epoll_set_shutdown();
    return NULL;
  }
  return &vtable;
}
#else
#if defined(GRPC_POSIX_SOCKET)
#include "src/core/lib/iomgr/ev_posix.h"
const grpc_event_engine_vtable *grpc_init_epoll1_linux(bool explicit_request) {
  return NULL;
}
#endif
#endif
