       
#include <poll.h>
#include <sys/eventfd.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <android-base/unique_fd.h>
<<<<<<< HEAD
#include <snapuserd/block_server.h>
#include "handler_manager.h"
||||||| d61ce22b0
#include "handler_manager.h"
=======
>>>>>>> 8cbbc2dc
#include "snapuserd_core.h"
namespace android {
namespace snapshot {
static constexpr uint32_t kMaxPacketSize = 512;
static constexpr uint8_t kMaxMergeThreads = 2;
enum class DaemonOps {
    INIT,
    START,
    QUERY,
    STOP,
    DELETE,
    DETACH,
    SUPPORTS,
    INITIATE,
    PERCENTAGE,
    GETSTATUS,
    UPDATE_VERIFY,
    INVALID,
};
class HandlerThread {
  public:
    explicit HandlerThread(std::shared_ptr<SnapshotHandler> snapuserd);
    void FreeResources() {
        if (snapuserd_) {
            snapuserd_->FreeResources();
            snapuserd_ = nullptr;
        }
    }
    const std::shared_ptr<SnapshotHandler>& snapuserd() const { return snapuserd_; }
    std::thread& thread() { return thread_; }
    const std::string& misc_name() const { return misc_name_; }
    bool ThreadTerminated() { return thread_terminated_; }
    void SetThreadTerminated() { thread_terminated_ = true; }
  private:
    std::thread thread_;
    std::shared_ptr<SnapshotHandler> snapuserd_;
    std::string misc_name_;
    bool thread_terminated_ = false;
};
class UserSnapshotServer {
  private:
    android::base::unique_fd sockfd_;
    bool terminating_;
    volatile bool received_socket_signal_ = false;
    std::vector<struct pollfd> watched_fds_;
    bool is_socket_present_ = false;
    int num_partitions_merge_complete_ = 0;
    int active_merge_threads_ = 0;
    bool stop_monitor_merge_thread_ = false;
    bool is_server_running_ = false;
    bool io_uring_enabled_ = false;
<<<<<<< HEAD
    std::unique_ptr<ISnapshotHandlerManager> handlers_;
    std::unique_ptr<IBlockServerFactory> block_server_factory_;
||||||| d61ce22b0
    std::unique_ptr<ISnapshotHandlerManager> handlers_;
=======
    std::optional<bool> is_merge_monitor_started_;
    android::base::unique_fd monitor_merge_event_fd_;
>>>>>>> 8cbbc2dc
    std::mutex lock_;
    using HandlerList = std::vector<std::shared_ptr<HandlerThread>>;
    HandlerList dm_users_;
    std::queue<std::shared_ptr<HandlerThread>> merge_handlers_;
    void AddWatchedFd(android::base::borrowed_fd fd, int events);
    void AcceptClient();
    bool HandleClient(android::base::borrowed_fd fd, int revents);
    bool Recv(android::base::borrowed_fd fd, std::string* data);
    bool Sendmsg(android::base::borrowed_fd fd, const std::string& msg);
    bool Receivemsg(android::base::borrowed_fd fd, const std::string& str);
    void ShutdownThreads();
    bool RemoveAndJoinHandler(const std::string& control_device);
    DaemonOps Resolveop(std::string& input);
    std::string GetDaemonStatus();
    void Parsemsg(std::string const& msg, const char delim, std::vector<std::string>& out);
    bool IsTerminating() { return terminating_; }
    void RunThread(std::shared_ptr<HandlerThread> handler);
    void MonitorMerge();
    void JoinAllThreads();
    bool StartWithSocket(bool start_listening);
    HandlerList::iterator FindHandler(std::lock_guard<std::mutex>* proof_of_lock,
                                      const std::string& misc_name);
    double GetMergePercentage(std::lock_guard<std::mutex>* proof_of_lock);
    void TerminateMergeThreads(std::lock_guard<std::mutex>* proof_of_lock);
    bool UpdateVerification(std::lock_guard<std::mutex>* proof_of_lock);
  public:
    UserSnapshotServer();
    ~UserSnapshotServer();
    bool Start(const std::string& socketname);
    bool Run();
    void Interrupt();
    bool RunForSocketHandoff();
    bool WaitForSocket();
    std::shared_ptr<HandlerThread> AddHandler(const std::string& misc_name,
                                              const std::string& cow_device_path,
                                              const std::string& backing_device,
                                              const std::string& base_path_merge);
    bool StartHandler(const std::shared_ptr<HandlerThread>& handler);
    bool StartMerge(std::lock_guard<std::mutex>* proof_of_lock,
                    const std::shared_ptr<HandlerThread>& handler);
    std::string GetMergeStatus(const std::shared_ptr<HandlerThread>& handler);
    void WakeupMonitorMergeThread();
    void SetTerminating() { terminating_ = true; }
    void ReceivedSocketSignal() { received_socket_signal_ = true; }
    void SetServerRunning() { is_server_running_ = true; }
    bool IsServerRunning() { return is_server_running_; }
    void SetIouringEnabled() { io_uring_enabled_ = true; }
    bool IsIouringEnabled() { return io_uring_enabled_; }
};
}
}
