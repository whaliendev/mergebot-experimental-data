#include "commands.h"
#include <errno.h>
#include <inttypes.h>
#include <regex>
#include <stdlib.h>
#include <sys/capability.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <cutils/log.h>
#include <cutils/sched_policy.h>
#include <diskusage/dirsize.h>
#include <logwrap/logwrap.h>
#include <private/android_filesystem_config.h>
#include <selinux/android.h>
#include <system/thread_defs.h>
#include <globals.h>
#include <installd_deps.h>
#include <otapreopt_utils.h>
#include <utils.h>
#ifndef LOG_TAG
#define LOG_TAG "installd"
#endif
using android::base::EndsWith;
using android::base::StringPrintf;
namespace android {
namespace installd {
static constexpr const char* kCpPath = "/system/bin/cp";
static constexpr const char* kXattrDefault = "user.default";
static constexpr const char* PKG_LIB_POSTFIX = "/lib";
static constexpr const char* CACHE_DIR_POSTFIX = "/cache";
static constexpr const char* CODE_CACHE_DIR_POSTFIX = "/code_cache";
static constexpr const char* IDMAP_PREFIX = "/data/resource-cache/";
static constexpr const char* IDMAP_SUFFIX = "@idmap";
static constexpr int FLAG_STORAGE_DE = 1 << 0;
static constexpr int FLAG_STORAGE_CE = 1 << 1;
static constexpr int FLAG_CLEAR_CACHE_ONLY = 1 << 8;
static constexpr int FLAG_CLEAR_CODE_CACHE_ONLY = 1 << 9;
static constexpr int DEX2OAT_FROM_SCRATCH = 1;
static constexpr int DEX2OAT_FOR_BOOT_IMAGE = 2;
static constexpr int DEX2OAT_FOR_FILTER = 3;
static constexpr int DEX2OAT_FOR_RELOCATION = 4;
static constexpr int PATCHOAT_FOR_RELOCATION = 5;
#define MIN_RESTRICTED_HOME_SDK_VERSION 24
typedef int fd_t;
namespace {
constexpr const char* kDump = "android.permission.DUMP";
binder::Status checkPermission(const char* permission) {
    pid_t pid;
    uid_t uid;
    if (checkCallingPermission(String16(permission), reinterpret_cast<int32_t*>(&pid),
            reinterpret_cast<int32_t*>(&uid))) {
        return binder::Status::ok();
    } else {
        auto err = StringPrintf("UID %d / PID %d lacks permission %s", uid, pid, permission);
        return binder::Status::fromExceptionCode(binder::Status::EX_SECURITY, String8(err.c_str()));
    }
}
binder::Status checkUid(uid_t expectedUid) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid == expectedUid || uid == AID_ROOT) {
        return binder::Status::ok();
    } else {
        auto err = StringPrintf("UID %d is not expected UID %d", uid, expectedUid);
        return binder::Status::fromExceptionCode(binder::Status::EX_SECURITY, String8(err.c_str()));
    }
}
#define ENFORCE_UID(uid) { \
    binder::Status status = checkUid((uid)); \
    if (!status.isOk()) { \
        return status; \
    } \
}
}
status_t InstalldNativeService::start() {
    IPCThreadState::self()->disableBackgroundScheduling(true);
    status_t ret = BinderService<InstalldNativeService>::publish();
    if (ret != android::OK) {
        return ret;
    }
    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
    ps->giveThreadPoolName();
    return android::OK;
}
status_t InstalldNativeService::dump(int fd, const Vector<String16> & ) {
    const binder::Status dump_permission = checkPermission(kDump);
    if (!dump_permission.isOk()) {
        const String8 msg(dump_permission.toString8());
        write(fd, msg.string(), msg.size());
        return PERMISSION_DENIED;
    }
    std::string msg = "installd is happy\n";
    write(fd, msg.c_str(), strlen(msg.c_str()));
    return NO_ERROR;
}
static bool property_get_bool(const char* property_name, bool default_value = false) {
    char tmp_property_value[kPropertyValueMax];
    bool have_property = get_property(property_name, tmp_property_value, nullptr) > 0;
    if (!have_property) {
        return default_value;
    }
    return strcmp(tmp_property_value, "true") == 0;
}
constexpr const char* PRIMARY_PROFILE_NAME = "primary.prof";
static std::string create_primary_profile(const std::string& profile_dir) {
    return StringPrintf("%s/%s", profile_dir.c_str(), PRIMARY_PROFILE_NAME);
}
static int restorecon_app_data_lazy(const std::string& path, const std::string& seInfo, uid_t uid) {
    int res = 0;
    char* before = nullptr;
    char* after = nullptr;
    if (lgetfilecon(path.c_str(), &before) < 0) {
        PLOG(ERROR) << "Failed before getfilecon for " << path;
        goto fail;
    }
    if (selinux_android_restorecon_pkgdir(path.c_str(), seInfo.c_str(), uid, 0) < 0) {
        PLOG(ERROR) << "Failed top-level restorecon for " << path;
        goto fail;
    }
    if (lgetfilecon(path.c_str(), &after) < 0) {
        PLOG(ERROR) << "Failed after getfilecon for " << path;
        goto fail;
    }
    if (strcmp(before, after)) {
        LOG(DEBUG) << "Detected label change from " << before << " to " << after << " at " << path
                << "; running recursive restorecon";
        if (selinux_android_restorecon_pkgdir(path.c_str(), seInfo.c_str(), uid,
                SELINUX_ANDROID_RESTORECON_RECURSE) < 0) {
            PLOG(ERROR) << "Failed recursive restorecon for " << path;
            goto fail;
        }
    }
    goto done;
fail:
    res = -1;
done:
    free(before);
    free(after);
    return res;
}
static int restorecon_app_data_lazy(const std::string& parent, const char* name,
        const std::string& seInfo, uid_t uid) {
    return restorecon_app_data_lazy(StringPrintf("%s/%s", parent.c_str(), name), seInfo, uid);
}
static int prepare_app_dir(const std::string& path, mode_t target_mode, uid_t uid) {
    if (fs_prepare_dir_strict(path.c_str(), target_mode, uid, uid) != 0) {
        PLOG(ERROR) << "Failed to prepare " << path;
        return -1;
    }
    return 0;
}
static int prepare_app_dir(const std::string& parent, const char* name, mode_t target_mode,
        uid_t uid) {
    return prepare_app_dir(StringPrintf("%s/%s", parent.c_str(), name), target_mode, uid);
}
binder::Status InstalldNativeService::createAppData(const std::unique_ptr<std::string>& uuid,
        const std::string& packageName, int32_t userId, int32_t flags, int32_t appId,
        const std::string& seInfo, int32_t targetSdkVersion) {
    ENFORCE_UID(AID_SYSTEM);
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();
    uid_t uid = multiuser_get_uid(userId, appId);
    mode_t target_mode = targetSdkVersion >= MIN_RESTRICTED_HOME_SDK_VERSION ? 0700 : 0751;
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid_, userId, pkgname);
        if (prepare_app_dir(path, target_mode, uid) ||
                prepare_app_dir(path, "cache", 0771, uid) ||
                prepare_app_dir(path, "code_cache", 0771, uid)) {
            return binder::Status::fromServiceSpecificError(-1);
        }
        if (restorecon_app_data_lazy(path, seInfo, uid) ||
                restorecon_app_data_lazy(path, "cache", seInfo, uid) ||
                restorecon_app_data_lazy(path, "code_cache", seInfo, uid)) {
            return binder::Status::fromServiceSpecificError(-1);
        }
        if (write_path_inode(path, "cache", kXattrInodeCache) ||
                write_path_inode(path, "code_cache", kXattrInodeCodeCache)) {
            return binder::Status::fromServiceSpecificError(-1);
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        auto path = create_data_user_de_package_path(uuid_, userId, pkgname);
        if (prepare_app_dir(path, target_mode, uid)) {
            return binder::Status::ok();
        }
        if (restorecon_app_data_lazy(path, seInfo, uid)) {
            return binder::Status::fromServiceSpecificError(-1);
        }
        if (property_get_bool("dalvik.vm.usejitprofiles")) {
            const std::string profile_path = create_data_user_profile_package_path(userId, pkgname);
            if (fs_prepare_dir_strict(profile_path.c_str(), 0700, uid, uid) != 0) {
                PLOG(ERROR) << "Failed to prepare " << profile_path;
                return binder::Status::fromServiceSpecificError(-1);
            }
            std::string profile_file = create_primary_profile(profile_path);
            if (fs_prepare_file_strict(profile_file.c_str(), 0600, uid, uid) != 0) {
                PLOG(ERROR) << "Failed to prepare " << profile_path;
                return binder::Status::fromServiceSpecificError(-1);
            }
            const std::string ref_profile_path = create_data_ref_profile_package_path(pkgname);
            appid_t shared_app_gid = multiuser_get_shared_app_gid(uid);
            if (fs_prepare_dir_strict(
                    ref_profile_path.c_str(), 0700, shared_app_gid, shared_app_gid) != 0) {
                PLOG(ERROR) << "Failed to prepare " << ref_profile_path;
                return binder::Status::fromServiceSpecificError(-1);
            }
        }
    }
    return binder::Status::ok();
}
int migrate_app_data(const char *uuid, const char *pkgname, userid_t userid, int flags) {
    auto ce_path = create_data_user_ce_package_path(uuid, userid, pkgname);
    auto de_path = create_data_user_de_package_path(uuid, userid, pkgname);
    if (getxattr(ce_path.c_str(), kXattrDefault, nullptr, 0) == -1
            && getxattr(de_path.c_str(), kXattrDefault, nullptr, 0) == -1) {
        if (setxattr(ce_path.c_str(), kXattrDefault, nullptr, 0, 0) != 0) {
            PLOG(ERROR) << "Failed to mark default storage " << ce_path;
            return -1;
        }
    }
    auto target = (flags & FLAG_STORAGE_DE) ? de_path : ce_path;
    auto source = (flags & FLAG_STORAGE_DE) ? ce_path : de_path;
    if (getxattr(target.c_str(), kXattrDefault, nullptr, 0) == -1) {
        LOG(WARNING) << "Requested default storage " << target
                << " is not active; migrating from " << source;
        if (delete_dir_contents_and_dir(target) != 0) {
            PLOG(ERROR) << "Failed to delete";
            return -1;
        }
        if (rename(source.c_str(), target.c_str()) != 0) {
            PLOG(ERROR) << "Failed to rename";
            return -1;
        }
    }
    return 0;
}
static bool clear_profile(const std::string& profile) {
    base::unique_fd ufd(open(profile.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
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
static bool clear_reference_profile(const char* pkgname) {
    std::string reference_profile_dir = create_data_ref_profile_package_path(pkgname);
    std::string reference_profile = create_primary_profile(reference_profile_dir);
    return clear_profile(reference_profile);
}
static bool clear_current_profile(const char* pkgname, userid_t user) {
    std::string profile_dir = create_data_user_profile_package_path(user, pkgname);
    std::string profile = create_primary_profile(profile_dir);
    return clear_profile(profile);
}
static bool clear_current_profiles(const char* pkgname) {
    bool success = true;
    std::vector<userid_t> users = get_known_users( nullptr);
    for (auto user : users) {
        success &= clear_current_profile(pkgname, user);
    }
    return success;
}
int clear_app_profiles(const char* pkgname) {
    bool success = true;
    success &= clear_reference_profile(pkgname);
    success &= clear_current_profiles(pkgname);
    return success ? 0 : -1;
}
int clear_app_data(const char *uuid, const char *pkgname, userid_t userid, int flags,
        ino_t ce_data_inode) {
    int res = 0;
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid, userid, pkgname, ce_data_inode);
        if (flags & FLAG_CLEAR_CACHE_ONLY) {
            path = read_path_inode(path, "cache", kXattrInodeCache);
        } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
            path = read_path_inode(path, "code_cache", kXattrInodeCodeCache);
        }
        if (access(path.c_str(), F_OK) == 0) {
            res |= delete_dir_contents(path);
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        std::string suffix = "";
        bool only_cache = false;
        if (flags & FLAG_CLEAR_CACHE_ONLY) {
            suffix = CACHE_DIR_POSTFIX;
            only_cache = true;
        } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
            suffix = CODE_CACHE_DIR_POSTFIX;
            only_cache = true;
        }
        auto path = create_data_user_de_package_path(uuid, userid, pkgname) + suffix;
        if (access(path.c_str(), F_OK) == 0) {
            delete_dir_contents(path);
        }
        if (!only_cache) {
            if (!clear_current_profile(pkgname, userid)) {
                res |= -1;
            }
        }
    }
    return res;
}
static int destroy_app_reference_profile(const char *pkgname) {
    return delete_dir_contents_and_dir(
        create_data_ref_profile_package_path(pkgname),
                              true);
}
static int destroy_app_current_profiles(const char *pkgname, userid_t userid) {
    return delete_dir_contents_and_dir(
        create_data_user_profile_package_path(userid, pkgname),
                              true);
}
int destroy_app_profiles(const char *pkgname) {
    int result = 0;
    std::vector<userid_t> users = get_known_users( nullptr);
    for (auto user : users) {
        result |= destroy_app_current_profiles(pkgname, user);
    }
    result |= destroy_app_reference_profile(pkgname);
    return result;
}
int destroy_app_data(const char *uuid, const char *pkgname, userid_t userid, int flags,
        ino_t ce_data_inode) {
    int res = 0;
    if (flags & FLAG_STORAGE_CE) {
        res |= delete_dir_contents_and_dir(
                create_data_user_ce_package_path(uuid, userid, pkgname, ce_data_inode));
    }
    if (flags & FLAG_STORAGE_DE) {
        res |= delete_dir_contents_and_dir(
                create_data_user_de_package_path(uuid, userid, pkgname));
        destroy_app_current_profiles(pkgname, userid);
        destroy_app_reference_profile(pkgname);
    }
    return res;
}
binder::Status InstalldNativeService::moveCompleteApp(const std::unique_ptr<std::string>& fromUuid,
        const std::unique_ptr<std::string>& toUuid, const std::string& packageName,
        const std::string& dataAppName, int32_t appId, const std::string& seInfo,
        int32_t targetSdkVersion) {
    const char* from_uuid = fromUuid ? fromUuid->c_str() : nullptr;
    const char* to_uuid = toUuid ? toUuid->c_str() : nullptr;
    const char* package_name = packageName.c_str();
    const char* data_app_name = dataAppName.c_str();
    const char* seinfo = seInfo.c_str();
    std::vector<userid_t> users = get_known_users(from_uuid);
    {
        auto from = create_data_app_package_path(from_uuid, data_app_name);
        auto to = create_data_app_package_path(to_uuid, data_app_name);
        auto to_parent = create_data_app_path(to_uuid);
        char *argv[] = {
            (char*) kCpPath,
            (char*) "-F",
            (char*) "-p",
            (char*) "-R",
            (char*) "-P",
            (char*) "-d",
            (char*) from.c_str(),
            (char*) to_parent.c_str()
        };
        LOG(DEBUG) << "Copying " << from << " to " << to;
        int rc = android_fork_execvp(ARRAY_SIZE(argv), argv, NULL, false, true);
        if (rc != 0) {
            LOG(ERROR) << "Failed copying " << from << " to " << to
                    << ": status " << rc;
            goto fail;
        }
        if (selinux_android_restorecon(to.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            LOG(ERROR) << "Failed to restorecon " << to;
            goto fail;
        }
    }
    for (auto user : users) {
        auto from_ce = create_data_user_ce_package_path(from_uuid, user, package_name);
        if (access(from_ce.c_str(), F_OK) != 0) {
            LOG(INFO) << "Missing source " << from_ce;
            continue;
        }
        if (!createAppData(toUuid, packageName, user, FLAG_STORAGE_CE | FLAG_STORAGE_DE, appId,
                seInfo, targetSdkVersion).isOk()) {
            LOG(ERROR) << "Failed to create package target on " << to_uuid;
            goto fail;
        }
        char *argv[] = {
            (char*) kCpPath,
            (char*) "-F",
            (char*) "-p",
            (char*) "-R",
            (char*) "-P",
            (char*) "-d",
            nullptr,
            nullptr
        };
        {
            auto from = create_data_user_de_package_path(from_uuid, user, package_name);
            auto to = create_data_user_de_path(to_uuid, user);
            argv[6] = (char*) from.c_str();
            argv[7] = (char*) to.c_str();
            LOG(DEBUG) << "Copying " << from << " to " << to;
            int rc = android_fork_execvp(ARRAY_SIZE(argv), argv, NULL, false, true);
            if (rc != 0) {
                LOG(ERROR) << "Failed copying " << from << " to " << to << " with status " << rc;
                goto fail;
            }
        }
        {
            auto from = create_data_user_ce_package_path(from_uuid, user, package_name);
            auto to = create_data_user_ce_path(to_uuid, user);
            argv[6] = (char*) from.c_str();
            argv[7] = (char*) to.c_str();
            LOG(DEBUG) << "Copying " << from << " to " << to;
            int rc = android_fork_execvp(ARRAY_SIZE(argv), argv, NULL, false, true);
            if (rc != 0) {
                LOG(ERROR) << "Failed copying " << from << " to " << to << " with status " << rc;
                goto fail;
            }
        }
        if (restorecon_app_data(to_uuid, package_name, user, FLAG_STORAGE_CE | FLAG_STORAGE_DE,
                appId, seinfo) != 0) {
            LOG(ERROR) << "Failed to restorecon";
            goto fail;
        }
    }
    return binder::Status::ok();
fail:
    {
        auto to = create_data_app_package_path(to_uuid, data_app_name);
        if (delete_dir_contents(to.c_str(), 1, NULL) != 0) {
            LOG(WARNING) << "Failed to rollback " << to;
        }
    }
    for (auto user : users) {
        {
            auto to = create_data_user_de_package_path(to_uuid, user, package_name);
            if (delete_dir_contents(to.c_str(), 1, NULL) != 0) {
                LOG(WARNING) << "Failed to rollback " << to;
            }
        }
        {
            auto to = create_data_user_ce_package_path(to_uuid, user, package_name);
            if (delete_dir_contents(to.c_str(), 1, NULL) != 0) {
                LOG(WARNING) << "Failed to rollback " << to;
            }
        }
    }
    return binder::Status::fromServiceSpecificError(-1);
}
int create_user_data(const char *uuid, userid_t userid, int user_serial ATTRIBUTE_UNUSED,
        int flags) {
    if (flags & FLAG_STORAGE_DE) {
        if (uuid == nullptr) {
            return ensure_config_user_dirs(userid);
        }
    }
    return 0;
}
int destroy_user_data(const char *uuid, userid_t userid, int flags) {
    int res = 0;
    if (flags & FLAG_STORAGE_DE) {
        res |= delete_dir_contents_and_dir(create_data_user_de_path(uuid, userid), true);
        if (uuid == nullptr) {
            res |= delete_dir_contents_and_dir(create_data_misc_legacy_path(userid), true);
            res |= delete_dir_contents_and_dir(create_data_user_profiles_path(userid), true);
        }
    }
    if (flags & FLAG_STORAGE_CE) {
        res |= delete_dir_contents_and_dir(create_data_user_ce_path(uuid, userid), true);
        res |= delete_dir_contents_and_dir(create_data_media_path(uuid, userid), true);
    }
    return res;
}
int free_cache(const char *uuid, int64_t free_size) {
    cache_t* cache;
    int64_t avail;
    auto data_path = create_data_path(uuid);
    avail = data_disk_free(data_path);
    if (avail < 0) return -1;
    ALOGI("free_cache(%" PRId64 ") avail %" PRId64 "\n", free_size, avail);
    if (avail >= free_size) return 0;
    cache = start_cache_collection();
    auto users = get_known_users(uuid);
    for (auto user : users) {
        add_cache_files(cache, create_data_user_ce_path(uuid, user));
        add_cache_files(cache, create_data_user_de_path(uuid, user));
        add_cache_files(cache,
                StringPrintf("%s/Android/data", create_data_media_path(uuid, user).c_str()));
    }
    clear_cache_files(data_path, cache, free_size);
    finish_cache_collection(cache);
    return data_disk_free(data_path) >= free_size ? 0 : -1;
}
int rm_dex(const char *path, const char *instruction_set)
{
    char dex_path[PKG_PATH_MAX];
    if (validate_apk_path(path) && validate_system_app_path(path)) {
        ALOGE("invalid apk path '%s' (bad prefix)\n", path);
        return -1;
    }
    if (!create_cache_path(dex_path, path, instruction_set)) return -1;
    ALOGV("unlink %s\n", dex_path);
    if (unlink(dex_path) < 0) {
        if (errno != ENOENT) {
            ALOGE("Couldn't unlink %s: %s\n", dex_path, strerror(errno));
        }
        return -1;
    } else {
        return 0;
    }
}
static void add_app_data_size(std::string& path, int64_t *codesize, int64_t *datasize,
        int64_t *cachesize) {
    DIR *d;
    int dfd;
    struct dirent *de;
    struct stat s;
    d = opendir(path.c_str());
    if (d == nullptr) {
        PLOG(WARNING) << "Failed to open " << path;
        return;
    }
    dfd = dirfd(d);
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        int64_t statsize = 0;
        if (fstatat(dfd, name, &s, AT_SYMLINK_NOFOLLOW) == 0) {
            statsize = stat_size(&s);
        }
        if (de->d_type == DT_DIR) {
            int subfd;
            int64_t dirsize = 0;
            if (name[0] == '.') {
                if (name[1] == 0) continue;
                if ((name[1] == '.') && (name[2] == 0)) continue;
            }
            subfd = openat(dfd, name, O_RDONLY | O_DIRECTORY);
            if (subfd >= 0) {
                dirsize = calculate_dir_size(subfd);
                close(subfd);
            }
            if (!strcmp(name, "cache") || !strcmp(name, "code_cache")) {
                *datasize += statsize;
                *cachesize += dirsize;
            } else {
                *datasize += dirsize + statsize;
            }
        } else if (de->d_type == DT_LNK && !strcmp(name, "lib")) {
            *codesize += statsize;
        } else {
            *datasize += statsize;
        }
    }
    closedir(d);
}
int get_app_size(const char *uuid, const char *pkgname, int userid, int flags, ino_t ce_data_inode,
        const char *code_path, int64_t *codesize, int64_t *datasize, int64_t *cachesize,
        int64_t* asecsize) {
    DIR *d;
    int dfd;
    d = opendir(code_path);
    if (d != nullptr) {
        dfd = dirfd(d);
        *codesize += calculate_dir_size(dfd);
        closedir(d);
    }
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid, userid, pkgname, ce_data_inode);
        add_app_data_size(path, codesize, datasize, cachesize);
    }
    if (flags & FLAG_STORAGE_DE) {
        auto path = create_data_user_de_package_path(uuid, userid, pkgname);
        add_app_data_size(path, codesize, datasize, cachesize);
    }
    *asecsize = 0;
    return 0;
}
int get_app_data_inode(const char *uuid, const char *pkgname, int userid, int flags, ino_t *inode) {
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid, userid, pkgname);
        return get_path_inode(path, inode);
    }
    return -1;
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
static void run_patchoat(int input_oat_fd, int input_vdex_fd, int out_oat_fd, int out_vdex_fd,
    const char* input_oat_file_name, const char* input_vdex_file_name,
    const char* output_oat_file_name, const char* output_vdex_file_name,
    const char *pkgname ATTRIBUTE_UNUSED, const char *instruction_set)
{
    static const int MAX_INT_LEN = 12;
    static const unsigned int MAX_INSTRUCTION_SET_LEN = 7;
    static const char* PATCHOAT_BIN = "/system/bin/patchoat";
    if (strlen(instruction_set) >= MAX_INSTRUCTION_SET_LEN) {
        ALOGE("Instruction set %s longer than max length of %d",
              instruction_set, MAX_INSTRUCTION_SET_LEN);
        return;
    }
    char instruction_set_arg[strlen("--instruction-set=") + MAX_INSTRUCTION_SET_LEN];
    char input_oat_fd_arg[strlen("--input-oat-fd=") + MAX_INT_LEN];
    char input_vdex_fd_arg[strlen("--input-vdex-fd=") + MAX_INT_LEN];
    char output_oat_fd_arg[strlen("--output-oat-fd=") + MAX_INT_LEN];
    char output_vdex_fd_arg[strlen("--output-vdex-fd=") + MAX_INT_LEN];
    const char* patched_image_location_arg = "--patched-image-location=/system/framework/boot.art";
    const char* no_lock_arg = "--no-lock-output";
    sprintf(instruction_set_arg, "--instruction-set=%s", instruction_set);
    sprintf(output_oat_fd_arg, "--output-oat-fd=%d", out_oat_fd);
    sprintf(input_oat_fd_arg, "--input-oat-fd=%d", input_oat_fd);
    ALOGV("Running %s isa=%s in-oat-fd=%d (%s) in-vdex-fd=%d (%s) "
          "out-oat-fd=%d (%s) out-vdex-fd=%d (%s)\n",
          PATCHOAT_BIN, instruction_set,
          input_oat_fd, input_oat_file_name,
          input_vdex_fd, input_vdex_file_name,
          out_oat_fd, output_oat_file_name,
          out_vdex_fd, output_vdex_file_name);
    char* argv[9];
    argv[0] = (char*) PATCHOAT_BIN;
    argv[1] = (char*) patched_image_location_arg;
    argv[2] = (char*) no_lock_arg;
    argv[3] = instruction_set_arg;
    argv[4] = input_oat_fd_arg;
    argv[5] = input_vdex_fd_arg;
    argv[6] = output_oat_fd_arg;
    argv[7] = output_vdex_fd_arg;
    argv[8] = NULL;
    execv(PATCHOAT_BIN, (char* const *)argv);
    ALOGE("execv(%s) failed: %s\n", PATCHOAT_BIN, strerror(errno));
}
static void run_dex2oat(int zip_fd, int oat_fd, int input_vdex_fd, int output_vdex_fd, int image_fd,
        const char* input_file_name, const char* output_file_name, int swap_fd,
        const char *instruction_set, const char* compiler_filter, bool vm_safe_mode,
        bool debuggable, bool post_bootcomplete, int profile_fd, const char* shared_libraries) {
    static const unsigned int MAX_INSTRUCTION_SET_LEN = 7;
    if (strlen(instruction_set) >= MAX_INSTRUCTION_SET_LEN) {
        ALOGE("Instruction set %s longer than max length of %d",
              instruction_set, MAX_INSTRUCTION_SET_LEN);
        return;
    }
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
    bool generate_debug_info = property_get_bool("debug.generate-debug-info");
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
    static const char* DEX2OAT_BIN = "/system/bin/dex2oat";
    static const char* RUNTIME_ARG = "--runtime-arg";
    static const int MAX_INT_LEN = 12;
    char zip_fd_arg[strlen("--zip-fd=") + MAX_INT_LEN];
    char zip_location_arg[strlen("--zip-location=") + PKG_PATH_MAX];
    char input_vdex_fd_arg[strlen("--input-vdex-fd=") + MAX_INT_LEN];
    char output_vdex_fd_arg[strlen("--output-vdex-fd=") + MAX_INT_LEN];
    char oat_fd_arg[strlen("--oat-fd=") + MAX_INT_LEN];
    char oat_location_arg[strlen("--oat-location=") + PKG_PATH_MAX];
    char instruction_set_arg[strlen("--instruction-set=") + MAX_INSTRUCTION_SET_LEN];
    char instruction_set_variant_arg[strlen("--instruction-set-variant=") + kPropertyValueMax];
    char instruction_set_features_arg[strlen("--instruction-set-features=") + kPropertyValueMax];
    char dex2oat_Xms_arg[strlen("-Xms") + kPropertyValueMax];
    char dex2oat_Xmx_arg[strlen("-Xmx") + kPropertyValueMax];
    char dex2oat_compiler_filter_arg[strlen("--compiler-filter=") + kPropertyValueMax];
    bool have_dex2oat_swap_fd = false;
    char dex2oat_swap_fd[strlen("--swap-fd=") + MAX_INT_LEN];
    bool have_dex2oat_image_fd = false;
    char dex2oat_image_fd[strlen("--app-image-fd=") + MAX_INT_LEN];
    sprintf(zip_fd_arg, "--zip-fd=%d", zip_fd);
    sprintf(zip_location_arg, "--zip-location=%s", input_file_name);
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
    bool have_dex2oat_compiler_filter_flag;
    if (skip_compilation) {
        strcpy(dex2oat_compiler_filter_arg, "--compiler-filter=verify-none");
        have_dex2oat_compiler_filter_flag = true;
        have_dex2oat_relocation_skip_flag = true;
    } else if (vm_safe_mode) {
        strcpy(dex2oat_compiler_filter_arg, "--compiler-filter=interpret-only");
        have_dex2oat_compiler_filter_flag = true;
    } else if (compiler_filter != nullptr &&
            strlen(compiler_filter) + strlen("--compiler-filter=") <
                    arraysize(dex2oat_compiler_filter_arg)) {
        sprintf(dex2oat_compiler_filter_arg, "--compiler-filter=%s", compiler_filter);
        have_dex2oat_compiler_filter_flag = true;
    } else {
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
    ALOGV("Running %s in=%s out=%s\n", DEX2OAT_BIN, input_file_name, output_file_name);
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
                     + (shared_libraries != nullptr ? 4 : 0)
                     + (have_dex2oat_large_app_threshold ? 1 : 0)];
    int i = 0;
    argv[i++] = DEX2OAT_BIN;
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
    if (shared_libraries != nullptr) {
        argv[i++] = RUNTIME_ARG;
        argv[i++] = "-classpath";
        argv[i++] = RUNTIME_ARG;
        argv[i++] = shared_libraries;
    }
    argv[i] = NULL;
    execv(DEX2OAT_BIN, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", DEX2OAT_BIN, strerror(errno));
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
    bool is_low_mem = property_get_bool("ro.config.low_ram");
    if (is_low_mem) {
        return true;
    }
    return kDefaultProvideSwapFile;
}
static void SetDex2OatAndPatchOatScheduling(bool set_to_bg) {
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
static void close_all_fds(const std::vector<fd_t>& fds, const char* description) {
    for (size_t i = 0; i < fds.size(); i++) {
        if (close(fds[i]) != 0) {
            PLOG(WARNING) << "Failed to close fd for " << description << " at index " << i;
        }
    }
}
static fd_t open_profile_dir(const std::string& profile_dir) {
    fd_t profile_dir_fd = TEMP_FAILURE_RETRY(open(profile_dir.c_str(),
            O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (profile_dir_fd < 0) {
        if (errno != ENOENT) {
            PLOG(ERROR) << "Failed to open profile_dir: " << profile_dir;
        }
    }
    return profile_dir_fd;
}
static fd_t open_primary_profile_file_from_dir(const std::string& profile_dir, mode_t open_mode) {
    fd_t profile_dir_fd = open_profile_dir(profile_dir);
    if (profile_dir_fd < 0) {
        return -1;
    }
    fd_t profile_fd = -1;
    std::string profile_file = create_primary_profile(profile_dir);
    profile_fd = TEMP_FAILURE_RETRY(open(profile_file.c_str(), open_mode | O_NOFOLLOW));
    if (profile_fd == -1) {
        if (errno != ENOENT) {
            PLOG(ERROR) << "Failed to lstat profile_dir: " << profile_dir;
        }
    }
    if (close(profile_dir_fd) != 0) {
        PLOG(WARNING) << "Could not close profile dir " << profile_dir;
    }
    return profile_fd;
}
static fd_t open_primary_profile_file(userid_t user, const char* pkgname) {
    std::string profile_dir = create_data_user_profile_package_path(user, pkgname);
    return open_primary_profile_file_from_dir(profile_dir, O_RDONLY);
}
static fd_t open_reference_profile(uid_t uid, const char* pkgname, bool read_write) {
    std::string reference_profile_dir = create_data_ref_profile_package_path(pkgname);
    int flags = read_write ? O_RDWR | O_CREAT : O_RDONLY;
    fd_t fd = open_primary_profile_file_from_dir(reference_profile_dir, flags);
    if (fd < 0) {
        return -1;
    }
    if (read_write) {
        if (fchown(fd, uid, uid) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}
static void open_profile_files(uid_t uid, const char* pkgname,
                    std::vector<fd_t>* profiles_fd, fd_t* reference_profile_fd) {
    *reference_profile_fd = open_reference_profile(uid, pkgname, true);
    if (*reference_profile_fd < 0) {
        return;
    }
    std::vector<userid_t> users = get_known_users( nullptr);
    for (auto user : users) {
        fd_t profile_fd = open_primary_profile_file(user, pkgname);
        if (profile_fd >= 0) {
            profiles_fd->push_back(profile_fd);
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
static void run_profman_merge(const std::vector<fd_t>& profiles_fd, fd_t reference_profile_fd) {
    static const size_t MAX_INT_LEN = 32;
    static const char* PROFMAN_BIN = "/system/bin/profman";
    std::vector<std::string> profile_args(profiles_fd.size());
    char profile_buf[strlen("--profile-file-fd=") + MAX_INT_LEN];
    for (size_t k = 0; k < profiles_fd.size(); k++) {
        sprintf(profile_buf, "--profile-file-fd=%d", profiles_fd[k]);
        profile_args[k].assign(profile_buf);
    }
    char reference_profile_arg[strlen("--reference-profile-file-fd=") + MAX_INT_LEN];
    sprintf(reference_profile_arg, "--reference-profile-file-fd=%d", reference_profile_fd);
    const char* argv[3 + profiles_fd.size()];
    int i = 0;
    argv[i++] = PROFMAN_BIN;
    argv[i++] = reference_profile_arg;
    for (size_t k = 0; k < profile_args.size(); k++) {
        argv[i++] = profile_args[k].c_str();
    }
    argv[i] = NULL;
    execv(PROFMAN_BIN, (char * const *)argv);
    ALOGE("execv(%s) failed: %s\n", PROFMAN_BIN, strerror(errno));
    exit(68);
}
static bool analyse_profiles(uid_t uid, const char* pkgname) {
    std::vector<fd_t> profiles_fd;
    fd_t reference_profile_fd = -1;
    open_profile_files(uid, pkgname, &profiles_fd, &reference_profile_fd);
    if (profiles_fd.empty() || (reference_profile_fd == -1)) {
        close_all_fds(profiles_fd, "profiles_fd");
        if ((reference_profile_fd != - 1) && (close(reference_profile_fd) != 0)) {
            PLOG(WARNING) << "Failed to close fd for reference profile";
        }
        return false;
    }
    ALOGV("PROFMAN (MERGE): --- BEGIN '%s' ---\n", pkgname);
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
        LOG(WARNING) << "profman failed for package " << pkgname << ": " << return_code;
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
                LOG(WARNING) << "Bad profiles for package " << pkgname;
                need_to_compile = false;
                should_clear_current_profiles = true;
                should_clear_reference_profile = true;
                break;
            case PROFMAN_BIN_RETURN_CODE_ERROR_IO:
            case PROFMAN_BIN_RETURN_CODE_ERROR_LOCKING:
                LOG(WARNING) << "IO error while reading profiles for package " << pkgname;
                need_to_compile = false;
                should_clear_current_profiles = false;
                should_clear_reference_profile = false;
                break;
           default:
                LOG(WARNING) << "Unknown error code while processing profiles for package " << pkgname
                        << ": " << return_code;
                need_to_compile = false;
                should_clear_current_profiles = true;
                should_clear_reference_profile = true;
                break;
        }
    }
    close_all_fds(profiles_fd, "profiles_fd");
    if (close(reference_profile_fd) != 0) {
        PLOG(WARNING) << "Failed to close fd for reference profile";
    }
    if (should_clear_current_profiles) {
        clear_current_profiles(pkgname);
    }
    if (should_clear_reference_profile) {
        clear_reference_profile(pkgname);
    }
    return need_to_compile;
}
static void run_profman_dump(const std::vector<fd_t>& profile_fds,
                             fd_t reference_profile_fd,
                             const std::vector<std::string>& dex_locations,
                             const std::vector<fd_t>& apk_fds,
                             fd_t output_fd) {
    std::vector<std::string> profman_args;
    static const char* PROFMAN_BIN = "/system/bin/profman";
    profman_args.push_back(PROFMAN_BIN);
    profman_args.push_back("--dump-only");
    profman_args.push_back(StringPrintf("--dump-output-to-fd=%d", output_fd));
    if (reference_profile_fd != -1) {
        profman_args.push_back(StringPrintf("--reference-profile-file-fd=%d",
                                            reference_profile_fd));
    }
    for (fd_t profile_fd : profile_fds) {
        profman_args.push_back(StringPrintf("--profile-file-fd=%d", profile_fd));
    }
    for (const std::string& dex_location : dex_locations) {
        profman_args.push_back(StringPrintf("--dex-location=%s", dex_location.c_str()));
    }
    for (fd_t apk_fd : apk_fds) {
        profman_args.push_back(StringPrintf("--apk-fd=%d", apk_fd));
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
static const char* get_location_from_path(const char* path) {
    static constexpr char kLocationSeparator = '/';
    const char *location = strrchr(path, kLocationSeparator);
    if (location == NULL) {
        return path;
    } else {
        return location + 1;
    }
}
bool dump_profile(uid_t uid, const char* pkgname, const char* code_path_string) {
    std::vector<fd_t> profile_fds;
    fd_t reference_profile_fd = -1;
    std::string out_file_name = StringPrintf("/data/misc/profman/%s.txt", pkgname);
    ALOGV("PROFMAN (DUMP): --- BEGIN '%s' ---\n", pkgname);
    open_profile_files(uid, pkgname, &profile_fds, &reference_profile_fd);
    const bool has_reference_profile = (reference_profile_fd != -1);
    const bool has_profiles = !profile_fds.empty();
    if (!has_reference_profile && !has_profiles) {
        ALOGE("profman dump: no profiles to dump for '%s'", pkgname);
        return false;
    }
    fd_t output_fd = open(out_file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW);
    if (fchmod(output_fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH) < 0) {
        ALOGE("installd cannot chmod '%s' dump_profile\n", out_file_name.c_str());
        return false;
    }
    std::vector<std::string> code_full_paths = base::Split(code_path_string, ";");
    std::vector<std::string> dex_locations;
    std::vector<fd_t> apk_fds;
    for (const std::string& code_full_path : code_full_paths) {
        const char* full_path = code_full_path.c_str();
        fd_t apk_fd = open(full_path, O_RDONLY | O_NOFOLLOW);
        if (apk_fd == -1) {
            ALOGE("installd cannot open '%s'\n", full_path);
            return false;
        }
        dex_locations.push_back(get_location_from_path(full_path));
        apk_fds.push_back(apk_fd);
    }
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        run_profman_dump(profile_fds, reference_profile_fd, dex_locations,
                         apk_fds, output_fd);
        exit(68);
    }
    close_all_fds(apk_fds, "apk_fds");
    close_all_fds(profile_fds, "profile_fds");
    if (close(reference_profile_fd) != 0) {
        PLOG(WARNING) << "Failed to close fd for reference profile";
    }
    int return_code = wait_child(pid);
    if (!WIFEXITED(return_code)) {
        LOG(WARNING) << "profman failed for package " << pkgname << ": "
                << return_code;
        return false;
    }
    return true;
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
static bool set_permissions_and_ownership(int fd, bool is_public, int uid, const char* path) {
    if (fchmod(fd,
               S_IRUSR|S_IWUSR|S_IRGRP |
               (is_public ? S_IROTH : 0)) < 0) {
        ALOGE("installd cannot chmod '%s' during dexopt\n", path);
        return false;
    } else if (fchown(fd, AID_SYSTEM, uid) < 0) {
        ALOGE("installd cannot chown '%s' during dexopt\n", path);
        return false;
    }
    return true;
}
static bool IsOutputDalvikCache(const char* oat_dir) {
  return oat_dir == nullptr || oat_dir[0] == '!';
}
static bool create_oat_out_path(const char* apk_path, const char* instruction_set,
            const char* oat_dir, char* out_oat_path) {
    if (strlen(apk_path) >= (PKG_PATH_MAX - 8)) {
        ALOGE("apk_path too long '%s'\n", apk_path);
        return false;
    }
    if (!IsOutputDalvikCache(oat_dir)) {
        if (validate_apk_path(oat_dir)) {
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
bool merge_profiles(uid_t uid, const char *pkgname) {
    return analyse_profiles(uid, pkgname);
}
static const char* parse_null(const char* arg) {
    if (strcmp(arg, "!") == 0) {
        return nullptr;
    } else {
        return arg;
    }
}
int dexopt(const char* const params[DEXOPT_PARAM_COUNT]) {
    return dexopt(params[0],
                  atoi(params[1]),
                  params[2],
                  params[3],
                  atoi(params[4]),
                  params[5],
                  atoi(params[6]),
                  params[7],
                  parse_null(params[8]),
                  parse_null(params[9]));
    static_assert(DEXOPT_PARAM_COUNT == 10U, "Unexpected dexopt param count");
}
template <typename Cleanup>
class Dex2oatFileWrapper {
 public:
    Dex2oatFileWrapper() : value_(-1), cleanup_(), do_cleanup_(true) {
    }
    Dex2oatFileWrapper(int value, Cleanup cleanup)
            : value_(value), cleanup_(cleanup), do_cleanup_(true) {}
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
        if (value_ >= 0) {
            close(value_);
        }
        if (do_cleanup_ && cleanup_ != nullptr) {
            cleanup_();
        }
        value_ = new_value;
    }
    void reset(int new_value, Cleanup new_cleanup) {
        if (value_ >= 0) {
            close(value_);
        }
        if (do_cleanup_ && cleanup_ != nullptr) {
            cleanup_();
        }
        value_ = new_value;
        cleanup_ = new_cleanup;
    }
 private:
    int value_;
    Cleanup cleanup_;
    bool do_cleanup_;
};
int dexopt(const char* apk_path, uid_t uid, const char* pkgname, const char* instruction_set,
           int dexopt_needed, const char* oat_dir, int dexopt_flags, const char* compiler_filter,
           const char* volume_uuid ATTRIBUTE_UNUSED, const char* shared_libraries)
{
    bool is_public = ((dexopt_flags & DEXOPT_PUBLIC) != 0);
    bool vm_safe_mode = (dexopt_flags & DEXOPT_SAFEMODE) != 0;
    bool debuggable = (dexopt_flags & DEXOPT_DEBUGGABLE) != 0;
    bool boot_complete = (dexopt_flags & DEXOPT_BOOTCOMPLETE) != 0;
    bool profile_guided = (dexopt_flags & DEXOPT_PROFILE_GUIDED) != 0;
    CHECK(pkgname != nullptr);
    CHECK(pkgname[0] != 0);
    Dex2oatFileWrapper<std::function<void ()>> reference_profile_fd;
    if (!is_public && pkgname[0] != '*') {
        const std::string pkgname_str(pkgname);
        reference_profile_fd.reset(open_reference_profile(uid, pkgname, false),
                                   [pkgname_str]() {
                                       clear_reference_profile(pkgname_str.c_str());
                                   });
    }
    if ((dexopt_flags & ~DEXOPT_MASK) != 0) {
        LOG_FATAL("dexopt flags contains unknown fields\n");
    }
    char out_oat_path[PKG_PATH_MAX];
    if (!create_oat_out_path(apk_path, instruction_set, oat_dir, out_oat_path)) {
        return false;
    }
    const char *input_file;
    char in_odex_path[PKG_PATH_MAX];
    int dexopt_action = abs(dexopt_needed);
    bool is_odex_location = dexopt_needed < 0;
    switch (dexopt_action) {
        case DEX2OAT_FROM_SCRATCH:
        case DEX2OAT_FOR_BOOT_IMAGE:
        case DEX2OAT_FOR_FILTER:
        case DEX2OAT_FOR_RELOCATION:
            input_file = apk_path;
            break;
        case PATCHOAT_FOR_RELOCATION:
            if (is_odex_location) {
                if (!calculate_odex_file_path(in_odex_path, apk_path, instruction_set)) {
                    return -1;
                }
                input_file = in_odex_path;
            } else {
                input_file = out_oat_path;
            }
            break;
        default:
            ALOGE("Invalid dexopt needed: %d\n", dexopt_needed);
            return 72;
    }
    struct stat input_stat;
    memset(&input_stat, 0, sizeof(input_stat));
    stat(input_file, &input_stat);
    base::unique_fd input_fd(open(input_file, O_RDONLY, 0));
    if (input_fd.get() < 0) {
        ALOGE("installd cannot open '%s' for input during dexopt\n", input_file);
        return -1;
    }
    const std::string out_oat_path_str(out_oat_path);
    Dex2oatFileWrapper<std::function<void ()>> out_oat_fd(
            open_output_file(out_oat_path, true, 0644),
            [out_oat_path_str]() { unlink(out_oat_path_str.c_str()); });
    if (out_oat_fd.get() < 0) {
        ALOGE("installd cannot open '%s' for output during dexopt\n", out_oat_path);
        return -1;
    }
    if (!set_permissions_and_ownership(out_oat_fd.get(), is_public, uid, out_oat_path)) {
        return -1;
    }
    base::unique_fd in_vdex_fd;
    std::string in_vdex_path_str;
    if (dexopt_action == PATCHOAT_FOR_RELOCATION) {
        in_vdex_path_str = create_vdex_filename(input_file);
        if (in_vdex_path_str.empty()) {
            ALOGE("installd cannot compute input vdex location for '%s'\n", input_file);
            return -1;
        }
        in_vdex_fd.reset(open(in_vdex_path_str.c_str(), O_RDONLY, 0));
        if (in_vdex_fd.get() < 0) {
            ALOGE("installd cannot open '%s' for input during dexopt: %s\n",
                in_vdex_path_str.c_str(), strerror(errno));
            return -1;
        }
    } else if (dexopt_action != DEX2OAT_FROM_SCRATCH) {
        const char* path = nullptr;
        if (is_odex_location) {
            if (calculate_odex_file_path(in_odex_path, apk_path, instruction_set)) {
                path = in_odex_path;
            } else {
                ALOGE("installd cannot compute input vdex location for '%s'\n", apk_path);
                return -1;
            }
        } else {
            path = out_oat_path;
        }
        in_vdex_path_str = create_vdex_filename(path);
        if (in_vdex_path_str.empty()) {
            ALOGE("installd cannot compute input vdex location for '%s'\n", path);
            return -1;
        }
        in_vdex_fd.reset(open(in_vdex_path_str.c_str(), O_RDONLY, 0));
    }
    const std::string out_vdex_path_str = create_vdex_filename(out_oat_path_str);
    if (out_vdex_path_str.empty()) {
        return -1;
    }
    Dex2oatFileWrapper<std::function<void ()>> out_vdex_fd(
            open_output_file(out_vdex_path_str.c_str(), true, 0644),
            [out_vdex_path_str]() { unlink(out_vdex_path_str.c_str()); });
    if (out_vdex_fd.get() < 0) {
        ALOGE("installd cannot open '%s' for output during dexopt\n", out_vdex_path_str.c_str());
        return -1;
    }
    if (!set_permissions_and_ownership(out_vdex_fd.get(), is_public,
                uid, out_vdex_path_str.c_str())) {
        return -1;
    }
    base::unique_fd swap_fd;
    if (ShouldUseSwapFileForDexopt()) {
        char swap_file_name[PKG_PATH_MAX];
        strcpy(swap_file_name, out_oat_path);
        if (add_extension_to_file_name(swap_file_name, ".swap")) {
            swap_fd.reset(open_output_file(swap_file_name, true, 0600));
        }
        if (swap_fd.get() < 0) {
            ALOGE("installd could not create '%s' for swap during dexopt\n", swap_file_name);
        } else {
            if (unlink(swap_file_name) < 0) {
                PLOG(ERROR) << "Couldn't unlink swap file " << swap_file_name;
            }
        }
    }
    Dex2oatFileWrapper<std::function<void ()>> image_fd;
    const std::string image_path = create_image_filename(out_oat_path);
    if (dexopt_action != PATCHOAT_FOR_RELOCATION && !image_path.empty()) {
        char app_image_format[kPropertyValueMax];
        bool have_app_image_format =
                get_property("dalvik.vm.appimageformat", app_image_format, NULL) > 0;
        if (profile_guided && have_app_image_format) {
            image_fd.reset(open_output_file(image_path.c_str(),
                                            true ,
                                            0600 ),
                           [image_path]() { unlink(image_path.c_str()); }
                           );
            if (image_fd.get() < 0) {
                LOG(ERROR) << "installd could not create '"
                        << image_path
                        << "' for image file during dexopt";
            } else if (!set_permissions_and_ownership(image_fd.get(),
                                                      is_public,
                                                      uid,
                                                      image_path.c_str())) {
                image_fd.reset(-1);
            }
        }
        if (image_fd.get() < 0) {
            if (unlink(image_path.c_str()) < 0) {
                if (errno != ENOENT) {
                    PLOG(ERROR) << "Couldn't unlink image file " << image_path;
                }
            }
        }
    }
    ALOGV("DexInv: --- BEGIN '%s' ---\n", input_file);
    pid_t pid = fork();
    if (pid == 0) {
        drop_capabilities(uid);
        SetDex2OatAndPatchOatScheduling(boot_complete);
        if (flock(out_oat_fd.get(), LOCK_EX | LOCK_NB) != 0) {
            ALOGE("flock(%s) failed: %s\n", out_oat_path, strerror(errno));
            _exit(67);
        }
        if (dexopt_action == PATCHOAT_FOR_RELOCATION) {
            run_patchoat(input_fd.get(),
                         in_vdex_fd.get(),
                         out_oat_fd.get(),
                         out_vdex_fd.get(),
                         input_file,
                         in_vdex_path_str.c_str(),
                         out_oat_path,
                         out_vdex_path_str.c_str(),
                         pkgname,
                         instruction_set);
        } else {
            const char *input_file_name = get_location_from_path(input_file);
            run_dex2oat(input_fd.get(),
                        out_oat_fd.get(),
                        in_vdex_fd.get(),
                        out_vdex_fd.get(),
                        image_fd.get(),
                        input_file_name,
                        out_oat_path,
                        swap_fd.get(),
                        instruction_set,
                        compiler_filter,
                        vm_safe_mode,
                        debuggable,
                        boot_complete,
                        reference_profile_fd.get(),
                        shared_libraries);
        }
        _exit(68);
    } else {
        int res = wait_child(pid);
        if (res == 0) {
            ALOGV("DexInv: --- END '%s' (success) ---\n", input_file);
        } else {
            ALOGE("DexInv: --- END '%s' --- status=0x%04x, process failed\n", input_file, res);
            return -1;
        }
    }
    struct utimbuf ut;
    ut.actime = input_stat.st_atime;
    ut.modtime = input_stat.st_mtime;
    utime(out_oat_path, &ut);
    out_oat_fd.SetCleanup(false);
    out_vdex_fd.SetCleanup(false);
    image_fd.SetCleanup(false);
    reference_profile_fd.SetCleanup(false);
    return 0;
}
int mark_boot_complete(const char* instruction_set)
{
  char boot_marker_path[PKG_PATH_MAX];
  sprintf(boot_marker_path,
          "%s/%s/%s/.booting",
          android_data_dir.path,
          DALVIK_CACHE,
          instruction_set);
  ALOGV("mark_boot_complete : %s", boot_marker_path);
  if (unlink(boot_marker_path) != 0) {
      ALOGE("Unable to unlink boot marker at %s, error=%s", boot_marker_path,
            strerror(errno));
      return -1;
  }
  return 0;
}
void mkinnerdirs(char* path, int basepos, mode_t mode, int uid, int gid,
        struct stat* statbuf)
{
    while (path[basepos] != 0) {
        if (path[basepos] == '/') {
            path[basepos] = 0;
            if (lstat(path, statbuf) < 0) {
                ALOGV("Making directory: %s\n", path);
                if (mkdir(path, mode) == 0) {
                    chown(path, uid, gid);
                } else {
                    ALOGW("Unable to make directory %s: %s\n", path, strerror(errno));
                }
            }
            path[basepos] = '/';
            basepos++;
        }
        basepos++;
    }
}
int linklib(const char* uuid, const char* pkgname, const char* asecLibDir, int userId)
{
    struct stat s, libStat;
    int rc = 0;
    std::string _pkgdir(create_data_user_ce_package_path(uuid, userId, pkgname));
    std::string _libsymlink(_pkgdir + PKG_LIB_POSTFIX);
    const char* pkgdir = _pkgdir.c_str();
    const char* libsymlink = _libsymlink.c_str();
    if (stat(pkgdir, &s) < 0) return -1;
    if (chown(pkgdir, AID_INSTALL, AID_INSTALL) < 0) {
        ALOGE("failed to chown '%s': %s\n", pkgdir, strerror(errno));
        return -1;
    }
    if (chmod(pkgdir, 0700) < 0) {
        ALOGE("linklib() 1: failed to chmod '%s': %s\n", pkgdir, strerror(errno));
        rc = -1;
        goto out;
    }
    if (lstat(libsymlink, &libStat) < 0) {
        if (errno != ENOENT) {
            ALOGE("couldn't stat lib dir: %s\n", strerror(errno));
            rc = -1;
            goto out;
        }
    } else {
        if (S_ISDIR(libStat.st_mode)) {
            if (delete_dir_contents(libsymlink, 1, NULL) < 0) {
                rc = -1;
                goto out;
            }
        } else if (S_ISLNK(libStat.st_mode)) {
            if (unlink(libsymlink) < 0) {
                ALOGE("couldn't unlink lib dir: %s\n", strerror(errno));
                rc = -1;
                goto out;
            }
        }
    }
    if (symlink(asecLibDir, libsymlink) < 0) {
        ALOGE("couldn't symlink directory '%s' -> '%s': %s\n", libsymlink, asecLibDir,
                strerror(errno));
        rc = -errno;
        goto out;
    }
out:
    if (chmod(pkgdir, s.st_mode) < 0) {
        ALOGE("linklib() 2: failed to chmod '%s': %s\n", pkgdir, strerror(errno));
        rc = -errno;
    }
    if (chown(pkgdir, s.st_uid, s.st_gid) < 0) {
        ALOGE("failed to chown '%s' : %s\n", pkgdir, strerror(errno));
        return -errno;
    }
    return rc;
}
static void run_idmap(const char *target_apk, const char *overlay_apk, int idmap_fd)
{
    static const char *IDMAP_BIN = "/system/bin/idmap";
    static const size_t MAX_INT_LEN = 32;
    char idmap_str[MAX_INT_LEN];
    snprintf(idmap_str, sizeof(idmap_str), "%d", idmap_fd);
    execl(IDMAP_BIN, IDMAP_BIN, "--fd", target_apk, overlay_apk, idmap_str, (char*)NULL);
    ALOGE("execl(%s) failed: %s\n", IDMAP_BIN, strerror(errno));
}
static int flatten_path(const char *prefix, const char *suffix,
        const char *overlay_path, char *idmap_path, size_t N)
{
    if (overlay_path == NULL || idmap_path == NULL) {
        return -1;
    }
    const size_t len_overlay_path = strlen(overlay_path);
    if (len_overlay_path < 2 || *overlay_path != '/') {
        return -1;
    }
    const size_t len_idmap_root = strlen(prefix);
    const size_t len_suffix = strlen(suffix);
    if (SIZE_MAX - len_idmap_root < len_overlay_path ||
            SIZE_MAX - (len_idmap_root + len_overlay_path) < len_suffix) {
        return -1;
    }
    if (N < len_idmap_root + len_overlay_path + len_suffix) {
        return -1;
    }
    memset(idmap_path, 0, N);
    snprintf(idmap_path, N, "%s%s%s", prefix, overlay_path + 1, suffix);
    char *ch = idmap_path + len_idmap_root;
    while (*ch != '\0') {
        if (*ch == '/') {
            *ch = '@';
        }
        ++ch;
    }
    return 0;
}
int idmap(const char *target_apk, const char *overlay_apk, uid_t uid)
{
    ALOGV("idmap target_apk=%s overlay_apk=%s uid=%d\n", target_apk, overlay_apk, uid);
    int idmap_fd = -1;
    char idmap_path[PATH_MAX];
    if (flatten_path(IDMAP_PREFIX, IDMAP_SUFFIX, overlay_apk,
                idmap_path, sizeof(idmap_path)) == -1) {
        ALOGE("idmap cannot generate idmap path for overlay %s\n", overlay_apk);
        goto fail;
    }
    unlink(idmap_path);
    idmap_fd = open(idmap_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (idmap_fd < 0) {
        ALOGE("idmap cannot open '%s' for output: %s\n", idmap_path, strerror(errno));
        goto fail;
    }
    if (fchown(idmap_fd, AID_SYSTEM, uid) < 0) {
        ALOGE("idmap cannot chown '%s'\n", idmap_path);
        goto fail;
    }
    if (fchmod(idmap_fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) < 0) {
        ALOGE("idmap cannot chmod '%s'\n", idmap_path);
        goto fail;
    }
    pid_t pid;
    pid = fork();
    if (pid == 0) {
        if (setgid(uid) != 0) {
            ALOGE("setgid(%d) failed during idmap\n", uid);
            exit(1);
        }
        if (setuid(uid) != 0) {
            ALOGE("setuid(%d) failed during idmap\n", uid);
            exit(1);
        }
        if (flock(idmap_fd, LOCK_EX | LOCK_NB) != 0) {
            ALOGE("flock(%s) failed during idmap: %s\n", idmap_path, strerror(errno));
            exit(1);
        }
        run_idmap(target_apk, overlay_apk, idmap_fd);
        exit(1);
    } else {
        int status = wait_child(pid);
        if (status != 0) {
            ALOGE("idmap failed, status=0x%04x\n", status);
            goto fail;
        }
    }
    close(idmap_fd);
    return 0;
fail:
    if (idmap_fd >= 0) {
        close(idmap_fd);
        unlink(idmap_path);
    }
    return -1;
}
int restorecon_app_data(const char* uuid, const char* pkgName, userid_t userid, int flags,
        appid_t appid, const char* seinfo) {
    int res = 0;
    unsigned int seflags = SELINUX_ANDROID_RESTORECON_RECURSE;
    if (!pkgName || !seinfo) {
        ALOGE("Package name or seinfo tag is null when trying to restorecon.");
        return -1;
    }
    uid_t uid = multiuser_get_uid(userid, appid);
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid, userid, pkgName);
        if (selinux_android_restorecon_pkgdir(path.c_str(), seinfo, uid, seflags) < 0) {
            PLOG(ERROR) << "restorecon failed for " << path;
            res = -1;
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        auto path = create_data_user_de_package_path(uuid, userid, pkgName);
        if (selinux_android_restorecon_pkgdir(path.c_str(), seinfo, uid, seflags) < 0) {
            PLOG(ERROR) << "restorecon failed for " << path;
        }
    }
    return res;
}
int create_oat_dir(const char* oat_dir, const char* instruction_set)
{
    char oat_instr_dir[PKG_PATH_MAX];
    if (validate_apk_path(oat_dir)) {
        ALOGE("invalid apk path '%s' (bad prefix)\n", oat_dir);
        return -1;
    }
    if (fs_prepare_dir(oat_dir, S_IRWXU | S_IRWXG | S_IXOTH, AID_SYSTEM, AID_INSTALL)) {
        return -1;
    }
    if (selinux_android_restorecon(oat_dir, 0)) {
        ALOGE("cannot restorecon dir '%s': %s\n", oat_dir, strerror(errno));
        return -1;
    }
    snprintf(oat_instr_dir, PKG_PATH_MAX, "%s/%s", oat_dir, instruction_set);
    if (fs_prepare_dir(oat_instr_dir, S_IRWXU | S_IRWXG | S_IXOTH, AID_SYSTEM, AID_INSTALL)) {
        return -1;
    }
    return 0;
}
int rm_package_dir(const char* apk_path)
{
    if (validate_apk_path(apk_path)) {
        ALOGE("invalid apk path '%s' (bad prefix)\n", apk_path);
        return -1;
    }
    return delete_dir_contents(apk_path, 1 , NULL );
}
int link_file(const char* relative_path, const char* from_base, const char* to_base) {
    char from_path[PKG_PATH_MAX];
    char to_path[PKG_PATH_MAX];
    snprintf(from_path, PKG_PATH_MAX, "%s/%s", from_base, relative_path);
    snprintf(to_path, PKG_PATH_MAX, "%s/%s", to_base, relative_path);
    if (validate_apk_path_subdirs(from_path)) {
        ALOGE("invalid app data sub-path '%s' (bad prefix)\n", from_path);
        return -1;
    }
    if (validate_apk_path_subdirs(to_path)) {
        ALOGE("invalid app data sub-path '%s' (bad prefix)\n", to_path);
        return -1;
    }
    const int ret = link(from_path, to_path);
    if (ret < 0) {
        ALOGE("link(%s, %s) failed : %s", from_path, to_path, strerror(errno));
        return -1;
    }
    return 0;
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
int move_ab(const char* apk_path, const char* instruction_set, const char* oat_dir) {
    if (apk_path == nullptr || instruction_set == nullptr || oat_dir == nullptr) {
        LOG(ERROR) << "Cannot move_ab with null input";
        return -1;
    }
    std::string slot_suffix;
    {
        char buf[kPropertyValueMax];
        if (get_property("ro.boot.slot_suffix", buf, nullptr) <= 0) {
            return -1;
        }
        slot_suffix = buf;
        if (!ValidateTargetSlotSuffix(slot_suffix)) {
            LOG(ERROR) << "Target slot suffix not legal: " << slot_suffix;
            return -1;
        }
    }
    if (validate_apk_path(apk_path) != 0) {
        LOG(ERROR) << "invalid apk_path " << apk_path;
        return -1;
    }
    if (validate_apk_path(oat_dir) != 0) {
        LOG(ERROR) << "invalid oat_dir " << oat_dir;
        return -1;
    }
    char a_path[PKG_PATH_MAX];
    if (!calculate_oat_file_path(a_path, oat_dir, apk_path, instruction_set)) {
        return -1;
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
    return success ? 0 : -1;
}
bool delete_odex(const char *apk_path, const char *instruction_set, const char *oat_dir) {
    char out_path[PKG_PATH_MAX];
    if (!create_oat_out_path(apk_path, instruction_set, oat_dir, out_path)) {
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
    return return_value_oat && return_value_art;
}
}
}
