#define LOG_TAG "installed"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <cutils/properties.h>
#include <cutils/sched_policy.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <selinux/android.h>
#include <system/thread_defs.h>
#include "dexopt.h"
#include "installd_deps.h"
#include "otapreopt_utils.h"
#include "utils.h"
using android::base::StringPrintf;
using android::base::EndsWith;
using android::base::unique_fd;
namespace android {
namespace installd {
struct FreeDelete {
  void operator()(const void* ptr) const {
    free(const_cast<void*>(ptr));
  }
};
template <typename T>
using UniqueCPtr = std::unique_ptr<T, FreeDelete>;
static unique_fd invalid_unique_fd() {
    return unique_fd(-1);
}
static bool is_debug_runtime() {
    return android::base::GetProperty("persist.sys.dalvik.vm.lib.2", "") == "libartd.so";
}
static bool clear_profile(const std::string& profile) {
    unique_fd ufd(open(profile.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (ufd.get() < 0) {
        if (errno != ENOENT) {
            PLOG(WARNING) << "Could not open profile " << profile;
            return false;
        } else {
            return true;
        }
    }
    if (flock(ufd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK) {
            PLOG(WARNING) << "Error locking profile " << profile;
        }
        return false;
    }
    bool truncated = ftruncate(ufd.get(), 0) == 0;
    if (!truncated) {
        PLOG(WARNING) << "Could not truncate " << profile;
    }
    if (flock(ufd.get(), LOCK_UN) != 0) {
        PLOG(WARNING) << "Error unlocking profile " << profile;
    }
    return truncated;
}
static bool clear_reference_profile(const std::string& location, bool is_secondary_dex) {
    return clear_profile(create_reference_profile_path(location, is_secondary_dex));
}
static bool clear_current_profile(const std::string& pkgname, userid_t user,
        bool is_secondary_dex) {
    return clear_profile(create_current_profile_path(user, pkgname, is_secondary_dex));
}
bool clear_primary_reference_profile(const std::string& pkgname) {
    return clear_reference_profile(pkgname, false);
}
bool clear_primary_current_profiles(const std::string& pkgname) {
    bool success = true;
    std::vector<userid_t> users = get_known_users( nullptr);
    for (auto user : users) {
        success &= clear_current_profile(pkgname, user, false);
    }
    return success;
}
bool clear_primary_current_profile(const std::string& pkgname, userid_t user) {
    return clear_current_profile(pkgname, user, false);
}
static int split_count(const char *str)
{
  char *ctx;
  int count = 0;
  char buf[kPropertyValueMax];
  strncpy(buf, str, sizeof(buf));
  char *pBuf = buf;
  while(strtok_r(pBuf, " ", &ctx) != NULL) {
    count++;
    pBuf = NULL;
  }
  return count;
}
static int split(char *buf, const char **argv)
{
  char *ctx;
  int count = 0;
  char *tok;
  char *pBuf = buf;
  while((tok = strtok_r(pBuf, " ", &ctx)) != NULL) {
    argv[count++] = tok;
    pBuf = NULL;
  }
  return count;
}
static const char* get_location_from_path(const char* path) {
    static constexpr char kLocationSeparator = '/';
    const char *location = strrchr(path, kLocationSeparator);
    if (location == NULL) {
        return path;
    } else {
        return location + 1;
    }
}
static void run_dex2oat(int zip_fd, int oat_fd, int input_vdex_fd, int output_vdex_fd, int image_fd,
        const char* input_file_name, const char* output_file_name, int swap_fd,
        const char* instruction_set, const char* compiler_filter,
        bool debuggable, bool post_bootcomplete, int profile_fd, const char* class_loader_context) {
    static const unsigned int MAX_INSTRUCTION_SET_LEN = 7;
    if (strlen(instruction_set) >= MAX_INSTRUCTION_SET_LEN) {
        ALOGE("Instruction set %s longer than max length of %d",
              instruction_set, MAX_INSTRUCTION_SET_LEN);
        return;
    }
    const char* relative_input_file_name = get_location_from_path(input_file_name);
    char dex2oat_Xms_flag[kPropertyValueMax];
    bool have_dex2oat_Xms_flag = get_property("dalvik.vm.dex2oat-Xms", dex2oat_Xms_flag, NULL) > 0;
    char dex2oat_Xmx_flag[kPropertyValueMax];
    bool have_dex2oat_Xmx_flag = get_property("dalvik.vm.dex2oat-Xmx", dex2oat_Xmx_flag, NULL) > 0;
    char dex2oat_threads_buf[kPropertyValueMax];
    bool have_dex2oat_threads_flag = get_property(post_bootcomplete
                                                      ? "dalvik.vm.dex2oat-threads"
                                                      : "dalvik.vm.boot-dex2oat-threads",
                                                  dex2oat_threads_buf,
                                                  NULL) > 0;
    char dex2oat_threads_arg[kPropertyValueMax + 2];
    if (have_dex2oat_threads_flag) {
        sprintf(dex2oat_threads_arg, "-j%s", dex2oat_threads_buf);
    }
    char dex2oat_isa_features_key[kPropertyKeyMax];
    sprintf(dex2oat_isa_features_key, "dalvik.vm.isa.%s.features", instruction_set);
    char dex2oat_isa_features[kPropertyValueMax];
    bool have_dex2oat_isa_features = get_property(dex2oat_isa_features_key,
                                                  dex2oat_isa_features, NULL) > 0;
    char dex2oat_isa_variant_key[kPropertyKeyMax];
    sprintf(dex2oat_isa_variant_key, "dalvik.vm.isa.%s.variant", instruction_set);
    char dex2oat_isa_variant[kPropertyValueMax];
    bool have_dex2oat_isa_variant = get_property(dex2oat_isa_variant_key,
                                                 dex2oat_isa_variant, NULL) > 0;
    const char *dex2oat_norelocation = "-Xnorelocate";
    bool have_dex2oat_relocation_skip_flag = false;
    char dex2oat_flags[kPropertyValueMax];
    int dex2oat_flags_count = get_property("dalvik.vm.dex2oat-flags",
                                 dex2oat_flags, NULL) <= 0 ? 0 : split_count(dex2oat_flags);
    ALOGV("dalvik.vm.dex2oat-flags=%s\n", dex2oat_flags);
    char vold_decrypt[kPropertyValueMax];
    bool have_vold_decrypt = get_property("vold.decrypt", vold_decrypt, "") > 0;
    bool skip_compilation = (have_vold_decrypt &&
                             (strcmp(vold_decrypt, "trigger_restart_min_framework") == 0 ||
                             (strcmp(vold_decrypt, "1") == 0)));
    bool generate_debug_info = property_get_bool("debug.generate-debug-info", false);
    char app_image_format[kPropertyValueMax];
    char image_format_arg[strlen("--image-format=") + kPropertyValueMax];
    bool have_app_image_format =
            image_fd >= 0 && get_property("dalvik.vm.appimageformat", app_image_format, NULL) > 0;
    if (have_app_image_format) {
        sprintf(image_format_arg, "--image-format=%s", app_image_format);
    }
    char dex2oat_large_app_threshold[kPropertyValueMax];
    bool have_dex2oat_large_app_threshold =
            get_property("dalvik.vm.dex2oat-very-large", dex2oat_large_app_threshold, NULL) > 0;
    char dex2oat_large_app_threshold_arg[strlen("--very-large-app-threshold=") + kPropertyValueMax];
    if (have_dex2oat_large_app_threshold) {
        sprintf(dex2oat_large_app_threshold_arg,
                "--very-large-app-threshold=%s",
                dex2oat_large_app_threshold);
    }
    const char* dex2oat_bin = is_debug_runtime() ? "/system/bin/dex2oatd" : "/system/bin/dex2oat";
    static const char* RUNTIME_ARG = "--runtime-arg";
    static const int MAX_INT_LEN = 12;
    char zip_fd_arg[arraysize("--zip-fd=") + MAX_INT_LEN];
    char zip_location_arg[arraysize("--zip-location=") + PKG_PATH_MAX];
    char input_vdex_fd_arg[arraysize("--input-vdex-fd=") + MAX_INT_LEN];
    char output_vdex_fd_arg[arraysize("--output-vdex-fd=") + MAX_INT_LEN];
    char oat_fd_arg[arraysize("--oat-fd=") + MAX_INT_LEN];
    char oat_location_arg[arraysize("--oat-location=") + PKG_PATH_MAX];
    char instruction_set_arg[arraysize("--instruction-set=") + MAX_INSTRUCTION_SET_LEN];
    char instruction_set_variant_arg[arraysize("--instruction-set-variant=") + kPropertyValueMax];
    char instruction_set_features_arg[arraysize("--instruction-set-features=") + kPropertyValueMax];
    char dex2oat_Xms_arg[arraysize("-Xms") + kPropertyValueMax];
    char dex2oat_Xmx_arg[arraysize("-Xmx") + kPropertyValueMax];
    char dex2oat_compiler_filter_arg[arraysize("--compiler-filter=") + kPropertyValueMax];
    bool have_dex2oat_swap_fd = false;
    char dex2oat_swap_fd[arraysize("--swap-fd=") + MAX_INT_LEN];
    bool have_dex2oat_image_fd = false;
    char dex2oat_image_fd[arraysize("--app-image-fd=") + MAX_INT_LEN];
    size_t class_loader_context_size = arraysize("--class-loader-context=") + PKG_PATH_MAX;
    char class_loader_context_arg[class_loader_context_size];
    if (class_loader_context != nullptr) {
        snprintf(class_loader_context_arg, class_loader_context_size, "--class-loader-context=%s",
            class_loader_context);
    }
    sprintf(zip_fd_arg, "--zip-fd=%d", zip_fd);
    sprintf(zip_location_arg, "--zip-location=%s", relative_input_file_name);
    sprintf(input_vdex_fd_arg, "--input-vdex-fd=%d", input_vdex_fd);
    sprintf(output_vdex_fd_arg, "--output-vdex-fd=%d", output_vdex_fd);
    sprintf(oat_fd_arg, "--oat-fd=%d", oat_fd);
    sprintf(oat_location_arg, "--oat-location=%s", output_file_name);
    sprintf(instruction_set_arg, "--instruction-set=%s", instruction_set);
    sprintf(instruction_set_variant_arg, "--instruction-set-variant=%s", dex2oat_isa_variant);
    sprintf(instruction_set_features_arg, "--instruction-set-features=%s", dex2oat_isa_features);
    if (swap_fd >= 0) {
        have_dex2oat_swap_fd = true;
        sprintf(dex2oat_swap_fd, "--swap-fd=%d", swap_fd);
    }
    if (image_fd >= 0) {
        have_dex2oat_image_fd = true;
        sprintf(dex2oat_image_fd, "--app-image-fd=%d", image_fd);
    }
    if (have_dex2oat_Xms_flag) {
        sprintf(dex2oat_Xms_arg, "-Xms%s", dex2oat_Xms_flag);
    }
    if (have_dex2oat_Xmx_flag) {
        sprintf(dex2oat_Xmx_arg, "-Xmx%s", dex2oat_Xmx_flag);
    }
    bool have_dex2oat_compiler_filter_flag = false;
    if (skip_compilation) {
        strcpy(dex2oat_compiler_filter_arg, "--compiler-filter=extract");
        have_dex2oat_compiler_filter_flag = true;
        have_dex2oat_relocation_skip_flag = true;
    } else if (compiler_filter != nullptr) {
        if (strlen(compiler_filter) + strlen("--compiler-filter=") <
                    arraysize(dex2oat_compiler_filter_arg)) {
            sprintf(dex2oat_compiler_filter_arg, "--compiler-filter=%s", compiler_filter);
            have_dex2oat_compiler_filter_flag = true;
        } else {
            ALOGW("Compiler filter name '%s' is too large (max characters is %zu)",
                  compiler_filter,
                  kPropertyValueMax);
        }
    }
    if (!have_dex2oat_compiler_filter_flag) {
        char dex2oat_compiler_filter_flag[kPropertyValueMax];
        have_dex2oat_compiler_filter_flag = get_property("dalvik.vm.dex2oat-filter",
                                                         dex2oat_compiler_filter_flag, NULL) > 0;
        if (have_dex2oat_compiler_filter_flag) {
            sprintf(dex2oat_compiler_filter_arg,
                    "--compiler-filter=%s",
                    dex2oat_compiler_filter_flag);
        }
    }
    if (!debuggable) {
        char prop_buf[kPropertyValueMax];
        debuggable =
                (get_property("dalvik.vm.always_debuggable", prop_buf, "0") > 0) &&
                (prop_buf[0] == '1');
    }
    char profile_arg[strlen("--profile-file-fd=") + MAX_INT_LEN];
    if (profile_fd != -1) {
        sprintf(profile_arg, "--profile-file-fd=%d", profile_fd);
    }
    char base_dir[arraysize("--classpath-dir=") + PKG_PATH_MAX];
    std::string apk_dir(input_file_name);
    unsigned long dir_index = apk_dir.rfind('/');
    bool has_base_dir = dir_index != std::string::npos;
    if (has_base_dir) {
        apk_dir = apk_dir.substr(0, dir_index);
        sprintf(base_dir, "--classpath-dir=%s", apk_dir.c_str());
    }
    ALOGV("Running %s in=%s out=%s\n", dex2oat_bin, relative_input_file_name, output_file_name);
    const char* argv[9
                     + (have_dex2oat_isa_variant ? 1 : 0)
                     + (have_dex2oat_isa_features ? 1 : 0)
                     + (have_dex2oat_Xms_flag ? 2 : 0)
                     + (have_dex2oat_Xmx_flag ? 2 : 0)
                     + (have_dex2oat_compiler_filter_flag ? 1 : 0)
                     + (have_dex2oat_threads_flag ? 1 : 0)
                     + (have_dex2oat_swap_fd ? 1 : 0)
                     + (have_dex2oat_image_fd ? 1 : 0)
                     + (have_dex2oat_relocation_skip_flag ? 2 : 0)
                     + (generate_debug_info ? 1 : 0)
                     + (debuggable ? 1 : 0)
                     + (have_app_image_format ? 1 : 0)
                     + dex2oat_flags_count
                     + (profile_fd == -1 ? 0 : 1)
                     + (class_loader_context != nullptr ? 1 : 0)
                     + (has_base_dir ? 1 : 0)
                     + (have_dex2oat_large_app_threshold ? 1 : 0)];
    int i = 0;
    argv[i++] = dex2oat_bin;
    argv[i++] = zip_fd_arg;
    argv[i++] = zip_location_arg;
    argv[i++] = input_vdex_fd_arg;
    argv[i++] = output_vdex_fd_arg;
    argv[i++] = oat_fd_arg;
    argv[i++] = oat_location_arg;
    argv[i++] = instruction_set_arg;
    if (have_dex2oat_isa_variant) {
        argv[i++] = instruction_set_variant_arg;
    }
    if (have_dex2oat_isa_features) {
        argv[i++] = instruction_set_features_arg;
    }
    if (have_dex2oat_Xms_flag) {
        argv[i++] = RUNTIME_ARG;
        argv[i++] = dex2oat_Xms_arg;
    }
    if (have_dex2oat_Xmx_flag) {
        argv[i++] = RUNTIME_ARG;
        argv[i++] = dex2oat_Xmx_arg;
    }
    if (have_dex2oat_compiler_filter_flag) {
        argv[i++] = dex2oat_compiler_filter_arg;
    }
    if (have_dex2oat_threads_flag) {
        argv[i++] = dex2oat_threads_arg;
    }
    if (have_dex2oat_swap_fd) {
        argv[i++] = dex2oat_swap_fd;
    }
    if (have_dex2oat_image_fd) {
        argv[i++] = dex2oat_image_fd;
    }
    if (generate_debug_info) {
        argv[i++] = "--generate-debug-info";
    }
    if (debuggable) {
        argv[i++] = "--debuggable";
    }
    if (have_app_image_format) {
        argv[i++] = image_format_arg;
    }
    if (have_dex2oat_large_app_threshold) {
        argv[i++] = dex2oat_large_app_threshold_arg;
    }
    if (dex2oat_flags_count) {
        i += split(dex2oat_flags, argv + i);
    }
    if (have_dex2oat_relocation_skip_flag) {
        argv[i++] = RUNTIME_ARG;
        argv[i++] = dex2oat_norelocation;
    }
    if (profile_fd != -1) {
        argv[i++] = profile_arg;
    }
    if (has_base_dir) {
        argv[i++] = base_dir;
    }
    if (class_loader_context != nullptr) {
        argv[i++] = class_loader_context_arg;
    }
    argv[i] = NULL;
    execv(dex2oat_bin, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", dex2oat_bin, strerror(errno));
}
static bool kAlwaysProvideSwapFile = false;
static bool kDefaultProvideSwapFile = true;
static bool ShouldUseSwapFileForDexopt() {
    if (kAlwaysProvideSwapFile) {
        return true;
    }
    char dex2oat_prop_buf[kPropertyValueMax];
    if (get_property("dalvik.vm.dex2oat-swap", dex2oat_prop_buf, "") > 0) {
        if (strcmp(dex2oat_prop_buf, "true") == 0) {
            return true;
        } else {
            return false;
        }
    }
    if (kDefaultProvideSwapFile) {
        return true;
    }
    bool is_low_mem = property_get_bool("ro.config.low_ram", false);
    if (is_low_mem) {
        return true;
    }
    return kDefaultProvideSwapFile;
}
static void SetDex2OatScheduling(bool set_to_bg) {
    if (set_to_bg) {
        if (set_sched_policy(0, SP_BACKGROUND) < 0) {
            ALOGE("set_sched_policy failed: %s\n", strerror(errno));
            exit(70);
        }
        if (setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_BACKGROUND) < 0) {
            ALOGE("setpriority failed: %s\n", strerror(errno));
            exit(71);
        }
    }
}
static bool create_profile(int uid, const std::string& profile) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(profile.c_str(), O_CREAT | O_NOFOLLOW, 0600)));
    if (fd.get() < 0) {
        if (errno == EEXIST) {
            return true;
        } else {
            PLOG(ERROR) << "Failed to create profile " << profile;
            return false;
        }
    }
    if (fchown(fd.get(), uid, uid) < 0) {
        PLOG(ERROR) << "Could not chwon profile " << profile;
        return false;
    }
    return true;
}
static unique_fd open_profile(int uid, const std::string& profile, bool read_write) {
    if (read_write) {
        if (!create_profile(uid, profile)) {
            return invalid_unique_fd();
        }
    }
    int flags = read_write ? O_RDWR : O_RDONLY;
    flags |= O_NOFOLLOW;
    unique_fd fd(TEMP_FAILURE_RETRY(open(profile.c_str(), flags)));
    if (fd.get() < 0) {
        if (errno != ENOENT) {
            PLOG(ERROR) << "Failed to open profile " << profile;
        }
        return invalid_unique_fd();
    }
    return fd;
}
static unique_fd open_current_profile(uid_t uid, userid_t user, const std::string& location,
        bool is_secondary_dex) {
    std::string profile = create_current_profile_path(user, location, is_secondary_dex);
    return open_profile(uid, profile, false);
}
static unique_fd open_reference_profile(uid_t uid, const std::string& location, bool read_write,
        bool is_secondary_dex) {
    std::string profile = create_reference_profile_path(location, is_secondary_dex);
    return open_profile(uid, profile, read_write);
}
static void open_profile_files(uid_t uid, const std::string& location, bool is_secondary_dex,
                    std::vector<unique_fd>* profiles_fd, unique_fd* reference_profile_fd) {
    *reference_profile_fd = open_reference_profile(uid, location, true,
            is_secondary_dex);
    std::vector<userid_t> users;
    if (is_secondary_dex){
        users.push_back(multiuser_get_user_id(uid));
    } else {
        users = get_known_users( nullptr);
    }
    for (auto user : users) {
        unique_fd profile_fd = open_current_profile(uid, user, location, is_secondary_dex);
        if (profile_fd.get() >= 0) {
            profiles_fd->push_back(std::move(profile_fd));
        }
    }
}
static void drop_capabilities(uid_t uid) {
    if (setgid(uid) != 0) {
        ALOGE("setgid(%d) failed in installd during dexopt\n", uid);
        exit(64);
    }
    if (setuid(uid) != 0) {
        ALOGE("setuid(%d) failed in installd during dexopt\n", uid);
        exit(65);
    }
    struct __user_cap_header_struct capheader;
    struct __user_cap_data_struct capdata[2];
    memset(&capheader, 0, sizeof(capheader));
    memset(&capdata, 0, sizeof(capdata));
    capheader.version = _LINUX_CAPABILITY_VERSION_3;
    if (capset(&capheader, &capdata[0]) < 0) {
        ALOGE("capset failed: %s\n", strerror(errno));
        exit(66);
    }
}
static constexpr int PROFMAN_BIN_RETURN_CODE_COMPILE = 0;
static constexpr int PROFMAN_BIN_RETURN_CODE_SKIP_COMPILATION = 1;
static constexpr int PROFMAN_BIN_RETURN_CODE_BAD_PROFILES = 2;
static constexpr int PROFMAN_BIN_RETURN_CODE_ERROR_IO = 3;
static constexpr int PROFMAN_BIN_RETURN_CODE_ERROR_LOCKING = 4;
static void run_profman_merge(const std::vector<unique_fd>& profiles_fd,
        const unique_fd& reference_profile_fd) {
    static const size_t MAX_INT_LEN = 32;
    const char* profman_bin = is_debug_runtime() ? "/system/bin/profmand" : "/system/bin/profman";
    std::vector<std::string> profile_args(profiles_fd.size());
    char profile_buf[strlen("--profile-file-fd=") + MAX_INT_LEN];
    for (size_t k = 0; k < profiles_fd.size(); k++) {
        sprintf(profile_buf, "--profile-file-fd=%d", profiles_fd[k].get());
        profile_args[k].assign(profile_buf);
    }
    char reference_profile_arg[strlen("--reference-profile-file-fd=") + MAX_INT_LEN];
    sprintf(reference_profile_arg, "--reference-profile-file-fd=%d", reference_profile_fd.get());
    const char* argv[3 + profiles_fd.size()];
    int i = 0;
    argv[i++] = profman_bin;
    argv[i++] = reference_profile_arg;
    for (size_t k = 0; k < profile_args.size(); k++) {
        argv[i++] = profile_args[k].c_str();
    }
    argv[i] = NULL;
    execv(profman_bin, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", profman_bin, strerror(errno));
    exit(68);
}
static bool analyze_profiles(uid_t uid, const std::string& location, bool is_secondary_dex) {
    std::vector<unique_fd> profiles_fd;
    unique_fd reference_profile_fd;
    open_profile_files(uid, location, is_secondary_dex, &profiles_fd, &reference_profile_fd);
    if (profiles_fd.empty() || (reference_profile_fd.get() < 0)) {
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        run_profman_merge(profiles_fd, reference_profile_fd);
        exit(68);
    }
    int return_code = wait_child(pid);
    bool need_to_compile = false;
    bool should_clear_current_profiles = false;
    bool should_clear_reference_profile = false;
    if (!WIFEXITED(return_code)) {
        LOG(WARNING) << "profman failed for location " << location << ": " << return_code;
    } else {
        return_code = WEXITSTATUS(return_code);
        switch (return_code) {
            case PROFMAN_BIN_RETURN_CODE_COMPILE:
                need_to_compile = true;
                should_clear_current_profiles = true;
                should_clear_reference_profile = false;
                break;
            case PROFMAN_BIN_RETURN_CODE_SKIP_COMPILATION:
                need_to_compile = false;
                should_clear_current_profiles = false;
                should_clear_reference_profile = false;
                break;
            case PROFMAN_BIN_RETURN_CODE_BAD_PROFILES:
                LOG(WARNING) << "Bad profiles for location " << location;
                need_to_compile = false;
                should_clear_current_profiles = true;
                should_clear_reference_profile = true;
                break;
            case PROFMAN_BIN_RETURN_CODE_ERROR_IO:
            case PROFMAN_BIN_RETURN_CODE_ERROR_LOCKING:
                LOG(WARNING) << "IO error while reading profiles for location " << location;
                need_to_compile = false;
                should_clear_current_profiles = false;
                should_clear_reference_profile = false;
                break;
           default:
                LOG(WARNING) << "Unknown error code while processing profiles for location "
                        << location << ": " << return_code;
                need_to_compile = false;
                should_clear_current_profiles = true;
                should_clear_reference_profile = true;
                break;
        }
    }
    if (should_clear_current_profiles) {
        if (is_secondary_dex) {
            clear_current_profile(location, multiuser_get_user_id(uid), is_secondary_dex);
        } else {
            clear_primary_current_profiles(location);
        }
    }
    if (should_clear_reference_profile) {
        clear_reference_profile(location, is_secondary_dex);
    }
    return need_to_compile;
}
bool analyze_primary_profiles(uid_t uid, const std::string& pkgname) {
    return analyze_profiles(uid, pkgname, false);
}
static void run_profman_dump(const std::vector<unique_fd>& profile_fds,
                             const unique_fd& reference_profile_fd,
                             const std::vector<std::string>& dex_locations,
                             const std::vector<unique_fd>& apk_fds,
                             const unique_fd& output_fd) {
    std::vector<std::string> profman_args;
    static const char* PROFMAN_BIN = "/system/bin/profman";
    profman_args.push_back(PROFMAN_BIN);
    profman_args.push_back("--dump-only");
    profman_args.push_back(StringPrintf("--dump-output-to-fd=%d", output_fd.get()));
    if (reference_profile_fd != -1) {
        profman_args.push_back(StringPrintf("--reference-profile-file-fd=%d",
                                            reference_profile_fd.get()));
    }
    for (size_t i = 0; i < profile_fds.size(); i++) {
        profman_args.push_back(StringPrintf("--profile-file-fd=%d", profile_fds[i].get()));
    }
    for (const std::string& dex_location : dex_locations) {
        profman_args.push_back(StringPrintf("--dex-location=%s", dex_location.c_str()));
    }
    for (size_t i = 0; i < apk_fds.size(); i++) {
        profman_args.push_back(StringPrintf("--apk-fd=%d", apk_fds[i].get()));
    }
    const char **argv = new const char*[profman_args.size() + 1];
    size_t i = 0;
    for (const std::string& profman_arg : profman_args) {
        argv[i++] = profman_arg.c_str();
    }
    argv[i] = NULL;
    execv(PROFMAN_BIN, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", PROFMAN_BIN, strerror(errno));
    exit(68);
}
bool dump_profiles(int32_t uid, const std::string& pkgname, const char* code_paths) {
    std::vector<unique_fd> profile_fds;
    unique_fd reference_profile_fd;
    std::string out_file_name = StringPrintf("/data/misc/profman/%s.txt", pkgname.c_str());
    open_profile_files(uid, pkgname, false,
            &profile_fds, &reference_profile_fd);
    const bool has_reference_profile = (reference_profile_fd.get() != -1);
    const bool has_profiles = !profile_fds.empty();
    if (!has_reference_profile && !has_profiles) {
        LOG(ERROR) << "profman dump: no profiles to dump for " << pkgname;
        return false;
    }
    unique_fd output_fd(open(out_file_name.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644));
    if (fchmod(output_fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH) < 0) {
        ALOGE("installd cannot chmod '%s' dump_profile\n", out_file_name.c_str());
        return false;
    }
    std::vector<std::string> code_full_paths = base::Split(code_paths, ";");
    std::vector<std::string> dex_locations;
    std::vector<unique_fd> apk_fds;
    for (const std::string& code_full_path : code_full_paths) {
        const char* full_path = code_full_path.c_str();
        unique_fd apk_fd(open(full_path, O_RDONLY | O_NOFOLLOW));
        if (apk_fd == -1) {
            ALOGE("installd cannot open '%s'\n", full_path);
            return false;
        }
        dex_locations.push_back(get_location_from_path(full_path));
        apk_fds.push_back(std::move(apk_fd));
    }
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        run_profman_dump(profile_fds, reference_profile_fd, dex_locations,
                         apk_fds, output_fd);
        exit(68);
    }
    int return_code = wait_child(pid);
    if (!WIFEXITED(return_code)) {
        LOG(WARNING) << "profman failed for package " << pkgname << ": "
                << return_code;
        return false;
    }
    return true;
}
bool copy_system_profile(const std::string& system_profile,
        uid_t packageUid, const std::string& data_profile_location) {
    unique_fd in_fd(open(system_profile.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    unique_fd out_fd(open_reference_profile(packageUid,
                     data_profile_location,
                                    true,
                                   false));
    if (in_fd.get() < 0) {
        PLOG(WARNING) << "Could not open profile " << system_profile;
        return false;
    }
    if (out_fd.get() < 0) {
        PLOG(WARNING) << "Could not open profile " << data_profile_location;
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(packageUid);
        if (flock(out_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK) {
                PLOG(WARNING) << "Error locking profile " << data_profile_location;
            }
            PLOG(WARNING) << "Failed to flock " << data_profile_location;
            return false;
        }
        bool truncated = ftruncate(out_fd.get(), 0) == 0;
        if (!truncated) {
            PLOG(WARNING) << "Could not truncate " << data_profile_location;
        }
        static constexpr size_t kBufferSize = 4 * 1024;
        char buffer[kBufferSize];
        while (true) {
            ssize_t bytes = read(in_fd.get(), buffer, kBufferSize);
            if (bytes == 0) {
                break;
            }
            write(out_fd.get(), buffer, bytes);
        }
        if (flock(out_fd.get(), LOCK_UN) != 0) {
            PLOG(WARNING) << "Error unlocking profile " << data_profile_location;
        }
        _exit(0);
    }
    int return_code = wait_child(pid);
    return return_code == 0;
}
static std::string replace_file_extension(const std::string& oat_path, const std::string& new_ext) {
  if (EndsWith(oat_path, ".dex")) {
    std::string new_path = oat_path;
    new_path.replace(new_path.length() - strlen(".dex"), strlen(".dex"), new_ext);
    CHECK(EndsWith(new_path, new_ext.c_str()));
    return new_path;
  }
  size_t odex_pos = oat_path.rfind(".odex");
  if (odex_pos != std::string::npos) {
    std::string new_path = oat_path;
    new_path.replace(odex_pos, strlen(".odex"), new_ext);
    CHECK_NE(new_path.find(new_ext), std::string::npos);
    return new_path;
  }
  return "";
}
static std::string create_image_filename(const std::string& oat_path) {
    return replace_file_extension(oat_path, ".art");
}
static std::string create_vdex_filename(const std::string& oat_path) {
    return replace_file_extension(oat_path, ".vdex");
}
static bool add_extension_to_file_name(char* file_name, const char* extension) {
    if (strlen(file_name) + strlen(extension) + 1 > PKG_PATH_MAX) {
        return false;
    }
    strcat(file_name, extension);
    return true;
}
static int open_output_file(const char* file_name, bool recreate, int permissions) {
    int flags = O_RDWR | O_CREAT;
    if (recreate) {
        if (unlink(file_name) < 0) {
            if (errno != ENOENT) {
                PLOG(ERROR) << "open_output_file: Couldn't unlink " << file_name;
            }
        }
        flags |= O_EXCL;
    }
    return open(file_name, flags, permissions);
}
static bool set_permissions_and_ownership(
        int fd, bool is_public, int uid, const char* path, bool is_secondary_dex) {
    int owning_uid = is_secondary_dex ? uid : AID_SYSTEM;
    if (fchmod(fd,
               S_IRUSR|S_IWUSR|S_IRGRP |
               (is_public ? S_IROTH : 0)) < 0) {
        ALOGE("installd cannot chmod '%s' during dexopt\n", path);
        return false;
    } else if (fchown(fd, owning_uid, uid) < 0) {
        ALOGE("installd cannot chown '%s' during dexopt\n", path);
        return false;
    }
    return true;
}
static bool IsOutputDalvikCache(const char* oat_dir) {
  return oat_dir == nullptr || oat_dir[0] == '!';
}
static bool validate_dex_path_size(const std::string& dex_path) {
    if (dex_path.size() >= (PKG_PATH_MAX - 8)) {
        LOG(ERROR) << "dex_path too long: " << dex_path;
        return false;
    }
    return true;
}
static bool create_oat_out_path(const char* apk_path, const char* instruction_set,
            const char* oat_dir, bool is_secondary_dex, char* out_oat_path) {
    if (!validate_dex_path_size(apk_path)) {
        return false;
    }
    if (!IsOutputDalvikCache(oat_dir)) {
        if (!is_secondary_dex && validate_apk_path(oat_dir)) {
            ALOGE("cannot validate apk path with oat_dir '%s'\n", oat_dir);
            return false;
        }
        if (!calculate_oat_file_path(out_oat_path, oat_dir, apk_path, instruction_set)) {
            return false;
        }
    } else {
        if (!create_cache_path(out_oat_path, apk_path, instruction_set)) {
            return false;
        }
    }
    return true;
}
class Dex2oatFileWrapper {
 public:
    Dex2oatFileWrapper() : value_(-1), cleanup_(), do_cleanup_(true), auto_close_(true) {
    }
    Dex2oatFileWrapper(int value, std::function<void ()> cleanup)
            : value_(value), cleanup_(cleanup), do_cleanup_(true), auto_close_(true) {}
    Dex2oatFileWrapper(Dex2oatFileWrapper&& other) {
        value_ = other.value_;
        cleanup_ = other.cleanup_;
        do_cleanup_ = other.do_cleanup_;
        auto_close_ = other.auto_close_;
        other.release();
    }
    Dex2oatFileWrapper& operator=(Dex2oatFileWrapper&& other) {
        value_ = other.value_;
        cleanup_ = other.cleanup_;
        do_cleanup_ = other.do_cleanup_;
        auto_close_ = other.auto_close_;
        other.release();
        return *this;
    }
    ~Dex2oatFileWrapper() {
        reset(-1);
    }
    int get() {
        return value_;
    }
    void SetCleanup(bool cleanup) {
        do_cleanup_ = cleanup;
    }
    void reset(int new_value) {
        if (auto_close_ && value_ >= 0) {
            close(value_);
        }
        if (do_cleanup_ && cleanup_ != nullptr) {
            cleanup_();
        }
        value_ = new_value;
    }
    void reset(int new_value, std::function<void ()> new_cleanup) {
        if (auto_close_ && value_ >= 0) {
            close(value_);
        }
        if (do_cleanup_ && cleanup_ != nullptr) {
            cleanup_();
        }
        value_ = new_value;
        cleanup_ = new_cleanup;
    }
    void DisableAutoClose() {
        auto_close_ = false;
    }
 private:
    void release() {
        value_ = -1;
        do_cleanup_ = false;
        cleanup_ = nullptr;
    }
    int value_;
    std::function<void ()> cleanup_;
    bool do_cleanup_;
    bool auto_close_;
};
Dex2oatFileWrapper maybe_open_app_image(const char* out_oat_path, bool profile_guided,
        bool is_public, int uid, bool is_secondary_dex) {
    if (is_secondary_dex) {
        return Dex2oatFileWrapper();
    }
    const std::string image_path = create_image_filename(out_oat_path);
    if (image_path.empty()) {
        return Dex2oatFileWrapper();
    }
    if (!profile_guided) {
        unlink(image_path.c_str());
        return Dex2oatFileWrapper();
    }
    char app_image_format[kPropertyValueMax];
    bool have_app_image_format =
            get_property("dalvik.vm.appimageformat", app_image_format, NULL) > 0;
    if (!have_app_image_format) {
        return Dex2oatFileWrapper();
    }
    Dex2oatFileWrapper wrapper_fd(
            open_output_file(image_path.c_str(), true , 0600 ),
            [image_path]() { unlink(image_path.c_str()); });
    if (wrapper_fd.get() < 0) {
        LOG(ERROR) << "installd could not create '" << image_path
                << "' for image file during dexopt";
        if (unlink(image_path.c_str()) < 0) {
            if (errno != ENOENT) {
                PLOG(ERROR) << "Couldn't unlink image file " << image_path;
            }
        }
    } else if (!set_permissions_and_ownership(
                wrapper_fd.get(), is_public, uid, image_path.c_str(), is_secondary_dex)) {
        ALOGE("installd cannot set owner '%s' for image during dexopt\n", image_path.c_str());
        wrapper_fd.reset(-1);
    }
    return wrapper_fd;
}
unique_fd maybe_open_dexopt_swap_file(const char* out_oat_path) {
    if (!ShouldUseSwapFileForDexopt()) {
        return invalid_unique_fd();
    }
    char swap_file_name[PKG_PATH_MAX];
    strcpy(swap_file_name, out_oat_path);
    if (!add_extension_to_file_name(swap_file_name, ".swap")) {
        return invalid_unique_fd();
    }
    unique_fd swap_fd(open_output_file(
            swap_file_name, true, 0600));
    if (swap_fd.get() < 0) {
        ALOGE("installd could not create '%s' for swap during dexopt\n", swap_file_name);
    } else {
        if (unlink(swap_file_name) < 0) {
            PLOG(ERROR) << "Couldn't unlink swap file " << swap_file_name;
        }
    }
    return swap_fd;
}
Dex2oatFileWrapper maybe_open_reference_profile(const std::string& pkgname,
        const std::string& dex_path, bool profile_guided, bool is_public, int uid,
        bool is_secondary_dex) {
    if (!profile_guided || is_public || (pkgname[0] == '*')) {
        return Dex2oatFileWrapper();
    }
    const std::string location = is_secondary_dex ? dex_path : pkgname;
    unique_fd ufd = open_reference_profile(uid, location, false, is_secondary_dex);
    const auto& cleanup = [location, is_secondary_dex]() {
        clear_reference_profile(location.c_str(), is_secondary_dex);
    };
    return Dex2oatFileWrapper(ufd.release(), cleanup);
}
bool open_vdex_files(const char* apk_path, const char* out_oat_path, int dexopt_needed,
        const char* instruction_set, bool is_public, int uid, bool is_secondary_dex,
        bool profile_guided, Dex2oatFileWrapper* in_vdex_wrapper_fd,
        Dex2oatFileWrapper* out_vdex_wrapper_fd) {
    CHECK(in_vdex_wrapper_fd != nullptr);
    CHECK(out_vdex_wrapper_fd != nullptr);
    char in_odex_path[PKG_PATH_MAX];
    int dexopt_action = abs(dexopt_needed);
    bool is_odex_location = dexopt_needed < 0;
    std::string in_vdex_path_str;
    const std::string out_vdex_path_str = create_vdex_filename(out_oat_path);
    if (out_vdex_path_str.empty()) {
        return false;
    }
    bool update_vdex_in_place = false;
    if (dexopt_action != DEX2OAT_FROM_SCRATCH) {
        const char* path = nullptr;
        if (is_odex_location) {
            if (calculate_odex_file_path(in_odex_path, apk_path, instruction_set)) {
                path = in_odex_path;
            } else {
                ALOGE("installd cannot compute input vdex location for '%s'\n", apk_path);
                return false;
            }
        } else {
            path = out_oat_path;
        }
        in_vdex_path_str = create_vdex_filename(path);
        if (in_vdex_path_str.empty()) {
            ALOGE("installd cannot compute input vdex location for '%s'\n", path);
            return false;
        }
        update_vdex_in_place =
            (in_vdex_path_str == out_vdex_path_str) &&
            (dexopt_action == DEX2OAT_FOR_BOOT_IMAGE) &&
            !profile_guided;
        if (update_vdex_in_place) {
            in_vdex_wrapper_fd->reset(open(in_vdex_path_str.c_str(), O_RDWR, 0));
            if (in_vdex_wrapper_fd->get() == -1) {
                update_vdex_in_place = false;
            }
        } else {
            in_vdex_wrapper_fd->reset(open(in_vdex_path_str.c_str(), O_RDONLY, 0));
        }
    }
    if (update_vdex_in_place) {
        out_vdex_wrapper_fd->reset(
              in_vdex_wrapper_fd->get(),
              [out_vdex_path_str]() { unlink(out_vdex_path_str.c_str()); });
        in_vdex_wrapper_fd->DisableAutoClose();
    } else {
        out_vdex_wrapper_fd->reset(
              open_output_file(out_vdex_path_str.c_str(), true, 0644),
              [out_vdex_path_str]() { unlink(out_vdex_path_str.c_str()); });
        if (out_vdex_wrapper_fd->get() < 0) {
            ALOGE("installd cannot open vdex'%s' during dexopt\n", out_vdex_path_str.c_str());
            return false;
        }
    }
    if (!set_permissions_and_ownership(out_vdex_wrapper_fd->get(), is_public, uid,
            out_vdex_path_str.c_str(), is_secondary_dex)) {
        ALOGE("installd cannot set owner '%s' for vdex during dexopt\n", out_vdex_path_str.c_str());
        return false;
    }
    return true;
}
Dex2oatFileWrapper open_oat_out_file(const char* apk_path, const char* oat_dir,
        bool is_public, int uid, const char* instruction_set, bool is_secondary_dex,
        char* out_oat_path) {
    if (!create_oat_out_path(apk_path, instruction_set, oat_dir, is_secondary_dex, out_oat_path)) {
        return Dex2oatFileWrapper();
    }
    const std::string out_oat_path_str(out_oat_path);
    Dex2oatFileWrapper wrapper_fd(
            open_output_file(out_oat_path, true, 0644),
            [out_oat_path_str]() { unlink(out_oat_path_str.c_str()); });
    if (wrapper_fd.get() < 0) {
        PLOG(ERROR) << "installd cannot open output during dexopt" << out_oat_path;
    } else if (!set_permissions_and_ownership(
                wrapper_fd.get(), is_public, uid, out_oat_path, is_secondary_dex)) {
        ALOGE("installd cannot set owner '%s' for output during dexopt\n", out_oat_path);
        wrapper_fd.reset(-1);
    }
    return wrapper_fd;
}
void update_out_oat_access_times(const char* apk_path, const char* out_oat_path) {
    struct stat input_stat;
    memset(&input_stat, 0, sizeof(input_stat));
    if (stat(apk_path, &input_stat) != 0) {
        PLOG(ERROR) << "Could not stat " << apk_path << " during dexopt";
        return;
    }
    struct utimbuf ut;
    ut.actime = input_stat.st_atime;
    ut.modtime = input_stat.st_mtime;
    if (utime(out_oat_path, &ut) != 0) {
        PLOG(WARNING) << "Could not update access times for " << apk_path << " during dexopt";
    }
}
static void exec_dexoptanalyzer(const std::string& dex_file, const std::string& instruction_set,
        const std::string& compiler_filter, bool profile_was_updated, bool downgrade) {
    const char* dexoptanalyzer_bin =
            is_debug_runtime()
                    ? "/system/bin/dexoptanalyzerd"
                    : "/system/bin/dexoptanalyzer";
    static const unsigned int MAX_INSTRUCTION_SET_LEN = 7;
    if (instruction_set.size() >= MAX_INSTRUCTION_SET_LEN) {
        LOG(ERROR) << "Instruction set " << instruction_set
                << " longer than max length of " << MAX_INSTRUCTION_SET_LEN;
        return;
    }
    std::string dex_file_arg = "--dex-file=" + dex_file;
    std::string isa_arg = "--isa=" + instruction_set;
    std::string compiler_filter_arg = "--compiler-filter=" + compiler_filter;
    const char* assume_profile_changed = "--assume-profile-changed";
    const char* downgrade_flag = "--downgrade";
    const int argc = 5 +
        (profile_was_updated ? 1 : 0) +
        (downgrade ? 1 : 0);
    const char* argv[argc];
    int i = 0;
    argv[i++] = dexoptanalyzer_bin;
    argv[i++] = dex_file_arg.c_str();
    argv[i++] = isa_arg.c_str();
    argv[i++] = compiler_filter_arg.c_str();
    if (profile_was_updated) {
        argv[i++] = assume_profile_changed;
    }
    if (downgrade) {
        argv[i++] = downgrade_flag;
    }
    argv[i] = NULL;
    execv(dexoptanalyzer_bin, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", dexoptanalyzer_bin, strerror(errno));
}
static bool prepare_secondary_dex_oat_dir(const std::string& dex_path, int uid,
        const char* instruction_set, std::string* oat_dir_out) {
    unsigned long dirIndex = dex_path.rfind('/');
    if (dirIndex == std::string::npos) {
        LOG(ERROR ) << "Unexpected dir structure for secondary dex " << dex_path;
        return false;
    }
    std::string dex_dir = dex_path.substr(0, dirIndex);
    mode_t oat_dir_mode = S_IRWXU | S_IRWXG | S_IXOTH;
    if (prepare_app_cache_dir(dex_dir, "oat", oat_dir_mode, uid, uid) != 0) {
        LOG(ERROR) << "Could not prepare oat dir for secondary dex: " << dex_path;
        return false;
    }
    char oat_dir[PKG_PATH_MAX];
    snprintf(oat_dir, PKG_PATH_MAX, "%s/oat", dex_dir.c_str());
    oat_dir_out->assign(oat_dir);
    if (prepare_app_cache_dir(*oat_dir_out, instruction_set, oat_dir_mode, uid, uid) != 0) {
        LOG(ERROR) << "Could not prepare oat/isa dir for secondary dex: " << dex_path;
        return false;
    }
    return true;
}
static int constexpr DEXOPTANALYZER_BIN_EXEC_ERROR = 200;
static bool process_dexoptanalyzer_result(const std::string& dex_path, int result,
            int* dexopt_needed_out) {
    switch (result) {
        case 0:
            *dexopt_needed_out = NO_DEXOPT_NEEDED; return true;
        case 1:
            *dexopt_needed_out = DEX2OAT_FROM_SCRATCH; return true;
        case 5:
            *dexopt_needed_out = -DEX2OAT_FOR_BOOT_IMAGE; return true;
        case 6:
            *dexopt_needed_out = -DEX2OAT_FOR_FILTER; return true;
        case 7:
            *dexopt_needed_out = -DEX2OAT_FOR_RELOCATION; return true;
        case 2:
        case 3:
        case 4:
            LOG(ERROR) << "Dexoptnalyzer return the status of an oat file."
                    << " Expected odex file status for secondary dex " << dex_path
                    << " : dexoptanalyzer result=" << result;
            return false;
        default:
            LOG(ERROR) << "Unexpected result for dexoptanalyzer " << dex_path
                    << " exec_dexoptanalyzer result=" << result;
            return false;
    }
}
static bool process_secondary_dex_dexopt(const char* original_dex_path, const char* pkgname,
        int dexopt_flags, const char* volume_uuid, int uid, const char* instruction_set,
        const char* compiler_filter, bool* is_public_out, int* dexopt_needed_out,
        std::string* oat_dir_out, std::string* dex_path_out, bool downgrade) {
    int storage_flag;
    if ((dexopt_flags & DEXOPT_STORAGE_CE) != 0) {
        storage_flag = FLAG_STORAGE_CE;
        if ((dexopt_flags & DEXOPT_STORAGE_DE) != 0) {
            LOG(ERROR) << "Ambiguous secondary dex storage flag. Both, CE and DE, flags are set";
            return false;
        }
    } else if ((dexopt_flags & DEXOPT_STORAGE_DE) != 0) {
        storage_flag = FLAG_STORAGE_DE;
    } else {
        LOG(ERROR) << "Secondary dex storage flag must be set";
        return false;
    }
    {
        UniqueCPtr<char> dex_real_path_cstr(realpath(original_dex_path, nullptr));
        if (dex_real_path_cstr == nullptr) {
            PLOG(ERROR) << "Could not get the real path of the secondary dex file "
                    << original_dex_path;
            return false;
        } else {
            dex_path_out->assign(dex_real_path_cstr.get());
        }
    }
    const std::string& dex_path = *dex_path_out;
    if (!validate_dex_path_size(dex_path)) {
        return false;
    }
    if (!validate_secondary_dex_path(pkgname, dex_path, volume_uuid, uid, storage_flag)) {
        LOG(ERROR) << "Could not validate secondary dex path " << dex_path;
        return false;
    }
    struct stat dex_path_stat;
    if (stat(dex_path.c_str(), &dex_path_stat) != 0) {
        if (errno == ENOENT) {
            ALOGV("Secondary dex does not exist %s", dex_path.c_str());
            return NO_DEXOPT_NEEDED;
        } else {
            PLOG(ERROR) << "Could not access secondary dex " << dex_path;
        }
    }
    *is_public_out = ((dexopt_flags & DEXOPT_PUBLIC) != 0) &&
            ((dex_path_stat.st_mode & S_IROTH) != 0);
    if (!prepare_secondary_dex_oat_dir(dex_path, uid, instruction_set, oat_dir_out)) {
        return false;
    }
    bool profile_was_updated = analyze_profiles(uid, dex_path, true);
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        exec_dexoptanalyzer(dex_path, instruction_set, compiler_filter, profile_was_updated,
                            downgrade);
        exit(DEXOPTANALYZER_BIN_EXEC_ERROR);
    }
    int result = wait_child(pid);
    if (!WIFEXITED(result)) {
        LOG(ERROR) << "dexoptanalyzer failed for path " << dex_path << ": " << result;
        return false;
    }
    result = WEXITSTATUS(result);
    bool success = process_dexoptanalyzer_result(dex_path, result, dexopt_needed_out);
    if (success && ((dexopt_flags & DEXOPT_FORCE) != 0)) {
        *dexopt_needed_out = DEX2OAT_FROM_SCRATCH;
    }
    return success;
}
int dexopt(const char* dex_path, uid_t uid, const char* pkgname, const char* instruction_set,
        int dexopt_needed, const char* oat_dir, int dexopt_flags, const char* compiler_filter,
        const char* volume_uuid, const char* class_loader_context, const char* se_info,
        bool downgrade) {
    CHECK(pkgname != nullptr);
    CHECK(pkgname[0] != 0);
    if ((dexopt_flags & ~DEXOPT_MASK) != 0) {
        LOG_FATAL("dexopt flags contains unknown fields\n");
    }
    if (!validate_dex_path_size(dex_path)) {
        return -1;
    }
    if (class_loader_context != nullptr && strlen(class_loader_context) > PKG_PATH_MAX) {
        LOG(ERROR) << "Class loader context exceeds the allowed size: " << class_loader_context;
        return -1;
    }
    bool is_public = (dexopt_flags & DEXOPT_PUBLIC) != 0;
    bool debuggable = (dexopt_flags & DEXOPT_DEBUGGABLE) != 0;
    bool boot_complete = (dexopt_flags & DEXOPT_BOOTCOMPLETE) != 0;
    bool profile_guided = (dexopt_flags & DEXOPT_PROFILE_GUIDED) != 0;
    bool is_secondary_dex = (dexopt_flags & DEXOPT_SECONDARY_DEX) != 0;
    std::string oat_dir_str;
    std::string dex_real_path;
    if (is_secondary_dex) {
        if (process_secondary_dex_dexopt(dex_path, pkgname, dexopt_flags, volume_uuid, uid,
                instruction_set, compiler_filter, &is_public, &dexopt_needed, &oat_dir_str,
                &dex_real_path,
                downgrade)) {
            oat_dir = oat_dir_str.c_str();
            dex_path = dex_real_path.c_str();
            if (dexopt_needed == NO_DEXOPT_NEEDED) {
                return 0;
            }
        } else {
            return -1;
        }
    } else {
        CHECK((dexopt_flags & DEXOPT_STORAGE_CE) == 0);
        CHECK((dexopt_flags & DEXOPT_STORAGE_DE) == 0);
    }
    unique_fd input_fd(open(dex_path, O_RDONLY, 0));
    if (input_fd.get() < 0) {
        ALOGE("installd cannot open '%s' for input during dexopt\n", dex_path);
        return -1;
    }
    char out_oat_path[PKG_PATH_MAX];
    Dex2oatFileWrapper out_oat_fd = open_oat_out_file(dex_path, oat_dir, is_public, uid,
            instruction_set, is_secondary_dex, out_oat_path);
    if (out_oat_fd.get() < 0) {
        return -1;
    }
    Dex2oatFileWrapper in_vdex_fd;
    Dex2oatFileWrapper out_vdex_fd;
    if (!open_vdex_files(dex_path, out_oat_path, dexopt_needed, instruction_set, is_public, uid,
            is_secondary_dex, profile_guided, &in_vdex_fd, &out_vdex_fd)) {
        return -1;
    }
    if (is_secondary_dex) {
        if (selinux_android_restorecon_pkgdir(oat_dir, se_info, uid,
                SELINUX_ANDROID_RESTORECON_RECURSE)) {
            LOG(ERROR) << "Failed to restorecon " << oat_dir;
            return -1;
        }
    }
    unique_fd swap_fd = maybe_open_dexopt_swap_file(out_oat_path);
    Dex2oatFileWrapper image_fd =
            maybe_open_app_image(out_oat_path, profile_guided, is_public, uid, is_secondary_dex);
    Dex2oatFileWrapper reference_profile_fd = maybe_open_reference_profile(
            pkgname, dex_path, profile_guided, is_public, uid, is_secondary_dex);
    ALOGV("DexInv: --- BEGIN '%s' ---\n", dex_path);
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        SetDex2OatScheduling(boot_complete);
        if (flock(out_oat_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            ALOGE("flock(%s) failed: %s\n", out_oat_path, strerror(errno));
            _exit(67);
        }
        run_dex2oat(input_fd.get(),
                    out_oat_fd.get(),
                    in_vdex_fd.get(),
                    out_vdex_fd.get(),
                    image_fd.get(),
                    dex_path,
                    out_oat_path,
                    swap_fd.get(),
                    instruction_set,
                    compiler_filter,
                    debuggable,
                    boot_complete,
                    reference_profile_fd.get(),
                    class_loader_context);
        _exit(68);
    } else {
        int res = wait_child(pid);
        if (res == 0) {
            ALOGV("DexInv: --- END '%s' (success) ---\n", dex_path);
        } else {
            ALOGE("DexInv: --- END '%s' --- status=0x%04x, process failed\n", dex_path, res);
            return res;
        }
    }
    update_out_oat_access_times(dex_path, out_oat_path);
    out_oat_fd.SetCleanup(false);
    out_vdex_fd.SetCleanup(false);
    image_fd.SetCleanup(false);
    reference_profile_fd.SetCleanup(false);
    return 0;
}
static bool rmdir_if_empty(const char* dir) {
    if (rmdir(dir) == 0) {
        return true;
    }
    if (errno == ENOENT || errno == ENOTEMPTY) {
        return true;
    }
    PLOG(ERROR) << "Failed to remove dir: " << dir;
    return false;
}
static bool unlink_if_exists(const std::string& file) {
    if (unlink(file.c_str()) == 0) {
        return true;
    }
    if (errno == ENOENT) {
        return true;
    }
    PLOG(ERROR) << "Could not unlink: " << file;
    return false;
}
static bool create_secondary_dex_oat_layout(const std::string& dex_path, const std::string& isa,
               char* out_oat_dir, char* out_oat_isa_dir, char* out_oat_path) {
    size_t dirIndex = dex_path.rfind('/');
    if (dirIndex == std::string::npos) {
        LOG(ERROR) << "Unexpected dir structure for dex file " << dex_path;
        return false;
    }
    std::string apk_dir = dex_path.substr(0, dirIndex);
    snprintf(out_oat_dir, PKG_PATH_MAX, "%s/oat", apk_dir.c_str());
    snprintf(out_oat_isa_dir, PKG_PATH_MAX, "%s/%s", out_oat_dir, isa.c_str());
    if (!create_oat_out_path(dex_path.c_str(), isa.c_str(), out_oat_dir,
                                true, out_oat_path)) {
        LOG(ERROR) << "Could not create oat path for secondary dex " << dex_path;
        return false;
    }
    return true;
}
bool reconcile_secondary_dex_file(const std::string& dex_path,
        const std::string& pkgname, int uid, const std::vector<std::string>& isas,
        const std::unique_ptr<std::string>& volume_uuid, int storage_flag,
               bool* out_secondary_dex_exists) {
    *out_secondary_dex_exists = false;
    if (!validate_dex_path_size(dex_path)) {
        return false;
    }
    if (isas.size() == 0) {
        LOG(ERROR) << "reconcile_secondary_dex_file called with empty isas vector";
        return false;
    }
    const char* volume_uuid_cstr = volume_uuid == nullptr ? nullptr : volume_uuid->c_str();
    if (!validate_secondary_dex_path(pkgname.c_str(), dex_path.c_str(), volume_uuid_cstr,
            uid, storage_flag, false)) {
        LOG(ERROR) << "Could not validate secondary dex path " << dex_path;
        return false;
    }
    if (access(dex_path.c_str(), F_OK) == 0) {
        *out_secondary_dex_exists = true;
        return true;
    } else if (errno != ENOENT) {
        PLOG(ERROR) << "Failed to check access to secondary dex " << dex_path;
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        char oat_path[PKG_PATH_MAX];
        char oat_dir[PKG_PATH_MAX];
        char oat_isa_dir[PKG_PATH_MAX];
        bool result = true;
        drop_capabilities(uid);
        for (size_t i = 0; i < isas.size(); i++) {
            if (!create_secondary_dex_oat_layout(dex_path,
                                                 isas[i],
                                                 oat_dir,
                                                 oat_isa_dir,
                                                 oat_path)) {
                LOG(ERROR) << "Could not create secondary odex layout: "
                           << dex_path;
                result = false;
                continue;
            }
            result = unlink_if_exists(oat_path) && result;
            result = unlink_if_exists(create_vdex_filename(oat_path)) && result;
            result = unlink_if_exists(create_image_filename(oat_path)) && result;
            std::string current_profile = create_current_profile_path(
                multiuser_get_user_id(uid), dex_path, true);
            std::string reference_profile = create_reference_profile_path(
                dex_path, true);
            result = unlink_if_exists(current_profile) && result;
            result = unlink_if_exists(reference_profile) && result;
            std::string old_current_profile = dex_path + ".prof";
            result = unlink_if_exists(old_current_profile);
            result = rmdir_if_empty(oat_isa_dir) && result;
            result = rmdir_if_empty(oat_dir) && result;
        }
        result ? _exit(0) : _exit(1);
    }
    int return_code = wait_child(pid);
    return return_code == 0;
}
static bool unlink_and_rename(const char* from, const char* to) {
    struct stat s;
    if (stat(to, &s) == 0) {
        if (!S_ISREG(s.st_mode)) {
            LOG(ERROR) << from << " is not a regular file to replace for A/B.";
            return false;
        }
        if (unlink(to) != 0) {
            LOG(ERROR) << "Could not unlink " << to << " to move A/B.";
            return false;
        }
    } else {
    }
    if (rename(from, to) != 0) {
        PLOG(ERROR) << "Could not rename " << from << " to " << to;
        return false;
    }
    return true;
}
static bool move_ab_path(const std::string& b_path, const std::string& a_path) {
    {
        struct stat s;
        if (stat(b_path.c_str(), &s) != 0) {
            return false;
        }
        if (!S_ISREG(s.st_mode)) {
            LOG(ERROR) << "A/B artifact " << b_path << " is not a regular file.";
            unlink(b_path.c_str());
            return false;
        }
    }
    if (!unlink_and_rename(b_path.c_str(), a_path.c_str())) {
        if (unlink(b_path.c_str()) != 0) {
            PLOG(ERROR) << "Could not unlink " << b_path;
        }
        return false;
    }
    return true;
}
bool move_ab(const char* apk_path, const char* instruction_set, const char* oat_dir) {
    std::string slot_suffix;
    {
        char buf[kPropertyValueMax];
        if (get_property("ro.boot.slot_suffix", buf, nullptr) <= 0) {
            return false;
        }
        slot_suffix = buf;
        if (!ValidateTargetSlotSuffix(slot_suffix)) {
            LOG(ERROR) << "Target slot suffix not legal: " << slot_suffix;
            return false;
        }
    }
    if (validate_apk_path(apk_path) != 0) {
        LOG(ERROR) << "Invalid apk_path: " << apk_path;
        return false;
    }
    if (validate_apk_path(oat_dir) != 0) {
        LOG(ERROR) << "Invalid oat_dir: " << oat_dir;
        return false;
    }
    char a_path[PKG_PATH_MAX];
    if (!calculate_oat_file_path(a_path, oat_dir, apk_path, instruction_set)) {
        return false;
    }
    const std::string a_vdex_path = create_vdex_filename(a_path);
    const std::string a_image_path = create_image_filename(a_path);
    const std::string b_path = StringPrintf("%s.%s", a_path, slot_suffix.c_str());
    const std::string b_vdex_path = StringPrintf("%s.%s", a_vdex_path.c_str(), slot_suffix.c_str());
    const std::string b_image_path = StringPrintf("%s.%s",
                                                  a_image_path.c_str(),
                                                  slot_suffix.c_str());
    bool success = true;
    if (move_ab_path(b_path, a_path)) {
        if (move_ab_path(b_vdex_path, a_vdex_path)) {
            constexpr bool kIgnoreAppImageFailure = true;
            if (!a_image_path.empty()) {
                if (!move_ab_path(b_image_path, a_image_path)) {
                    unlink(a_image_path.c_str());
                    if (!kIgnoreAppImageFailure) {
                        success = false;
                    }
                }
            }
        } else {
            unlink(b_image_path.c_str());
            success = false;
        }
    } else {
        unlink(b_vdex_path.c_str());
        unlink(b_image_path.c_str());
        success = false;
    }
    return success;
}
bool delete_odex(const char* apk_path, const char* instruction_set, const char* oat_dir) {
    char out_path[PKG_PATH_MAX];
    if (!create_oat_out_path(apk_path, instruction_set, oat_dir,
                                false, out_path)) {
        return false;
    }
    auto unlink_and_check = [](const char* path) -> bool {
        int result = unlink(path);
        if (result != 0) {
            if (errno == EACCES || errno == EPERM) {
                PLOG(ERROR) << "Could not unlink " << path;
                return false;
            }
            PLOG(WARNING) << "Could not unlink " << path;
        }
        return true;
    };
    bool return_value_oat = unlink_and_check(out_path);
    bool return_value_art = unlink_and_check(create_image_filename(out_path).c_str());
    bool return_value_vdex = unlink_and_check(create_vdex_filename(out_path).c_str());
    return return_value_oat && return_value_art && return_value_vdex;
}
}
}
