#include "init.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <paths.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>
#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libavb/libavb.h>
#include <private/android_filesystem_config.h>
#include <fstream>
#include <memory>
#include <vector>
#include "action.h"
#include "bootchart.h"
#include "devices.h"
#include "import_parser.h"
#include "init_first_stage.h"
#include "init_parser.h"
#include "keychords.h"
#include "log.h"
#include "property_service.h"
#include "reboot.h"
#include "service.h"
#include "signal_handler.h"
#include "ueventd.h"
#include "util.h"
#include "watchdogd.h"
using android::base::boot_clock;
using android::base::GetProperty;
using android::base::StringPrintf;
struct selabel_handle *sehandle;
struct selabel_handle *sehandle_prop;
static int property_triggers_enabled = 0;
static char qemu[32];
std::string default_console = "/dev/console";
static time_t process_needs_restart_at;
const char *ENV[32];
static int epoll_fd = -1;
static std::unique_ptr<Timer> waiting_for_prop(nullptr);
static std::string wait_prop_name;
static std::string wait_prop_value;
void DumpState() {
    ServiceManager::GetInstance().DumpState();
    ActionManager::GetInstance().DumpState();
}
void register_epoll_handler(int fd, void (*fn)()) {
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = reinterpret_cast<void*>(fn);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        PLOG(ERROR) << "epoll_ctl failed";
    }
}
int add_environment(const char *key, const char *val)
{
    size_t n;
    size_t key_len = strlen(key);
    for (n = 0; n < (arraysize(ENV) - 1); n++) {
        if (ENV[n] != NULL) {
            size_t entry_key_len = strcspn(ENV[n], "=");
            if ((entry_key_len == key_len) && (strncmp(ENV[n], key, entry_key_len) == 0)) {
                free((char*)ENV[n]);
                ENV[n] = NULL;
            }
        }
        if (ENV[n] == NULL) {
            char* entry;
            asprintf(&entry, "%s=%s", key, val);
            ENV[n] = entry;
            return 0;
        }
    }
    LOG(ERROR) << "No env. room to store: '" << key << "':'" << val << "'";
    return -1;
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
void property_changed(const std::string& name, const std::string& value) {
    if (name == "sys.powerctl") HandlePowerctlMessage(value);
    if (property_triggers_enabled) ActionManager::GetInstance().QueuePropertyChange(name, value);
    if (waiting_for_prop) {
        if (wait_prop_name == name && wait_prop_value == value) {
            wait_prop_name.clear();
            wait_prop_value.clear();
            LOG(INFO) << "Wait for property took " << *waiting_for_prop;
            waiting_for_prop.reset();
        }
    }
}
static void restart_processes()
{
    process_needs_restart_at = 0;
    ServiceManager::GetInstance().ForEachServiceWithFlags(SVC_RESTARTING, [](Service* s) {
        s->RestartIfNeeded(&process_needs_restart_at);
    });
}
void handle_control_message(const std::string& msg, const std::string& name) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(name);
    if (svc == nullptr) {
        LOG(ERROR) << "no such service '" << name << "'";
        return;
    }
    if (msg == "start") {
        svc->Start();
    } else if (msg == "stop") {
        svc->Stop();
    } else if (msg == "restart") {
        svc->Restart();
    } else {
        LOG(ERROR) << "unknown control msg '" << msg << "'";
    }
}
static int wait_for_coldboot_done_action(const std::vector<std::string>& args) {
    Timer t;
    LOG(VERBOSE) << "Waiting for " COLDBOOT_DONE "...";
    if (wait_for_file(COLDBOOT_DONE, 60s) < 0) {
        LOG(ERROR) << "Timed out waiting for " COLDBOOT_DONE;
        panic();
    }
    property_set("ro.boottime.init.cold_boot_wait", std::to_string(t.duration_ms()).c_str());
    return 0;
}
static int mix_hwrng_into_linux_rng_action(const std::vector<std::string>& args)
{
    int result = -1;
    int hwrandom_fd = -1;
    int urandom_fd = -1;
    char buf[512];
    ssize_t chunk_size;
    size_t total_bytes_written = 0;
    hwrandom_fd = TEMP_FAILURE_RETRY(
            open("/dev/hw_random", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (hwrandom_fd == -1) {
        if (errno == ENOENT) {
            LOG(ERROR) << "/dev/hw_random not found";
            result = 0;
        } else {
            PLOG(ERROR) << "Failed to open /dev/hw_random";
        }
        goto ret;
    }
    urandom_fd = TEMP_FAILURE_RETRY(
            open("/dev/urandom", O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (urandom_fd == -1) {
        PLOG(ERROR) << "Failed to open /dev/urandom";
        goto ret;
    }
    while (total_bytes_written < sizeof(buf)) {
        chunk_size = TEMP_FAILURE_RETRY(
                read(hwrandom_fd, buf, sizeof(buf) - total_bytes_written));
        if (chunk_size == -1) {
            PLOG(ERROR) << "Failed to read from /dev/hw_random";
            goto ret;
        } else if (chunk_size == 0) {
            LOG(ERROR) << "Failed to read from /dev/hw_random: EOF";
            goto ret;
        }
        chunk_size = TEMP_FAILURE_RETRY(write(urandom_fd, buf, chunk_size));
        if (chunk_size == -1) {
            PLOG(ERROR) << "Failed to write to /dev/urandom";
            goto ret;
        }
        total_bytes_written += chunk_size;
    }
    LOG(INFO) << "Mixed " << total_bytes_written << " bytes from /dev/hw_random into /dev/urandom";
    result = 0;
ret:
    if (hwrandom_fd != -1) {
        close(hwrandom_fd);
    }
    if (urandom_fd != -1) {
        close(urandom_fd);
    }
    return result;
}
static void security_failure() {
    LOG(ERROR) << "Security failure...";
    panic();
}
static bool set_highest_available_option_value(std::string path, int min, int max)
{
    std::ifstream inf(path, std::fstream::in);
    if (!inf) {
        LOG(ERROR) << "Cannot open for reading: " << path;
        return false;
    }
    int current = max;
    while (current >= min) {
        std::string str_val = std::to_string(current);
        std::ofstream of(path, std::fstream::out);
        if (!of) {
            LOG(ERROR) << "Cannot open for writing: " << path;
            return false;
        }
        of << str_val << std::endl;
        of.close();
        inf.seekg(0);
        std::string str_rec;
        inf >> str_rec;
        if (str_val.compare(str_rec) == 0) {
            break;
        }
        current--;
    }
    inf.close();
    if (current < min) {
        LOG(ERROR) << "Unable to set minimum option value " << min << " in " << path;
        return false;
    }
    return true;
}
#define MMAP_RND_PATH "/proc/sys/vm/mmap_rnd_bits"
#define MMAP_RND_COMPAT_PATH "/proc/sys/vm/mmap_rnd_compat_bits"
static bool __attribute__((unused)) set_mmap_rnd_bits_min(int start, int min, bool compat) {
    std::string path;
    if (compat) {
        path = MMAP_RND_COMPAT_PATH;
    } else {
        path = MMAP_RND_PATH;
    }
    return set_highest_available_option_value(path, min, start);
}
static int set_mmap_rnd_bits_action(const std::vector<std::string>& args)
{
    int ret = -1;
#if defined(__aarch64__)
    if (set_mmap_rnd_bits_min(33, 24, false)
            && set_mmap_rnd_bits_min(16, 16, true)) {
        ret = 0;
    }
#elif defined(__x86_64__)
    if (set_mmap_rnd_bits_min(32, 32, false)
            && set_mmap_rnd_bits_min(16, 16, true)) {
        ret = 0;
    }
#elif defined(__arm__) || defined(__i386__)
    bool h64 = !access(MMAP_RND_COMPAT_PATH, F_OK);
    if (set_mmap_rnd_bits_min(16, 16, h64)) {
        ret = 0;
    }
#elif defined(__mips__) || defined(__mips64__)
    ret = 0;
#else
    LOG(ERROR) << "Unknown architecture";
#endif
    if (ret == -1) {
        LOG(ERROR) << "Unable to set adequate mmap entropy value!";
        security_failure();
    }
    return ret;
}
#define KPTR_RESTRICT_PATH "/proc/sys/kernel/kptr_restrict"
#define KPTR_RESTRICT_MINVALUE 2
#define KPTR_RESTRICT_MAXVALUE 4
static int set_kptr_restrict_action(const std::vector<std::string>& args)
{
    std::string path = KPTR_RESTRICT_PATH;
    if (!set_highest_available_option_value(path, KPTR_RESTRICT_MINVALUE, KPTR_RESTRICT_MAXVALUE)) {
        LOG(ERROR) << "Unable to set adequate kptr_restrict value!";
        security_failure();
    }
    return 0;
}
static int keychord_init_action(const std::vector<std::string>& args)
{
    keychord_init();
    return 0;
}
static int console_init_action(const std::vector<std::string>& args)
{
    std::string console = GetProperty("ro.boot.console", "");
    if (!console.empty()) {
        default_console = "/dev/" + console;
    }
    return 0;
}
static void import_kernel_nv(const std::string& key, const std::string& value, bool for_emulator) {
    if (key.empty()) return;
    if (for_emulator) {
        property_set(StringPrintf("ro.kernel.%s", key.c_str()).c_str(), value.c_str());
        return;
    }
    if (key == "qemu") {
        strlcpy(qemu, value.c_str(), sizeof(qemu));
    } else if (android::base::StartsWith(key, "androidboot.")) {
        property_set(StringPrintf("ro.boot.%s", key.c_str() + 12).c_str(), value.c_str());
    }
}
static void export_oem_lock_status() {
    if (!android::base::GetBoolProperty("ro.oem_unlock_supported", false)) {
        return;
    }
    std::string value = GetProperty("ro.boot.verifiedbootstate", "");
    if (!value.empty()) {
        property_set("ro.boot.flash.locked", value == "orange" ? "0" : "1");
    }
}
static void export_kernel_boot_props() {
    struct {
        const char *src_prop;
        const char *dst_prop;
        const char *default_value;
    } prop_map[] = {
        { "ro.boot.serialno", "ro.serialno", "", },
        { "ro.boot.mode", "ro.bootmode", "unknown", },
        { "ro.boot.baseband", "ro.baseband", "unknown", },
        { "ro.boot.bootloader", "ro.bootloader", "unknown", },
        { "ro.boot.hardware", "ro.hardware", "unknown", },
        { "ro.boot.revision", "ro.revision", "0", },
    };
    for (size_t i = 0; i < arraysize(prop_map); i++) {
        std::string value = GetProperty(prop_map[i].src_prop, "");
        property_set(prop_map[i].dst_prop, (!value.empty()) ? value.c_str() : prop_map[i].default_value);
    }
}
static void process_kernel_dt() {
    if (!is_android_dt_value_expected("compatible", "android,firmware")) {
        return;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(kAndroidDtDir.c_str()), closedir);
    if (!dir) return;
    std::string dt_file;
    struct dirent *dp;
    while ((dp = readdir(dir.get())) != NULL) {
        if (dp->d_type != DT_REG || !strcmp(dp->d_name, "compatible") || !strcmp(dp->d_name, "name")) {
            continue;
        }
        std::string file_name = kAndroidDtDir + dp->d_name;
        android::base::ReadFileToString(file_name, &dt_file);
        std::replace(dt_file.begin(), dt_file.end(), ',', '.');
        std::string property_name = StringPrintf("ro.boot.%s", dp->d_name);
        property_set(property_name.c_str(), dt_file.c_str());
    }
}
static void process_kernel_cmdline() {
    import_kernel_cmdline(false, import_kernel_nv);
    if (qemu[0]) import_kernel_cmdline(true, import_kernel_nv);
}
static int property_enable_triggers_action(const std::vector<std::string>& args)
{
    property_triggers_enabled = 1;
    return 0;
}
static int queue_property_triggers_action(const std::vector<std::string>& args)
{
    ActionManager::GetInstance().QueueBuiltinAction(property_enable_triggers_action, "enable_property_trigger");
    ActionManager::GetInstance().QueueAllPropertyActions();
    return 0;
}
static void selinux_init_all_handles(void)
{
    sehandle = selinux_android_file_context_handle();
    selinux_android_set_sehandle(sehandle);
    sehandle_prop = selinux_android_prop_context_handle();
}
enum selinux_enforcing_status { SELINUX_PERMISSIVE, SELINUX_ENFORCING };
static selinux_enforcing_status selinux_status_from_cmdline() {
    selinux_enforcing_status status = SELINUX_ENFORCING;
    import_kernel_cmdline(false, [&](const std::string& key, const std::string& value, bool in_qemu) {
        if (key == "androidboot.selinux" && value == "permissive") {
            status = SELINUX_PERMISSIVE;
        }
    });
    return status;
}
static bool selinux_is_enforcing(void)
{
    if (ALLOW_PERMISSIVE_SELINUX) {
        return selinux_status_from_cmdline() == SELINUX_ENFORCING;
    }
    return true;
}
static int audit_callback(void *data, security_class_t , char *buf, size_t len) {
    property_audit_data *d = reinterpret_cast<property_audit_data*>(data);
    if (!d || !d->name || !d->cr) {
        LOG(ERROR) << "audit_callback invoked with null data arguments!";
        return 0;
    }
    snprintf(buf, len, "property=%s pid=%d uid=%d gid=%d", d->name,
            d->cr->pid, d->cr->uid, d->cr->gid);
    return 0;
}
static bool fork_execve_and_wait_for_completion(const char* filename, char* const argv[],
                                                char* const envp[]) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        PLOG(ERROR) << "Failed to create pipe";
        return false;
    }
    pid_t child_pid = fork();
    if (child_pid == -1) {
        PLOG(ERROR) << "Failed to fork for " << filename;
        return false;
    }
    if (child_pid == 0) {
        TEMP_FAILURE_RETRY(close(pipe_fds[0]));
        if (TEMP_FAILURE_RETRY(dup2(pipe_fds[1], STDERR_FILENO)) == -1) {
            PLOG(ERROR) << "Failed to redirect stderr of " << filename;
            _exit(127);
            return false;
        }
        TEMP_FAILURE_RETRY(close(pipe_fds[1]));
        if (execve(filename, argv, envp) == -1) {
            PLOG(ERROR) << "Failed to execve " << filename;
            return false;
        }
        _exit(127);
        return false;
    } else {
        TEMP_FAILURE_RETRY(close(pipe_fds[1]));
        const int child_out_fd = pipe_fds[0];
        std::string child_output;
        if (!android::base::ReadFdToString(child_out_fd, &child_output)) {
            PLOG(ERROR) << "Failed to capture full output of " << filename;
        }
        TEMP_FAILURE_RETRY(close(child_out_fd));
        if (!child_output.empty()) {
            std::istringstream in(child_output);
            std::string line;
            while (std::getline(in, line)) {
                LOG(ERROR) << filename << ": " << line;
            }
        }
        int status;
        if (TEMP_FAILURE_RETRY(waitpid(child_pid, &status, 0)) != child_pid) {
            PLOG(ERROR) << "Failed to wait for " << filename;
            return false;
        }
        if (WIFEXITED(status)) {
            int status_code = WEXITSTATUS(status);
            if (status_code == 0) {
                return true;
            } else {
                LOG(ERROR) << filename << " exited with status " << status_code;
            }
        } else if (WIFSIGNALED(status)) {
            LOG(ERROR) << filename << " killed by signal " << WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            LOG(ERROR) << filename << " stopped by signal " << WSTOPSIG(status);
        } else {
            LOG(ERROR) << "waitpid for " << filename << " returned unexpected status: " << status;
        }
        return false;
    }
}
static bool read_first_line(const char* file, std::string* line) {
    line->clear();
    std::string contents;
    if (!android::base::ReadFileToString(file, &contents, true )) {
        return false;
    }
    std::istringstream in(contents);
    std::getline(in, *line);
    return true;
}
static bool selinux_find_precompiled_split_policy(std::string* file) {
    file->clear();
    static constexpr const char precompiled_sepolicy[] = "/vendor/etc/selinux/precompiled_sepolicy";
    if (access(precompiled_sepolicy, R_OK) == -1) {
        return false;
    }
    std::string actual_plat_id;
    if (!read_first_line("/system/etc/selinux/plat_and_mapping_sepolicy.cil.sha256",
                         &actual_plat_id)) {
        PLOG(INFO) << "Failed to read "
                      "/system/etc/selinux/plat_and_mapping_sepolicy.cil.sha256";
        return false;
    }
    std::string precompiled_plat_id;
    if (!read_first_line("/vendor/etc/selinux/precompiled_sepolicy.plat_and_mapping.sha256",
                         &precompiled_plat_id)) {
        PLOG(INFO) << "Failed to read "
                      "/vendor/etc/selinux/"
                      "precompiled_sepolicy.plat_and_mapping.sha256";
        return false;
    }
    if ((actual_plat_id.empty()) || (actual_plat_id != precompiled_plat_id)) {
        return false;
    }
    *file = precompiled_sepolicy;
    return true;
}
static bool selinux_get_vendor_mapping_version(std::string* plat_vers) {
    if (!read_first_line("/vendor/etc/selinux/plat_sepolicy_vers.txt", plat_vers)) {
        PLOG(ERROR) << "Failed to read /vendor/etc/selinux/plat_sepolicy_vers.txt";
        return false;
    }
    if (plat_vers->empty()) {
        LOG(ERROR) << "No version present in plat_sepolicy_vers.txt";
        return false;
    }
    return true;
}
static constexpr const char plat_policy_cil_file[] = "/system/etc/selinux/plat_sepolicy.cil";
static bool selinux_is_split_policy_device() { return access(plat_policy_cil_file, R_OK) != -1; }
static bool selinux_load_split_policy() {
    std::string precompiled_sepolicy_file;
    if (selinux_find_precompiled_split_policy(&precompiled_sepolicy_file)) {
        android::base::unique_fd fd(
            open(precompiled_sepolicy_file.c_str(), O_RDONLY | O_CLOEXEC | O_BINARY));
        if (fd != -1) {
            if (selinux_android_load_policy_from_fd(fd, precompiled_sepolicy_file.c_str()) < 0) {
                LOG(ERROR) << "Failed to load SELinux policy from " << precompiled_sepolicy_file;
                return false;
            }
            return true;
        }
    }
    LOG(INFO) << "Compiling SELinux policy";
    set_selinuxmnt("/sys/fs/selinux");
    int max_policy_version = security_policyvers();
    if (max_policy_version == -1) {
        PLOG(ERROR) << "Failed to determine highest policy version supported by kernel";
        return false;
    }
    char compiled_sepolicy[] = "/dev/sepolicy.XXXXXX";
    android::base::unique_fd compiled_sepolicy_fd(mkostemp(compiled_sepolicy, O_CLOEXEC));
    if (compiled_sepolicy_fd < 0) {
        PLOG(ERROR) << "Failed to create temporary file " << compiled_sepolicy;
        return false;
    }
    std::string vend_plat_vers;
    if (!selinux_get_vendor_mapping_version(&vend_plat_vers)) {
        return false;
    }
    std::string mapping_file("/system/etc/selinux/mapping/" + vend_plat_vers + ".cil");
    const char* compile_args[] = {
        "/system/bin/secilc",
        plat_policy_cil_file,
        "-M", "true", "-G", "-N",
        "-c", std::to_string(max_policy_version).c_str(),
        mapping_file.c_str(),
        "/vendor/etc/selinux/nonplat_sepolicy.cil",
        "-o", compiled_sepolicy,
        "-f", "/sys/fs/selinux/null",
        nullptr};
    if (!fork_execve_and_wait_for_completion(compile_args[0], (char**)compile_args, (char**)ENV)) {
        unlink(compiled_sepolicy);
        return false;
    }
    unlink(compiled_sepolicy);
    LOG(INFO) << "Loading compiled SELinux policy";
    if (selinux_android_load_policy_from_fd(compiled_sepolicy_fd, compiled_sepolicy) < 0) {
        LOG(ERROR) << "Failed to load SELinux policy from " << compiled_sepolicy;
        return false;
    }
    return true;
}
static bool selinux_load_monolithic_policy() {
    LOG(VERBOSE) << "Loading SELinux policy from monolithic file";
    if (selinux_android_load_policy() < 0) {
        PLOG(ERROR) << "Failed to load monolithic SELinux policy";
        return false;
    }
    return true;
}
static bool selinux_load_policy() {
    return selinux_is_split_policy_device() ? selinux_load_split_policy()
                                            : selinux_load_monolithic_policy();
}
static void selinux_initialize(bool in_kernel_domain) {
    Timer t;
    selinux_callback cb;
    cb.func_log = selinux_klog_callback;
    selinux_set_callback(SELINUX_CB_LOG, cb);
    cb.func_audit = audit_callback;
    selinux_set_callback(SELINUX_CB_AUDIT, cb);
    if (in_kernel_domain) {
        LOG(INFO) << "Loading SELinux policy";
        if (!selinux_load_policy()) {
            panic();
        }
        bool kernel_enforcing = (security_getenforce() == 1);
        bool is_enforcing = selinux_is_enforcing();
        if (kernel_enforcing != is_enforcing) {
            if (security_setenforce(is_enforcing)) {
                PLOG(ERROR) << "security_setenforce(%s) failed" << (is_enforcing ? "true" : "false");
                security_failure();
            }
        }
        if (!write_file("/sys/fs/selinux/checkreqprot", "0")) {
            security_failure();
        }
        setenv("INIT_SELINUX_TOOK", std::to_string(t.duration_ms()).c_str(), 1);
    } else {
        selinux_init_all_handles();
    }
}
static void selinux_restore_context() {
    LOG(INFO) << "Running restorecon...";
    restorecon("/dev");
    restorecon("/dev/kmsg");
    if constexpr (WORLD_WRITABLE_KMSG) {
      restorecon("/dev/kmsg_debug");
    }
    restorecon("/dev/socket");
    restorecon("/dev/random");
    restorecon("/dev/urandom");
    restorecon("/dev/__properties__");
    restorecon("/file_contexts.bin");
    restorecon("/plat_file_contexts");
    restorecon("/nonplat_file_contexts");
    restorecon("/plat_property_contexts");
    restorecon("/nonplat_property_contexts");
    restorecon("/plat_seapp_contexts");
    restorecon("/nonplat_seapp_contexts");
    restorecon("/plat_service_contexts");
    restorecon("/nonplat_service_contexts");
    restorecon("/plat_hwservice_contexts");
    restorecon("/nonplat_hwservice_contexts");
    restorecon("/sepolicy");
    restorecon("/vndservice_contexts");
    restorecon("/sys", SELINUX_ANDROID_RESTORECON_RECURSE);
    restorecon("/dev/block", SELINUX_ANDROID_RESTORECON_RECURSE);
    restorecon("/dev/device-mapper");
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
static void install_reboot_signal_handlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigfillset(&action.sa_mask);
    action.sa_handler = [](int) {
        panic();
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
int main(int argc, char** argv) {
    if (!strcmp(basename(argv[0]), "ueventd")) {
        return ueventd_main(argc, argv);
    }
    if (!strcmp(basename(argv[0]), "watchdogd")) {
        return watchdogd_main(argc, argv);
    }
    if (REBOOT_BOOTLOADER_ON_PANIC) {
        install_reboot_signal_handlers();
    }
    add_environment("PATH", _PATH_DEFPATH);
    bool is_first_stage = (getenv("INIT_SECOND_STAGE") == nullptr);
    if (is_first_stage) {
        boot_clock::time_point start_time = boot_clock::now();
        umask(0);
        mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
        mkdir("/dev/pts", 0755);
        mkdir("/dev/socket", 0755);
        mount("devpts", "/dev/pts", "devpts", 0, NULL);
        #define MAKE_STR(x) __STRING(x)
        mount("proc", "/proc", "proc", 0, "hidepid=2,gid=" MAKE_STR(AID_READPROC));
        chmod("/proc/cmdline", 0440);
        gid_t groups[] = { AID_READPROC };
        setgroups(arraysize(groups), groups);
        mount("sysfs", "/sys", "sysfs", 0, NULL);
        mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL);
        mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
        if constexpr (WORLD_WRITABLE_KMSG) {
          mknod("/dev/kmsg_debug", S_IFCHR | 0622, makedev(1, 11));
        }
        mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
        mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
        InitKernelLogging(argv);
        LOG(INFO) << "init first stage started!";
        if (!DoFirstStageMount()) {
            LOG(ERROR) << "Failed to mount required partitions early ...";
            panic();
        }
        SetInitAvbVersionInRecovery();
        selinux_initialize(true);
        if (restorecon("/init") == -1) {
            PLOG(ERROR) << "restorecon failed";
            security_failure();
        }
        setenv("INIT_SECOND_STAGE", "true", 1);
        static constexpr uint32_t kNanosecondsPerMillisecond = 1e6;
        uint64_t start_ms = start_time.time_since_epoch().count() / kNanosecondsPerMillisecond;
        setenv("INIT_STARTED_AT", StringPrintf("%" PRIu64, start_ms).c_str(), 1);
        char* path = argv[0];
        char* args[] = { path, nullptr };
        execv(path, args);
        PLOG(ERROR) << "execv(\"" << path << "\") failed";
        security_failure();
    }
    InitKernelLogging(argv);
    LOG(INFO) << "init second stage started!";
    close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));
    property_init();
    process_kernel_dt();
    process_kernel_cmdline();
    export_kernel_boot_props();
    property_set("ro.boottime.init", getenv("INIT_STARTED_AT"));
    property_set("ro.boottime.init.selinux", getenv("INIT_SELINUX_TOOK"));
    const char* avb_version = getenv("INIT_AVB_VERSION");
    if (avb_version) property_set("ro.boot.avb_version", avb_version);
    unsetenv("INIT_SECOND_STAGE");
    unsetenv("INIT_STARTED_AT");
    unsetenv("INIT_SELINUX_TOOK");
    unsetenv("INIT_AVB_VERSION");
    selinux_initialize(false);
    selinux_restore_context();
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        PLOG(ERROR) << "epoll_create1 failed";
        exit(1);
    }
    signal_handler_init();
    property_load_boot_defaults();
    export_oem_lock_status();
    start_property_service();
    set_usb_controller();
    const BuiltinFunctionMap function_map;
    Action::set_function_map(&function_map);
    ActionManager& am = ActionManager::GetInstance();
    ServiceManager& sm = ServiceManager::GetInstance();
    Parser& parser = Parser::GetInstance();
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&sm));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&am));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));
    std::string bootscript = GetProperty("ro.boot.init_rc", "");
    if (bootscript.empty()) {
        parser.ParseConfig("/init.rc");
        parser.set_is_system_etc_init_loaded(
                parser.ParseConfig("/system/etc/init"));
        parser.set_is_vendor_etc_init_loaded(
                parser.ParseConfig("/vendor/etc/init"));
        parser.set_is_odm_etc_init_loaded(parser.ParseConfig("/odm/etc/init"));
    } else {
        parser.ParseConfig(bootscript);
        parser.set_is_system_etc_init_loaded(true);
        parser.set_is_vendor_etc_init_loaded(true);
        parser.set_is_odm_etc_init_loaded(true);
    }
    if (false) DumpState();
    am.QueueEventTrigger("early-init");
    am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");
    am.QueueBuiltinAction(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");
    am.QueueBuiltinAction(set_mmap_rnd_bits_action, "set_mmap_rnd_bits");
    am.QueueBuiltinAction(set_kptr_restrict_action, "set_kptr_restrict");
    am.QueueBuiltinAction(keychord_init_action, "keychord_init");
    am.QueueBuiltinAction(console_init_action, "console_init");
    am.QueueEventTrigger("init");
    am.QueueBuiltinAction(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");
    std::string bootmode = GetProperty("ro.bootmode", "");
    if (bootmode == "charger") {
        am.QueueEventTrigger("charger");
    } else {
        am.QueueEventTrigger("late-init");
    }
    am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");
    while (true) {
        int epoll_timeout_ms = -1;
        if (!(waiting_for_prop || sm.IsWaitingForExec())) {
            am.ExecuteOneCommand();
        }
        if (!(waiting_for_prop || sm.IsWaitingForExec())) {
            restart_processes();
            if (process_needs_restart_at != 0) {
                epoll_timeout_ms = (process_needs_restart_at - time(nullptr)) * 1000;
                if (epoll_timeout_ms < 0) epoll_timeout_ms = 0;
            }
            if (am.HasMoreCommands()) epoll_timeout_ms = 0;
        }
        epoll_event ev;
        int nr = TEMP_FAILURE_RETRY(epoll_wait(epoll_fd, &ev, 1, epoll_timeout_ms));
        if (nr == -1) {
            PLOG(ERROR) << "epoll_wait failed";
        } else if (nr == 1) {
            ((void (*)()) ev.data.ptr)();
        }
    }
    return 0;
}
