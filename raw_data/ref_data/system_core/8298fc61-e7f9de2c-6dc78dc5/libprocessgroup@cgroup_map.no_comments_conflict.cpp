#define LOG_TAG "libprocessgroup"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <regex>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <cgroup_map.h>
#include <json/reader.h>
#include <json/value.h>
#include <processgroup/processgroup.h>
using android::base::GetBoolProperty;
using android::base::StringPrintf;
using android::base::unique_fd;
static constexpr const char* CGROUP_PROCS_FILE = "/cgroup.procs";
static constexpr const char* CGROUP_TASKS_FILE = "/tasks";
static constexpr const char* CGROUP_TASKS_FILE_V2 = "/cgroup.tasks";
<<<<<<< HEAD
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
bool CgroupController::HasValue() const {
    return controller_ != nullptr;
}
std::string CgroupController::GetTasksFilePath(const std::string& rel_path) const {
    std::string tasks_path = path();
    if (!rel_path.empty()) {
        tasks_path += "/" + rel_path;
    }
    return (version() == 1) ? tasks_path + CGROUP_TASKS_FILE : tasks_path + CGROUP_TASKS_FILE_V2;
}
std::string CgroupController::GetProcsFilePath(const std::string& rel_path, uid_t uid,
||||||| 6dc78dc56
static bool Mkdir(const std::string& path, mode_t mode, const std::string& uid,
                  const std::string& gid) {
    if (mode == 0) {
        mode = 0755;
    }
    if (mkdir(path.c_str(), mode) != 0) {
        if (errno == EEXIST) {
            if (fchmodat(AT_FDCWD, path.c_str(), mode, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != EROFS) {
                    PLOG(ERROR) << "fchmodat() failed for " << path;
                    return false;
                }
            }
        } else {
            PLOG(ERROR) << "mkdir() failed for " << path;
            return false;
        }
    }
    if (uid.empty()) {
        return true;
    }
    passwd* uid_pwd = getpwnam(uid.c_str());
    if (!uid_pwd) {
        PLOG(ERROR) << "Unable to decode UID for '" << uid << "'";
        return false;
    }
    uid_t pw_uid = uid_pwd->pw_uid;
    gid_t gr_gid = -1;
    if (!gid.empty()) {
        group* gid_pwd = getgrnam(gid.c_str());
        if (!gid_pwd) {
            PLOG(ERROR) << "Unable to decode GID for '" << gid << "'";
            return false;
        }
        gr_gid = gid_pwd->gr_gid;
    }
    if (lchown(path.c_str(), pw_uid, gr_gid) < 0) {
        PLOG(ERROR) << "lchown() failed for " << path;
        return false;
    }
    if (mode & (S_ISUID | S_ISGID)) {
        if (fchmodat(AT_FDCWD, path.c_str(), mode, AT_SYMLINK_NOFOLLOW) != 0) {
            PLOG(ERROR) << "fchmodat() failed for " << path;
            return false;
        }
    }
    return true;
}
static bool ReadDescriptorsFromFile(const std::string& file_name,
                                    std::map<std::string, CgroupDescriptor>* descriptors) {
    std::vector<CgroupDescriptor> result;
    std::string json_doc;
    if (!android::base::ReadFileToString(file_name, &json_doc)) {
        PLOG(ERROR) << "Failed to read task profiles from " << file_name;
        return false;
    }
    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(json_doc, root)) {
        LOG(ERROR) << "Failed to parse cgroups description: " << reader.getFormattedErrorMessages();
        return false;
    }
    if (root.isMember("Cgroups")) {
        const Json::Value& cgroups = root["Cgroups"];
        for (Json::Value::ArrayIndex i = 0; i < cgroups.size(); ++i) {
            std::string name = cgroups[i]["Controller"].asString();
            auto iter = descriptors->find(name);
            if (iter == descriptors->end()) {
                descriptors->emplace(name, CgroupDescriptor(1, name, cgroups[i]["Path"].asString(),
                                     std::strtoul(cgroups[i]["Mode"].asString().c_str(), 0, 8),
                                     cgroups[i]["UID"].asString(), cgroups[i]["GID"].asString()));
            } else {
                iter->second = CgroupDescriptor(1, name, cgroups[i]["Path"].asString(),
                                     std::strtoul(cgroups[i]["Mode"].asString().c_str(), 0, 8),
                                     cgroups[i]["UID"].asString(), cgroups[i]["GID"].asString());
            }
        }
    }
    if (root.isMember("Cgroups2")) {
        const Json::Value& cgroups2 = root["Cgroups2"];
        auto iter = descriptors->find(CGROUPV2_CONTROLLER_NAME);
        if (iter == descriptors->end()) {
            descriptors->emplace(CGROUPV2_CONTROLLER_NAME, CgroupDescriptor(2, CGROUPV2_CONTROLLER_NAME, cgroups2["Path"].asString(),
                                 std::strtoul(cgroups2["Mode"].asString().c_str(), 0, 8),
                                 cgroups2["UID"].asString(), cgroups2["GID"].asString()));
        } else {
            iter->second = CgroupDescriptor(2, CGROUPV2_CONTROLLER_NAME, cgroups2["Path"].asString(),
                                 std::strtoul(cgroups2["Mode"].asString().c_str(), 0, 8),
                                 cgroups2["UID"].asString(), cgroups2["GID"].asString());
        }
    }
    return true;
}
static bool ReadDescriptors(std::map<std::string, CgroupDescriptor>* descriptors) {
    if (!ReadDescriptorsFromFile(CGROUPS_DESC_FILE, descriptors)) {
        return false;
    }
    if (!access(CGROUPS_DESC_VENDOR_FILE, F_OK) &&
        !ReadDescriptorsFromFile(CGROUPS_DESC_VENDOR_FILE, descriptors)) {
        return false;
    }
    return true;
}
#if defined(__ANDROID__)
static bool SetupCgroup(const CgroupDescriptor& descriptor) {
    const CgroupController* controller = descriptor.controller();
    if (!Mkdir(controller->path(), descriptor.mode(), descriptor.uid(), descriptor.gid())) {
        LOG(ERROR) << "Failed to create directory for " << controller->name() << " cgroup";
        return false;
    }
    int result;
    if (controller->version() == 2) {
        result = mount("none", controller->path(), "cgroup2", MS_NODEV | MS_NOEXEC | MS_NOSUID,
                       nullptr);
    } else {
        if (!strcmp(controller->name(), "cpuset")) {
            result = mount("none", controller->path(), controller->name(),
                           MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr);
        } else {
            result = mount("none", controller->path(), "cgroup", MS_NODEV | MS_NOEXEC | MS_NOSUID,
                           controller->name());
        }
    }
    if (result < 0) {
        PLOG(ERROR) << "Failed to mount " << controller->name() << " cgroup";
        return false;
    }
    return true;
}
#else
static bool SetupCgroup(const CgroupDescriptor&) {
    return false;
}
#endif
static bool WriteRcFile(const std::map<std::string, CgroupDescriptor>& descriptors) {
    int fd = TEMP_FAILURE_RETRY(open(CGROUPS_RC_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
                                     S_IRUSR | S_IRGRP | S_IROTH));
    if (fd < 0) {
        PLOG(ERROR) << "open() failed for " << CGROUPS_RC_PATH;
        return false;
    }
    CgroupFile fl;
    fl.version_ = CgroupFile::FILE_CURR_VERSION;
    fl.controller_count_ = descriptors.size();
    int ret = TEMP_FAILURE_RETRY(write(fd, &fl, sizeof(fl)));
    if (ret < 0) {
        PLOG(ERROR) << "write() failed for " << CGROUPS_RC_PATH;
        return false;
    }
    for (const auto& [name, descriptor] : descriptors) {
        ret = TEMP_FAILURE_RETRY(write(fd, descriptor.controller(), sizeof(CgroupController)));
        if (ret < 0) {
            PLOG(ERROR) << "write() failed for " << CGROUPS_RC_PATH;
            return false;
        }
    }
    return true;
}
CgroupController::CgroupController(uint32_t version, const std::string& name,
                                   const std::string& path) {
    version_ = version;
    strncpy(name_, name.c_str(), sizeof(name_) - 1);
    name_[sizeof(name_) - 1] = '\0';
    strncpy(path_, path.c_str(), sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';
}
std::string CgroupController::GetTasksFilePath(const std::string& path) const {
    std::string tasks_path = path_;
    if (!path.empty()) {
        tasks_path += "/" + path;
    }
    return (version_ == 1) ? tasks_path + CGROUP_TASKS_FILE : tasks_path + CGROUP_TASKS_FILE_V2;
}
std::string CgroupController::GetProcsFilePath(const std::string& path, uid_t uid,
=======
CgroupController::CgroupController(uint32_t version, const std::string& name,
                                   const std::string& path) {
    version_ = version;
    strncpy(name_, name.c_str(), sizeof(name_) - 1);
    name_[sizeof(name_) - 1] = '\0';
    strncpy(path_, path.c_str(), sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';
}
std::string CgroupController::GetTasksFilePath(const std::string& path) const {
    std::string tasks_path = path_;
    if (!path.empty()) {
        tasks_path += "/" + path;
    }
    return (version_ == 1) ? tasks_path + CGROUP_TASKS_FILE : tasks_path + CGROUP_TASKS_FILE_V2;
}
std::string CgroupController::GetProcsFilePath(const std::string& path, uid_t uid,
>>>>>>> e7f9de2c
                                               pid_t pid) const {
    std::string proc_path(path());
    proc_path.append("/").append(rel_path);
    proc_path = regex_replace(proc_path, std::regex("<uid>"), std::to_string(uid));
    proc_path = regex_replace(proc_path, std::regex("<pid>"), std::to_string(pid));
    return proc_path.append(CGROUP_PROCS_FILE);
}
bool CgroupController::GetTaskGroup(int tid, std::string* group) const {
    std::string file_name = StringPrintf("/proc/%d/cgroup", tid);
    std::string content;
    if (!android::base::ReadFileToString(file_name, &content)) {
        PLOG(ERROR) << "Failed to read " << file_name;
        return false;
    }
    if (group == nullptr) {
        return true;
    }
    std::string cg_tag = StringPrintf(":%s:", name());
    size_t start_pos = content.find(cg_tag);
    if (start_pos == std::string::npos) {
        return false;
    }
    start_pos += cg_tag.length() + 1;
    size_t end_pos = content.find('\n', start_pos);
    if (end_pos == std::string::npos) {
        *group = content.substr(start_pos, std::string::npos);
    } else {
        *group = content.substr(start_pos, end_pos - start_pos);
    }
    return true;
}
<<<<<<< HEAD
CgroupMap::CgroupMap() {
||||||| 6dc78dc56
CgroupDescriptor::CgroupDescriptor(uint32_t version, const std::string& name,
                                   const std::string& path, mode_t mode, const std::string& uid,
                                   const std::string& gid)
    : controller_(version, name, path), mode_(mode), uid_(uid), gid_(gid) {}
CgroupMap::CgroupMap() : cg_file_data_(nullptr), cg_file_size_(0) {
=======
CgroupMap::CgroupMap() : cg_file_data_(nullptr), cg_file_size_(0) {
>>>>>>> e7f9de2c
    if (!LoadRcFile()) {
        LOG(ERROR) << "CgroupMap::LoadRcFile called for [" << getpid() << "] failed";
    }
}
CgroupMap& CgroupMap::GetInstance() {
    static auto* instance = new CgroupMap;
    return *instance;
}
bool CgroupMap::LoadRcFile() {
    if (!loaded_) {
        loaded_ = (ACgroupFile_getVersion() != 0);
    }
    return loaded_;
}
void CgroupMap::Print() const {
    if (!loaded_) {
        LOG(ERROR) << "CgroupMap::Print called for [" << getpid()
                   << "] failed, RC file was not initialized properly";
        return;
    }
    LOG(INFO) << "File version = " << ACgroupFile_getVersion();
    LOG(INFO) << "File controller count = " << ACgroupFile_getControllerCount();
    LOG(INFO) << "Mounted cgroups:";
    auto controller_count = ACgroupFile_getControllerCount();
    for (uint32_t i = 0; i < controller_count; ++i) {
        const ACgroupController* controller = ACgroupFile_getController(i);
        LOG(INFO) << "\t" << ACgroupController_getName(controller) << " ver "
                  << ACgroupController_getVersion(controller) << " path "
                  << ACgroupController_getPath(controller);
    }
}
<<<<<<< HEAD
CgroupController CgroupMap::FindController(const std::string& name) const {
    if (!loaded_) {
||||||| 6dc78dc56
bool CgroupMap::SetupCgroups() {
    std::map<std::string, CgroupDescriptor> descriptors;
    if (getpid() != 1) {
        LOG(ERROR) << "Cgroup setup can be done only by init process";
        return false;
    }
    if (access(CGROUPS_RC_PATH, F_OK) == 0) {
        LOG(WARNING) << "Attempt to call SetupCgroups more than once";
        return true;
    }
    if (!ReadDescriptors(&descriptors)) {
        LOG(ERROR) << "Failed to load cgroup description file";
        return false;
    }
    for (const auto& [name, descriptor] : descriptors) {
        if (!SetupCgroup(descriptor)) {
            LOG(WARNING) << "Failed to setup " << name << " cgroup";
        }
    }
    if (!Mkdir(android::base::Dirname(CGROUPS_RC_PATH), 0711, "system", "system")) {
        LOG(ERROR) << "Failed to create directory for " << CGROUPS_RC_PATH << " file";
        return false;
    }
    if (!WriteRcFile(descriptors)) {
        LOG(ERROR) << "Failed to write " << CGROUPS_RC_PATH << " file";
        return false;
    }
    if (fchmodat(AT_FDCWD, CGROUPS_RC_PATH, 0644, AT_SYMLINK_NOFOLLOW) < 0) {
        PLOG(ERROR) << "fchmodat() failed";
        return false;
    }
    return true;
}
const CgroupController* CgroupMap::FindController(const std::string& name) const {
    if (!cg_file_data_) {
=======
const CgroupController* CgroupMap::FindController(const std::string& name) const {
    if (!cg_file_data_) {
>>>>>>> e7f9de2c
        LOG(ERROR) << "CgroupMap::FindController called for [" << getpid()
                   << "] failed, RC file was not initialized properly";
        return CgroupController(nullptr);
    }
    auto controller_count = ACgroupFile_getControllerCount();
    for (uint32_t i = 0; i < controller_count; ++i) {
        const ACgroupController* controller = ACgroupFile_getController(i);
        if (name == ACgroupController_getName(controller)) {
            return CgroupController(controller);
        }
    }
    return CgroupController(nullptr);
}
