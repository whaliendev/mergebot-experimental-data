#include <grpc/support/port_platform.h>
#include "src/core/lib/iomgr/port.h"
#if GRPC_ARES == 1 && defined(GRPC_WINDOWS_SOCKET_ARES_EV_DRIVER)
#include <ares.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/log_windows.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <string.h>
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/iomgr/iocp_windows.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/sockaddr_windows.h"
#include "src/core/lib/iomgr/socket_windows.h"
#include "src/core/lib/iomgr/tcp_windows.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
struct iovec {
  void* iov_base;
  size_t iov_len;
};
namespace grpc_core {
class WSAErrorContext {
 public:
  explicit WSAErrorContext(){};
  ~WSAErrorContext() {
    if (error_ != 0) {
      WSASetLastError(error_);
    }
  }
  WSAErrorContext(const WSAErrorContext&) = delete;
  WSAErrorContext& operator=(const WSAErrorContext&) = delete;
  void SetWSAError(int error) { error_ = error; }
 private:
  int error_ = 0;
};
class GrpcPolledFdWindows {
 public:
  enum WriteState {
    WRITE_IDLE,
    WRITE_REQUESTED,
    WRITE_PENDING,
    WRITE_WAITING_FOR_VERIFICATION_UPON_RETRY,
  };
  GrpcPolledFdWindows(ares_socket_t as,
                      std::shared_ptr<WorkSerializer> work_serializer,
                      int address_family, int socket_type)
      : read_buf_(grpc_empty_slice()),
        write_buf_(grpc_empty_slice()),
        tcp_write_state_(WRITE_IDLE),
        gotten_into_driver_list_(false),
        address_family_(address_family),
        socket_type_(socket_type),
        work_serializer_(std::move(work_serializer)) {
    gpr_asprintf(&name_, "c-ares socket: %" PRIdPTR, as);
    GRPC_CLOSURE_INIT(&outer_read_closure_,
                      &GrpcPolledFdWindows::OnIocpReadable, this,
                      grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&outer_write_closure_,
                      &GrpcPolledFdWindows::OnIocpWriteable, this,
                      grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&on_tcp_connect_locked_,
                      &GrpcPolledFdWindows::OnTcpConnect, this,
                      grpc_schedule_on_exec_ctx);
    winsocket_ = grpc_winsocket_create(as, name_);
  }
  ~GrpcPolledFdWindows() {
    grpc_slice_unref_internal(read_buf_);
    grpc_slice_unref_internal(write_buf_);
    GPR_ASSERT(read_closure_ == nullptr);
    GPR_ASSERT(write_closure_ == nullptr);
    grpc_winsocket_destroy(winsocket_);
    gpr_free(name_);
  }
  void ScheduleAndNullReadClosure(grpc_error* error) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, read_closure_, error);
    read_closure_ = nullptr;
  }
  void ScheduleAndNullWriteClosure(grpc_error* error) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, write_closure_, error);
    write_closure_ = nullptr;
  }
  void RegisterForOnReadableLocked(grpc_closure* read_closure) {
    GPR_ASSERT(read_closure_ == nullptr);
    read_closure_ = read_closure;
    GPR_ASSERT(GRPC_SLICE_LENGTH(read_buf_) == 0);
    grpc_slice_unref_internal(read_buf_);
    GPR_ASSERT(!read_buf_has_data_);
    read_buf_ = GRPC_SLICE_MALLOC(4192);
    if (connect_done_) {
      work_serializer_->Run([this]() { ContinueRegisterForOnReadableLocked(); },
                            DEBUG_LOCATION);
    } else {
      GPR_ASSERT(pending_continue_register_for_on_readable_locked_ == false);
      pending_continue_register_for_on_readable_locked_ = true;
    }
  }
  void ContinueRegisterForOnReadableLocked() {
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| InnerContinueRegisterForOnReadableLocked "
        "wsa_connect_error_:%d",
        GetName(), wsa_connect_error_);
    GPR_ASSERT(connect_done_);
    if (wsa_connect_error_ != 0) {
      ScheduleAndNullReadClosure(GRPC_WSA_ERROR(wsa_connect_error_, "connect"));
      return;
    }
    WSABUF buffer;
    buffer.buf = (char*)GRPC_SLICE_START_PTR(read_buf_);
    buffer.len = GRPC_SLICE_LENGTH(read_buf_);
    memset(&winsocket_->read_info.overlapped, 0, sizeof(OVERLAPPED));
    recv_from_source_addr_len_ = sizeof(recv_from_source_addr_);
    DWORD flags = 0;
    if (WSARecvFrom(grpc_winsocket_wrapped_socket(winsocket_), &buffer, 1,
                    nullptr, &flags, (sockaddr*)recv_from_source_addr_,
                    &recv_from_source_addr_len_,
                    &winsocket_->read_info.overlapped, nullptr)) {
      int wsa_last_error = WSAGetLastError();
      char* msg = gpr_format_message(wsa_last_error);
      GRPC_CARES_TRACE_LOG(
          "fd:|%s| RegisterForOnReadableLocked WSARecvFrom error code:|%d| "
          "msg:|%s|",
          GetName(), wsa_last_error, msg);
      gpr_free(msg);
      if (wsa_last_error != WSA_IO_PENDING) {
        ScheduleAndNullReadClosure(
            GRPC_WSA_ERROR(wsa_last_error, "WSARecvFrom"));
        return;
      }
    }
    grpc_socket_notify_on_read(winsocket_, &outer_read_closure_);
  }
  void RegisterForOnWriteableLocked(grpc_closure* write_closure) {
    if (socket_type_ == SOCK_DGRAM) {
      GRPC_CARES_TRACE_LOG("fd:|%s| RegisterForOnWriteableLocked called",
                           GetName());
    } else {
      GPR_ASSERT(socket_type_ == SOCK_STREAM);
      GRPC_CARES_TRACE_LOG(
          "fd:|%s| RegisterForOnWriteableLocked called tcp_write_state_: %d",
          GetName(), tcp_write_state_);
    }
    GPR_ASSERT(write_closure_ == nullptr);
    write_closure_ = write_closure;
    if (connect_done_) {
      work_serializer_->Run(
          [this]() { ContinueRegisterForOnWriteableLocked(); }, DEBUG_LOCATION);
    } else {
      GPR_ASSERT(pending_continue_register_for_on_writeable_locked_ == false);
      pending_continue_register_for_on_writeable_locked_ = true;
    }
  }
  void ContinueRegisterForOnWriteableLocked() {
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| InnerContinueRegisterForOnWriteableLocked "
        "wsa_connect_error_:%d",
        GetName(), wsa_connect_error_);
    GPR_ASSERT(connect_done_);
    if (wsa_connect_error_ != 0) {
      ScheduleAndNullWriteClosure(
          GRPC_WSA_ERROR(wsa_connect_error_, "connect"));
      return;
    }
    if (socket_type_ == SOCK_DGRAM) {
      ScheduleAndNullWriteClosure(GRPC_ERROR_NONE);
    } else {
      GPR_ASSERT(socket_type_ == SOCK_STREAM);
      int wsa_error_code = 0;
      switch (tcp_write_state_) {
        case WRITE_IDLE:
          ScheduleAndNullWriteClosure(GRPC_ERROR_NONE);
          break;
        case WRITE_REQUESTED:
          tcp_write_state_ = WRITE_PENDING;
          if (SendWriteBuf(nullptr, &winsocket_->write_info.overlapped,
                           &wsa_error_code) != 0) {
            ScheduleAndNullWriteClosure(
                GRPC_WSA_ERROR(wsa_error_code, "WSASend (overlapped)"));
          } else {
            grpc_socket_notify_on_write(winsocket_, &outer_write_closure_);
          }
          break;
        case WRITE_PENDING:
        case WRITE_WAITING_FOR_VERIFICATION_UPON_RETRY:
          abort();
      }
    }
  }
  bool IsFdStillReadableLocked() { return GRPC_SLICE_LENGTH(read_buf_) > 0; }
  void ShutdownLocked(grpc_error* error) {
    grpc_winsocket_shutdown(winsocket_);
  }
  ares_socket_t GetWrappedAresSocketLocked() {
    return grpc_winsocket_wrapped_socket(winsocket_);
  }
  const char* GetName() { return name_; }
  ares_ssize_t RecvFrom(WSAErrorContext* wsa_error_ctx, void* data,
                        ares_socket_t data_len, int flags,
                        struct sockaddr* from, ares_socklen_t* from_len) {
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| RecvFrom called read_buf_has_data:%d Current read buf "
        "length:|%d|",
        GetName(), read_buf_has_data_, GRPC_SLICE_LENGTH(read_buf_));
    if (!read_buf_has_data_) {
      wsa_error_ctx->SetWSAError(WSAEWOULDBLOCK);
      return -1;
    }
    ares_ssize_t bytes_read = 0;
    for (size_t i = 0; i < GRPC_SLICE_LENGTH(read_buf_) && i < data_len; i++) {
      ((char*)data)[i] = GRPC_SLICE_START_PTR(read_buf_)[i];
      bytes_read++;
    }
    read_buf_ = grpc_slice_sub_no_ref(read_buf_, bytes_read,
                                      GRPC_SLICE_LENGTH(read_buf_));
    if (GRPC_SLICE_LENGTH(read_buf_) == 0) {
      read_buf_has_data_ = false;
    }
    if (from != nullptr) {
      GPR_ASSERT(*from_len <= recv_from_source_addr_len_);
      memcpy(from, &recv_from_source_addr_, recv_from_source_addr_len_);
      *from_len = recv_from_source_addr_len_;
    }
    return bytes_read;
  }
  grpc_slice FlattenIovec(const struct iovec* iov, int iov_count) {
    int total = 0;
    for (int i = 0; i < iov_count; i++) {
      total += iov[i].iov_len;
    }
    grpc_slice out = GRPC_SLICE_MALLOC(total);
    size_t cur = 0;
    for (int i = 0; i < iov_count; i++) {
      for (int k = 0; k < iov[i].iov_len; k++) {
        GRPC_SLICE_START_PTR(out)[cur++] = ((char*)iov[i].iov_base)[k];
      }
    }
    return out;
  }
  int SendWriteBuf(LPDWORD bytes_sent_ptr, LPWSAOVERLAPPED overlapped,
                   int* wsa_error_code) {
    WSABUF buf;
    buf.len = GRPC_SLICE_LENGTH(write_buf_);
    buf.buf = (char*)GRPC_SLICE_START_PTR(write_buf_);
    DWORD flags = 0;
    int out = WSASend(grpc_winsocket_wrapped_socket(winsocket_), &buf, 1,
                      bytes_sent_ptr, flags, overlapped, nullptr);
    *wsa_error_code = WSAGetLastError();
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| SendWriteBuf WSASend buf.len:%d *bytes_sent_ptr:%d "
        "overlapped:%p "
        "return:%d *wsa_error_code:%d",
        GetName(), buf.len, bytes_sent_ptr != nullptr ? *bytes_sent_ptr : 0,
        overlapped, out, *wsa_error_code);
    return out;
  }
  ares_ssize_t SendV(WSAErrorContext* wsa_error_ctx, const struct iovec* iov,
                     int iov_count) {
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| SendV called connect_done_:%d wsa_connect_error_:%d",
        GetName(), connect_done_, wsa_connect_error_);
    if (!connect_done_) {
      wsa_error_ctx->SetWSAError(WSAEWOULDBLOCK);
      return -1;
    }
    if (wsa_connect_error_ != 0) {
      wsa_error_ctx->SetWSAError(wsa_connect_error_);
      return -1;
    }
    switch (socket_type_) {
      case SOCK_DGRAM:
        return SendVUDP(wsa_error_ctx, iov, iov_count);
      case SOCK_STREAM:
        return SendVTCP(wsa_error_ctx, iov, iov_count);
      default:
        abort();
    }
  }
  ares_ssize_t SendVUDP(WSAErrorContext* wsa_error_ctx, const struct iovec* iov,
                        int iov_count) {
    GRPC_CARES_TRACE_LOG("fd:|%s| SendVUDP called", GetName());
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
    grpc_slice_unref_internal(write_buf_);
    write_buf_ = FlattenIovec(iov, iov_count);
    DWORD bytes_sent = 0;
    int wsa_error_code = 0;
    if (SendWriteBuf(&bytes_sent, nullptr, &wsa_error_code) != 0) {
      wsa_error_ctx->SetWSAError(wsa_error_code);
      char* msg = gpr_format_message(wsa_error_code);
      GRPC_CARES_TRACE_LOG(
          "fd:|%s| SendVUDP SendWriteBuf error code:%d msg:|%s|", GetName(),
          wsa_error_code, msg);
      gpr_free(msg);
      return -1;
    }
    write_buf_ = grpc_slice_sub_no_ref(write_buf_, bytes_sent,
                                       GRPC_SLICE_LENGTH(write_buf_));
    return bytes_sent;
  }
  ares_ssize_t SendVTCP(WSAErrorContext* wsa_error_ctx, const struct iovec* iov,
                        int iov_count) {
    GRPC_CARES_TRACE_LOG("fd:|%s| SendVTCP called tcp_write_state_:%d",
                         GetName(), tcp_write_state_);
    switch (tcp_write_state_) {
      case WRITE_IDLE:
        tcp_write_state_ = WRITE_REQUESTED;
        GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
        grpc_slice_unref_internal(write_buf_);
        write_buf_ = FlattenIovec(iov, iov_count);
        wsa_error_ctx->SetWSAError(WSAEWOULDBLOCK);
        return -1;
      case WRITE_REQUESTED:
      case WRITE_PENDING:
        wsa_error_ctx->SetWSAError(WSAEWOULDBLOCK);
        return -1;
      case WRITE_WAITING_FOR_VERIFICATION_UPON_RETRY:
        grpc_slice currently_attempted = FlattenIovec(iov, iov_count);
        GPR_ASSERT(GRPC_SLICE_LENGTH(currently_attempted) >=
                   GRPC_SLICE_LENGTH(write_buf_));
        ares_ssize_t total_sent = 0;
        for (size_t i = 0; i < GRPC_SLICE_LENGTH(write_buf_); i++) {
          GPR_ASSERT(GRPC_SLICE_START_PTR(currently_attempted)[i] ==
                     GRPC_SLICE_START_PTR(write_buf_)[i]);
          total_sent++;
        }
        grpc_slice_unref_internal(currently_attempted);
        tcp_write_state_ = WRITE_IDLE;
        return total_sent;
    }
    abort();
  }
  static void OnTcpConnect(void* arg, grpc_error* error) {
    GrpcPolledFdWindows* grpc_polled_fd =
        static_cast<GrpcPolledFdWindows*>(arg);
    GRPC_ERROR_REF(error);
    grpc_polled_fd->work_serializer_->Run(
        [grpc_polled_fd, error]() {
          grpc_polled_fd->OnTcpConnectLocked(error);
        },
        DEBUG_LOCATION);
  }
  void OnTcpConnectLocked(grpc_error* error) {
    GRPC_CARES_TRACE_LOG(
        "fd:%s InnerOnTcpConnectLocked error:|%s| "
        "pending_register_for_readable:%d"
        " pending_register_for_writeable:%d",
        GetName(), grpc_error_string(error),
        pending_continue_register_for_on_readable_locked_,
        pending_continue_register_for_on_writeable_locked_);
    GPR_ASSERT(!connect_done_);
    connect_done_ = true;
    GPR_ASSERT(wsa_connect_error_ == 0);
    if (error == GRPC_ERROR_NONE) {
      DWORD transferred_bytes = 0;
      DWORD flags;
      BOOL wsa_success =
          WSAGetOverlappedResult(grpc_winsocket_wrapped_socket(winsocket_),
                                 &winsocket_->write_info.overlapped,
                                 &transferred_bytes, FALSE, &flags);
      GPR_ASSERT(transferred_bytes == 0);
      if (!wsa_success) {
        wsa_connect_error_ = WSAGetLastError();
        char* msg = gpr_format_message(wsa_connect_error_);
        GRPC_CARES_TRACE_LOG(
            "fd:%s InnerOnTcpConnectLocked WSA overlapped result code:%d "
            "msg:|%s|",
            GetName(), wsa_connect_error_, msg);
        gpr_free(msg);
      }
    } else {
      wsa_connect_error_ = WSA_OPERATION_ABORTED;
    }
    if (pending_continue_register_for_on_readable_locked_) {
      work_serializer_->Run([this]() { ContinueRegisterForOnReadableLocked(); },
                            DEBUG_LOCATION);
    }
    if (pending_continue_register_for_on_writeable_locked_) {
      work_serializer_->Run(
          [this]() { ContinueRegisterForOnWriteableLocked(); }, DEBUG_LOCATION);
    }
    GRPC_ERROR_UNREF(error);
  }
  int Connect(WSAErrorContext* wsa_error_ctx, const struct sockaddr* target,
              ares_socklen_t target_len) {
    switch (socket_type_) {
      case SOCK_DGRAM:
        return ConnectUDP(wsa_error_ctx, target, target_len);
      case SOCK_STREAM:
        return ConnectTCP(wsa_error_ctx, target, target_len);
      default:
        abort();
    }
  }
  int ConnectUDP(WSAErrorContext* wsa_error_ctx, const struct sockaddr* target,
                 ares_socklen_t target_len) {
    GRPC_CARES_TRACE_LOG("fd:%s ConnectUDP", GetName());
    GPR_ASSERT(!connect_done_);
    GPR_ASSERT(wsa_connect_error_ == 0);
    SOCKET s = grpc_winsocket_wrapped_socket(winsocket_);
    int out =
        WSAConnect(s, target, target_len, nullptr, nullptr, nullptr, nullptr);
    wsa_connect_error_ = WSAGetLastError();
    wsa_error_ctx->SetWSAError(wsa_connect_error_);
    connect_done_ = true;
    char* msg = gpr_format_message(wsa_connect_error_);
    GRPC_CARES_TRACE_LOG("fd:%s WSAConnect error code:|%d| msg:|%s|", GetName(),
                         wsa_connect_error_, msg);
    gpr_free(msg);
    return out == 0 ? 0 : -1;
  }
  int ConnectTCP(WSAErrorContext* wsa_error_ctx, const struct sockaddr* target,
                 ares_socklen_t target_len) {
    GRPC_CARES_TRACE_LOG("fd:%s ConnectTCP", GetName());
    LPFN_CONNECTEX ConnectEx;
    GUID guid = WSAID_CONNECTEX;
    DWORD ioctl_num_bytes;
    SOCKET s = grpc_winsocket_wrapped_socket(winsocket_);
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &ConnectEx, sizeof(ConnectEx), &ioctl_num_bytes, nullptr,
                 nullptr) != 0) {
      int wsa_last_error = WSAGetLastError();
      wsa_error_ctx->SetWSAError(wsa_last_error);
      char* msg = gpr_format_message(wsa_last_error);
      GRPC_CARES_TRACE_LOG(
          "fd:%s WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER) error code:%d "
          "msg:|%s|",
          GetName(), wsa_last_error, msg);
      gpr_free(msg);
      connect_done_ = true;
      wsa_connect_error_ = wsa_last_error;
      return -1;
    }
    grpc_resolved_address wildcard4_addr;
    grpc_resolved_address wildcard6_addr;
    grpc_sockaddr_make_wildcards(0, &wildcard4_addr, &wildcard6_addr);
    grpc_resolved_address* local_address = nullptr;
    if (address_family_ == AF_INET) {
      local_address = &wildcard4_addr;
    } else {
      local_address = &wildcard6_addr;
    }
    if (bind(s, (struct sockaddr*)local_address->addr,
             (int)local_address->len) != 0) {
      int wsa_last_error = WSAGetLastError();
      wsa_error_ctx->SetWSAError(wsa_last_error);
      char* msg = gpr_format_message(wsa_last_error);
      GRPC_CARES_TRACE_LOG("fd:%s bind error code:%d msg:|%s|", GetName(),
                           wsa_last_error, msg);
      gpr_free(msg);
      connect_done_ = true;
      wsa_connect_error_ = wsa_last_error;
      return -1;
    }
    int out = 0;
    if (ConnectEx(s, target, target_len, nullptr, 0, nullptr,
                  &winsocket_->write_info.overlapped) == 0) {
      out = -1;
      int wsa_last_error = WSAGetLastError();
      wsa_error_ctx->SetWSAError(wsa_last_error);
      char* msg = gpr_format_message(wsa_last_error);
      GRPC_CARES_TRACE_LOG("fd:%s ConnectEx error code:%d msg:|%s|", GetName(),
                           wsa_last_error, msg);
      gpr_free(msg);
      if (wsa_last_error == WSA_IO_PENDING) {
        wsa_error_ctx->SetWSAError(WSAEWOULDBLOCK);
      } else {
        connect_done_ = true;
        wsa_connect_error_ = wsa_last_error;
        return -1;
      }
    }
    grpc_socket_notify_on_write(winsocket_, &on_tcp_connect_locked_);
    return out;
  }
  static void OnIocpReadable(void* arg, grpc_error* error) {
    GrpcPolledFdWindows* polled_fd = static_cast<GrpcPolledFdWindows*>(arg);
    GRPC_ERROR_REF(error);
    polled_fd->work_serializer_->Run(
        [polled_fd, error]() { polled_fd->OnIocpReadableLocked(error); },
        DEBUG_LOCATION);
  }
  void OnIocpReadableLocked(grpc_error* error) {
    if (error == GRPC_ERROR_NONE) {
      if (winsocket_->read_info.wsa_error != 0) {
        if (winsocket_->read_info.wsa_error != WSAEMSGSIZE) {
          error = GRPC_WSA_ERROR(winsocket_->read_info.wsa_error,
                                 "OnIocpReadableInner");
          GRPC_CARES_TRACE_LOG(
              "fd:|%s| OnIocpReadableInner winsocket_->read_info.wsa_error "
              "code:|%d| msg:|%s|",
              GetName(), winsocket_->read_info.wsa_error,
              grpc_error_string(error));
        }
      }
    }
    if (error == GRPC_ERROR_NONE) {
      read_buf_ = grpc_slice_sub_no_ref(
          read_buf_, 0, winsocket_->read_info.bytes_transferred);
      read_buf_has_data_ = true;
    } else {
      grpc_slice_unref_internal(read_buf_);
      read_buf_ = grpc_empty_slice();
    }
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| OnIocpReadable finishing. read buf length now:|%d|", GetName(),
        GRPC_SLICE_LENGTH(read_buf_));
    ScheduleAndNullReadClosure(error);
  }
  static void OnIocpWriteable(void* arg, grpc_error* error) {
    GrpcPolledFdWindows* polled_fd = static_cast<GrpcPolledFdWindows*>(arg);
    GRPC_ERROR_REF(error);
    polled_fd->work_serializer_->Run(
        [polled_fd, error]() { polled_fd->OnIocpWriteableLocked(error); },
        DEBUG_LOCATION);
  }
  void OnIocpWriteableLocked(grpc_error* error) {
    GRPC_CARES_TRACE_LOG("OnIocpWriteableInner. fd:|%s|", GetName());
    GPR_ASSERT(socket_type_ == SOCK_STREAM);
    if (error == GRPC_ERROR_NONE) {
      if (winsocket_->write_info.wsa_error != 0) {
        error = GRPC_WSA_ERROR(winsocket_->write_info.wsa_error,
                               "OnIocpWriteableInner");
        GRPC_CARES_TRACE_LOG(
            "fd:|%s| OnIocpWriteableInner. winsocket_->write_info.wsa_error "
            "code:|%d| msg:|%s|",
            GetName(), winsocket_->write_info.wsa_error,
            grpc_error_string(error));
      }
    }
    GPR_ASSERT(tcp_write_state_ == WRITE_PENDING);
    if (error == GRPC_ERROR_NONE) {
      tcp_write_state_ = WRITE_WAITING_FOR_VERIFICATION_UPON_RETRY;
      write_buf_ = grpc_slice_sub_no_ref(
          write_buf_, 0, winsocket_->write_info.bytes_transferred);
      GRPC_CARES_TRACE_LOG("fd:|%s| OnIocpWriteableInner. bytes transferred:%d",
                           GetName(), winsocket_->write_info.bytes_transferred);
    } else {
      grpc_slice_unref_internal(write_buf_);
      write_buf_ = grpc_empty_slice();
    }
    ScheduleAndNullWriteClosure(error);
  }
  bool gotten_into_driver_list() const { return gotten_into_driver_list_; }
  void set_gotten_into_driver_list() { gotten_into_driver_list_ = true; }
  std::shared_ptr<WorkSerializer> work_serializer_;
  char recv_from_source_addr_[200];
  ares_socklen_t recv_from_source_addr_len_;
  grpc_slice read_buf_;
  bool read_buf_has_data_ = false;
  grpc_slice write_buf_;
  grpc_closure* read_closure_ = nullptr;
  grpc_closure* write_closure_ = nullptr;
  grpc_closure outer_read_closure_;
  grpc_closure outer_write_closure_;
  grpc_winsocket* winsocket_;
  WriteState tcp_write_state_;
  char* name_ = nullptr;
  bool gotten_into_driver_list_;
  int address_family_;
  int socket_type_;
  grpc_closure on_tcp_connect_locked_;
  bool connect_done_ = false;
  int wsa_connect_error_ = 0;
  bool pending_continue_register_for_on_readable_locked_ = false;
  bool pending_continue_register_for_on_writeable_locked_ = false;
};
struct SockToPolledFdEntry {
  SockToPolledFdEntry(SOCKET s, GrpcPolledFdWindows* fd)
      : socket(s), polled_fd(fd) {}
  SOCKET socket;
  GrpcPolledFdWindows* polled_fd;
  SockToPolledFdEntry* next = nullptr;
};
class SockToPolledFdMap {
 public:
  SockToPolledFdMap(std::shared_ptr<WorkSerializer> work_serializer)
      : work_serializer_(std::move(work_serializer)) {}
  ~SockToPolledFdMap() { GPR_ASSERT(head_ == nullptr); }
  void AddNewSocket(SOCKET s, GrpcPolledFdWindows* polled_fd) {
    SockToPolledFdEntry* new_node = new SockToPolledFdEntry(s, polled_fd);
    new_node->next = head_;
    head_ = new_node;
  }
  GrpcPolledFdWindows* LookupPolledFd(SOCKET s) {
    for (SockToPolledFdEntry* node = head_; node != nullptr;
         node = node->next) {
      if (node->socket == s) {
        GPR_ASSERT(node->polled_fd != nullptr);
        return node->polled_fd;
      }
    }
    abort();
  }
  void RemoveEntry(SOCKET s) {
    GPR_ASSERT(head_ != nullptr);
    SockToPolledFdEntry** prev = &head_;
    for (SockToPolledFdEntry* node = head_; node != nullptr;
         node = node->next) {
      if (node->socket == s) {
        *prev = node->next;
        delete node;
        return;
      }
      prev = &node->next;
    }
    abort();
  }
  static ares_socket_t Socket(int af, int type, int protocol, void* user_data) {
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
      GRPC_CARES_TRACE_LOG("Socket called with invalid socket type:%d", type);
      return INVALID_SOCKET;
    }
    SockToPolledFdMap* map = static_cast<SockToPolledFdMap*>(user_data);
    SOCKET s = WSASocket(af, type, protocol, nullptr, 0,
                         grpc_get_default_wsa_socket_flags());
    if (s == INVALID_SOCKET) {
      GRPC_CARES_TRACE_LOG(
          "WSASocket failed with params af:%d type:%d protocol:%d", af, type,
          protocol);
      return s;
    }
    grpc_tcp_set_non_block(s);
    GrpcPolledFdWindows* polled_fd =
        new GrpcPolledFdWindows(s, map->work_serializer_, af, type);
    GRPC_CARES_TRACE_LOG(
        "fd:|%s| created with params af:%d type:%d protocol:%d",
        polled_fd->GetName(), af, type, protocol);
    map->AddNewSocket(s, polled_fd);
    return s;
  }
  static int Connect(ares_socket_t as, const struct sockaddr* target,
                     ares_socklen_t target_len, void* user_data) {
    WSAErrorContext wsa_error_ctx;
    SockToPolledFdMap* map = static_cast<SockToPolledFdMap*>(user_data);
    GrpcPolledFdWindows* polled_fd = map->LookupPolledFd(as);
    return polled_fd->Connect(&wsa_error_ctx, target, target_len);
  }
  static ares_ssize_t SendV(ares_socket_t as, const struct iovec* iov,
                            int iovec_count, void* user_data) {
    WSAErrorContext wsa_error_ctx;
    SockToPolledFdMap* map = static_cast<SockToPolledFdMap*>(user_data);
    GrpcPolledFdWindows* polled_fd = map->LookupPolledFd(as);
    return polled_fd->SendV(&wsa_error_ctx, iov, iovec_count);
  }
  static ares_ssize_t RecvFrom(ares_socket_t as, void* data, size_t data_len,
                               int flags, struct sockaddr* from,
                               ares_socklen_t* from_len, void* user_data) {
    WSAErrorContext wsa_error_ctx;
    SockToPolledFdMap* map = static_cast<SockToPolledFdMap*>(user_data);
    GrpcPolledFdWindows* polled_fd = map->LookupPolledFd(as);
    return polled_fd->RecvFrom(&wsa_error_ctx, data, data_len, flags, from,
                               from_len);
  }
  static int CloseSocket(SOCKET s, void* user_data) {
    SockToPolledFdMap* map = static_cast<SockToPolledFdMap*>(user_data);
    GrpcPolledFdWindows* polled_fd = map->LookupPolledFd(s);
    map->RemoveEntry(s);
    GRPC_CARES_TRACE_LOG("CloseSocket called for socket: %s",
                         polled_fd->GetName());
    if (!polled_fd->gotten_into_driver_list()) {
      polled_fd->ShutdownLocked(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "Shut down c-ares fd before without it ever having made it into the "
          "driver's list"));
    }
    delete polled_fd;
    return 0;
  }
 private:
  SockToPolledFdEntry* head_ = nullptr;
  std::shared_ptr<WorkSerializer> work_serializer_;
};
const struct ares_socket_functions custom_ares_sock_funcs = {
    &SockToPolledFdMap::Socket ,
    &SockToPolledFdMap::CloseSocket ,
    &SockToPolledFdMap::Connect ,
    &SockToPolledFdMap::RecvFrom ,
    &SockToPolledFdMap::SendV ,
};
class GrpcPolledFdWindowsWrapper : public GrpcPolledFd {
 public:
  GrpcPolledFdWindowsWrapper(GrpcPolledFdWindows* wrapped)
      : wrapped_(wrapped) {}
  ~GrpcPolledFdWindowsWrapper() {}
  void RegisterForOnReadableLocked(grpc_closure* read_closure) override {
    wrapped_->RegisterForOnReadableLocked(read_closure);
  }
  void RegisterForOnWriteableLocked(grpc_closure* write_closure) override {
    wrapped_->RegisterForOnWriteableLocked(write_closure);
  }
  bool IsFdStillReadableLocked() override {
    return wrapped_->IsFdStillReadableLocked();
  }
  void ShutdownLocked(grpc_error* error) override {
    wrapped_->ShutdownLocked(error);
  }
  ares_socket_t GetWrappedAresSocketLocked() override {
    return wrapped_->GetWrappedAresSocketLocked();
  }
  const char* GetName() override { return wrapped_->GetName(); }
 private:
  GrpcPolledFdWindows* wrapped_;
};
class GrpcPolledFdFactoryWindows : public GrpcPolledFdFactory {
 public:
  GrpcPolledFdFactoryWindows(std::shared_ptr<WorkSerializer> work_serializer)
      : sock_to_polled_fd_map_(work_serializer) {}
  GrpcPolledFd* NewGrpcPolledFdLocked(
      ares_socket_t as, grpc_pollset_set* driver_pollset_set,
      std::shared_ptr<WorkSerializer> work_serializer) override {
    GrpcPolledFdWindows* polled_fd = sock_to_polled_fd_map_.LookupPolledFd(as);
    polled_fd->set_gotten_into_driver_list();
    return new GrpcPolledFdWindowsWrapper(polled_fd);
  }
  void ConfigureAresChannelLocked(ares_channel channel) override {
    ares_set_socket_functions(channel, &custom_ares_sock_funcs,
                              &sock_to_polled_fd_map_);
  }
 private:
  SockToPolledFdMap sock_to_polled_fd_map_;
};
std::unique_ptr<GrpcPolledFdFactory> NewGrpcPolledFdFactory(
    std::shared_ptr<WorkSerializer> work_serializer) {
  return absl::make_unique<GrpcPolledFdFactoryWindows>(
      std::move(work_serializer));
}
}
#endif
