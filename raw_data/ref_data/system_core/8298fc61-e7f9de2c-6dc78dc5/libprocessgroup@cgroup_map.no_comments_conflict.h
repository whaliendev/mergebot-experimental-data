       
#include <sys/cdefs.h>
#include <sys/types.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <android/cgrouprc.h>
class CgroupController {
  public:
    explicit CgroupController(const ACgroupController* controller) : controller_(controller) {}
    uint32_t version() const;
    const char* name() const;
    const char* path() const;
    bool HasValue() const;
    std::string GetTasksFilePath(const std::string& path) const;
    std::string GetProcsFilePath(const std::string& path, uid_t uid, pid_t pid) const;
    bool GetTaskGroup(int tid, std::string* group) const;
  private:
<<<<<<< HEAD
    const ACgroupController* controller_ = nullptr;
||||||| 6dc78dc56
    static constexpr size_t CGROUP_NAME_BUF_SZ = 16;
    static constexpr size_t CGROUP_PATH_BUF_SZ = 32;
    uint32_t version_;
    char name_[CGROUP_NAME_BUF_SZ];
    char path_[CGROUP_PATH_BUF_SZ];
};
class CgroupDescriptor {
  public:
    CgroupDescriptor(uint32_t version, const std::string& name, const std::string& path,
                     mode_t mode, const std::string& uid, const std::string& gid);
    const CgroupController* controller() const { return &controller_; }
    mode_t mode() const { return mode_; }
    std::string uid() const { return uid_; }
    std::string gid() const { return gid_; }
  private:
    CgroupController controller_;
    mode_t mode_;
    std::string uid_;
    std::string gid_;
};
struct CgroupFile {
    static constexpr uint32_t FILE_VERSION_1 = 1;
    static constexpr uint32_t FILE_CURR_VERSION = FILE_VERSION_1;
    uint32_t version_;
    uint32_t controller_count_;
    CgroupController controllers_[];
=======
    static constexpr size_t CGROUP_NAME_BUF_SZ = 16;
    static constexpr size_t CGROUP_PATH_BUF_SZ = 32;
    uint32_t version_;
    char name_[CGROUP_NAME_BUF_SZ];
    char path_[CGROUP_PATH_BUF_SZ];
};
struct CgroupFile {
    static constexpr uint32_t FILE_VERSION_1 = 1;
    static constexpr uint32_t FILE_CURR_VERSION = FILE_VERSION_1;
    uint32_t version_;
    uint32_t controller_count_;
    CgroupController controllers_[];
>>>>>>> e7f9de2c
};
class CgroupMap {
  public:
    static bool SetupCgroups();
    static CgroupMap& GetInstance();
    CgroupController FindController(const std::string& name) const;
  private:
    bool loaded_ = false;
    CgroupMap();
    bool LoadRcFile();
    void Print() const;
};
