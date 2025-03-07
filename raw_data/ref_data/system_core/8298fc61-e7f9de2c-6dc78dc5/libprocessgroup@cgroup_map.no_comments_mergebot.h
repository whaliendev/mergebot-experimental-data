       
#include <sys/cdefs.h>
#include <sys/types.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <android/cgrouprc.h>
class CgroupController {
 private:
  const ACgroupController* controller_ = nullptr;
 public:
  explicitCgroupController(const ACgroupController* controller)
      : controller_(controller) {}
  uint32_t version() const;
  const char* name() const;
  const char* path() const;
  bool HasValue() const;
  uint32_t CgroupController::version() const {
    CHECK(HasValue());
    return ACgroupController_getVersion(controller_);
  }
  const char* CgroupController::name() const {
    CHECK(HasValue());
    return ACgroupController_getName(controller_);
  }
  const char* CgroupController::path() const {
    CHECK(HasValue());
    return ACgroupController_getPath(controller_);
  }
  std::string GetTasksFilePath(const std::string& path) const;
  std::string GetProcsFilePath(const std::string& path, uid_t uid,
                               pid_t pid) const;
  bool GetTaskGroup(int tid, std::string* group) const;
};
class CgroupMap {
 private:
  bool loaded_ = false;
 public:
  static bool SetupCgroups();
  static CgroupMap& GetInstance();
  CgroupController FindController(const std::string& name) const;
 private:
  CgroupMap();
  bool LoadRcFile();
  void Print() const;
};
