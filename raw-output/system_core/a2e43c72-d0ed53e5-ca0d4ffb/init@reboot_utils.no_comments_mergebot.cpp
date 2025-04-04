#include <sys/capability.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <android-base/logging.h>
#include <cutils/android_reboot.h>
#include <string>
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/strings.h"
#include "backtrace/Backtrace.h"
#include "cutils/android_reboot.h"
#include "capabilities.h"
namespace android {
namespace init {
bool IsRebootCapable() {
    if (!CAP_IS_SUPPORTED(CAP_SYS_BOOT)) {
        PLOG(WARNING) << "CAP_SYS_BOOT is not supported";
        return true;
    }
    ScopedCaps caps(cap_get_proc());
    if (!caps) {
        PLOG(WARNING) << "cap_get_proc() failed";
        return true;
    }
    cap_flag_value_t value = CAP_SET;
    if (cap_get_flag(caps.get(), CAP_SYS_BOOT, CAP_EFFECTIVE, &value) != 0) {
        PLOG(WARNING) << "cap_get_flag(CAP_SYS_BOOT, EFFECTIVE) failed";
        return true;
    }
    return value == CAP_SET;
}
void __attribute__((noreturn)) RebootSystem(unsigned int cmd, const std::string& rebootTarget) {
    LOG(INFO) << "Reboot ending, jumping to kernel";
    if (!IsRebootCapable()) {
        exit(0);
    }
    switch (cmd) {
        case ANDROID_RB_POWEROFF:
            reboot(RB_POWER_OFF);
            break;
        case ANDROID_RB_RESTART2:
            syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                    LINUX_REBOOT_CMD_RESTART2, rebootTarget.c_str());
            break;
        case ANDROID_RB_THERMOFF:
            reboot(RB_POWER_OFF);
            break;
    }
    PLOG(ERROR) << "reboot call returned";
    abort();
}
void __attribute__((noreturn)) InitFatalReboot() {
    auto pid = fork();
    if (pid == -1) {
        RebootSystem(ANDROID_RB_RESTART2, "bootloader");
    } else if (pid == 0) {
        sleep(5);
        RebootSystem(ANDROID_RB_RESTART2, "bootloader");
    }
    std::unique_ptr<Backtrace> backtrace(
            Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
    if (!backtrace->Unwind(0)) {
        LOG(ERROR) << __FUNCTION__ << ": Failed to unwind callstack.";
    }
    for (size_t i = 0; i < backtrace->NumFrames(); i++) {
        LOG(ERROR) << backtrace->FormatFrameData(i);
    }
    RebootSystem(ANDROID_RB_RESTART2, "bootloader");
}
void InstallRebootSignalHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigfillset(&action.sa_mask);
    action.sa_handler = [](int signal) {
        if (getpid() != 1) {
            _exit(signal);
        }
        InitFatalReboot();
    };
    action.sa_flags = SA_RESTART;
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGSEGV, &action, nullptr);
#if defined(SIGSTKFLT)
    sigaction(SIGSTKFLT, &action, nullptr);
#endif
    sigaction(SIGSYS, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);
}
static std::string init_fatal_reboot_target = "bootloader";
}
}
