#include <grpc/support/port_platform.h>
#include "src/core/lib/iomgr/port.h"
#if GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET_ARES_EV_DRIVER)
#include <ares.h>
#include <string.h>
#include <sys/ioctl.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
namespace grpc_core {
class GrpcPolledFdPosix : public GrpcPolledFd {
 public:
  GrpcPolledFdPosix(ares_socket_t as, grpc_pollset_set* driver_pollset_set)
      : as_(as) {
    gpr_asprintf(&name_, "c-ares fd: %d", (int)as);
    fd_ = grpc_fd_create((int)as, name_, false);
    driver_pollset_set_ = driver_pollset_set;
    grpc_pollset_set_add_fd(driver_pollset_set_, fd_);
  }
  ~GrpcPolledFdPosix() {
    gpr_free(name_);
    grpc_pollset_set_del_fd(driver_pollset_set_, fd_);
    int dummy_release_fd;
    grpc_fd_orphan(fd_, nullptr, &dummy_release_fd, "c-ares query finished");
  }
  void RegisterForOnReadableLocked(grpc_closure* read_closure) override {
    grpc_fd_notify_on_read(fd_, read_closure);
  }
  void RegisterForOnWriteableLocked(grpc_closure* write_closure) override {
    grpc_fd_notify_on_write(fd_, write_closure);
  }
  bool IsFdStillReadableLocked() override {
    size_t bytes_available = 0;
    return ioctl(grpc_fd_wrapped_fd(fd_), FIONREAD, &bytes_available) == 0 &&
           bytes_available > 0;
  }
  void ShutdownLocked(grpc_error* error) override {
    grpc_fd_shutdown(fd_, error);
  }
  ares_socket_t GetWrappedAresSocketLocked() override { return as_; }
  const char* GetName() override { return name_; }
  char* name_;
  ares_socket_t as_;
  grpc_fd* fd_;
  grpc_pollset_set* driver_pollset_set_;
};
class GrpcPolledFdFactoryPosix : public GrpcPolledFdFactory {
 public:
  GrpcPolledFd* NewGrpcPolledFdLocked(
      ares_socket_t as, grpc_pollset_set* driver_pollset_set,
      std::shared_ptr<WorkSerializer> ) override {
    return new GrpcPolledFdPosix(as, driver_pollset_set);
  }
  void ConfigureAresChannelLocked(ares_channel ) override {}
};
std::unique_ptr<GrpcPolledFdFactory> NewGrpcPolledFdFactory(
    std::shared_ptr<WorkSerializer> work_serializer) {
  return absl::make_unique<GrpcPolledFdFactoryPosix>();
}
}
#endif
