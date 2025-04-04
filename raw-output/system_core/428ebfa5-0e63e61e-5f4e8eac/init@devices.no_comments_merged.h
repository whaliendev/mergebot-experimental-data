#ifndef _INIT_DEVICES_H
#define _INIT_DEVICES_H 
<<<<<<< HEAD
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <string>
#include <vector>
||||||| 5f4e8eac8
#include <sys/stat.h>
#include <chrono>
#include <functional>
#include <optional>
=======
#include <functional>
#include <sys/stat.h>
>>>>>>> 0e63e61e
#include <android-base/file.h>
#include <selinux/label.h>
#include "uevent.h"
class Permissions {
  public:
    Permissions(const std::string& name, mode_t perm, uid_t uid, gid_t gid);
    bool Match(const std::string& path) const;
    mode_t perm() const { return perm_; }
    uid_t uid() const { return uid_; }
    gid_t gid() const { return gid_; }
  protected:
    const std::string& name() const { return name_; }
  private:
    std::string name_;
    mode_t perm_;
    uid_t uid_;
    gid_t gid_;
    bool prefix_;
    bool wildcard_;
};
class SysfsPermissions : public Permissions {
  public:
    SysfsPermissions(const std::string& name, const std::string& attribute, mode_t perm, uid_t uid,
                     gid_t gid)
        : Permissions(name, perm, uid, gid), attribute_(attribute) {}
    bool MatchWithSubsystem(const std::string& path, const std::string& subsystem) const;
    void SetPermissions(const std::string& path) const;
  private:
    const std::string attribute_;
};
class Subsystem {
  public:
    friend class SubsystemParser;
<<<<<<< HEAD
    Subsystem() {}
||||||| 5f4e8eac8
extern int add_dev_perms(const char *name, const char *attr,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, unsigned short prefix,
                         unsigned short wildcard);
=======
extern int add_dev_perms(const char *name, const char *attr,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, unsigned short prefix,
                         unsigned short wildcard);
int get_device_fd();
>>>>>>> 0e63e61e
    std::string ParseDevPath(const Uevent& uevent) const {
        std::string devname = devname_source_ == DevnameSource::DEVNAME_UEVENT_DEVNAME
                                  ? uevent.device_name
                                  : android::base::Basename(uevent.path);
<<<<<<< HEAD
        return dir_name_ + "/" + devname;
    }
    bool operator==(const std::string& string_name) const { return name_ == string_name; }
  private:
    enum class DevnameSource {
        DEVNAME_UEVENT_DEVNAME,
        DEVNAME_UEVENT_DEVPATH,
    };
    std::string name_;
    std::string dir_name_ = "/dev";
    DevnameSource devname_source_;
};
class DeviceHandler {
  public:
    friend class DeviceHandlerTester;
    DeviceHandler();
    DeviceHandler(std::vector<Permissions> dev_permissions,
                  std::vector<SysfsPermissions> sysfs_permissions,
                  std::vector<Subsystem> subsystems, bool skip_restorecon);
    ~DeviceHandler(){};
    void HandleDeviceEvent(const Uevent& uevent);
    std::vector<std::string> GetBlockDeviceSymlinks(const Uevent& uevent) const;
    void set_skip_restorecon(bool value) { skip_restorecon_ = value; }
  private:
    bool FindPlatformDevice(std::string path, std::string* platform_device_path) const;
    std::tuple<mode_t, uid_t, gid_t> GetDevicePermissions(
        const std::string& path, const std::vector<std::string>& links) const;
    void MakeDevice(const std::string& path, int block, int major, int minor,
                    const std::vector<std::string>& links) const;
    std::vector<std::string> GetCharacterDeviceSymlinks(const Uevent& uevent) const;
    void HandleDevice(const std::string& action, const std::string& devpath, int block, int major,
                      int minor, const std::vector<std::string>& links) const;
    void FixupSysPermissions(const std::string& upath, const std::string& subsystem) const;
    void HandleBlockDeviceEvent(const Uevent& uevent) const;
    void HandleGenericDeviceEvent(const Uevent& uevent) const;
    std::vector<Permissions> dev_permissions_;
    std::vector<SysfsPermissions> sysfs_permissions_;
    std::vector<Subsystem> subsystems_;
    selabel_handle* sehandle_;
    bool skip_restorecon_;
    std::string sysfs_mount_point_;
};
void SanitizePartitionName(std::string* string);
#endif
||||||| 5f4e8eac8
void device_poll(const coldboot_callback& callback = nullptr,
                 const std::optional<std::chrono::milliseconds> relative_timeout = {});
#endif
=======
#endif
>>>>>>> 0e63e61e
