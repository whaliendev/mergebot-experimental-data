       
#include <signal.h>
#include <sys/types.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <android-base/chrono_utils.h>
#include <cutils/iosched_policy.h>
#include "action.h"
#include "capabilities.h"
#include "interprocess_fifo.h"
#include "keyword_map.h"
#include "mount_namespace.h"
#include "parser.h"
#include "service_utils.h"
#include "subcontext.h"
#define SVC_DISABLED 0x001
#define SVC_ONESHOT 0x002
#define SVC_RUNNING 0x004
#define SVC_RESTARTING 0x008
#define SVC_CONSOLE 0x010
#define SVC_CRITICAL 0x020
#define SVC_RESET 0x040
#define SVC_RC_DISABLED 0x080
#define SVC_RESTART 0x100
#define SVC_DISABLED_START 0x200
#define SVC_EXEC 0x400
#define SVC_SHUTDOWN_CRITICAL 0x800
#define SVC_TEMPORARY 0x1000
#define SVC_GENTLE_KILL 0x2000
#define NR_SVC_SUPP_GIDS 32
#define NR_SVC_SUPP_GIDS 12
namespace android {
namespace init {
class Service {
    friend class ServiceParser;
public:
    Service(const std::string& name, Subcontext* subcontext_for_restart_commands,
            const std::string& filename, const std::vector<std::string>& args);
    Service(const std::string& name, unsigned flags, std::optional<uid_t> uid, gid_t gid,
            const std::vector<gid_t>& supp_gids, int namespace_flags, const std::string& seclabel,
            Subcontext* subcontext_for_restart_commands, const std::string& filename,
            const std::vector<std::string>& args);
    Service(const Service&)
    void operator=(const Service&) = delete
bool Service::is_exec_service_running_ = false;
    bool IsRunning() { return (flags_ & SVC_RUNNING) != 0; }
    bool IsEnabled() { return (flags_ & SVC_DISABLED) == 0; }
    Result<void> ExecStart();
    Result<void> Start();
    Result<void> StartIfNotDisabled();
    Result<void> Enable();
    void Reset();
    void Stop();
    void Terminate();
    void Timeout();
    void Restart();
    void Reap(const siginfo_t& siginfo);
    void DumpState() const;
    void SetShutdownCritical() { flags_ |= SVC_SHUTDOWN_CRITICAL; }
    bool IsShutdownCritical() const { return (flags_ & SVC_SHUTDOWN_CRITICAL) != 0; }
    void UnSetExec() {
        is_exec_service_running_ = false;
        flags_ &= ~SVC_EXEC;
    }
    void AddReapCallback(std::function<void(const siginfo_t& siginfo)> callback) {
        reap_callbacks_.emplace_back(std::move(callback));
    }
    void SetStartedInFirstStage(pid_t pid);
    bool MarkSocketPersistent(const std::string& socket_name);
    size_t CheckAllCommands() const { return onrestart_.CheckAllCommands(){ return onrestart_.CheckAllCommands(); }
    static bool is_exec_service_running() { return is_exec_service_running_; }
    const std::string& name() const { return name_; }
    const std::set<std::string>& classnames() const { return classnames_; }
    unsigned flags() const { return flags_; }
    pid_t pid() const { return pid_; }
    android::base::boot_clock::time_point time_started() const { return time_started_; }
    int crash_count() const { return crash_count_; }
    int was_last_exit_ok() const { return was_last_exit_ok_; }
    uid_t uid() const { return proc_attr_.uid(){ return proc_attr_.uid(); }
    gid_t gid() const { return proc_attr_.gid; }
    int namespace_flags() const { return namespaces_.flags; }
    const std::vector<gid_t>& supp_gids() const { return proc_attr_.supp_gids; }
    const std::string& seclabel() const { return seclabel_; }
    const std::vector<int>& keycodes() const { return keycodes_; }
    IoSchedClass ioprio_class() const { return proc_attr_.ioprio_class; }
    int ioprio_pri() const { return proc_attr_.ioprio_pri; }
    const std::set<std::string>& interfaces() const { return interfaces_; }
    int priority() const { return proc_attr_.priority; }
    int oom_score_adjust() const { return oom_score_adjust_; }
    bool is_override() const { return override_; }
    bool process_cgroup_empty() const { return process_cgroup_empty_; }
    unsigned long start_order() const { return start_order_; }
    void set_sigstop(bool value) { sigstop_ = value; }
    std::chrono::seconds restart_period() const {
        if (!was_last_exit_ok_) {
            return std::max(restart_period_, default_restart_period_);
        }
        return restart_period_;
    }
    std::optional<std::chrono::seconds> timeout_period() const { return timeout_period_; }
    const std::vector<std::string>& args() const { return args_; }
    bool is_updatable() const { return updatable_; }
    bool is_post_data() const { return post_data_; }
    bool is_from_apex() const { return base::StartsWith(filename_, "/apex/"); }
    void set_oneshot(bool value) {
        if (value) {
            flags_ |= SVC_ONESHOT;
        } else {
            flags_ &= ~SVC_ONESHOT;
        }
    }
    const Subcontext* subcontext() const { return subcontext_; }
    const std::string& filename() const { return filename_; }
    void set_filename(const std::string& name) { filename_ = name; }
    static int GetSigchldFd() {
        static int sigchld_fd = CreateSigchldFd().release();
        return sigchld_fd;
    }
private:
    void NotifyStateChange(const std::string& new_state) const;
    void StopOrReset(int how);
    void KillProcessGroup(int signal);
    void SetProcessAttributesAndCaps(InterprocessFifo setsid_finished);
    void ResetFlagsForStart();
    Result<void> CheckConsole();
    void ConfigureMemcg();
    void RunService(const std::vector<Descriptor>& descriptors, InterprocessFifo cgroups_activated,
                    InterprocessFifo setsid_finished);
    void SetMountNamespace();
    static ::android::base::unique_fd CreateSigchldFd();
    static unsigned long next_start_order_;
    static bool is_exec_service_running_;
    const std::string name_;
    std::set<std::string> classnames_;
    unsigned flags_;
    pid_t pid_;
android::base::boot_clock::time_point time_started_;
android::base::boot_clock::time_point time_crashed_;
int crash_count_;
bool upgraded_mte_ = false;
std::chrono::minutes fatal_crash_window_ = 4min;
std::optional<std::string> fatal_reboot_target_;
bool was_last_exit_ok_ =
            true;
    std::optional<CapSet> capabilities_;
    ProcessAttributes proc_attr_;
    NamespaceInfo namespaces_;
    std::string seclabel_;
    std::vector<SocketDescriptor> sockets_;
    std::vector<FileDescriptor> files_;
bool Service::is_exec_service_running_ = false;
bool Service::is_exec_service_running_ = false;
    const Subcontext* const subcontext_;
Action onrestart_;
    std::vector<std::string> writepid_files_;
    std::vector<std::string> task_profiles_;
std::set<std::string> interfaces_;
    std::vector<int> keycodes_;
    int oom_score_adjust_;
    int swappiness_ = -1;
    int soft_limit_in_bytes_ = -1;
    int limit_in_bytes_ = -1;
    int limit_percent_ = -1;
    std::string limit_property_;
    bool process_cgroup_empty_ = false;
    bool override_ = false;
    unsigned long start_order_;
    bool sigstop_ = false;
    const std::chrono::seconds default_restart_period_ = 5s;
    std::chrono::seconds restart_period_ = default_restart_period_;
    std::optional<std::chrono::seconds> timeout_period_;
    bool updatable_ = false;
    const std::vector<std::string> args_;
bool Service::is_exec_service_running_ = false;
    std::optional<MountNamespace> mount_namespace_;
    bool post_data_ = false;
    std::optional<std::string> on_failure_reboot_target_;
    std::string filename_;
};
}
}
