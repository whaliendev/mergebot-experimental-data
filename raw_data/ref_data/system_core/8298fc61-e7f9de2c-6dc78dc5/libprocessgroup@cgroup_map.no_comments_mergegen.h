       
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
const ACgroupController* controller_ = nullptr;
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
