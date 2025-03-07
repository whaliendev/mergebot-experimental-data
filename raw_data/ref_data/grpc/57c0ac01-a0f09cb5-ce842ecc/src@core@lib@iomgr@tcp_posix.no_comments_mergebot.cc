#include <grpc/support/port_platform.h>
#include "src/core/lib/iomgr/port.h"
#ifdef GRPC_POSIX_SOCKET_TCP
#include "src/core/lib/iomgr/tcp_posix.h"
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/buffer_list.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef TCP_INQ
#define TCP_INQ 36
#define TCP_CM_INQ TCP_INQ
#endif
#ifdef GRPC_HAVE_MSG_NOSIGNAL
#define SENDMSG_FLAGS MSG_NOSIGNAL
#else
#define SENDMSG_FLAGS 0
#endif
#ifdef GRPC_MSG_IOVLEN_TYPE
typedef GRPC_MSG_IOVLEN_TYPE msg_iovlen_type;
#else
typedef size_t msg_iovlen_type;
#endif
extern grpc_core::TraceFlag grpc_tcp_trace;
namespace {
struct grpc_tcp {
  grpc_endpoint base;
  grpc_fd* em_fd;
  int fd;
  bool is_first_read;
  double target_length;
  double bytes_read_this_round;
  grpc_core::RefCount refcount;
  gpr_atm shutdown_count;
  int min_read_chunk_size;
  int max_read_chunk_size;
  grpc_slice_buffer last_read_buffer;
  grpc_slice_buffer* incoming_buffer;
  int inq;
  bool inq_capable;
  grpc_slice_buffer* outgoing_buffer;
  size_t outgoing_byte_idx;
  grpc_closure* read_cb;
  grpc_closure* write_cb;
  grpc_closure* release_fd_cb;
  int* release_fd;
  grpc_closure read_done_closure;
  grpc_closure write_done_closure;
  grpc_closure error_closure;
  char* peer_string;
  grpc_resource_user* resource_user;
  grpc_resource_user_slice_allocator slice_allocator;
  grpc_core::TracedBuffer* tb_head;
  gpr_mu tb_mu;
  void* outgoing_buffer_arg;
  int bytes_counter;
  bool socket_ts_enabled;
  bool ts_capable;
  gpr_atm stop_error_notification;
};
struct backup_poller {
  gpr_mu* pollset_mu;
  grpc_closure run_poller;
};
}
#define BACKUP_POLLER_POLLSET(b) ((grpc_pollset*)((b) + 1))
static gpr_atm g_uncovered_notifications_pending;
static gpr_atm g_backup_poller;
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_drop_uncovered_then_handle_write(void* arg ,
                                                 grpc_error* error);
static void done_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p destroy", p);
  }
  grpc_pollset_destroy(BACKUP_POLLER_POLLSET(p));
  gpr_free(p);
}
static void run_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p run", p);
  }
  gpr_mu_lock(p->pollset_mu);
  grpc_millis deadline = grpc_core::ExecCtx::Get()->Now() + 10 * GPR_MS_PER_SEC;
  GRPC_STATS_INC_TCP_BACKUP_POLLER_POLLS();
  GRPC_LOG_IF_ERROR(
      "backup_poller:pollset_work",
      grpc_pollset_work(BACKUP_POLLER_POLLSET(p), nullptr, deadline));
  gpr_mu_unlock(p->pollset_mu);
  if (gpr_atm_no_barrier_load(&g_uncovered_notifications_pending) == 1 &&
      gpr_atm_full_cas(&g_uncovered_notifications_pending, 1, 0)) {
    gpr_mu_lock(p->pollset_mu);
    bool cas_ok = gpr_atm_full_cas(&g_backup_poller, (gpr_atm)p, 0);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p done cas_ok=%d", p, cas_ok);
    }
    gpr_mu_unlock(p->pollset_mu);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p shutdown", p);
    }
    grpc_pollset_shutdown(BACKUP_POLLER_POLLSET(p),
                          GRPC_CLOSURE_INIT(&p->run_poller, done_poller, p,
                                            grpc_schedule_on_exec_ctx));
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p reschedule", p);
    }
    grpc_core::Executor::Run(&p->run_poller, GRPC_ERROR_NONE,
                             grpc_core::ExecutorType::DEFAULT,
                             grpc_core::ExecutorJobType::LONG);
  }
}
static void drop_uncovered(grpc_tcp* ) {
  backup_poller* p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller);
  gpr_atm old_count =
      gpr_atm_full_fetch_add(&g_uncovered_notifications_pending, -1);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p uncover cnt %d->%d", p,
            static_cast<int>(old_count), static_cast<int>(old_count) - 1);
  }
  GPR_ASSERT(old_count != 1);
}
static void cover_self(grpc_tcp* tcp) {
  backup_poller* p;
  gpr_atm old_count =
      gpr_atm_no_barrier_fetch_add(&g_uncovered_notifications_pending, 2);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER: cover cnt %d->%d",
            static_cast<int>(old_count), 2 + static_cast<int>(old_count));
  }
  if (old_count == 0) {
    GRPC_STATS_INC_TCP_BACKUP_POLLERS_CREATED();
    p = static_cast<backup_poller*>(
        gpr_zalloc(sizeof(*p) + grpc_pollset_size()));
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p create", p);
    }
    grpc_pollset_init(BACKUP_POLLER_POLLSET(p), &p->pollset_mu);
    gpr_atm_rel_store(&g_backup_poller, (gpr_atm)p);
    grpc_core::Executor::Run(
        GRPC_CLOSURE_INIT(&p->run_poller, run_poller, p, nullptr),
        GRPC_ERROR_NONE, grpc_core::ExecutorType::DEFAULT,
        grpc_core::ExecutorJobType::LONG);
  } else {
    while ((p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller)) ==
           nullptr) {
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p add %p", p, tcp);
  }
  grpc_pollset_add_fd(BACKUP_POLLER_POLLSET(p), tcp->em_fd);
  if (old_count != 0) {
    drop_uncovered(tcp);
  }
}
static void notify_on_read(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_read", tcp);
  }
  grpc_fd_notify_on_read(tcp->em_fd, &tcp->read_done_closure);
}
static void notify_on_write(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_write", tcp);
  }
  if (!grpc_event_engine_run_in_background()) {
    cover_self(tcp);
  }
  grpc_fd_notify_on_write(tcp->em_fd, &tcp->write_done_closure);
}
static void tcp_drop_uncovered_then_handle_write(void* arg, grpc_error* error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_write: %s", arg, grpc_error_string(error));
  }
  drop_uncovered(static_cast<grpc_tcp*>(arg));
  tcp_handle_write(arg, error);
}
static void add_to_estimate(grpc_tcp* tcp, size_t bytes) {
  tcp->bytes_read_this_round += static_cast<double>(bytes);
}
static void finish_estimate(grpc_tcp* tcp) {
  if (tcp->bytes_read_this_round > tcp->target_length * 0.8) {
    tcp->target_length =
        GPR_MAX(2 * tcp->target_length, tcp->bytes_read_this_round);
  } else {
    tcp->target_length =
        0.99 * tcp->target_length + 0.01 * tcp->bytes_read_this_round;
  }
  tcp->bytes_read_this_round = 0;
}
static size_t get_target_read_size(grpc_tcp* tcp) {
  grpc_resource_quota* rq = grpc_resource_user_quota(tcp->resource_user);
  double pressure = grpc_resource_quota_get_memory_pressure(rq);
  double target =
      tcp->target_length * (pressure > 0.8 ? (1.0 - pressure) / 0.2 : 1.0);
  size_t sz = ((static_cast<size_t> GPR_CLAMP(target, tcp->min_read_chunk_size,
                                              tcp->max_read_chunk_size)) +
               255) &
              ~static_cast<size_t>(255);
  size_t rqmax = grpc_resource_quota_peek_size(rq);
  if (sz > rqmax / 16 && rqmax > 1024) {
    sz = rqmax / 16;
  }
  return sz;
}
static grpc_error* tcp_annotate_error(grpc_error* src_error, grpc_tcp* tcp) {
  return grpc_error_set_str(
      grpc_error_set_int(
          grpc_error_set_int(src_error, GRPC_ERROR_INT_FD, tcp->fd),
          GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE),
      GRPC_ERROR_STR_TARGET_ADDRESS,
      grpc_slice_from_copied_string(tcp->peer_string));
}
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_shutdown(grpc_endpoint* ep, grpc_error* why) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_fd_shutdown(tcp->em_fd, why);
  grpc_resource_user_shutdown(tcp->resource_user);
}
static void tcp_free(grpc_tcp* tcp) {
  grpc_fd_orphan(tcp->em_fd, tcp->release_fd_cb, tcp->release_fd,
                 "tcp_unref_orphan");
  grpc_slice_buffer_destroy_internal(&tcp->last_read_buffer);
  grpc_resource_user_unref(tcp->resource_user);
  gpr_free(tcp->peer_string);
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::Shutdown(
      &tcp->tb_head, tcp->outgoing_buffer_arg,
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("endpoint destroyed"));
  gpr_mu_unlock(&tcp->tb_mu);
  tcp->outgoing_buffer_arg = nullptr;
  gpr_mu_destroy(&tcp->tb_mu);
  gpr_free(tcp);
}
#ifndef NDEBUG
#define TCP_UNREF(tcp,reason) tcp_unref((tcp), (reason), DEBUG_LOCATION)
#define TCP_REF(tcp,reason) tcp_ref((tcp), (reason), DEBUG_LOCATION)
static void tcp_unref(grpc_tcp* tcp, const char* reason,
                      const grpc_core::DebugLocation& debug_location) {
  if (GPR_UNLIKELY(tcp->refcount.Unref(debug_location, reason))) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp, const char* reason,
                    const grpc_core::DebugLocation& debug_location) {
  tcp->refcount.Ref(debug_location, reason);
}
#else
#define TCP_UNREF(tcp,reason) tcp_unref((tcp))
#define TCP_REF(tcp,reason) tcp_ref((tcp))
static void tcp_unref(grpc_tcp* tcp) {
  if (GPR_UNLIKELY(tcp->refcount.Unref())) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp) { tcp->refcount.Ref(); }
#endif
static void tcp_destroy(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
static void call_read_cb(grpc_tcp* tcp, grpc_error* error) {
  grpc_closure* cb = tcp->read_cb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p call_cb %p %p:%p", tcp, cb, cb->cb, cb->cb_arg);
    size_t i;
    const char* str = grpc_error_string(error);
    gpr_log(GPR_INFO, "READ %p (peer=%s) error=%s", tcp, tcp->peer_string, str);
    if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
      for (i = 0; i < tcp->incoming_buffer->count; i++) {
        char* dump = grpc_dump_slice(tcp->incoming_buffer->slices[i],
                                     GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", dump);
        gpr_free(dump);
      }
    }
  }
  tcp->read_cb = nullptr;
  tcp->incoming_buffer = nullptr;
  GRPC_CLOSURE_RUN(cb, error);
}
#define MAX_READ_IOVEC 4
static void tcp_do_read(grpc_tcp* tcp) {
  GPR_TIMER_SCOPE("tcp_do_read", 0);
  struct msghdr msg;
  struct iovec iov[MAX_READ_IOVEC];
  ssize_t read_bytes;
  size_t total_read_bytes = 0;
  size_t iov_len =
      std::min<size_t>(MAX_READ_IOVEC, tcp->incoming_buffer->count);
#ifdef GRPC_LINUX_ERRQUEUE
  constexpr size_t cmsg_alloc_space =
      CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) + CMSG_SPACE(sizeof(int));
#else
  constexpr size_t cmsg_alloc_space = 24 ;
#endif
  char cmsgbuf[cmsg_alloc_space];
  for (size_t i = 0; i < iov_len; i++) {
    iov[i].iov_base = GRPC_SLICE_START_PTR(tcp->incoming_buffer->slices[i]);
    iov[i].iov_len = GRPC_SLICE_LENGTH(tcp->incoming_buffer->slices[i]);
  }
  do {
    tcp->inq = 1;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<msg_iovlen_type>(iov_len);
    if (tcp->inq_capable) {
      msg.msg_control = cmsgbuf;
      msg.msg_controllen = sizeof(cmsgbuf);
    } else {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }
    msg.msg_flags = 0;
    GRPC_STATS_INC_TCP_READ_OFFER(tcp->incoming_buffer->length);
    GRPC_STATS_INC_TCP_READ_OFFER_IOV_SIZE(tcp->incoming_buffer->count);
    do {
      GPR_TIMER_SCOPE("recvmsg", 0);
      GRPC_STATS_INC_SYSCALL_READ();
      read_bytes = recvmsg(tcp->fd, &msg, 0);
    } while (read_bytes < 0 && errno == EINTR);
    if (read_bytes <= 0 && total_read_bytes > 0) {
      tcp->inq = 1;
      break;
    }
    if (read_bytes < 0) {
      if (errno == EAGAIN) {
        finish_estimate(tcp);
        tcp->inq = 0;
        notify_on_read(tcp);
      } else {
        grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
        call_read_cb(tcp,
                     tcp_annotate_error(GRPC_OS_ERROR(errno, "recvmsg"), tcp));
        TCP_UNREF(tcp, "read");
      }
      return;
    }
    if (read_bytes == 0) {
      grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
      call_read_cb(
          tcp, tcp_annotate_error(
                   GRPC_ERROR_CREATE_FROM_STATIC_STRING("Socket closed"), tcp));
      TCP_UNREF(tcp, "read");
      return;
    }
    GRPC_STATS_INC_TCP_READ_SIZE(read_bytes);
    add_to_estimate(tcp, static_cast<size_t>(read_bytes));
    GPR_DEBUG_ASSERT((size_t)read_bytes <=
                     tcp->incoming_buffer->length - total_read_bytes);
#ifdef GRPC_HAVE_TCP_INQ
    if (tcp->inq_capable) {
      GPR_DEBUG_ASSERT(!(msg.msg_flags & MSG_CTRUNC));
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      for (; cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_TCP && cmsg->cmsg_type == TCP_CM_INQ &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
          tcp->inq = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
          break;
        }
      }
    }
#endif
    total_read_bytes += read_bytes;
    if (tcp->inq == 0 || total_read_bytes == tcp->incoming_buffer->length) {
      break;
    }
    size_t remaining = read_bytes;
    size_t j = 0;
    for (size_t i = 0; i < iov_len; i++) {
      if (remaining >= iov[i].iov_len) {
        remaining -= iov[i].iov_len;
        continue;
      }
      if (remaining > 0) {
        iov[j].iov_base = static_cast<char*>(iov[i].iov_base) + remaining;
        iov[j].iov_len = iov[i].iov_len - remaining;
        remaining = 0;
      } else {
        iov[j].iov_base = iov[i].iov_base;
        iov[j].iov_len = iov[i].iov_len;
      }
      ++j;
    }
    iov_len = j;
  } while (true);
  if (tcp->inq == 0) {
    finish_estimate(tcp);
  }
  GPR_DEBUG_ASSERT(total_read_bytes > 0);
  if (total_read_bytes < tcp->incoming_buffer->length) {
    grpc_slice_buffer_trim_end(tcp->incoming_buffer,
                               tcp->incoming_buffer->length - total_read_bytes,
                               &tcp->last_read_buffer);
  }
  call_read_cb(tcp, GRPC_ERROR_NONE);
  TCP_UNREF(tcp, "read");
}
static void tcp_read_allocation_done(void* tcpp, grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(tcpp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p read_allocation_done: %s", tcp,
            grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_do_read(tcp);
  }
}
static void tcp_continue_read(grpc_tcp* tcp) {
  size_t target_read_size = get_target_read_size(tcp);
  if (tcp->incoming_buffer->length == 0 &&
      tcp->incoming_buffer->count < MAX_READ_IOVEC) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "TCP:%p alloc_slices", tcp);
    }
    if (GPR_UNLIKELY(!grpc_resource_user_alloc_slices(&tcp->slice_allocator,
                                                      target_read_size, 1,
                                                      tcp->incoming_buffer))) {
      return;
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p do_read", tcp);
  }
  tcp_do_read(tcp);
}
static void tcp_handle_read(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_read: %s", tcp, grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_continue_read(tcp);
  }
}
static void tcp_read(grpc_endpoint* ep, grpc_slice_buffer* incoming_buffer,
                     grpc_closure* cb, bool urgent) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(tcp->read_cb == nullptr);
  tcp->read_cb = cb;
  tcp->incoming_buffer = incoming_buffer;
  grpc_slice_buffer_reset_and_unref_internal(incoming_buffer);
  grpc_slice_buffer_swap(incoming_buffer, &tcp->last_read_buffer);
  TCP_REF(tcp, "read");
  if (tcp->is_first_read) {
    tcp->is_first_read = false;
    notify_on_read(tcp);
  } else if (!urgent && tcp->inq == 0) {
    notify_on_read(tcp);
  } else {
    GRPC_CLOSURE_RUN(&tcp->read_done_closure, GRPC_ERROR_NONE);
  }
}
ssize_t tcp_send(int fd, const struct msghdr* msg) {
  GPR_TIMER_SCOPE("sendmsg", 1);
  ssize_t sent_length;
  do {
    GRPC_STATS_INC_SYSCALL_WRITE();
    sent_length = sendmsg(fd, msg, SENDMSG_FLAGS);
  } while (sent_length < 0 && errno == EINTR);
  return sent_length;
}
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length);
static void tcp_handle_error(void* arg , grpc_error* error);
#ifdef GRPC_LINUX_ERRQUEUE
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length) {
  if (!tcp->socket_ts_enabled) {
    uint32_t opt = grpc_core::kTimestampingSocketOptions;
    if (setsockopt(tcp->fd, SOL_SOCKET, SO_TIMESTAMPING,
                   static_cast<void*>(&opt), sizeof(opt)) != 0) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Failed to set timestamping options on the socket.");
      }
      return false;
    }
    tcp->bytes_counter = -1;
    tcp->socket_ts_enabled = true;
  }
  union {
    char cmsg_buf[CMSG_SPACE(sizeof(uint32_t))];
    struct cmsghdr align;
  } u;
  cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(u.cmsg_buf);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPING;
  cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
  *reinterpret_cast<int*>(CMSG_DATA(cmsg)) =
      grpc_core::kTimestampingRecordingOptions;
  msg->msg_control = u.cmsg_buf;
  msg->msg_controllen = CMSG_SPACE(sizeof(uint32_t));
  ssize_t length = tcp_send(tcp->fd, msg);
  *sent_length = length;
  if (sending_length == static_cast<size_t>(length)) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::AddNewEntry(
        &tcp->tb_head, static_cast<uint32_t>(tcp->bytes_counter + length),
        tcp->fd, tcp->outgoing_buffer_arg);
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
  return true;
}
struct cmsghdr* process_timestamp(grpc_tcp* tcp, msghdr* msg,
                                  struct cmsghdr* cmsg) {
  auto next_cmsg = CMSG_NXTHDR(msg, cmsg);
  cmsghdr* opt_stats = nullptr;
  if (next_cmsg == nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Received timestamp without extended error");
    }
    return cmsg;
  }
  if (next_cmsg->cmsg_level == SOL_SOCKET &&
      next_cmsg->cmsg_type == SCM_TIMESTAMPING_OPT_STATS) {
    opt_stats = next_cmsg;
    next_cmsg = CMSG_NXTHDR(msg, opt_stats);
    if (next_cmsg == nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Received timestamp without extended error");
      }
      return opt_stats;
    }
  }
  if (!(next_cmsg->cmsg_level == SOL_IP || next_cmsg->cmsg_level == SOL_IPV6) ||
      !(next_cmsg->cmsg_type == IP_RECVERR ||
        next_cmsg->cmsg_type == IPV6_RECVERR)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Unexpected control message");
    }
    return cmsg;
  }
  auto tss =
      reinterpret_cast<struct grpc_core::scm_timestamping*>(CMSG_DATA(cmsg));
  auto serr = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(next_cmsg));
  if (serr->ee_errno != ENOMSG ||
      serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
    gpr_log(GPR_ERROR, "Unexpected control message");
    return cmsg;
  }
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::ProcessTimestamp(&tcp->tb_head, serr, opt_stats,
                                            tss);
  gpr_mu_unlock(&tcp->tb_mu);
  return next_cmsg;
}
static void process_errors(grpc_tcp* tcp) {
  while (true) {
    struct iovec iov;
    iov.iov_base = nullptr;
    iov.iov_len = 0;
    struct msghdr msg;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 0;
    msg.msg_flags = 0;
    constexpr size_t cmsg_alloc_space =
        CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) +
        CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in)) +
        CMSG_SPACE(32 * NLA_ALIGN(NLA_HDRLEN + sizeof(uint64_t)));
    union {
      char rbuf[cmsg_alloc_space];
      struct cmsghdr align;
    } aligned_buf;
    memset(&aligned_buf, 0, sizeof(aligned_buf));
    msg.msg_control = aligned_buf.rbuf;
    msg.msg_controllen = sizeof(aligned_buf.rbuf);
    int r, saved_errno;
    do {
      r = recvmsg(tcp->fd, &msg, MSG_ERRQUEUE);
      saved_errno = errno;
    } while (r < 0 && saved_errno == EINTR);
    if (r == -1 && saved_errno == EAGAIN) {
      return;
    }
    if (r == -1) {
      return;
    }
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
      gpr_log(GPR_ERROR, "Error message was truncated.");
    }
    if (msg.msg_controllen == 0) {
      return;
    }
    bool seen = false;
    for (auto cmsg = CMSG_FIRSTHDR(&msg); cmsg && cmsg->cmsg_len;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_TIMESTAMPING) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
          gpr_log(GPR_INFO,
                  "unknown control message cmsg_level:%d cmsg_type:%d",
                  cmsg->cmsg_level, cmsg->cmsg_type);
        }
        return;
      }
      cmsg = process_timestamp(tcp, &msg, cmsg);
      seen = true;
    }
    if (!seen) {
      return;
    }
  }
}
static void tcp_handle_error(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_error: %s", tcp, grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE ||
      static_cast<bool>(gpr_atm_acq_load(&tcp->stop_error_notification))) {
    TCP_UNREF(tcp, "error-tracking");
    return;
  }
  process_errors(tcp);
  grpc_fd_set_readable(tcp->em_fd);
  grpc_fd_set_writable(tcp->em_fd);
  grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
}
#else
static bool tcp_write_with_timestamps(grpc_tcp* , struct msghdr* ,
                                      size_t ,
                                      ssize_t* ) {
  gpr_log(GPR_ERROR, "Write with timestamps not supported for this platform");
  GPR_ASSERT(0);
  return false;
}
static void tcp_handle_error(void* ,
                             grpc_error* ) {
  gpr_log(GPR_ERROR, "Error handling is not supported for this platform");
  GPR_ASSERT(0);
}
#endif
void tcp_shutdown_buffer_list(grpc_tcp* tcp) {
  if (tcp->outgoing_buffer_arg) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::Shutdown(
        &tcp->tb_head, tcp->outgoing_buffer_arg,
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("TracedBuffer list shutdown"));
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
}
#if defined(IOV_MAX) && IOV_MAX < 1000
#define MAX_WRITE_IOVEC IOV_MAX
#else
#define MAX_WRITE_IOVEC 1000
#endif
static bool tcp_flush(grpc_tcp* tcp, grpc_error** error) {
  struct msghdr msg;
  struct iovec iov[MAX_WRITE_IOVEC];
  msg_iovlen_type iov_size;
  ssize_t sent_length = 0;
  size_t sending_length;
  size_t trailing;
  size_t unwind_slice_idx;
  size_t unwind_byte_idx;
  size_t outgoing_slice_idx = 0;
  for (;;) {
    sending_length = 0;
    unwind_slice_idx = outgoing_slice_idx;
    unwind_byte_idx = tcp->outgoing_byte_idx;
    for (iov_size = 0; outgoing_slice_idx != tcp->outgoing_buffer->count &&
                       iov_size != MAX_WRITE_IOVEC;
         iov_size++) {
      iov[iov_size].iov_base =
          GRPC_SLICE_START_PTR(
              tcp->outgoing_buffer->slices[outgoing_slice_idx]) +
          tcp->outgoing_byte_idx;
      iov[iov_size].iov_len =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]) -
          tcp->outgoing_byte_idx;
      sending_length += iov[iov_size].iov_len;
      outgoing_slice_idx++;
      tcp->outgoing_byte_idx = 0;
    }
    GPR_ASSERT(iov_size > 0);
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = iov_size;
    msg.msg_flags = 0;
    bool tried_sending_message = false;
    if (tcp->outgoing_buffer_arg != nullptr) {
      if (!tcp->ts_capable ||
          !tcp_write_with_timestamps(tcp, &msg, sending_length, &sent_length)) {
        tcp->ts_capable = false;
        tcp_shutdown_buffer_list(tcp);
      } else {
        tried_sending_message = true;
      }
    }
    if (!tried_sending_message) {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
      GRPC_STATS_INC_TCP_WRITE_SIZE(sending_length);
      GRPC_STATS_INC_TCP_WRITE_IOV_SIZE(iov_size);
      sent_length = tcp_send(tcp->fd, &msg);
    }
    if (sent_length < 0) {
      if (errno == EAGAIN) {
        tcp->outgoing_byte_idx = unwind_byte_idx;
        for (size_t idx = 0; idx < unwind_slice_idx; ++idx) {
          grpc_slice_buffer_remove_first(tcp->outgoing_buffer);
        }
        return false;
      } else if (errno == EPIPE) {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      } else {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      }
    }
    GPR_ASSERT(tcp->outgoing_byte_idx == 0);
    tcp->bytes_counter += sent_length;
    trailing = sending_length - static_cast<size_t>(sent_length);
    while (trailing > 0) {
      size_t slice_length;
      outgoing_slice_idx--;
      slice_length =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]);
      if (slice_length > trailing) {
        tcp->outgoing_byte_idx = slice_length - trailing;
        break;
      } else {
        trailing -= slice_length;
      }
    }
    if (outgoing_slice_idx == tcp->outgoing_buffer->count) {
      *error = GRPC_ERROR_NONE;
      grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
      return true;
    }
  }
}
static void tcp_handle_write(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  grpc_closure* cb;
  if (error != GRPC_ERROR_NONE) {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    GRPC_CLOSURE_RUN(cb, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "write");
    return;
  }
  if (!tcp_flush(tcp, &error)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
    GPR_DEBUG_ASSERT(error == GRPC_ERROR_NONE);
  } else {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    GRPC_CLOSURE_RUN(cb, error);
    TCP_UNREF(tcp, "write");
  }
}
static void tcp_write(grpc_endpoint* ep, grpc_slice_buffer* buf,
                      grpc_closure* cb, void* arg) {
  GPR_TIMER_SCOPE("tcp_write", 0);
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_error* error = GRPC_ERROR_NONE;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    size_t i;
    for (i = 0; i < buf->count; i++) {
      gpr_log(GPR_INFO, "WRITE %p (peer=%s)", tcp, tcp->peer_string);
      if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
        char* data =
            grpc_dump_slice(buf->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", data);
        gpr_free(data);
      }
    }
  }
  GPR_ASSERT(tcp->write_cb == nullptr);
  tcp->outgoing_buffer_arg = arg;
  if (buf->length == 0) {
    GRPC_CLOSURE_RUN(cb,
                     grpc_fd_is_shutdown(tcp->em_fd)
                         ? tcp_annotate_error(
                               GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF"), tcp)
                         : GRPC_ERROR_NONE);
    tcp_shutdown_buffer_list(tcp);
    return;
  }
  tcp->outgoing_buffer = buf;
  tcp->outgoing_byte_idx = 0;
  if (arg) {
    GPR_ASSERT(grpc_event_engine_can_track_errors());
  }
  if (!tcp_flush(tcp, &error)) {
    TCP_REF(tcp, "write");
    tcp->write_cb = cb;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    GRPC_CLOSURE_RUN(cb, error);
  }
}
static void tcp_add_to_pollset(grpc_endpoint* ep, grpc_pollset* pollset) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_add_fd(pollset, tcp->em_fd);
}
static void tcp_add_to_pollset_set(grpc_endpoint* ep,
                                   grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_add_fd(pollset_set, tcp->em_fd);
}
static void tcp_delete_from_pollset_set(grpc_endpoint* ep,
                                        grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_del_fd(pollset_set, tcp->em_fd);
}
static char* tcp_get_peer(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return gpr_strdup(tcp->peer_string);
}
static int tcp_get_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->fd;
}
static grpc_resource_user* tcp_get_resource_user(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->resource_user;
}
static bool tcp_can_track_err(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  if (!grpc_event_engine_can_track_errors()) {
    return false;
  }
  struct sockaddr addr;
  socklen_t len = sizeof(addr);
  if (getsockname(tcp->fd, &addr, &len) < 0) {
    return false;
  }
  if (addr.sa_family == AF_INET || addr.sa_family == AF_INET6) {
    return true;
  }
  return false;
}
static const grpc_endpoint_vtable vtable = {tcp_read,
                                            tcp_write,
                                            tcp_add_to_pollset,
                                            tcp_add_to_pollset_set,
                                            tcp_delete_from_pollset_set,
                                            tcp_shutdown,
                                            tcp_destroy,
                                            tcp_get_resource_user,
                                            tcp_get_peer,
                                            tcp_get_fd,
                                            tcp_can_track_err};
#define MAX_CHUNK_SIZE 32 * 1024 * 1024
grpc_endpoint* grpc_tcp_create(grpc_fd* em_fd,
                               const grpc_channel_args* channel_args,
                               const char* peer_string) {
  int tcp_read_chunk_size = GRPC_TCP_DEFAULT_READ_SLICE_SIZE;
  int tcp_max_read_chunk_size = 4 * 1024 * 1024;
  int tcp_min_read_chunk_size = 256;
  grpc_resource_quota* resource_quota = grpc_resource_quota_create(nullptr);
  if (channel_args != nullptr) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 ==
          strcmp(channel_args->args[i].key, GRPC_ARG_TCP_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_min_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_max_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 ==
                 strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(resource_quota);
        resource_quota =
            grpc_resource_quota_ref_internal(static_cast<grpc_resource_quota*>(
                channel_args->args[i].value.pointer.p));
      }
    }
  }
  if (tcp_min_read_chunk_size > tcp_max_read_chunk_size) {
    tcp_min_read_chunk_size = tcp_max_read_chunk_size;
  }
  tcp_read_chunk_size = GPR_CLAMP(tcp_read_chunk_size, tcp_min_read_chunk_size,
                                  tcp_max_read_chunk_size);
  grpc_tcp* tcp = static_cast<grpc_tcp*>(gpr_malloc(sizeof(grpc_tcp)));
  tcp->base.vtable = &vtable;
  tcp->peer_string = gpr_strdup(peer_string);
  tcp->fd = grpc_fd_wrapped_fd(em_fd);
  tcp->read_cb = nullptr;
  tcp->write_cb = nullptr;
  tcp->release_fd_cb = nullptr;
  tcp->release_fd = nullptr;
  tcp->incoming_buffer = nullptr;
  tcp->target_length = static_cast<double>(tcp_read_chunk_size);
  tcp->min_read_chunk_size = tcp_min_read_chunk_size;
  tcp->max_read_chunk_size = tcp_max_read_chunk_size;
  tcp->bytes_read_this_round = 0;
  tcp->is_first_read = true;
  tcp->bytes_counter = -1;
  tcp->socket_ts_enabled = false;
  tcp->ts_capable = true;
  tcp->outgoing_buffer_arg = nullptr;
  new (&tcp->refcount) grpc_core::RefCount(1, &grpc_tcp_trace);
  gpr_atm_no_barrier_store(&tcp->shutdown_count, 0);
  tcp->em_fd = em_fd;
  grpc_slice_buffer_init(&tcp->last_read_buffer);
  tcp->resource_user = grpc_resource_user_create(resource_quota, peer_string);
  grpc_resource_user_slice_allocator_init(
      &tcp->slice_allocator, tcp->resource_user, tcp_read_allocation_done, tcp);
  grpc_resource_quota_unref_internal(resource_quota);
  gpr_mu_init(&tcp->tb_mu);
  tcp->tb_head = nullptr;
  GRPC_CLOSURE_INIT(&tcp->read_done_closure, tcp_handle_read, tcp,
                    grpc_schedule_on_exec_ctx);
  if (grpc_event_engine_run_in_background()) {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure, tcp_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  } else {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure,
                      tcp_drop_uncovered_then_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  }
  tcp->inq = 1;
#ifdef GRPC_HAVE_TCP_INQ
  int one = 1;
  if (setsockopt(tcp->fd, SOL_TCP, TCP_INQ, &one, sizeof(one)) == 0) {
    tcp->inq_capable = true;
  } else {
    gpr_log(GPR_DEBUG, "cannot set inq fd=%d errno=%d", tcp->fd, errno);
    tcp->inq_capable = false;
  }
#else
  tcp->inq_capable = false;
#endif
  if (grpc_event_engine_can_track_errors()) {
    TCP_REF(tcp, "error-tracking");
    gpr_atm_rel_store(&tcp->stop_error_notification, 0);
    GRPC_CLOSURE_INIT(&tcp->error_closure, tcp_handle_error, tcp,
                      grpc_schedule_on_exec_ctx);
    grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
  }
  return &tcp->base;
}
int grpc_tcp_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  return grpc_fd_wrapped_fd(tcp->em_fd);
}
void grpc_tcp_destroy_and_release_fd(grpc_endpoint* ep, int* fd,
                                     grpc_closure* done) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  tcp->release_fd = fd;
  tcp->release_fd_cb = done;
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
#endif
#ifdef GRPC_POSIX_SOCKET_TCP
#include "src/core/lib/iomgr/tcp_posix.h"
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/buffer_list.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef TCP_INQ
#define TCP_INQ 36
#define TCP_CM_INQ TCP_INQ
#endif
#ifdef GRPC_HAVE_MSG_NOSIGNAL
#define SENDMSG_FLAGS MSG_NOSIGNAL
#else
#define SENDMSG_FLAGS 0
#endif
#ifdef GRPC_MSG_IOVLEN_TYPE
typedef GRPC_MSG_IOVLEN_TYPE msg_iovlen_type;
#else
typedef size_t msg_iovlen_type;
#endif
extern grpc_core::TraceFlag grpc_tcp_trace;
namespace {
struct grpc_tcp {
  grpc_endpoint base;
  grpc_fd* em_fd;
  int fd;
  bool is_first_read;
  double target_length;
  double bytes_read_this_round;
  grpc_core::RefCount refcount;
  gpr_atm shutdown_count;
  int min_read_chunk_size;
  int max_read_chunk_size;
  grpc_slice_buffer last_read_buffer;
  grpc_slice_buffer* incoming_buffer;
  int inq;
  bool inq_capable;
  grpc_slice_buffer* outgoing_buffer;
  size_t outgoing_byte_idx;
  grpc_closure* read_cb;
  grpc_closure* write_cb;
  grpc_closure* release_fd_cb;
  int* release_fd;
  grpc_closure read_done_closure;
  grpc_closure write_done_closure;
  grpc_closure error_closure;
  char* peer_string;
  grpc_resource_user* resource_user;
  grpc_resource_user_slice_allocator slice_allocator;
  grpc_core::TracedBuffer* tb_head;
  gpr_mu tb_mu;
  void* outgoing_buffer_arg;
  int bytes_counter;
  bool socket_ts_enabled;
  bool ts_capable;
  gpr_atm stop_error_notification;
};
struct backup_poller {
  gpr_mu* pollset_mu;
  grpc_closure run_poller;
};
}
#define BACKUP_POLLER_POLLSET(b) ((grpc_pollset*)((b) + 1))
static gpr_atm g_uncovered_notifications_pending;
static gpr_atm g_backup_poller;
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_drop_uncovered_then_handle_write(void* arg ,
                                                 grpc_error* error);
static void done_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p destroy", p);
  }
  grpc_pollset_destroy(BACKUP_POLLER_POLLSET(p));
  gpr_free(p);
}
static void run_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p run", p);
  }
  gpr_mu_lock(p->pollset_mu);
  grpc_millis deadline = grpc_core::ExecCtx::Get()->Now() + 10 * GPR_MS_PER_SEC;
  GRPC_STATS_INC_TCP_BACKUP_POLLER_POLLS();
  GRPC_LOG_IF_ERROR(
      "backup_poller:pollset_work",
      grpc_pollset_work(BACKUP_POLLER_POLLSET(p), nullptr, deadline));
  gpr_mu_unlock(p->pollset_mu);
  if (gpr_atm_no_barrier_load(&g_uncovered_notifications_pending) == 1 &&
      gpr_atm_full_cas(&g_uncovered_notifications_pending, 1, 0)) {
    gpr_mu_lock(p->pollset_mu);
    bool cas_ok = gpr_atm_full_cas(&g_backup_poller, (gpr_atm)p, 0);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p done cas_ok=%d", p, cas_ok);
    }
    gpr_mu_unlock(p->pollset_mu);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p shutdown", p);
    }
    grpc_pollset_shutdown(BACKUP_POLLER_POLLSET(p),
                          GRPC_CLOSURE_INIT(&p->run_poller, done_poller, p,
                                            grpc_schedule_on_exec_ctx));
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p reschedule", p);
    }
    grpc_core::Executor::Run(&p->run_poller, GRPC_ERROR_NONE,
                             grpc_core::ExecutorType::DEFAULT,
                             grpc_core::ExecutorJobType::LONG);
  }
}
static void drop_uncovered(grpc_tcp* ) {
  backup_poller* p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller);
  gpr_atm old_count =
      gpr_atm_full_fetch_add(&g_uncovered_notifications_pending, -1);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p uncover cnt %d->%d", p,
            static_cast<int>(old_count), static_cast<int>(old_count) - 1);
  }
  GPR_ASSERT(old_count != 1);
}
static void cover_self(grpc_tcp* tcp) {
  backup_poller* p;
  gpr_atm old_count =
      gpr_atm_no_barrier_fetch_add(&g_uncovered_notifications_pending, 2);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER: cover cnt %d->%d",
            static_cast<int>(old_count), 2 + static_cast<int>(old_count));
  }
  if (old_count == 0) {
    GRPC_STATS_INC_TCP_BACKUP_POLLERS_CREATED();
    p = static_cast<backup_poller*>(
        gpr_zalloc(sizeof(*p) + grpc_pollset_size()));
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p create", p);
    }
    grpc_pollset_init(BACKUP_POLLER_POLLSET(p), &p->pollset_mu);
    gpr_atm_rel_store(&g_backup_poller, (gpr_atm)p);
    grpc_core::Executor::Run(
        GRPC_CLOSURE_INIT(&p->run_poller, run_poller, p, nullptr),
        GRPC_ERROR_NONE, grpc_core::ExecutorType::DEFAULT,
        grpc_core::ExecutorJobType::LONG);
  } else {
    while ((p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller)) ==
           nullptr) {
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p add %p", p, tcp);
  }
  grpc_pollset_add_fd(BACKUP_POLLER_POLLSET(p), tcp->em_fd);
  if (old_count != 0) {
    drop_uncovered(tcp);
  }
}
static void notify_on_read(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_read", tcp);
  }
  grpc_fd_notify_on_read(tcp->em_fd, &tcp->read_done_closure);
}
static void notify_on_write(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_write", tcp);
  }
  if (!grpc_event_engine_run_in_background()) {
    cover_self(tcp);
  }
  grpc_fd_notify_on_write(tcp->em_fd, &tcp->write_done_closure);
}
static void tcp_drop_uncovered_then_handle_write(void* arg, grpc_error* error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_write: %s", arg, grpc_error_string(error));
  }
  drop_uncovered(static_cast<grpc_tcp*>(arg));
  tcp_handle_write(arg, error);
}
static void add_to_estimate(grpc_tcp* tcp, size_t bytes) {
  tcp->bytes_read_this_round += static_cast<double>(bytes);
}
static void finish_estimate(grpc_tcp* tcp) {
  if (tcp->bytes_read_this_round > tcp->target_length * 0.8) {
    tcp->target_length =
        GPR_MAX(2 * tcp->target_length, tcp->bytes_read_this_round);
  } else {
    tcp->target_length =
        0.99 * tcp->target_length + 0.01 * tcp->bytes_read_this_round;
  }
  tcp->bytes_read_this_round = 0;
}
static size_t get_target_read_size(grpc_tcp* tcp) {
  grpc_resource_quota* rq = grpc_resource_user_quota(tcp->resource_user);
  double pressure = grpc_resource_quota_get_memory_pressure(rq);
  double target =
      tcp->target_length * (pressure > 0.8 ? (1.0 - pressure) / 0.2 : 1.0);
  size_t sz = ((static_cast<size_t> GPR_CLAMP(target, tcp->min_read_chunk_size,
                                              tcp->max_read_chunk_size)) +
               255) &
              ~static_cast<size_t>(255);
  size_t rqmax = grpc_resource_quota_peek_size(rq);
  if (sz > rqmax / 16 && rqmax > 1024) {
    sz = rqmax / 16;
  }
  return sz;
}
static grpc_error* tcp_annotate_error(grpc_error* src_error, grpc_tcp* tcp) {
  return grpc_error_set_str(
      grpc_error_set_int(
          grpc_error_set_int(src_error, GRPC_ERROR_INT_FD, tcp->fd),
          GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE),
      GRPC_ERROR_STR_TARGET_ADDRESS,
      grpc_slice_from_copied_string(tcp->peer_string));
}
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_shutdown(grpc_endpoint* ep, grpc_error* why) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_fd_shutdown(tcp->em_fd, why);
  grpc_resource_user_shutdown(tcp->resource_user);
}
static void tcp_free(grpc_tcp* tcp) {
  grpc_fd_orphan(tcp->em_fd, tcp->release_fd_cb, tcp->release_fd,
                 "tcp_unref_orphan");
  grpc_slice_buffer_destroy_internal(&tcp->last_read_buffer);
  grpc_resource_user_unref(tcp->resource_user);
  gpr_free(tcp->peer_string);
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::Shutdown(
      &tcp->tb_head, tcp->outgoing_buffer_arg,
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("endpoint destroyed"));
  gpr_mu_unlock(&tcp->tb_mu);
  tcp->outgoing_buffer_arg = nullptr;
  gpr_mu_destroy(&tcp->tb_mu);
  gpr_free(tcp);
}
#ifndef NDEBUG
#define TCP_UNREF(tcp,reason) tcp_unref((tcp), (reason), DEBUG_LOCATION)
#define TCP_REF(tcp,reason) tcp_ref((tcp), (reason), DEBUG_LOCATION)
static void tcp_unref(grpc_tcp* tcp, const char* reason,
                      const grpc_core::DebugLocation& debug_location) {
  if (GPR_UNLIKELY(tcp->refcount.Unref(debug_location, reason))) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp, const char* reason,
                    const grpc_core::DebugLocation& debug_location) {
  tcp->refcount.Ref(debug_location, reason);
}
#else
#define TCP_UNREF(tcp,reason) tcp_unref((tcp))
#define TCP_REF(tcp,reason) tcp_ref((tcp))
static void tcp_unref(grpc_tcp* tcp) {
  if (GPR_UNLIKELY(tcp->refcount.Unref())) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp) { tcp->refcount.Ref(); }
#endif
static void tcp_destroy(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
static void call_read_cb(grpc_tcp* tcp, grpc_error* error) {
  grpc_closure* cb = tcp->read_cb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p call_cb %p %p:%p", tcp, cb, cb->cb, cb->cb_arg);
    size_t i;
    const char* str = grpc_error_string(error);
    gpr_log(GPR_INFO, "READ %p (peer=%s) error=%s", tcp, tcp->peer_string, str);
    if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
      for (i = 0; i < tcp->incoming_buffer->count; i++) {
        char* dump = grpc_dump_slice(tcp->incoming_buffer->slices[i],
                                     GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", dump);
        gpr_free(dump);
      }
    }
  }
  tcp->read_cb = nullptr;
  tcp->incoming_buffer = nullptr;
  GRPC_CLOSURE_SCHED(cb, error);
}
#define MAX_READ_IOVEC 4
static void tcp_do_read(grpc_tcp* tcp) {
  GPR_TIMER_SCOPE("tcp_do_read", 0);
  struct msghdr msg;
  struct iovec iov[MAX_READ_IOVEC];
  ssize_t read_bytes;
  size_t total_read_bytes = 0;
  size_t iov_len =
      std::min<size_t>(MAX_READ_IOVEC, tcp->incoming_buffer->count);
#ifdef GRPC_LINUX_ERRQUEUE
  constexpr size_t cmsg_alloc_space =
      CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) + CMSG_SPACE(sizeof(int));
#else
  constexpr size_t cmsg_alloc_space = 24 ;
#endif
  char cmsgbuf[cmsg_alloc_space];
  for (size_t i = 0; i < iov_len; i++) {
    iov[i].iov_base = GRPC_SLICE_START_PTR(tcp->incoming_buffer->slices[i]);
    iov[i].iov_len = GRPC_SLICE_LENGTH(tcp->incoming_buffer->slices[i]);
  }
  do {
    tcp->inq = 1;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<msg_iovlen_type>(iov_len);
    if (tcp->inq_capable) {
      msg.msg_control = cmsgbuf;
      msg.msg_controllen = sizeof(cmsgbuf);
    } else {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }
    msg.msg_flags = 0;
    GRPC_STATS_INC_TCP_READ_OFFER(tcp->incoming_buffer->length);
    GRPC_STATS_INC_TCP_READ_OFFER_IOV_SIZE(tcp->incoming_buffer->count);
    do {
      GPR_TIMER_SCOPE("recvmsg", 0);
      GRPC_STATS_INC_SYSCALL_READ();
      read_bytes = recvmsg(tcp->fd, &msg, 0);
    } while (read_bytes < 0 && errno == EINTR);
    if (read_bytes <= 0 && total_read_bytes > 0) {
      tcp->inq = 1;
      break;
    }
    if (read_bytes < 0) {
      if (errno == EAGAIN) {
        finish_estimate(tcp);
        tcp->inq = 0;
        notify_on_read(tcp);
      } else {
        grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
        call_read_cb(tcp,
                     tcp_annotate_error(GRPC_OS_ERROR(errno, "recvmsg"), tcp));
        TCP_UNREF(tcp, "read");
      }
      return;
    }
    if (read_bytes == 0) {
      grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
      call_read_cb(
          tcp, tcp_annotate_error(
                   GRPC_ERROR_CREATE_FROM_STATIC_STRING("Socket closed"), tcp));
      TCP_UNREF(tcp, "read");
      return;
    }
    GRPC_STATS_INC_TCP_READ_SIZE(read_bytes);
    add_to_estimate(tcp, static_cast<size_t>(read_bytes));
    GPR_DEBUG_ASSERT((size_t)read_bytes <=
                     tcp->incoming_buffer->length - total_read_bytes);
#ifdef GRPC_HAVE_TCP_INQ
    if (tcp->inq_capable) {
      GPR_DEBUG_ASSERT(!(msg.msg_flags & MSG_CTRUNC));
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      for (; cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_TCP && cmsg->cmsg_type == TCP_CM_INQ &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
          tcp->inq = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
          break;
        }
      }
    }
#endif
    total_read_bytes += read_bytes;
    if (tcp->inq == 0 || total_read_bytes == tcp->incoming_buffer->length) {
      break;
    }
    size_t remaining = read_bytes;
    size_t j = 0;
    for (size_t i = 0; i < iov_len; i++) {
      if (remaining >= iov[i].iov_len) {
        remaining -= iov[i].iov_len;
        continue;
      }
      if (remaining > 0) {
        iov[j].iov_base = static_cast<char*>(iov[i].iov_base) + remaining;
        iov[j].iov_len = iov[i].iov_len - remaining;
        remaining = 0;
      } else {
        iov[j].iov_base = iov[i].iov_base;
        iov[j].iov_len = iov[i].iov_len;
      }
      ++j;
    }
    iov_len = j;
  } while (true);
  if (tcp->inq == 0) {
    finish_estimate(tcp);
  }
  GPR_DEBUG_ASSERT(total_read_bytes > 0);
  if (total_read_bytes < tcp->incoming_buffer->length) {
    grpc_slice_buffer_trim_end(tcp->incoming_buffer,
                               tcp->incoming_buffer->length - total_read_bytes,
                               &tcp->last_read_buffer);
  }
  call_read_cb(tcp, GRPC_ERROR_NONE);
  TCP_UNREF(tcp, "read");
}
static void tcp_read_allocation_done(void* tcpp, grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(tcpp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p read_allocation_done: %s", tcp,
            grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_do_read(tcp);
  }
}
static void tcp_continue_read(grpc_tcp* tcp) {
  size_t target_read_size = get_target_read_size(tcp);
  if (tcp->incoming_buffer->length == 0 &&
      tcp->incoming_buffer->count < MAX_READ_IOVEC) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "TCP:%p alloc_slices", tcp);
    }
    if (GPR_UNLIKELY(!grpc_resource_user_alloc_slices(&tcp->slice_allocator,
                                                      target_read_size, 1,
                                                      tcp->incoming_buffer))) {
      return;
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p do_read", tcp);
  }
  tcp_do_read(tcp);
}
static void tcp_handle_read(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_read: %s", tcp, grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_continue_read(tcp);
  }
}
static void tcp_read(grpc_endpoint* ep, grpc_slice_buffer* incoming_buffer,
                     grpc_closure* cb, bool urgent) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(tcp->read_cb == nullptr);
  tcp->read_cb = cb;
  tcp->incoming_buffer = incoming_buffer;
  grpc_slice_buffer_reset_and_unref_internal(incoming_buffer);
  grpc_slice_buffer_swap(incoming_buffer, &tcp->last_read_buffer);
  TCP_REF(tcp, "read");
  if (tcp->is_first_read) {
    tcp->is_first_read = false;
    notify_on_read(tcp);
  } else if (!urgent && tcp->inq == 0) {
    notify_on_read(tcp);
  } else {
    GRPC_CLOSURE_SCHED(&tcp->read_done_closure, GRPC_ERROR_NONE);
  }
}
ssize_t tcp_send(int fd, const struct msghdr* msg) {
  GPR_TIMER_SCOPE("sendmsg", 1);
  ssize_t sent_length;
  do {
    GRPC_STATS_INC_SYSCALL_WRITE();
    sent_length = sendmsg(fd, msg, SENDMSG_FLAGS);
  } while (sent_length < 0 && errno == EINTR);
  return sent_length;
}
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length);
static void tcp_handle_error(void* arg , grpc_error* error);
#ifdef GRPC_LINUX_ERRQUEUE
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length) {
  if (!tcp->socket_ts_enabled) {
    uint32_t opt = grpc_core::kTimestampingSocketOptions;
    if (setsockopt(tcp->fd, SOL_SOCKET, SO_TIMESTAMPING,
                   static_cast<void*>(&opt), sizeof(opt)) != 0) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Failed to set timestamping options on the socket.");
      }
      return false;
    }
    tcp->bytes_counter = -1;
    tcp->socket_ts_enabled = true;
  }
  union {
    char cmsg_buf[CMSG_SPACE(sizeof(uint32_t))];
    struct cmsghdr align;
  } u;
  cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(u.cmsg_buf);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPING;
  cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
  *reinterpret_cast<int*>(CMSG_DATA(cmsg)) =
      grpc_core::kTimestampingRecordingOptions;
  msg->msg_control = u.cmsg_buf;
  msg->msg_controllen = CMSG_SPACE(sizeof(uint32_t));
  ssize_t length = tcp_send(tcp->fd, msg);
  *sent_length = length;
  if (sending_length == static_cast<size_t>(length)) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::AddNewEntry(
        &tcp->tb_head, static_cast<uint32_t>(tcp->bytes_counter + length),
        tcp->fd, tcp->outgoing_buffer_arg);
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
  return true;
}
struct cmsghdr* process_timestamp(grpc_tcp* tcp, msghdr* msg,
                                  struct cmsghdr* cmsg) {
  auto next_cmsg = CMSG_NXTHDR(msg, cmsg);
  cmsghdr* opt_stats = nullptr;
  if (next_cmsg == nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Received timestamp without extended error");
    }
    return cmsg;
  }
  if (next_cmsg->cmsg_level == SOL_SOCKET &&
      next_cmsg->cmsg_type == SCM_TIMESTAMPING_OPT_STATS) {
    opt_stats = next_cmsg;
    next_cmsg = CMSG_NXTHDR(msg, opt_stats);
    if (next_cmsg == nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Received timestamp without extended error");
      }
      return opt_stats;
    }
  }
  if (!(next_cmsg->cmsg_level == SOL_IP || next_cmsg->cmsg_level == SOL_IPV6) ||
      !(next_cmsg->cmsg_type == IP_RECVERR ||
        next_cmsg->cmsg_type == IPV6_RECVERR)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Unexpected control message");
    }
    return cmsg;
  }
  auto tss =
      reinterpret_cast<struct grpc_core::scm_timestamping*>(CMSG_DATA(cmsg));
  auto serr = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(next_cmsg));
  if (serr->ee_errno != ENOMSG ||
      serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
    gpr_log(GPR_ERROR, "Unexpected control message");
    return cmsg;
  }
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::ProcessTimestamp(&tcp->tb_head, serr, opt_stats,
                                            tss);
  gpr_mu_unlock(&tcp->tb_mu);
  return next_cmsg;
}
static void process_errors(grpc_tcp* tcp) {
  while (true) {
    struct iovec iov;
    iov.iov_base = nullptr;
    iov.iov_len = 0;
    struct msghdr msg;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 0;
    msg.msg_flags = 0;
    constexpr size_t cmsg_alloc_space =
        CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) +
        CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in)) +
        CMSG_SPACE(32 * NLA_ALIGN(NLA_HDRLEN + sizeof(uint64_t)));
    union {
      char rbuf[cmsg_alloc_space];
      struct cmsghdr align;
    } aligned_buf;
    memset(&aligned_buf, 0, sizeof(aligned_buf));
    msg.msg_control = aligned_buf.rbuf;
    msg.msg_controllen = sizeof(aligned_buf.rbuf);
    int r, saved_errno;
    do {
      r = recvmsg(tcp->fd, &msg, MSG_ERRQUEUE);
      saved_errno = errno;
    } while (r < 0 && saved_errno == EINTR);
    if (r == -1 && saved_errno == EAGAIN) {
      return;
    }
    if (r == -1) {
      return;
    }
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
      gpr_log(GPR_ERROR, "Error message was truncated.");
    }
    if (msg.msg_controllen == 0) {
      return;
    }
    bool seen = false;
    for (auto cmsg = CMSG_FIRSTHDR(&msg); cmsg && cmsg->cmsg_len;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_TIMESTAMPING) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
          gpr_log(GPR_INFO,
                  "unknown control message cmsg_level:%d cmsg_type:%d",
                  cmsg->cmsg_level, cmsg->cmsg_type);
        }
        return;
      }
      cmsg = process_timestamp(tcp, &msg, cmsg);
      seen = true;
    }
    if (!seen) {
      return;
    }
  }
}
static void tcp_handle_error(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_error: %s", tcp, grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE ||
      static_cast<bool>(gpr_atm_acq_load(&tcp->stop_error_notification))) {
    TCP_UNREF(tcp, "error-tracking");
    return;
  }
  process_errors(tcp);
  grpc_fd_set_readable(tcp->em_fd);
  grpc_fd_set_writable(tcp->em_fd);
  grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
}
#else
static bool tcp_write_with_timestamps(grpc_tcp* , struct msghdr* ,
                                      size_t ,
                                      ssize_t* ) {
  gpr_log(GPR_ERROR, "Write with timestamps not supported for this platform");
  GPR_ASSERT(0);
  return false;
}
static void tcp_handle_error(void* ,
                             grpc_error* ) {
  gpr_log(GPR_ERROR, "Error handling is not supported for this platform");
  GPR_ASSERT(0);
}
#endif
void tcp_shutdown_buffer_list(grpc_tcp* tcp) {
  if (tcp->outgoing_buffer_arg) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::Shutdown(
        &tcp->tb_head, tcp->outgoing_buffer_arg,
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("TracedBuffer list shutdown"));
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
}
#if defined(IOV_MAX) && IOV_MAX < 1000
#define MAX_WRITE_IOVEC IOV_MAX
#else
#define MAX_WRITE_IOVEC 1000
#endif
static bool tcp_flush(grpc_tcp* tcp, grpc_error** error) {
  struct msghdr msg;
  struct iovec iov[MAX_WRITE_IOVEC];
  msg_iovlen_type iov_size;
  ssize_t sent_length = 0;
  size_t sending_length;
  size_t trailing;
  size_t unwind_slice_idx;
  size_t unwind_byte_idx;
  size_t outgoing_slice_idx = 0;
  for (;;) {
    sending_length = 0;
    unwind_slice_idx = outgoing_slice_idx;
    unwind_byte_idx = tcp->outgoing_byte_idx;
    for (iov_size = 0; outgoing_slice_idx != tcp->outgoing_buffer->count &&
                       iov_size != MAX_WRITE_IOVEC;
         iov_size++) {
      iov[iov_size].iov_base =
          GRPC_SLICE_START_PTR(
              tcp->outgoing_buffer->slices[outgoing_slice_idx]) +
          tcp->outgoing_byte_idx;
      iov[iov_size].iov_len =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]) -
          tcp->outgoing_byte_idx;
      sending_length += iov[iov_size].iov_len;
      outgoing_slice_idx++;
      tcp->outgoing_byte_idx = 0;
    }
    GPR_ASSERT(iov_size > 0);
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = iov_size;
    msg.msg_flags = 0;
    bool tried_sending_message = false;
    if (tcp->outgoing_buffer_arg != nullptr) {
      if (!tcp->ts_capable ||
          !tcp_write_with_timestamps(tcp, &msg, sending_length, &sent_length)) {
        tcp->ts_capable = false;
        tcp_shutdown_buffer_list(tcp);
      } else {
        tried_sending_message = true;
      }
    }
    if (!tried_sending_message) {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
      GRPC_STATS_INC_TCP_WRITE_SIZE(sending_length);
      GRPC_STATS_INC_TCP_WRITE_IOV_SIZE(iov_size);
      sent_length = tcp_send(tcp->fd, &msg);
    }
    if (sent_length < 0) {
      if (errno == EAGAIN) {
        tcp->outgoing_byte_idx = unwind_byte_idx;
        for (size_t idx = 0; idx < unwind_slice_idx; ++idx) {
          grpc_slice_buffer_remove_first(tcp->outgoing_buffer);
        }
        return false;
      } else if (errno == EPIPE) {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      } else {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      }
    }
    GPR_ASSERT(tcp->outgoing_byte_idx == 0);
    tcp->bytes_counter += sent_length;
    trailing = sending_length - static_cast<size_t>(sent_length);
    while (trailing > 0) {
      size_t slice_length;
      outgoing_slice_idx--;
      slice_length =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]);
      if (slice_length > trailing) {
        tcp->outgoing_byte_idx = slice_length - trailing;
        break;
      } else {
        trailing -= slice_length;
      }
    }
    if (outgoing_slice_idx == tcp->outgoing_buffer->count) {
      *error = GRPC_ERROR_NONE;
      grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
      return true;
    }
  }
}
static void tcp_handle_write(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  grpc_closure* cb;
  if (error != GRPC_ERROR_NONE) {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    GRPC_CLOSURE_SCHED(cb, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "write");
    return;
  }
  if (!tcp_flush(tcp, &error)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
    GPR_DEBUG_ASSERT(error == GRPC_ERROR_NONE);
  } else {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    GRPC_CLOSURE_RUN(cb, error);
    TCP_UNREF(tcp, "write");
  }
}
static void tcp_write(grpc_endpoint* ep, grpc_slice_buffer* buf,
                      grpc_closure* cb, void* arg) {
  GPR_TIMER_SCOPE("tcp_write", 0);
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_error* error = GRPC_ERROR_NONE;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    size_t i;
    for (i = 0; i < buf->count; i++) {
      gpr_log(GPR_INFO, "WRITE %p (peer=%s)", tcp, tcp->peer_string);
      if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
        char* data =
            grpc_dump_slice(buf->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", data);
        gpr_free(data);
      }
    }
  }
  GPR_ASSERT(tcp->write_cb == nullptr);
  tcp->outgoing_buffer_arg = arg;
  if (buf->length == 0) {
    GRPC_CLOSURE_SCHED(
        cb, grpc_fd_is_shutdown(tcp->em_fd)
                ? tcp_annotate_error(
                      GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF"), tcp)
                : GRPC_ERROR_NONE);
    tcp_shutdown_buffer_list(tcp);
    return;
  }
  tcp->outgoing_buffer = buf;
  tcp->outgoing_byte_idx = 0;
  if (arg) {
    GPR_ASSERT(grpc_event_engine_can_track_errors());
  }
  if (!tcp_flush(tcp, &error)) {
    TCP_REF(tcp, "write");
    tcp->write_cb = cb;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    GRPC_CLOSURE_SCHED(cb, error);
  }
}
static void tcp_add_to_pollset(grpc_endpoint* ep, grpc_pollset* pollset) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_add_fd(pollset, tcp->em_fd);
}
static void tcp_add_to_pollset_set(grpc_endpoint* ep,
                                   grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_add_fd(pollset_set, tcp->em_fd);
}
static void tcp_delete_from_pollset_set(grpc_endpoint* ep,
                                        grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_del_fd(pollset_set, tcp->em_fd);
}
static char* tcp_get_peer(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return gpr_strdup(tcp->peer_string);
}
static int tcp_get_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->fd;
}
static grpc_resource_user* tcp_get_resource_user(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->resource_user;
}
static bool tcp_can_track_err(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  if (!grpc_event_engine_can_track_errors()) {
    return false;
  }
  struct sockaddr addr;
  socklen_t len = sizeof(addr);
  if (getsockname(tcp->fd, &addr, &len) < 0) {
    return false;
  }
  if (addr.sa_family == AF_INET || addr.sa_family == AF_INET6) {
    return true;
  }
  return false;
}
static const grpc_endpoint_vtable vtable = {tcp_read,
                                            tcp_write,
                                            tcp_add_to_pollset,
                                            tcp_add_to_pollset_set,
                                            tcp_delete_from_pollset_set,
                                            tcp_shutdown,
                                            tcp_destroy,
                                            tcp_get_resource_user,
                                            tcp_get_peer,
                                            tcp_get_fd,
                                            tcp_can_track_err};
#define MAX_CHUNK_SIZE 32 * 1024 * 1024
grpc_endpoint* grpc_tcp_create(grpc_fd* em_fd,
                               const grpc_channel_args* channel_args,
                               const char* peer_string) {
  int tcp_read_chunk_size = GRPC_TCP_DEFAULT_READ_SLICE_SIZE;
  int tcp_max_read_chunk_size = 4 * 1024 * 1024;
  int tcp_min_read_chunk_size = 256;
  grpc_resource_quota* resource_quota = grpc_resource_quota_create(nullptr);
  if (channel_args != nullptr) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 ==
          strcmp(channel_args->args[i].key, GRPC_ARG_TCP_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_min_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_max_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 ==
                 strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(resource_quota);
        resource_quota =
            grpc_resource_quota_ref_internal(static_cast<grpc_resource_quota*>(
                channel_args->args[i].value.pointer.p));
      }
    }
  }
  if (tcp_min_read_chunk_size > tcp_max_read_chunk_size) {
    tcp_min_read_chunk_size = tcp_max_read_chunk_size;
  }
  tcp_read_chunk_size = GPR_CLAMP(tcp_read_chunk_size, tcp_min_read_chunk_size,
                                  tcp_max_read_chunk_size);
  grpc_tcp* tcp = static_cast<grpc_tcp*>(gpr_malloc(sizeof(grpc_tcp)));
  tcp->base.vtable = &vtable;
  tcp->peer_string = gpr_strdup(peer_string);
  tcp->fd = grpc_fd_wrapped_fd(em_fd);
  tcp->read_cb = nullptr;
  tcp->write_cb = nullptr;
  tcp->release_fd_cb = nullptr;
  tcp->release_fd = nullptr;
  tcp->incoming_buffer = nullptr;
  tcp->target_length = static_cast<double>(tcp_read_chunk_size);
  tcp->min_read_chunk_size = tcp_min_read_chunk_size;
  tcp->max_read_chunk_size = tcp_max_read_chunk_size;
  tcp->bytes_read_this_round = 0;
  tcp->is_first_read = true;
  tcp->bytes_counter = -1;
  tcp->socket_ts_enabled = false;
  tcp->ts_capable = true;
  tcp->outgoing_buffer_arg = nullptr;
  new (&tcp->refcount) grpc_core::RefCount(1, &grpc_tcp_trace);
  gpr_atm_no_barrier_store(&tcp->shutdown_count, 0);
  tcp->em_fd = em_fd;
  grpc_slice_buffer_init(&tcp->last_read_buffer);
  tcp->resource_user = grpc_resource_user_create(resource_quota, peer_string);
  grpc_resource_user_slice_allocator_init(
      &tcp->slice_allocator, tcp->resource_user, tcp_read_allocation_done, tcp);
  grpc_resource_quota_unref_internal(resource_quota);
  gpr_mu_init(&tcp->tb_mu);
  tcp->tb_head = nullptr;
  GRPC_CLOSURE_INIT(&tcp->read_done_closure, tcp_handle_read, tcp,
                    grpc_schedule_on_exec_ctx);
  if (grpc_event_engine_run_in_background()) {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure, tcp_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  } else {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure,
                      tcp_drop_uncovered_then_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  }
  tcp->inq = 1;
#ifdef GRPC_HAVE_TCP_INQ
  int one = 1;
  if (setsockopt(tcp->fd, SOL_TCP, TCP_INQ, &one, sizeof(one)) == 0) {
    tcp->inq_capable = true;
  } else {
    gpr_log(GPR_DEBUG, "cannot set inq fd=%d errno=%d", tcp->fd, errno);
    tcp->inq_capable = false;
  }
#else
  tcp->inq_capable = false;
#endif
  if (grpc_event_engine_can_track_errors()) {
    TCP_REF(tcp, "error-tracking");
    gpr_atm_rel_store(&tcp->stop_error_notification, 0);
    GRPC_CLOSURE_INIT(&tcp->error_closure, tcp_handle_error, tcp,
                      grpc_schedule_on_exec_ctx);
    grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
  }
  return &tcp->base;
}
int grpc_tcp_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  return grpc_fd_wrapped_fd(tcp->em_fd);
}
void grpc_tcp_destroy_and_release_fd(grpc_endpoint* ep, int* fd,
                                     grpc_closure* done) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  tcp->release_fd = fd;
  tcp->release_fd_cb = done;
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
#endif
#ifdef GRPC_POSIX_SOCKET_TCP
#include "src/core/lib/iomgr/tcp_posix.h"
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/buffer_list.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef TCP_INQ
#define TCP_INQ 36
#define TCP_CM_INQ TCP_INQ
#endif
#ifdef GRPC_HAVE_MSG_NOSIGNAL
#define SENDMSG_FLAGS MSG_NOSIGNAL
#else
#define SENDMSG_FLAGS 0
#endif
#ifdef GRPC_MSG_IOVLEN_TYPE
typedef GRPC_MSG_IOVLEN_TYPE msg_iovlen_type;
#else
typedef size_t msg_iovlen_type;
#endif
extern grpc_core::TraceFlag grpc_tcp_trace;
namespace {
struct grpc_tcp {
  grpc_endpoint base;
  grpc_fd* em_fd;
  int fd;
  bool is_first_read;
  double target_length;
  double bytes_read_this_round;
  grpc_core::RefCount refcount;
  gpr_atm shutdown_count;
  int min_read_chunk_size;
  int max_read_chunk_size;
  grpc_slice_buffer last_read_buffer;
  grpc_slice_buffer* incoming_buffer;
  int inq;
  bool inq_capable;
  grpc_slice_buffer* outgoing_buffer;
  size_t outgoing_byte_idx;
  grpc_closure* read_cb;
  grpc_closure* write_cb;
  grpc_closure* release_fd_cb;
  int* release_fd;
  grpc_closure read_done_closure;
  grpc_closure write_done_closure;
  grpc_closure error_closure;
  char* peer_string;
  grpc_resource_user* resource_user;
  grpc_resource_user_slice_allocator slice_allocator;
  grpc_core::TracedBuffer* tb_head;
  gpr_mu tb_mu;
  void* outgoing_buffer_arg;
  int bytes_counter;
  bool socket_ts_enabled;
  bool ts_capable;
  gpr_atm stop_error_notification;
};
struct backup_poller {
  gpr_mu* pollset_mu;
  grpc_closure run_poller;
};
}
#define BACKUP_POLLER_POLLSET(b) ((grpc_pollset*)((b) + 1))
static gpr_atm g_uncovered_notifications_pending;
static gpr_atm g_backup_poller;
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_drop_uncovered_then_handle_write(void* arg ,
                                                 grpc_error* error);
static void done_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p destroy", p);
  }
  grpc_pollset_destroy(BACKUP_POLLER_POLLSET(p));
  gpr_free(p);
}
static void run_poller(void* bp, grpc_error* ) {
  backup_poller* p = static_cast<backup_poller*>(bp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p run", p);
  }
  gpr_mu_lock(p->pollset_mu);
  grpc_millis deadline = grpc_core::ExecCtx::Get()->Now() + 10 * GPR_MS_PER_SEC;
  GRPC_STATS_INC_TCP_BACKUP_POLLER_POLLS();
  GRPC_LOG_IF_ERROR(
      "backup_poller:pollset_work",
      grpc_pollset_work(BACKUP_POLLER_POLLSET(p), nullptr, deadline));
  gpr_mu_unlock(p->pollset_mu);
  if (gpr_atm_no_barrier_load(&g_uncovered_notifications_pending) == 1 &&
      gpr_atm_full_cas(&g_uncovered_notifications_pending, 1, 0)) {
    gpr_mu_lock(p->pollset_mu);
    bool cas_ok = gpr_atm_full_cas(&g_backup_poller, (gpr_atm)p, 0);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p done cas_ok=%d", p, cas_ok);
    }
    gpr_mu_unlock(p->pollset_mu);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p shutdown", p);
    }
    grpc_pollset_shutdown(BACKUP_POLLER_POLLSET(p),
                          GRPC_CLOSURE_INIT(&p->run_poller, done_poller, p,
                                            grpc_schedule_on_exec_ctx));
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p reschedule", p);
    }
    grpc_core::Executor::Run(&p->run_poller, GRPC_ERROR_NONE,
                             grpc_core::ExecutorType::DEFAULT,
                             grpc_core::ExecutorJobType::LONG);
  }
}
static void drop_uncovered(grpc_tcp* ) {
  backup_poller* p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller);
  gpr_atm old_count =
      gpr_atm_full_fetch_add(&g_uncovered_notifications_pending, -1);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p uncover cnt %d->%d", p,
            static_cast<int>(old_count), static_cast<int>(old_count) - 1);
  }
  GPR_ASSERT(old_count != 1);
}
static void cover_self(grpc_tcp* tcp) {
  backup_poller* p;
  gpr_atm old_count =
      gpr_atm_no_barrier_fetch_add(&g_uncovered_notifications_pending, 2);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER: cover cnt %d->%d",
            static_cast<int>(old_count), 2 + static_cast<int>(old_count));
  }
  if (old_count == 0) {
    GRPC_STATS_INC_TCP_BACKUP_POLLERS_CREATED();
    p = static_cast<backup_poller*>(
        gpr_zalloc(sizeof(*p) + grpc_pollset_size()));
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "BACKUP_POLLER:%p create", p);
    }
    grpc_pollset_init(BACKUP_POLLER_POLLSET(p), &p->pollset_mu);
    gpr_atm_rel_store(&g_backup_poller, (gpr_atm)p);
    grpc_core::Executor::Run(
        GRPC_CLOSURE_INIT(&p->run_poller, run_poller, p, nullptr),
        GRPC_ERROR_NONE, grpc_core::ExecutorType::DEFAULT,
        grpc_core::ExecutorJobType::LONG);
  } else {
    while ((p = (backup_poller*)gpr_atm_acq_load(&g_backup_poller)) ==
           nullptr) {
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "BACKUP_POLLER:%p add %p", p, tcp);
  }
  grpc_pollset_add_fd(BACKUP_POLLER_POLLSET(p), tcp->em_fd);
  if (old_count != 0) {
    drop_uncovered(tcp);
  }
}
static void notify_on_read(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_read", tcp);
  }
  grpc_fd_notify_on_read(tcp->em_fd, &tcp->read_done_closure);
}
static void notify_on_write(grpc_tcp* tcp) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p notify_on_write", tcp);
  }
  if (!grpc_event_engine_run_in_background()) {
    cover_self(tcp);
  }
  grpc_fd_notify_on_write(tcp->em_fd, &tcp->write_done_closure);
}
static void tcp_drop_uncovered_then_handle_write(void* arg, grpc_error* error) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_write: %s", arg, grpc_error_string(error));
  }
  drop_uncovered(static_cast<grpc_tcp*>(arg));
  tcp_handle_write(arg, error);
}
static void add_to_estimate(grpc_tcp* tcp, size_t bytes) {
  tcp->bytes_read_this_round += static_cast<double>(bytes);
}
static void finish_estimate(grpc_tcp* tcp) {
  if (tcp->bytes_read_this_round > tcp->target_length * 0.8) {
    tcp->target_length =
        GPR_MAX(2 * tcp->target_length, tcp->bytes_read_this_round);
  } else {
    tcp->target_length =
        0.99 * tcp->target_length + 0.01 * tcp->bytes_read_this_round;
  }
  tcp->bytes_read_this_round = 0;
}
static size_t get_target_read_size(grpc_tcp* tcp) {
  grpc_resource_quota* rq = grpc_resource_user_quota(tcp->resource_user);
  double pressure = grpc_resource_quota_get_memory_pressure(rq);
  double target =
      tcp->target_length * (pressure > 0.8 ? (1.0 - pressure) / 0.2 : 1.0);
  size_t sz = ((static_cast<size_t> GPR_CLAMP(target, tcp->min_read_chunk_size,
                                              tcp->max_read_chunk_size)) +
               255) &
              ~static_cast<size_t>(255);
  size_t rqmax = grpc_resource_quota_peek_size(rq);
  if (sz > rqmax / 16 && rqmax > 1024) {
    sz = rqmax / 16;
  }
  return sz;
}
static grpc_error* tcp_annotate_error(grpc_error* src_error, grpc_tcp* tcp) {
  return grpc_error_set_str(
      grpc_error_set_int(
          grpc_error_set_int(src_error, GRPC_ERROR_INT_FD, tcp->fd),
          GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE),
      GRPC_ERROR_STR_TARGET_ADDRESS,
      grpc_slice_from_copied_string(tcp->peer_string));
}
static void tcp_handle_read(void* arg , grpc_error* error);
static void tcp_handle_write(void* arg , grpc_error* error);
static void tcp_shutdown(grpc_endpoint* ep, grpc_error* why) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_fd_shutdown(tcp->em_fd, why);
  grpc_resource_user_shutdown(tcp->resource_user);
}
static void tcp_free(grpc_tcp* tcp) {
  grpc_fd_orphan(tcp->em_fd, tcp->release_fd_cb, tcp->release_fd,
                 "tcp_unref_orphan");
  grpc_slice_buffer_destroy_internal(&tcp->last_read_buffer);
  grpc_resource_user_unref(tcp->resource_user);
  gpr_free(tcp->peer_string);
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::Shutdown(
      &tcp->tb_head, tcp->outgoing_buffer_arg,
      GRPC_ERROR_CREATE_FROM_STATIC_STRING("endpoint destroyed"));
  gpr_mu_unlock(&tcp->tb_mu);
  tcp->outgoing_buffer_arg = nullptr;
  gpr_mu_destroy(&tcp->tb_mu);
  gpr_free(tcp);
}
#ifndef NDEBUG
#define TCP_UNREF(tcp,reason) tcp_unref((tcp), (reason), DEBUG_LOCATION)
#define TCP_REF(tcp,reason) tcp_ref((tcp), (reason), DEBUG_LOCATION)
static void tcp_unref(grpc_tcp* tcp, const char* reason,
                      const grpc_core::DebugLocation& debug_location) {
  if (GPR_UNLIKELY(tcp->refcount.Unref(debug_location, reason))) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp, const char* reason,
                    const grpc_core::DebugLocation& debug_location) {
  tcp->refcount.Ref(debug_location, reason);
}
#else
#define TCP_UNREF(tcp,reason) tcp_unref((tcp))
#define TCP_REF(tcp,reason) tcp_ref((tcp))
static void tcp_unref(grpc_tcp* tcp) {
  if (GPR_UNLIKELY(tcp->refcount.Unref())) {
    tcp_free(tcp);
  }
}
static void tcp_ref(grpc_tcp* tcp) { tcp->refcount.Ref(); }
#endif
static void tcp_destroy(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
static void call_read_cb(grpc_tcp* tcp, grpc_error* error) {
  grpc_closure* cb = tcp->read_cb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p call_cb %p %p:%p", tcp, cb, cb->cb, cb->cb_arg);
    size_t i;
    const char* str = grpc_error_string(error);
    gpr_log(GPR_INFO, "READ %p (peer=%s) error=%s", tcp, tcp->peer_string, str);
    if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
      for (i = 0; i < tcp->incoming_buffer->count; i++) {
        char* dump = grpc_dump_slice(tcp->incoming_buffer->slices[i],
                                     GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", dump);
        gpr_free(dump);
      }
    }
  }
  tcp->read_cb = nullptr;
  tcp->incoming_buffer = nullptr;
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, error);
}
#define MAX_READ_IOVEC 4
static void tcp_do_read(grpc_tcp* tcp) {
  GPR_TIMER_SCOPE("tcp_do_read", 0);
  struct msghdr msg;
  struct iovec iov[MAX_READ_IOVEC];
  ssize_t read_bytes;
  size_t total_read_bytes = 0;
  size_t iov_len =
      std::min<size_t>(MAX_READ_IOVEC, tcp->incoming_buffer->count);
#ifdef GRPC_LINUX_ERRQUEUE
  constexpr size_t cmsg_alloc_space =
      CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) + CMSG_SPACE(sizeof(int));
#else
  constexpr size_t cmsg_alloc_space = 24 ;
#endif
  char cmsgbuf[cmsg_alloc_space];
  for (size_t i = 0; i < iov_len; i++) {
    iov[i].iov_base = GRPC_SLICE_START_PTR(tcp->incoming_buffer->slices[i]);
    iov[i].iov_len = GRPC_SLICE_LENGTH(tcp->incoming_buffer->slices[i]);
  }
  do {
    tcp->inq = 1;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<msg_iovlen_type>(iov_len);
    if (tcp->inq_capable) {
      msg.msg_control = cmsgbuf;
      msg.msg_controllen = sizeof(cmsgbuf);
    } else {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }
    msg.msg_flags = 0;
    GRPC_STATS_INC_TCP_READ_OFFER(tcp->incoming_buffer->length);
    GRPC_STATS_INC_TCP_READ_OFFER_IOV_SIZE(tcp->incoming_buffer->count);
    do {
      GPR_TIMER_SCOPE("recvmsg", 0);
      GRPC_STATS_INC_SYSCALL_READ();
      read_bytes = recvmsg(tcp->fd, &msg, 0);
    } while (read_bytes < 0 && errno == EINTR);
    if (read_bytes <= 0 && total_read_bytes > 0) {
      tcp->inq = 1;
      break;
    }
    if (read_bytes < 0) {
      if (errno == EAGAIN) {
        finish_estimate(tcp);
        tcp->inq = 0;
        notify_on_read(tcp);
      } else {
        grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
        call_read_cb(tcp,
                     tcp_annotate_error(GRPC_OS_ERROR(errno, "recvmsg"), tcp));
        TCP_UNREF(tcp, "read");
      }
      return;
    }
    if (read_bytes == 0) {
      grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
      call_read_cb(
          tcp, tcp_annotate_error(
                   GRPC_ERROR_CREATE_FROM_STATIC_STRING("Socket closed"), tcp));
      TCP_UNREF(tcp, "read");
      return;
    }
    GRPC_STATS_INC_TCP_READ_SIZE(read_bytes);
    add_to_estimate(tcp, static_cast<size_t>(read_bytes));
    GPR_DEBUG_ASSERT((size_t)read_bytes <=
                     tcp->incoming_buffer->length - total_read_bytes);
#ifdef GRPC_HAVE_TCP_INQ
    if (tcp->inq_capable) {
      GPR_DEBUG_ASSERT(!(msg.msg_flags & MSG_CTRUNC));
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      for (; cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_TCP && cmsg->cmsg_type == TCP_CM_INQ &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
          tcp->inq = *reinterpret_cast<int*>(CMSG_DATA(cmsg));
          break;
        }
      }
    }
#endif
    total_read_bytes += read_bytes;
    if (tcp->inq == 0 || total_read_bytes == tcp->incoming_buffer->length) {
      break;
    }
    size_t remaining = read_bytes;
    size_t j = 0;
    for (size_t i = 0; i < iov_len; i++) {
      if (remaining >= iov[i].iov_len) {
        remaining -= iov[i].iov_len;
        continue;
      }
      if (remaining > 0) {
        iov[j].iov_base = static_cast<char*>(iov[i].iov_base) + remaining;
        iov[j].iov_len = iov[i].iov_len - remaining;
        remaining = 0;
      } else {
        iov[j].iov_base = iov[i].iov_base;
        iov[j].iov_len = iov[i].iov_len;
      }
      ++j;
    }
    iov_len = j;
  } while (true);
  if (tcp->inq == 0) {
    finish_estimate(tcp);
  }
  GPR_DEBUG_ASSERT(total_read_bytes > 0);
  if (total_read_bytes < tcp->incoming_buffer->length) {
    grpc_slice_buffer_trim_end(tcp->incoming_buffer,
                               tcp->incoming_buffer->length - total_read_bytes,
                               &tcp->last_read_buffer);
  }
  call_read_cb(tcp, GRPC_ERROR_NONE);
  TCP_UNREF(tcp, "read");
}
static void tcp_read_allocation_done(void* tcpp, grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(tcpp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p read_allocation_done: %s", tcp,
            grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_do_read(tcp);
  }
}
static void tcp_continue_read(grpc_tcp* tcp) {
  size_t target_read_size = get_target_read_size(tcp);
  if (tcp->incoming_buffer->length == 0 &&
      tcp->incoming_buffer->count < MAX_READ_IOVEC) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "TCP:%p alloc_slices", tcp);
    }
    if (GPR_UNLIKELY(!grpc_resource_user_alloc_slices(&tcp->slice_allocator,
                                                      target_read_size, 1,
                                                      tcp->incoming_buffer))) {
      return;
    }
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p do_read", tcp);
  }
  tcp_do_read(tcp);
}
static void tcp_handle_read(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_read: %s", tcp, grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    grpc_slice_buffer_reset_and_unref_internal(tcp->incoming_buffer);
    grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "read");
  } else {
    tcp_continue_read(tcp);
  }
}
static void tcp_read(grpc_endpoint* ep, grpc_slice_buffer* incoming_buffer,
                     grpc_closure* cb, bool urgent) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(tcp->read_cb == nullptr);
  tcp->read_cb = cb;
  tcp->incoming_buffer = incoming_buffer;
  grpc_slice_buffer_reset_and_unref_internal(incoming_buffer);
  grpc_slice_buffer_swap(incoming_buffer, &tcp->last_read_buffer);
  TCP_REF(tcp, "read");
  if (tcp->is_first_read) {
    tcp->is_first_read = false;
    notify_on_read(tcp);
  } else if (!urgent && tcp->inq == 0) {
    notify_on_read(tcp);
  } else {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, &tcp->read_done_closure,
                            GRPC_ERROR_NONE);
  }
}
ssize_t tcp_send(int fd, const struct msghdr* msg) {
  GPR_TIMER_SCOPE("sendmsg", 1);
  ssize_t sent_length;
  do {
    GRPC_STATS_INC_SYSCALL_WRITE();
    sent_length = sendmsg(fd, msg, SENDMSG_FLAGS);
  } while (sent_length < 0 && errno == EINTR);
  return sent_length;
}
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length);
static void tcp_handle_error(void* arg , grpc_error* error);
#ifdef GRPC_LINUX_ERRQUEUE
static bool tcp_write_with_timestamps(grpc_tcp* tcp, struct msghdr* msg,
                                      size_t sending_length,
                                      ssize_t* sent_length) {
  if (!tcp->socket_ts_enabled) {
    uint32_t opt = grpc_core::kTimestampingSocketOptions;
    if (setsockopt(tcp->fd, SOL_SOCKET, SO_TIMESTAMPING,
                   static_cast<void*>(&opt), sizeof(opt)) != 0) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Failed to set timestamping options on the socket.");
      }
      return false;
    }
    tcp->bytes_counter = -1;
    tcp->socket_ts_enabled = true;
  }
  union {
    char cmsg_buf[CMSG_SPACE(sizeof(uint32_t))];
    struct cmsghdr align;
  } u;
  cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(u.cmsg_buf);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPING;
  cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
  *reinterpret_cast<int*>(CMSG_DATA(cmsg)) =
      grpc_core::kTimestampingRecordingOptions;
  msg->msg_control = u.cmsg_buf;
  msg->msg_controllen = CMSG_SPACE(sizeof(uint32_t));
  ssize_t length = tcp_send(tcp->fd, msg);
  *sent_length = length;
  if (sending_length == static_cast<size_t>(length)) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::AddNewEntry(
        &tcp->tb_head, static_cast<uint32_t>(tcp->bytes_counter + length),
        tcp->fd, tcp->outgoing_buffer_arg);
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
  return true;
}
struct cmsghdr* process_timestamp(grpc_tcp* tcp, msghdr* msg,
                                  struct cmsghdr* cmsg) {
  auto next_cmsg = CMSG_NXTHDR(msg, cmsg);
  cmsghdr* opt_stats = nullptr;
  if (next_cmsg == nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Received timestamp without extended error");
    }
    return cmsg;
  }
  if (next_cmsg->cmsg_level == SOL_SOCKET &&
      next_cmsg->cmsg_type == SCM_TIMESTAMPING_OPT_STATS) {
    opt_stats = next_cmsg;
    next_cmsg = CMSG_NXTHDR(msg, opt_stats);
    if (next_cmsg == nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
        gpr_log(GPR_ERROR, "Received timestamp without extended error");
      }
      return opt_stats;
    }
  }
  if (!(next_cmsg->cmsg_level == SOL_IP || next_cmsg->cmsg_level == SOL_IPV6) ||
      !(next_cmsg->cmsg_type == IP_RECVERR ||
        next_cmsg->cmsg_type == IPV6_RECVERR)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_ERROR, "Unexpected control message");
    }
    return cmsg;
  }
  auto tss =
      reinterpret_cast<struct grpc_core::scm_timestamping*>(CMSG_DATA(cmsg));
  auto serr = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(next_cmsg));
  if (serr->ee_errno != ENOMSG ||
      serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
    gpr_log(GPR_ERROR, "Unexpected control message");
    return cmsg;
  }
  gpr_mu_lock(&tcp->tb_mu);
  grpc_core::TracedBuffer::ProcessTimestamp(&tcp->tb_head, serr, opt_stats,
                                            tss);
  gpr_mu_unlock(&tcp->tb_mu);
  return next_cmsg;
}
static void process_errors(grpc_tcp* tcp) {
  while (true) {
    struct iovec iov;
    iov.iov_base = nullptr;
    iov.iov_len = 0;
    struct msghdr msg;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 0;
    msg.msg_flags = 0;
    constexpr size_t cmsg_alloc_space =
        CMSG_SPACE(sizeof(grpc_core::scm_timestamping)) +
        CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in)) +
        CMSG_SPACE(32 * NLA_ALIGN(NLA_HDRLEN + sizeof(uint64_t)));
    union {
      char rbuf[cmsg_alloc_space];
      struct cmsghdr align;
    } aligned_buf;
    memset(&aligned_buf, 0, sizeof(aligned_buf));
    msg.msg_control = aligned_buf.rbuf;
    msg.msg_controllen = sizeof(aligned_buf.rbuf);
    int r, saved_errno;
    do {
      r = recvmsg(tcp->fd, &msg, MSG_ERRQUEUE);
      saved_errno = errno;
    } while (r < 0 && saved_errno == EINTR);
    if (r == -1 && saved_errno == EAGAIN) {
      return;
    }
    if (r == -1) {
      return;
    }
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
      gpr_log(GPR_ERROR, "Error message was truncated.");
    }
    if (msg.msg_controllen == 0) {
      return;
    }
    bool seen = false;
    for (auto cmsg = CMSG_FIRSTHDR(&msg); cmsg && cmsg->cmsg_len;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_TIMESTAMPING) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
          gpr_log(GPR_INFO,
                  "unknown control message cmsg_level:%d cmsg_type:%d",
                  cmsg->cmsg_level, cmsg->cmsg_type);
        }
        return;
      }
      cmsg = process_timestamp(tcp, &msg, cmsg);
      seen = true;
    }
    if (!seen) {
      return;
    }
  }
}
static void tcp_handle_error(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p got_error: %s", tcp, grpc_error_string(error));
  }
  if (error != GRPC_ERROR_NONE ||
      static_cast<bool>(gpr_atm_acq_load(&tcp->stop_error_notification))) {
    TCP_UNREF(tcp, "error-tracking");
    return;
  }
  process_errors(tcp);
  grpc_fd_set_readable(tcp->em_fd);
  grpc_fd_set_writable(tcp->em_fd);
  grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
}
#else
static bool tcp_write_with_timestamps(grpc_tcp* , struct msghdr* ,
                                      size_t ,
                                      ssize_t* ) {
  gpr_log(GPR_ERROR, "Write with timestamps not supported for this platform");
  GPR_ASSERT(0);
  return false;
}
static void tcp_handle_error(void* ,
                             grpc_error* ) {
  gpr_log(GPR_ERROR, "Error handling is not supported for this platform");
  GPR_ASSERT(0);
}
#endif
void tcp_shutdown_buffer_list(grpc_tcp* tcp) {
  if (tcp->outgoing_buffer_arg) {
    gpr_mu_lock(&tcp->tb_mu);
    grpc_core::TracedBuffer::Shutdown(
        &tcp->tb_head, tcp->outgoing_buffer_arg,
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("TracedBuffer list shutdown"));
    gpr_mu_unlock(&tcp->tb_mu);
    tcp->outgoing_buffer_arg = nullptr;
  }
}
#if defined(IOV_MAX) && IOV_MAX < 1000
#define MAX_WRITE_IOVEC IOV_MAX
#else
#define MAX_WRITE_IOVEC 1000
#endif
static bool tcp_flush(grpc_tcp* tcp, grpc_error** error) {
  struct msghdr msg;
  struct iovec iov[MAX_WRITE_IOVEC];
  msg_iovlen_type iov_size;
  ssize_t sent_length = 0;
  size_t sending_length;
  size_t trailing;
  size_t unwind_slice_idx;
  size_t unwind_byte_idx;
  size_t outgoing_slice_idx = 0;
  for (;;) {
    sending_length = 0;
    unwind_slice_idx = outgoing_slice_idx;
    unwind_byte_idx = tcp->outgoing_byte_idx;
    for (iov_size = 0; outgoing_slice_idx != tcp->outgoing_buffer->count &&
                       iov_size != MAX_WRITE_IOVEC;
         iov_size++) {
      iov[iov_size].iov_base =
          GRPC_SLICE_START_PTR(
              tcp->outgoing_buffer->slices[outgoing_slice_idx]) +
          tcp->outgoing_byte_idx;
      iov[iov_size].iov_len =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]) -
          tcp->outgoing_byte_idx;
      sending_length += iov[iov_size].iov_len;
      outgoing_slice_idx++;
      tcp->outgoing_byte_idx = 0;
    }
    GPR_ASSERT(iov_size > 0);
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = iov_size;
    msg.msg_flags = 0;
    bool tried_sending_message = false;
    if (tcp->outgoing_buffer_arg != nullptr) {
      if (!tcp->ts_capable ||
          !tcp_write_with_timestamps(tcp, &msg, sending_length, &sent_length)) {
        tcp->ts_capable = false;
        tcp_shutdown_buffer_list(tcp);
      } else {
        tried_sending_message = true;
      }
    }
    if (!tried_sending_message) {
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
      GRPC_STATS_INC_TCP_WRITE_SIZE(sending_length);
      GRPC_STATS_INC_TCP_WRITE_IOV_SIZE(iov_size);
      sent_length = tcp_send(tcp->fd, &msg);
    }
    if (sent_length < 0) {
      if (errno == EAGAIN) {
        tcp->outgoing_byte_idx = unwind_byte_idx;
        for (size_t idx = 0; idx < unwind_slice_idx; ++idx) {
          grpc_slice_buffer_remove_first(tcp->outgoing_buffer);
        }
        return false;
      } else if (errno == EPIPE) {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      } else {
        *error = tcp_annotate_error(GRPC_OS_ERROR(errno, "sendmsg"), tcp);
        grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
        tcp_shutdown_buffer_list(tcp);
        return true;
      }
    }
    GPR_ASSERT(tcp->outgoing_byte_idx == 0);
    tcp->bytes_counter += sent_length;
    trailing = sending_length - static_cast<size_t>(sent_length);
    while (trailing > 0) {
      size_t slice_length;
      outgoing_slice_idx--;
      slice_length =
          GRPC_SLICE_LENGTH(tcp->outgoing_buffer->slices[outgoing_slice_idx]);
      if (slice_length > trailing) {
        tcp->outgoing_byte_idx = slice_length - trailing;
        break;
      } else {
        trailing -= slice_length;
      }
    }
    if (outgoing_slice_idx == tcp->outgoing_buffer->count) {
      *error = GRPC_ERROR_NONE;
      grpc_slice_buffer_reset_and_unref_internal(tcp->outgoing_buffer);
      return true;
    }
  }
}
static void tcp_handle_write(void* arg , grpc_error* error) {
  grpc_tcp* tcp = static_cast<grpc_tcp*>(arg);
  grpc_closure* cb;
  if (error != GRPC_ERROR_NONE) {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, GRPC_ERROR_REF(error));
    TCP_UNREF(tcp, "write");
    return;
  }
  if (!tcp_flush(tcp, &error)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
    GPR_DEBUG_ASSERT(error == GRPC_ERROR_NONE);
  } else {
    cb = tcp->write_cb;
    tcp->write_cb = nullptr;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    GRPC_CLOSURE_RUN(cb, error);
    TCP_UNREF(tcp, "write");
  }
}
static void tcp_write(grpc_endpoint* ep, grpc_slice_buffer* buf,
                      grpc_closure* cb, void* arg) {
  GPR_TIMER_SCOPE("tcp_write", 0);
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_error* error = GRPC_ERROR_NONE;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    size_t i;
    for (i = 0; i < buf->count; i++) {
      gpr_log(GPR_INFO, "WRITE %p (peer=%s)", tcp, tcp->peer_string);
      if (gpr_should_log(GPR_LOG_SEVERITY_DEBUG)) {
        char* data =
            grpc_dump_slice(buf->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
        gpr_log(GPR_DEBUG, "DATA: %s", data);
        gpr_free(data);
      }
    }
  }
  GPR_ASSERT(tcp->write_cb == nullptr);
  tcp->outgoing_buffer_arg = arg;
  if (buf->length == 0) {
    grpc_core::ExecCtx::Run(
        DEBUG_LOCATION, cb,
        grpc_fd_is_shutdown(tcp->em_fd)
            ? tcp_annotate_error(GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF"),
                                 tcp)
            : GRPC_ERROR_NONE);
    tcp_shutdown_buffer_list(tcp);
    return;
  }
  tcp->outgoing_buffer = buf;
  tcp->outgoing_byte_idx = 0;
  if (arg) {
    GPR_ASSERT(grpc_event_engine_can_track_errors());
  }
  if (!tcp_flush(tcp, &error)) {
    TCP_REF(tcp, "write");
    tcp->write_cb = cb;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "write: delayed");
    }
    notify_on_write(tcp);
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      const char* str = grpc_error_string(error);
      gpr_log(GPR_INFO, "write: %s", str);
    }
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, error);
  }
}
static void tcp_add_to_pollset(grpc_endpoint* ep, grpc_pollset* pollset) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_add_fd(pollset, tcp->em_fd);
}
static void tcp_add_to_pollset_set(grpc_endpoint* ep,
                                   grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_add_fd(pollset_set, tcp->em_fd);
}
static void tcp_delete_from_pollset_set(grpc_endpoint* ep,
                                        grpc_pollset_set* pollset_set) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  grpc_pollset_set_del_fd(pollset_set, tcp->em_fd);
}
static char* tcp_get_peer(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return gpr_strdup(tcp->peer_string);
}
static int tcp_get_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->fd;
}
static grpc_resource_user* tcp_get_resource_user(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  return tcp->resource_user;
}
static bool tcp_can_track_err(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  if (!grpc_event_engine_can_track_errors()) {
    return false;
  }
  struct sockaddr addr;
  socklen_t len = sizeof(addr);
  if (getsockname(tcp->fd, &addr, &len) < 0) {
    return false;
  }
  if (addr.sa_family == AF_INET || addr.sa_family == AF_INET6) {
    return true;
  }
  return false;
}
static const grpc_endpoint_vtable vtable = {tcp_read,
                                            tcp_write,
                                            tcp_add_to_pollset,
                                            tcp_add_to_pollset_set,
                                            tcp_delete_from_pollset_set,
                                            tcp_shutdown,
                                            tcp_destroy,
                                            tcp_get_resource_user,
                                            tcp_get_peer,
                                            tcp_get_fd,
                                            tcp_can_track_err};
#define MAX_CHUNK_SIZE 32 * 1024 * 1024
grpc_endpoint* grpc_tcp_create(grpc_fd* em_fd,
                               const grpc_channel_args* channel_args,
                               const char* peer_string) {
  int tcp_read_chunk_size = GRPC_TCP_DEFAULT_READ_SLICE_SIZE;
  int tcp_max_read_chunk_size = 4 * 1024 * 1024;
  int tcp_min_read_chunk_size = 256;
  grpc_resource_quota* resource_quota = grpc_resource_quota_create(nullptr);
  if (channel_args != nullptr) {
    for (size_t i = 0; i < channel_args->num_args; i++) {
      if (0 ==
          strcmp(channel_args->args[i].key, GRPC_ARG_TCP_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MIN_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_min_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 == strcmp(channel_args->args[i].key,
                             GRPC_ARG_TCP_MAX_READ_CHUNK_SIZE)) {
        grpc_integer_options options = {tcp_read_chunk_size, 1, MAX_CHUNK_SIZE};
        tcp_max_read_chunk_size =
            grpc_channel_arg_get_integer(&channel_args->args[i], options);
      } else if (0 ==
                 strcmp(channel_args->args[i].key, GRPC_ARG_RESOURCE_QUOTA)) {
        grpc_resource_quota_unref_internal(resource_quota);
        resource_quota =
            grpc_resource_quota_ref_internal(static_cast<grpc_resource_quota*>(
                channel_args->args[i].value.pointer.p));
      }
    }
  }
  if (tcp_min_read_chunk_size > tcp_max_read_chunk_size) {
    tcp_min_read_chunk_size = tcp_max_read_chunk_size;
  }
  tcp_read_chunk_size = GPR_CLAMP(tcp_read_chunk_size, tcp_min_read_chunk_size,
                                  tcp_max_read_chunk_size);
  grpc_tcp* tcp = static_cast<grpc_tcp*>(gpr_malloc(sizeof(grpc_tcp)));
  tcp->base.vtable = &vtable;
  tcp->peer_string = gpr_strdup(peer_string);
  tcp->fd = grpc_fd_wrapped_fd(em_fd);
  tcp->read_cb = nullptr;
  tcp->write_cb = nullptr;
  tcp->release_fd_cb = nullptr;
  tcp->release_fd = nullptr;
  tcp->incoming_buffer = nullptr;
  tcp->target_length = static_cast<double>(tcp_read_chunk_size);
  tcp->min_read_chunk_size = tcp_min_read_chunk_size;
  tcp->max_read_chunk_size = tcp_max_read_chunk_size;
  tcp->bytes_read_this_round = 0;
  tcp->is_first_read = true;
  tcp->bytes_counter = -1;
  tcp->socket_ts_enabled = false;
  tcp->ts_capable = true;
  tcp->outgoing_buffer_arg = nullptr;
  new (&tcp->refcount) grpc_core::RefCount(1, &grpc_tcp_trace);
  gpr_atm_no_barrier_store(&tcp->shutdown_count, 0);
  tcp->em_fd = em_fd;
  grpc_slice_buffer_init(&tcp->last_read_buffer);
  tcp->resource_user = grpc_resource_user_create(resource_quota, peer_string);
  grpc_resource_user_slice_allocator_init(
      &tcp->slice_allocator, tcp->resource_user, tcp_read_allocation_done, tcp);
  grpc_resource_quota_unref_internal(resource_quota);
  gpr_mu_init(&tcp->tb_mu);
  tcp->tb_head = nullptr;
  GRPC_CLOSURE_INIT(&tcp->read_done_closure, tcp_handle_read, tcp,
                    grpc_schedule_on_exec_ctx);
  if (grpc_event_engine_run_in_background()) {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure, tcp_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  } else {
    GRPC_CLOSURE_INIT(&tcp->write_done_closure,
                      tcp_drop_uncovered_then_handle_write, tcp,
                      grpc_schedule_on_exec_ctx);
  }
  tcp->inq = 1;
#ifdef GRPC_HAVE_TCP_INQ
  int one = 1;
  if (setsockopt(tcp->fd, SOL_TCP, TCP_INQ, &one, sizeof(one)) == 0) {
    tcp->inq_capable = true;
  } else {
    gpr_log(GPR_DEBUG, "cannot set inq fd=%d errno=%d", tcp->fd, errno);
    tcp->inq_capable = false;
  }
#else
  tcp->inq_capable = false;
#endif
  if (grpc_event_engine_can_track_errors()) {
    TCP_REF(tcp, "error-tracking");
    gpr_atm_rel_store(&tcp->stop_error_notification, 0);
    GRPC_CLOSURE_INIT(&tcp->error_closure, tcp_handle_error, tcp,
                      grpc_schedule_on_exec_ctx);
    grpc_fd_notify_on_error(tcp->em_fd, &tcp->error_closure);
  }
  return &tcp->base;
}
int grpc_tcp_fd(grpc_endpoint* ep) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  return grpc_fd_wrapped_fd(tcp->em_fd);
}
void grpc_tcp_destroy_and_release_fd(grpc_endpoint* ep, int* fd,
                                     grpc_closure* done) {
  grpc_tcp* tcp = reinterpret_cast<grpc_tcp*>(ep);
  GPR_ASSERT(ep->vtable == &vtable);
  tcp->release_fd = fd;
  tcp->release_fd_cb = done;
  grpc_slice_buffer_reset_and_unref_internal(&tcp->last_read_buffer);
  if (grpc_event_engine_can_track_errors()) {
    gpr_atm_no_barrier_store(&tcp->stop_error_notification, true);
    grpc_fd_set_error(tcp->em_fd);
  }
  TCP_UNREF(tcp, "destroy");
}
#endif
