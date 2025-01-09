#include "ueventd.h"
#include <ctype.h>
#include <fcntl.h>
<<<<<<< HEAD
||||||| 5f4e8eac8
#include <grp.h>
#include <pwd.h>
=======
#include <grp.h>
#include <poll.h>
#include <pwd.h>
>>>>>>> 0e63e61e
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <set>
#include <thread>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <selinux/android.h>
#include <selinux/selinux.h>
#include "devices.h"
#include "firmware_handler.h"
#include "log.h"
#include "uevent_listener.h"
#include "ueventd_parser.h"
#include "util.h"
class ColdBoot {
  public:
    ColdBoot(UeventListener& uevent_listener, DeviceHandler& device_handler)
        : uevent_listener_(uevent_listener),
          device_handler_(device_handler),
          num_handler_subprocesses_(std::thread::hardware_concurrency() ?: 4) {}
    void Run();
  private:
    void UeventHandlerMain(unsigned int process_num, unsigned int total_processes);
    void RegenerateUevents();
    void ForkSubProcesses();
    void DoRestoreCon();
    void WaitForSubProcesses();
    UeventListener& uevent_listener_;
    DeviceHandler& device_handler_;
    unsigned int num_handler_subprocesses_;
    std::vector<Uevent> uevent_queue_;
    std::set<pid_t> subprocess_pids_;
};
void ColdBoot::UeventHandlerMain(unsigned int process_num, unsigned int total_processes) {
    for (unsigned int i = process_num; i < uevent_queue_.size(); i += total_processes) {
        auto& uevent = uevent_queue_[i];
        device_handler_.HandleDeviceEvent(uevent);
    }
    _exit(EXIT_SUCCESS);
}
void ColdBoot::RegenerateUevents() {
    uevent_listener_.RegenerateUevents([this](const Uevent& uevent) {
        HandleFirmwareEvent(uevent);
        uevent_queue_.emplace_back(std::move(uevent));
        return ListenerAction::kContinue;
    });
}
void ColdBoot::ForkSubProcesses() {
    for (unsigned int i = 0; i < num_handler_subprocesses_; ++i) {
        auto pid = fork();
        if (pid < 0) {
            PLOG(FATAL) << "fork() failed!";
        }
        if (pid == 0) {
            UeventHandlerMain(i, num_handler_subprocesses_);
        }
        subprocess_pids_.emplace(pid);
    }
}
void ColdBoot::DoRestoreCon() {
    selinux_android_restorecon("/sys", SELINUX_ANDROID_RESTORECON_RECURSE);
    device_handler_.set_skip_restorecon(false);
}
void ColdBoot::WaitForSubProcesses() {
    while (!subprocess_pids_.empty()) {
        int status;
        pid_t pid = TEMP_FAILURE_RETRY(waitpid(-1, &status, 0));
        if (pid == -1) {
            PLOG(ERROR) << "waitpid() failed";
            continue;
        }
        auto it = std::find(subprocess_pids_.begin(), subprocess_pids_.end(), pid);
        if (it == subprocess_pids_.end()) continue;
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == EXIT_SUCCESS) {
                subprocess_pids_.erase(it);
            } else {
                LOG(FATAL) << "subprocess exited with status " << WEXITSTATUS(status);
            }
        } else if (WIFSIGNALED(status)) {
            LOG(FATAL) << "subprocess killed by signal " << WTERMSIG(status);
        }
    }
}
void ColdBoot::Run() {
    Timer cold_boot_timer;
    RegenerateUevents();
    ForkSubProcesses();
    DoRestoreCon();
    WaitForSubProcesses();
    close(open(COLDBOOT_DONE, O_WRONLY | O_CREAT | O_CLOEXEC, 0000));
    LOG(INFO) << "Coldboot took " << cold_boot_timer;
}
DeviceHandler CreateDeviceHandler() {
    Parser parser;
    std::vector<Subsystem> subsystems;
    parser.AddSectionParser("subsystem", std::make_unique<SubsystemParser>(&subsystems));
    using namespace std::placeholders;
    std::vector<SysfsPermissions> sysfs_permissions;
    std::vector<Permissions> dev_permissions;
    parser.AddSingleLineParser(
        "/sys/", std::bind(ParsePermissionsLine, _1, _2, &sysfs_permissions, nullptr));
    parser.AddSingleLineParser("/dev/",
                               std::bind(ParsePermissionsLine, _1, _2, nullptr, &dev_permissions));
    parser.ParseConfig("/ueventd.rc");
    parser.ParseConfig("/vendor/ueventd.rc");
    parser.ParseConfig("/odm/ueventd.rc");
    std::string hardware = android::base::GetProperty("ro.hardware", "");
    parser.ParseConfig("/ueventd." + hardware + ".rc");
    return DeviceHandler(std::move(dev_permissions), std::move(sysfs_permissions),
                         std::move(subsystems), true);
}
<<<<<<< HEAD
int ueventd_main(int argc, char** argv) {
    umask(000);
    InitKernelLogging(argv);
    LOG(INFO) << "ueventd started!";
    selinux_callback cb;
    cb.func_log = selinux_klog_callback;
    selinux_set_callback(SELINUX_CB_LOG, cb);
    DeviceHandler device_handler = CreateDeviceHandler();
    UeventListener uevent_listener;
    if (access(COLDBOOT_DONE, F_OK) != 0) {
        ColdBoot cold_boot(uevent_listener, device_handler);
        cold_boot.Run();
    }
    uevent_listener.Poll([&device_handler](const Uevent& uevent) {
        HandleFirmwareEvent(uevent);
        device_handler.HandleDeviceEvent(uevent);
        return ListenerAction::kContinue;
    });
||||||| 5f4e8eac8
    device_poll();
=======
    pollfd ufd;
    ufd.events = POLLIN;
    ufd.fd = get_device_fd();
    while (true) {
        ufd.revents = 0;
        int nr = poll(&ufd, 1, -1);
        if (nr <= 0) {
            continue;
        }
        if (ufd.revents & POLLIN) {
            handle_device_fd();
        }
    }
>>>>>>> 0e63e61e
    return 0;
}
