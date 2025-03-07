#include "devices.h"
#include <errno.h>
#include <fnmatch.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <memory>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>
#include <selinux/android.h>
#include <selinux/selinux.h>
#include "ueventd.h"
#include "util.h"
#ifdef _INIT_INIT_H
#error "Do not include init.h in files used by ueventd or watchdogd; it will expose init's globals"
#endif
static bool FindPciDevicePrefix(const std::string& path, std::string* result) {
    result->clear();
    if (!android::base::StartsWith(path, "/devices/pci")) return false;
    std::string::size_type start = 9;
    auto end = path.find('/', start);
    if (end == std::string::npos) return false;
    end = path.find('/', end + 1);
    if (end == std::string::npos) return false;
    auto length = end - start;
    if (length <= 4) {
        return false;
    }
    *result = path.substr(start, length);
    return true;
}
static bool FindVbdDevicePrefix(const std::string& path, std::string* result) {
    result->clear();
    if (!android::base::StartsWith(path, "/devices/vbd-")) return false;
    std::string::size_type start = 13;
    auto end = path.find('/', start);
    if (end == std::string::npos) return false;
    auto length = end - start;
    if (length == 0) return false;
    *result = path.substr(start, length);
    return true;
}
Permissions::Permissions(const std::string& name, mode_t perm, uid_t uid, gid_t gid)
    : name_(name), perm_(perm), uid_(uid), gid_(gid), prefix_(false), wildcard_(false) {
    auto wildcard_position = name_.find('*');
    if (wildcard_position != std::string::npos) {
        if (wildcard_position == name_.length() - 1) {
            prefix_ = true;
            name_.pop_back();
        } else {
            wildcard_ = true;
        }
    }
}
bool Permissions::Match(const std::string& path) const {
    if (prefix_) return android::base::StartsWith(path, name_.c_str());
    if (wildcard_) return fnmatch(name_.c_str(), path.c_str(), FNM_PATHNAME) == 0;
    return path == name_;
}
bool SysfsPermissions::MatchWithSubsystem(const std::string& path,
                                          const std::string& subsystem) const {
    std::string path_basename = android::base::Basename(path);
    if (name().find(subsystem) != std::string::npos) {
        if (Match("/sys/class/" + subsystem + "/" + path_basename)) return true;
        if (Match("/sys/bus/" + subsystem + "/devices/" + path_basename)) return true;
    }
    return Match(path);
}
void SysfsPermissions::SetPermissions(const std::string& path) const {
    std::string attribute_file = path + "/" + attribute_;
    LOG(VERBOSE) << "fixup " << attribute_file << " " << uid() << " " << gid() << " " << std::oct
                 << perm();
    if (access(attribute_file.c_str(), F_OK) == 0) {
        if (chown(attribute_file.c_str(), uid(), gid()) != 0) {
            PLOG(ERROR) << "chown(" << attribute_file << ", " << uid() << ", " << gid()
                        << ") failed";
        }
        if (chmod(attribute_file.c_str(), perm()) != 0) {
            PLOG(ERROR) << "chmod(" << attribute_file << ", " << perm() << ") failed";
        }
    }
}
bool DeviceHandler::FindPlatformDevice(std::string path, std::string* platform_device_path) const {
    platform_device_path->clear();
    path.insert(0, sysfs_mount_point_);
    std::string directory = android::base::Dirname(path);
    while (directory != "/" && directory != ".") {
        std::string subsystem_link_path;
        if (android::base::Realpath(directory + "/subsystem", &subsystem_link_path) &&
            subsystem_link_path == sysfs_mount_point_ + "/bus/platform") {
            directory.erase(0, sysfs_mount_point_.size());
            *platform_device_path = directory;
            return true;
        }
        auto last_slash = path.rfind('/');
        if (last_slash == std::string::npos) return false;
        path.erase(last_slash);
        directory = android::base::Dirname(path);
    }
    return false;
}
void DeviceHandler::FixupSysPermissions(const std::string& upath,
                                        const std::string& subsystem) const {
    std::string path = "/sys" + upath;
    for (const auto& s : sysfs_permissions_) {
        if (s.MatchWithSubsystem(path, subsystem)) s.SetPermissions(path);
    }
    if (!skip_restorecon_ && access(path.c_str(), F_OK) == 0) {
        LOG(VERBOSE) << "restorecon_recursive: " << path;
        if (selinux_android_restorecon(path.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            PLOG(ERROR) << "selinux_android_restorecon(" << path << ") failed";
        }
    }
}
std::tuple<mode_t, uid_t, gid_t> DeviceHandler::GetDevicePermissions(
    const std::string& path, const std::vector<std::string>& links) const {
    for (auto it = dev_permissions_.crbegin(); it != dev_permissions_.crend(); ++it) {
        if (it->Match(path) || std::any_of(links.cbegin(), links.cend(),
                                           [it](const auto& link) { return it->Match(link); })) {
            return {it->perm(), it->uid(), it->gid()};
        }
    }
    return {0600, 0, 0};
}
void DeviceHandler::MakeDevice(const std::string& path, int block, int major, int minor,
                               const std::vector<std::string>& links) const {
    auto[mode, uid, gid] = GetDevicePermissions(path, links);
    mode |= (block ? S_IFBLK : S_IFCHR);
    char* secontext = nullptr;
    if (sehandle_) {
        std::vector<const char*> c_links;
        for (const auto& link : links) {
            c_links.emplace_back(link.c_str());
        }
        c_links.emplace_back(nullptr);
        if (selabel_lookup_best_match(sehandle_, &secontext, path.c_str(), &c_links[0], mode)) {
            PLOG(ERROR) << "Device '" << path << "' not created; cannot find SELinux label";
            return;
        }
        setfscreatecon(secontext);
    }
    dev_t dev = makedev(major, minor);
    if (setegid(gid)) {
        PLOG(ERROR) << "setegid(" << gid << ") for " << path << " device failed";
        goto out;
    }
    if (mknod(path.c_str(), mode, dev) && (errno == EEXIST) && secontext) {
        char* fcon = nullptr;
        int rc = lgetfilecon(path.c_str(), &fcon);
        if (rc < 0) {
            PLOG(ERROR) << "Cannot get SELinux label on '" << path << "' device";
            goto out;
        }
        bool different = strcmp(fcon, secontext) != 0;
        freecon(fcon);
        if (different && lsetfilecon(path.c_str(), secontext)) {
            PLOG(ERROR) << "Cannot set '" << secontext << "' SELinux label on '" << path
                        << "' device";
        }
    }
out:
    chown(path.c_str(), uid, -1);
    if (setegid(AID_ROOT)) {
        PLOG(FATAL) << "setegid(AID_ROOT) failed";
    }
    if (secontext) {
        freecon(secontext);
        setfscreatecon(nullptr);
    }
}
std::vector<std::string> DeviceHandler::GetCharacterDeviceSymlinks(const Uevent& uevent) const {
    std::string parent_device;
    if (!FindPlatformDevice(uevent.path, &parent_device)) return {};
    std::string path = uevent.path.substr(parent_device.length());
    if (!android::base::StartsWith(path, "/usb")) return {};
    std::string::size_type start = 0;
    start = path.find('/', start + 1);
    if (start == std::string::npos) return {};
    start = path.find('/', start + 1);
    if (start == std::string::npos) return {};
    auto end = path.find('/', start + 1);
    if (end == std::string::npos) return {};
    start++;
    auto length = end - start;
    if (length == 0) return {};
    auto name_string = path.substr(start, length);
    std::vector<std::string> links;
    links.emplace_back("/dev/usb/" + uevent.subsystem + name_string);
    mkdir("/dev/usb", 0755);
    return links;
}
void SanitizePartitionName(std::string* string) {
    const char* accept =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "_-.";
    if (!string) return;
    std::string::size_type pos = 0;
    while ((pos = string->find_first_not_of(accept, pos)) != std::string::npos) {
        (*string)[pos] = '_';
    }
}
std::vector<std::string> DeviceHandler::GetBlockDeviceSymlinks(const Uevent& uevent) const {
    std::string device;
    std::string type;
    if (FindPlatformDevice(uevent.path, &device)) {
        static const std::string devices_platform_prefix = "/devices/platform/";
        static const std::string devices_prefix = "/devices/";
        if (android::base::StartsWith(device, devices_platform_prefix.c_str())) {
            device = device.substr(devices_platform_prefix.length());
        } else if (android::base::StartsWith(device, devices_prefix.c_str())) {
            device = device.substr(devices_prefix.length());
        }
        type = "platform";
    } else if (FindPciDevicePrefix(uevent.path, &device)) {
        type = "pci";
    } else if (FindVbdDevicePrefix(uevent.path, &device)) {
        type = "vbd";
    } else {
        return {};
    }
    std::vector<std::string> links;
    LOG(VERBOSE) << "found " << type << " device " << device;
    auto link_path = "/dev/block/" + type + "/" + device;
    if (!uevent.partition_name.empty()) {
        std::string partition_name_sanitized(uevent.partition_name);
        SanitizePartitionName(&partition_name_sanitized);
        if (partition_name_sanitized != uevent.partition_name) {
            LOG(VERBOSE) << "Linking partition '" << uevent.partition_name << "' as '"
                         << partition_name_sanitized << "'";
        }
        links.emplace_back(link_path + "/by-name/" + partition_name_sanitized);
    }
    if (uevent.partition_num >= 0) {
        links.emplace_back(link_path + "/by-num/p" + std::to_string(uevent.partition_num));
    }
    auto last_slash = uevent.path.rfind('/');
    links.emplace_back(link_path + "/" + uevent.path.substr(last_slash + 1));
    return links;
}
void DeviceHandler::HandleDevice(const std::string& action, const std::string& devpath, int block,
                                 int major, int minor, const std::vector<std::string>& links) const {
    if (action == "add") {
        MakeDevice(devpath, block, major, minor, links);
        for (const auto& link : links) {
            if (mkdir_recursive(android::base::Dirname(link), 0755, sehandle_)) {
                PLOG(ERROR) << "Failed to create directory " << android::base::Dirname(link);
            }
            if (symlink(devpath.c_str(), link.c_str()) && errno != EEXIST) {
                PLOG(ERROR) << "Failed to symlink " << devpath << " to " << link;
            }
        }
    }
    if (action == "remove") {
        for (const auto& link : links) {
            std::string link_path;
            if (android::base::Readlink(link, &link_path) && link_path == devpath) {
                unlink(link.c_str());
            }
        }
        unlink(devpath.c_str());
    }
}
void DeviceHandler::HandleBlockDeviceEvent(const Uevent& uevent) const {
    if (uevent.major < 0 || uevent.minor < 0) return;
    const char* base = "/dev/block/";
    make_dir(base, 0755, sehandle_);
    std::string name = android::base::Basename(uevent.path);
    std::string devpath = base + name;
    std::vector<std::string> links;
    if (android::base::StartsWith(uevent.path, "/devices")) {
        links = GetBlockDeviceSymlinks(uevent);
    }
    HandleDevice(uevent.action, devpath, 1, uevent.major, uevent.minor, links);
}
void DeviceHandler::HandleGenericDeviceEvent(const Uevent& uevent) const {
    if (uevent.major < 0 || uevent.minor < 0) return;
    std::string devpath;
    if (android::base::StartsWith(uevent.subsystem, "usb")) {
        if (uevent.subsystem == "usb") {
            if (!uevent.device_name.empty()) {
                devpath = "/dev/" + uevent.device_name;
            } else {
                int bus_id = uevent.minor / 128 + 1;
                int device_id = uevent.minor % 128 + 1;
                devpath = android::base::StringPrintf("/dev/bus/usb/%03d/%03d", bus_id, device_id);
            }
        } else {
            return;
        }
    } else if (const auto subsystem =
                   std::find(subsystems_.cbegin(), subsystems_.cend(), uevent.subsystem);
               subsystem != subsystems_.cend()) {
        devpath = subsystem->ParseDevPath(uevent);
    } else {
        devpath = "/dev/" + android::base::Basename(uevent.path);
    }
    mkdir_recursive(android::base::Dirname(devpath), 0755, sehandle_);
    auto links = GetCharacterDeviceSymlinks(uevent);
    HandleDevice(uevent.action, devpath, 0, uevent.major, uevent.minor, links);
}
void DeviceHandler::HandleDeviceEvent(const Uevent& uevent) {
    if (uevent.action == "add" || uevent.action == "change" || uevent.action == "online") {
        FixupSysPermissions(uevent.path, uevent.subsystem);
    }
    if (uevent.subsystem == "block") {
        HandleBlockDeviceEvent(uevent);
    } else {
        HandleGenericDeviceEvent(uevent);
    }
}
DeviceHandler::DeviceHandler(std::vector<Permissions> dev_permissions,
                             std::vector<SysfsPermissions> sysfs_permissions,
                             std::vector<Subsystem> subsystems, bool skip_restorecon)
    : dev_permissions_(std::move(dev_permissions)),
      sysfs_permissions_(std::move(sysfs_permissions)),
      subsystems_(std::move(subsystems)),
      sehandle_(selinux_android_file_context_handle()),
      skip_restorecon_(skip_restorecon),
      sysfs_mount_point_("/sys") {}
DeviceHandler::DeviceHandler()
    : DeviceHandler(std::vector<Permissions>{}, std::vector<SysfsPermissions>{},
                    std::vector<Subsystem>{}, false) {}
