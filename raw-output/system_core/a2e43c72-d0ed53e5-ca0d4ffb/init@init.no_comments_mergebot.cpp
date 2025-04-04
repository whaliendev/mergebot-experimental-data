#include "init.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <seccomp_policy.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <optional>
#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <cutils/android_reboot.h>
#include <fs_avb/fs_avb.h>
#include <fs_mgr_vendor_overlay.h>
#include <keyutils.h>
#include <libavb/libavb.h>
#include <libgsi/libgsi.h>
#include <processgroup/processgroup.h>
#include <processgroup/setup.h>
#include <selinux/android.h>
#ifndef RECOVERY
#include <binder/ProcessState.h>
#endif
#include "action_parser.h"
#include "boringssl_self_test.h"
#include "epoll.h"
#include "first_stage_init.h"
#include "first_stage_mount.h"
#include "import_parser.h"
#include "keychords.h"
#include "mount_handler.h"
#include "mount_namespace.h"
#include "property_service.h"
#include "reboot.h"
#include "reboot_utils.h"
#include "security.h"
#include "selabel.h"
#include "selinux.h"
#include "sigchld_handler.h"
#include "util.h"
using namespace std::chrono_literals;
using namespace std::string_literals;
using android::base::boot_clock;
using android::base::GetProperty;
using android::base::ReadFileToString;
using android::base::StringPrintf;
using android::base::Timer;
using android::base::Trim;
using android::fs_mgr::AvbHandle;
namespace android {
namespace init {
static int property_triggers_enabled = 0;
static char qemu[32];
extern std::string default_console;
static int signal_fd = -1;
static std::unique_ptr<Timer> waiting_for_prop(nullptr);
static std::string wait_prop_name;
static std::string wait_prop_value;
static bool shutting_down;
static std::string shutdown_command;
static bool do_shutdown = false;
static bool load_debug_prop = false;
std::vector<std::string> late_import_paths;
static std::vector<Subcontext>* subcontexts;
void DumpState() {
    ServiceList::GetInstance().DumpState();
    ActionManager::GetInstance().DumpState();
}
Parser CreateParser(ActionManager& action_manager, ServiceList& service_list) {
    Parser parser;
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, subcontexts));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));
    return parser;
}
Parser CreateServiceOnlyParser(ServiceList& service_list) {
    Parser parser;
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontexts));
    return parser;
}
static void LoadBootScripts(ActionManager& action_manager, ServiceList& service_list) {
    Parser parser = CreateParser(action_manager, service_list);
    std::string bootscript = GetProperty("ro.boot.init_rc", "");
    if (bootscript.empty()) {
        parser.ParseConfig("/init.rc");
        if (!parser.ParseConfig("/system/etc/init")) {
            late_import_paths.emplace_back("/system/etc/init");
        }
        if (!parser.ParseConfig("/product/etc/init")) {
            late_import_paths.emplace_back("/product/etc/init");
        }
        if (!parser.ParseConfig("/product_services/etc/init")) {
            late_import_paths.emplace_back("/product_services/etc/init");
        }
        if (!parser.ParseConfig("/odm/etc/init")) {
            late_import_paths.emplace_back("/odm/etc/init");
        }
        if (!parser.ParseConfig("/vendor/etc/init")) {
            late_import_paths.emplace_back("/vendor/etc/init");
        }
    } else {
        parser.ParseConfig(bootscript);
    }
}
bool start_waiting_for_property(const char *name, const char *value)
{
    if (waiting_for_prop) {
        return false;
    }
    if (GetProperty(name, "") != value) {
        wait_prop_name = name;
        wait_prop_value = value;
        waiting_for_prop.reset(new Timer());
    } else {
        LOG(INFO) << "start_waiting_for_property(\""
                  << name << "\", \"" << value << "\"): already set";
    }
    return true;
}
void ResetWaitForProp() {
    wait_prop_name.clear();
    wait_prop_value.clear();
    waiting_for_prop.reset();
}
void property_changed(const std::string& name, const std::string& value) {
    if (name == "sys.powerctl") {
        shutdown_command = value;
        do_shutdown = true;
    }
    if (property_triggers_enabled) ActionManager::GetInstance().QueuePropertyChange(name, value);
    if (waiting_for_prop) {
        if (wait_prop_name == name && wait_prop_value == value) {
            LOG(INFO) << "Wait for property '" << wait_prop_name << "=" << wait_prop_value
                      << "' took " << *waiting_for_prop;
            ResetWaitForProp();
        }
    }
}
static std::optional<boot_clock::time_point> HandleProcessActions() {
    std::optional<boot_clock::time_point> next_process_action_time;
    for (const auto& s : ServiceList::GetInstance()) {
        if ((s->flags() & SVC_RUNNING) && s->timeout_period()) {
            auto timeout_time = s->time_started() + *s->timeout_period();
            if (boot_clock::now() > timeout_time) {
                s->Timeout();
            } else {
                if (!next_process_action_time || timeout_time < *next_process_action_time) {
                    next_process_action_time = timeout_time;
                }
            }
        }
        if (!(s->flags() & SVC_RESTARTING)) continue;
        auto restart_time = s->time_started() + s->restart_period();
        if (boot_clock::now() > restart_time) {
            if (auto result = s->Start(); !result) {
                LOG(ERROR) << "Could not restart process '" << s->name() << "': " << result.error();
            }
        } else {
            if (!next_process_action_time || restart_time < *next_process_action_time) {
                next_process_action_time = restart_time;
            }
        }
    }
    return next_process_action_time;
}
static Result<Success> DoControlStart(Service* service) {
    return service->Start();
}
static Result<Success> DoControlStop(Service* service) {
    service->Stop();
    return Success();
}
static Result<Success> DoControlRestart(Service* service) {
    service->Restart();
    return Success();
}
enum class ControlTarget {
SERVICE,
INTERFACE,
};
struct ControlMessageFunction {
    ControlTarget target;
    std::function<Result<Success>(Service*)> action;
};
static const std::map<std::string, ControlMessageFunction>& get_control_message_map() {
    static const std::map<std::string, ControlMessageFunction> control_message_functions = {
        {"sigstop_on", {ControlTarget::SERVICE,
                               [](auto* service) { service->set_sigstop(true); return Success(); }}},
        {"sigstop_off", {ControlTarget::SERVICE,
                               [](auto* service) { service->set_sigstop(false); return Success(); }}},
        {"start", {ControlTarget::SERVICE, DoControlStart}},
        {"stop", {ControlTarget::SERVICE, DoControlStop}},
        {"restart", {ControlTarget::SERVICE, DoControlRestart}},
        {"interface_start", {ControlTarget::INTERFACE, DoControlStart}},
        {"interface_stop", {ControlTarget::INTERFACE, DoControlStop}},
        {"interface_restart", {ControlTarget::INTERFACE, DoControlRestart}},
    };
    return control_message_functions;
}
bool HandleControlMessage(const std::string& msg, const std::string& name, pid_t pid) {
    const auto& map = get_control_message_map();
    const auto it = map.find(msg);
    if (it == map.end()) {
        LOG(ERROR) << "Unknown control msg '" << msg << "'";
        return false;
    }
    std::string cmdline_path = StringPrintf("proc/%d/cmdline", pid);
    std::string process_cmdline;
    if (ReadFileToString(cmdline_path, &process_cmdline)) {
        std::replace(process_cmdline.begin(), process_cmdline.end(), '\0', ' ');
        process_cmdline = Trim(process_cmdline);
    } else {
        process_cmdline = "unknown process";
    }
    LOG(INFO) << "Received control message '" << msg << "' for '" << name << "' from pid: " << pid
              << " (" << process_cmdline << ")";
    const ControlMessageFunction& function = it->second;
    Service* svc = nullptr;
    switch (function.target) {
        case ControlTarget::SERVICE:
            svc = ServiceList::GetInstance().FindService(name);
            break;
        case ControlTarget::INTERFACE:
            svc = ServiceList::GetInstance().FindInterface(name);
            break;
        default:
            LOG(ERROR) << "Invalid function target from static map key '" << msg << "': "
                       << static_cast<std::underlying_type<ControlTarget>::type>(function.target);
            return false;
    }
    if (svc == nullptr) {
        LOG(ERROR) << "Could not find '" << name << "' for ctl." << msg;
        return false;
    }
    if (auto result = function.action(svc); !result) {
        LOG(ERROR) << "Could not ctl." << msg << " for '" << name << "': " << result.error();
        return false;
    }
    return true;
}
static Result<Success> wait_for_coldboot_done_action(const BuiltinArguments& args) {
    Timer t;
    LOG(VERBOSE) << "Waiting for " COLDBOOT_DONE "...";
    if (wait_for_file(COLDBOOT_DONE, 60s) < 0) {
        LOG(FATAL) << "Timed out waiting for " COLDBOOT_DONE;
    }
    property_set("ro.boottime.init.cold_boot_wait", std::to_string(t.duration().count()));
    return Success();
}
static Result<Success> console_init_action(const BuiltinArguments& args) {
    std::string console = GetProperty("ro.boot.console", "");
    if (!console.empty()) {
        default_console = "/dev/" + console;
    }
    return Success();
}
static Result<Success> SetupCgroupsAction(const BuiltinArguments&) {
    make_dir(android::base::Dirname(CGROUPS_RC_PATH), 0711);
    if (!CgroupSetup()) {
        return ErrnoError() << "Failed to setup cgroups";
    }
    return Success();
}
static void import_kernel_nv(const std::string& key, const std::string& value, bool for_emulator) {
    if (key.empty()) return;
    if (for_emulator) {
        property_set("ro.kernel." + key, value);
        return;
    }
    if (key == "qemu") {
        strlcpy(qemu, value.c_str(), sizeof(qemu));
    } else if (android::base::StartsWith(key, "androidboot.")) {
        property_set("ro.boot." + key.substr(12), value);
    }
}
static void export_oem_lock_status() {
    if (!android::base::GetBoolProperty("ro.oem_unlock_supported", false)) {
        return;
    }
    import_kernel_cmdline(
            false, [](const std::string& key, const std::string& value, bool in_qemu) {
                if (key == "androidboot.verifiedbootstate") {
                    property_set("ro.boot.flash.locked", value == "orange" ? "0" : "1");
                }
            });
}
static void export_kernel_boot_props() {
    constexpr const char* UNSET = "";
    struct {
        const char *src_prop;
        const char *dst_prop;
        const char *default_value;
    } prop_map[] = {
        { "ro.boot.serialno", "ro.serialno", UNSET, },
        { "ro.boot.mode", "ro.bootmode", "unknown", },
        { "ro.boot.baseband", "ro.baseband", "unknown", },
        { "ro.boot.bootloader", "ro.bootloader", "unknown", },
        { "ro.boot.hardware", "ro.hardware", "unknown", },
        { "ro.boot.revision", "ro.revision", "0", },
    };
    for (const auto& prop : prop_map) {
        std::string value = GetProperty(prop.src_prop, prop.default_value);
        if (value != UNSET)
            property_set(prop.dst_prop, value);
    }
}
static void process_kernel_dt() {
    if (!is_android_dt_value_expected("compatible", "android,firmware")) {
        return;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(get_android_dt_dir().c_str()), closedir);
    if (!dir) return;
    std::string dt_file;
    struct dirent *dp;
    while ((dp = readdir(dir.get())) != NULL) {
        if (dp->d_type != DT_REG || !strcmp(dp->d_name, "compatible") || !strcmp(dp->d_name, "name")) {
            continue;
        }
        std::string file_name = get_android_dt_dir() + dp->d_name;
        android::base::ReadFileToString(file_name, &dt_file);
        std::replace(dt_file.begin(), dt_file.end(), ',', '.');
        property_set("ro.boot."s + dp->d_name, dt_file);
    }
}
static void process_kernel_cmdline() {
    import_kernel_cmdline(false, import_kernel_nv);
    if (qemu[0]) import_kernel_cmdline(true, import_kernel_nv);
}
static Result<Success> property_enable_triggers_action(const BuiltinArguments& args) {
    property_triggers_enabled = 1;
    return Success();
}
static Result<Success> queue_property_triggers_action(const BuiltinArguments& args) {
    ActionManager::GetInstance().QueueBuiltinAction(property_enable_triggers_action, "enable_property_trigger");
    ActionManager::GetInstance().QueueAllPropertyActions();
    return Success();
}
static Result<Success> InitBinder(const BuiltinArguments& args) {
#ifndef RECOVERY
    android::ProcessState::self()->setThreadPoolMaxThreadCount(0);
    android::ProcessState::self()->setCallRestriction(
            ProcessState::CallRestriction::ERROR_IF_NOT_ONEWAY);
#endif
    return Success();
}
static void set_usb_controller() {
    std::unique_ptr<DIR, decltype(&closedir)>dir(opendir("/sys/class/udc"), closedir);
    if (!dir) return;
    dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_name[0] == '.') continue;
        property_set("sys.usb.controller", dp->d_name);
        break;
    }
}
static void HandleSigtermSignal(const signalfd_siginfo& siginfo) {
    if (siginfo.ssi_pid != 0) {
        LOG(DEBUG) << "Ignoring SIGTERM from pid " << siginfo.ssi_pid;
        return;
    }
    HandlePowerctlMessage("shutdown,container");
}
static void HandleSignalFd() {
    signalfd_siginfo siginfo;
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(signal_fd, &siginfo, sizeof(siginfo)));
    if (bytes_read != sizeof(siginfo)) {
        PLOG(ERROR) << "Failed to read siginfo from signal_fd";
        return;
    }
    switch (siginfo.ssi_signo) {
        case SIGCHLD:
            ReapAnyOutstandingChildren();
            break;
        case SIGTERM:
            HandleSigtermSignal(siginfo);
            break;
        default:
            PLOG(ERROR) << "signal_fd: received unexpected signal " << siginfo.ssi_signo;
            break;
    }
}
static void UnblockSignals() {
    const struct sigaction act { .sa_handler = SIG_DFL };
    sigaction(SIGCHLD, &act, nullptr);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_UNBLOCK, &mask, nullptr) == -1) {
        PLOG(FATAL) << "failed to unblock signals for PID " << getpid();
    }
}
static void InstallSignalFdHandler(Epoll* epoll) {
    const struct sigaction act { .sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP };
    sigaction(SIGCHLD, &act, nullptr);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (!IsRebootCapable()) {
        sigaddset(&mask, SIGTERM);
    }
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        PLOG(FATAL) << "failed to block signals";
    }
    const int result = pthread_atfork(nullptr, nullptr, &UnblockSignals);
    if (result != 0) {
        LOG(FATAL) << "Failed to register a fork handler: " << strerror(result);
    }
    signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1) {
        PLOG(FATAL) << "failed to create signalfd";
    }
    if (auto result = epoll->RegisterHandler(signal_fd, HandleSignalFd); !result) {
        LOG(FATAL) << result.error();
    }
}
void HandleKeychord(const std::vector<int>& keycodes) {
    std::string adb_enabled = android::base::GetProperty("init.svc.adbd", "");
    if (adb_enabled != "running") {
        LOG(WARNING) << "Not starting service for keychord " << android::base::Join(keycodes, ' ')
                     << " because ADB is disabled";
        return;
    }
    auto found = false;
    for (const auto& service : ServiceList::GetInstance()) {
        auto svc = service.get();
        if (svc->keycodes() == keycodes) {
            found = true;
            LOG(INFO) << "Starting service '" << svc->name() << "' from keychord "
                      << android::base::Join(keycodes, ' ');
            if (auto result = svc->Start(); !result) {
                LOG(ERROR) << "Could not start service '" << svc->name() << "' from keychord "
                           << android::base::Join(keycodes, ' ') << ": " << result.error();
            }
        }
    }
    if (!found) {
        LOG(ERROR) << "Service for keychord " << android::base::Join(keycodes, ' ') << " not found";
    }
}
static void GlobalSeccomp() {
    import_kernel_cmdline(false, [](const std::string& key, const std::string& value,
                                    bool in_qemu) {
        if (key == "androidboot.seccomp" && value == "global" && !set_global_seccomp_filter()) {
            LOG(FATAL) << "Failed to globally enable seccomp!";
        }
    });
}
static void UmountDebugRamdisk() {
    if (umount("/debug_ramdisk") != 0) {
        LOG(ERROR) << "Failed to umount /debug_ramdisk";
    }
}
static void RecordStageBoottimes(const boot_clock::time_point& second_stage_start_time) {
    int64_t first_stage_start_time_ns = -1;
    if (auto first_stage_start_time_str = getenv(kEnvFirstStageStartedAt);
        first_stage_start_time_str) {
        property_set("ro.boottime.init", first_stage_start_time_str);
        android::base::ParseInt(first_stage_start_time_str, &first_stage_start_time_ns);
    }
    unsetenv(kEnvFirstStageStartedAt);
    int64_t selinux_start_time_ns = -1;
    if (auto selinux_start_time_str = getenv(kEnvSelinuxStartedAt); selinux_start_time_str) {
        android::base::ParseInt(selinux_start_time_str, &selinux_start_time_ns);
    }
    unsetenv(kEnvSelinuxStartedAt);
    if (selinux_start_time_ns == -1) return;
    if (first_stage_start_time_ns == -1) return;
    property_set("ro.boottime.init.first_stage",
                 std::to_string(selinux_start_time_ns - first_stage_start_time_ns));
    property_set("ro.boottime.init.selinux",
                 std::to_string(second_stage_start_time.time_since_epoch().count() -
                                selinux_start_time_ns));
}
int SecondStageMain(int argc, char** argv) {
    if (REBOOT_BOOTLOADER_ON_PANIC) {
        InstallRebootSignalHandlers();
    }
<<<<<<< HEAD
    boot_clock::time_point start_time = boot_clock::now();
    SetStdioToDevNull(argv);
    InitKernelLogging(argv);
||||||| ca0d4ffbe
    InitKernelLogging(argv, InitAborter);
=======
    SetStdioToDevNull(argv);
    InitKernelLogging(argv);
>>>>>>> d0ed53e5
    LOG(INFO) << "init second stage started!";
    if (auto result = WriteFile("/proc/1/oom_score_adj", "-1000"); !result) {
        LOG(ERROR) << "Unable to write -1000 to /proc/1/oom_score_adj: " << result.error();
    }
    GlobalSeccomp();
    keyctl_get_keyring_ID(KEY_SPEC_SESSION_KEYRING, 1);
    close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));
    property_init();
    process_kernel_dt();
    process_kernel_cmdline();
    export_kernel_boot_props();
    RecordStageBoottimes(start_time);
    const char* avb_version = getenv("INIT_AVB_VERSION");
    if (avb_version) property_set("ro.boot.avb_version", avb_version);
    const char* force_debuggable_env = getenv("INIT_FORCE_DEBUGGABLE");
    if (force_debuggable_env && AvbHandle::IsDeviceUnlocked()) {
        load_debug_prop = "true"s == force_debuggable_env;
    }
    unsetenv("INIT_AVB_VERSION");
    unsetenv("INIT_FORCE_DEBUGGABLE");
    SelinuxSetupKernelLogging();
    SelabelInitialize();
    SelinuxRestoreContext();
    Epoll epoll;
    if (auto result = epoll.Open(); !result) {
        PLOG(FATAL) << result.error();
    }
    InstallSignalFdHandler(&epoll);
    property_load_boot_defaults(load_debug_prop);
    UmountDebugRamdisk();
    fs_mgr_vendor_overlay_mount_all();
    export_oem_lock_status();
    StartPropertyService(&epoll);
    MountHandler mount_handler(&epoll);
    set_usb_controller();
    const BuiltinFunctionMap function_map;
    Action::set_function_map(&function_map);
    if (!SetupMountNamespaces()) {
        PLOG(FATAL) << "SetupMountNamespaces failed";
    }
    subcontexts = InitializeSubcontexts();
    ActionManager& am = ActionManager::GetInstance();
    ServiceList& sm = ServiceList::GetInstance();
    LoadBootScripts(am, sm);
    if (false) DumpState();
    if (android::gsi::IsGsiRunning()) {
        property_set("ro.gsid.image_running", "1");
    } else {
        property_set("ro.gsid.image_running", "0");
    }
    am.QueueBuiltinAction(SetupCgroupsAction, "SetupCgroups");
    am.QueueEventTrigger("early-init");
    am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");
    am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");
    am.QueueBuiltinAction(SetMmapRndBitsAction, "SetMmapRndBits");
    am.QueueBuiltinAction(SetKptrRestrictAction, "SetKptrRestrict");
    Keychords keychords;
    am.QueueBuiltinAction(
        [&epoll, &keychords](const BuiltinArguments& args) -> Result<Success> {
            for (const auto& svc : ServiceList::GetInstance()) {
                keychords.Register(svc->keycodes());
            }
            keychords.Start(&epoll, HandleKeychord);
            return Success();
        },
        "KeychordInit");
    am.QueueBuiltinAction(console_init_action, "console_init");
    am.QueueEventTrigger("init");
    am.QueueBuiltinAction(StartBoringSslSelfTest, "StartBoringSslSelfTest");
    am.QueueBuiltinAction(MixHwrngIntoLinuxRngAction, "MixHwrngIntoLinuxRng");
    am.QueueBuiltinAction(InitBinder, "InitBinder");
    std::string bootmode = GetProperty("ro.bootmode", "");
    if (bootmode == "charger") {
        am.QueueEventTrigger("charger");
    } else {
        am.QueueEventTrigger("late-init");
    }
    am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");
    while (true) {
        auto epoll_timeout = std::optional<std::chrono::milliseconds>{};
        if (do_shutdown && !shutting_down) {
            do_shutdown = false;
            if (HandlePowerctlMessage(shutdown_command)) {
                shutting_down = true;
            }
        }
        if (!(waiting_for_prop || Service::is_exec_service_running())) {
            am.ExecuteOneCommand();
        }
        if (!(waiting_for_prop || Service::is_exec_service_running())) {
            if (!shutting_down) {
                auto next_process_action_time = HandleProcessActions();
                if (next_process_action_time) {
                    epoll_timeout = std::chrono::ceil<std::chrono::milliseconds>(
                            *next_process_action_time - boot_clock::now());
                    if (*epoll_timeout < 0ms) epoll_timeout = 0ms;
                }
            }
            if (am.HasMoreCommands()) epoll_timeout = 0ms;
        }
        if (auto result = epoll.Wait(epoll_timeout); !result) {
            LOG(ERROR) << result.error();
        }
    }
    return 0;
}
}
}
