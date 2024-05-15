#include "odrefresh.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/macros.h"
#include "android-base/parseint.h"
#include "android-base/properties.h"
#include "android-base/result.h"
#include "android-base/scopeguard.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "android/log.h"
#include "arch/instruction_set.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/macros.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/string_view_cpp20.h"
#include "base/unix_file/fd_file.h"
#include "com_android_apex.h"
#include "com_android_art.h"
#include "dex/art_dex_file_loader.h"
#include "dexoptanalyzer.h"
#include "exec_utils.h"
#include "log/log.h"
#include "odr_artifacts.h"
#include "odr_common.h"
#include "odr_compilation_log.h"
#include "odr_config.h"
#include "odr_fs_utils.h"
#include "odr_metrics.h"
#include "odrefresh/odrefresh.h"
#include "palette/palette.h"
#include "palette/palette_types.h"
namespace art {
namespace odrefresh {
namespace apex = com::android::apex;
namespace art_apex = com::android::art;
using android::base::Result;
namespace {
constexpr const char* kCacheInfoFile = "cache-info.xml";
constexpr time_t kMaximumExecutionSeconds = 300;
constexpr time_t kMaxChildProcessSeconds = 90;
constexpr mode_t kFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
constexpr const char* kFirstBootImageBasename = "boot.art";
constexpr const char* kMinimalBootImageBasename = "boot_minimal.art";
void EraseFiles(const std::vector<std::unique_ptr<File>>& files) {
  for (auto& file : files) {
    file->Erase( true);
  }
}
bool MoveOrEraseFiles(const std::vector<std::unique_ptr<File>>& files,
                      std::string_view output_directory_path) {
  std::vector<std::unique_ptr<File>> output_files;
  for (auto& file : files) {
    const std::string file_basename(android::base::Basename(file->GetPath()));
    const std::string output_file_path = Concatenate({output_directory_path, "/", file_basename});
    const std::string input_file_path = file->GetPath();
    output_files.emplace_back(OS::CreateEmptyFileWriteOnly(output_file_path.c_str()));
    if (output_files.back() == nullptr) {
      PLOG(ERROR) << "Failed to open " << QuotePath(output_file_path);
      output_files.pop_back();
      EraseFiles(output_files);
      EraseFiles(files);
      return false;
    }
    if (fchmod(output_files.back()->Fd(), kFileMode) != 0) {
      PLOG(ERROR) << "Could not set file mode on " << QuotePath(output_file_path);
      EraseFiles(output_files);
      EraseFiles(files);
      return false;
    }
    const size_t file_bytes = file->GetLength();
    if (!output_files.back()->Copy(file.get(), 0, file_bytes)) {
      PLOG(ERROR) << "Failed to copy " << QuotePath(file->GetPath()) << " to "
                  << QuotePath(output_file_path);
      EraseFiles(output_files);
      EraseFiles(files);
      return false;
    }
    if (!file->Erase( true)) {
      PLOG(ERROR) << "Failed to erase " << QuotePath(file->GetPath());
      EraseFiles(output_files);
      EraseFiles(files);
      return false;
    }
    if (output_files.back()->FlushCloseOrErase() != 0) {
      PLOG(ERROR) << "Failed to flush and close file " << QuotePath(output_file_path);
      EraseFiles(output_files);
      EraseFiles(files);
      return false;
    }
  }
  return true;
}
std::optional<apex::ApexInfo> GetArtApexInfo(const std::vector<apex::ApexInfo>& info_list) {
  auto it = std::find_if(info_list.begin(), info_list.end(), [](const apex::ApexInfo& info) {
    return info.getModuleName() == "com.android.art";
  });
  return it != info_list.end() ? std::make_optional(*it) : std::nullopt;
}
art_apex::ModuleInfo GenerateModuleInfo(const apex::ApexInfo& apex_info) {
  int64_t last_update_millis =
      apex_info.hasLastUpdateMillis() ? apex_info.getLastUpdateMillis() : 0;
  return art_apex::ModuleInfo{apex_info.getModuleName(),
                              apex_info.getVersionCode(),
                              apex_info.getVersionName(),
                              last_update_millis};
}
std::vector<art_apex::ModuleInfo> GenerateModuleInfoList(
    const std::vector<apex::ApexInfo>& apex_info_list) {
  std::vector<art_apex::ModuleInfo> module_info_list;
  std::transform(apex_info_list.begin(),
                 apex_info_list.end(),
                 std::back_inserter(module_info_list),
                 GenerateModuleInfo);
  return module_info_list;
}
std::string AndroidRootRewrite(const std::string& path) {
  if (StartsWith(path, "/system/")) {
    return Concatenate({GetAndroidRoot(), path.substr(7)});
  } else {
    return path;
  }
}
template <typename T>
Result<void> CheckComponents(
    const std::vector<T>& expected_components,
    const std::vector<T>& actual_components,
    const std::function<Result<void>(const T& expected, const T& actual)>& custom_checker =
        [](const T&, const T&) -> Result<void> { return {}; }) {
  if (expected_components.size() != actual_components.size()) {
    return Errorf(
        "Component count differs ({} != {})", expected_components.size(), actual_components.size());
  }
  for (size_t i = 0; i < expected_components.size(); ++i) {
    const T& expected = expected_components[i];
    const T& actual = actual_components[i];
    if (expected.getFile() != actual.getFile()) {
      return Errorf(
          "Component {} file differs ('{}' != '{}')", i, expected.getFile(), actual.getFile());
    }
    if (expected.getSize() != actual.getSize()) {
      return Errorf(
          "Component {} size differs ({} != {})", i, expected.getSize(), actual.getSize());
    }
    if (expected.getChecksums() != actual.getChecksums()) {
      return Errorf("Component {} checksums differ ('{}' != '{}')",
                    i,
                    expected.getChecksums(),
                    actual.getChecksums());
    }
    Result<void> result = custom_checker(expected, actual);
    if (!result.ok()) {
      return Errorf("Component {} {}", i, result.error().message());
    }
  }
  return {};
}
Result<void> CheckSystemServerComponents(
    const std::vector<art_apex::SystemServerComponent>& expected_components,
    const std::vector<art_apex::SystemServerComponent>& actual_components) {
  return CheckComponents<art_apex::SystemServerComponent>(
      expected_components,
      actual_components,
      [](const art_apex::SystemServerComponent& expected,
         const art_apex::SystemServerComponent& actual) -> Result<void> {
        if (expected.getIsInClasspath() != actual.getIsInClasspath()) {
          return Errorf("isInClasspath differs ({} != {})",
                        expected.getIsInClasspath(),
                        actual.getIsInClasspath());
        }
        return {};
      });
}
template <typename T>
std::vector<T> GenerateComponents(
    const std::vector<std::string>& jars,
    const std::function<T(const std::string& path, uint64_t size, const std::string& checksum)>&
        custom_generator) {
  std::vector<T> components;
  ArtDexFileLoader loader;
  for (const std::string& path : jars) {
    std::string actual_path = AndroidRootRewrite(path);
    struct stat sb;
    if (stat(actual_path.c_str(), &sb) == -1) {
      PLOG(ERROR) << "Failed to stat component: " << QuotePath(actual_path);
      return {};
    }
    std::vector<uint32_t> checksums;
    std::vector<std::string> dex_locations;
    std::string error_msg;
    if (!loader.GetMultiDexChecksums(actual_path.c_str(), &checksums, &dex_locations, &error_msg)) {
      LOG(ERROR) << "Failed to get multi-dex checksums: " << error_msg;
      return {};
    }
    std::ostringstream oss;
    for (size_t i = 0; i < checksums.size(); ++i) {
      if (i != 0) {
        oss << ';';
      }
      oss << android::base::StringPrintf("%08x", checksums[i]);
    }
    const std::string checksum = oss.str();
    Result<T> component = custom_generator(path, static_cast<uint64_t>(sb.st_size), checksum);
    if (!component.ok()) {
      LOG(ERROR) << "Failed to generate component: " << component.error();
      return {};
    }
    components.push_back(*std::move(component));
  }
  return components;
}
std::vector<art_apex::Component> GenerateComponents(const std::vector<std::string>& jars) {
  return GenerateComponents<art_apex::Component>(
      jars, [](const std::string& path, uint64_t size, const std::string& checksum) {
        return art_apex::Component{path, size, checksum};
      });
}
bool ArtifactsExist(const OdrArtifacts& artifacts,
                    bool check_art_file,
                            std::string* error_msg,
                            std::vector<std::string>* checked_artifacts = nullptr) {
  std::vector<const char*> paths{artifacts.OatPath().c_str(), artifacts.VdexPath().c_str()};
  if (check_art_file) {
    paths.push_back(artifacts.ImagePath().c_str());
  }
  for (const char* path : paths) {
    if (!OS::FileExists(path)) {
      if (errno == EACCES) {
        PLOG(ERROR) << "Failed to stat() " << path;
      }
      *error_msg = "Missing file: " + QuotePath(path);
      return false;
    }
  }
  if (checked_artifacts != nullptr) {
    for (const char* path : paths) {
      checked_artifacts->emplace_back(path);
    }
  }
  return true;
}
void AddDex2OatCommonOptions( std::vector<std::string>& args) {
  args.emplace_back("--android-root=out/empty");
  args.emplace_back("--abort-on-hard-verifier-error");
  args.emplace_back("--no-abort-on-soft-verifier-error");
  args.emplace_back("--compilation-reason=boot");
  args.emplace_back("--image-format=lz4");
  args.emplace_back("--force-determinism");
  args.emplace_back("--resolve-startup-const-strings=true");
  args.emplace_back("--avoid-storing-invocation");
}
bool IsCpuSetSpecValid(const std::string& cpu_set) {
  for (auto& str : android::base::Split(cpu_set, ",")) {
    int id;
    if (!android::base::ParseInt(str, &id, 0)) {
      return false;
    }
  }
  return true;
}
bool AddDex2OatConcurrencyArguments( std::vector<std::string>& args) {
  std::string threads = android::base::GetProperty("dalvik.vm.boot-dex2oat-threads", "");
  if (!threads.empty()) {
    args.push_back("-j" + threads);
  }
  std::string cpu_set = android::base::GetProperty("dalvik.vm.boot-dex2oat-cpu-set", "");
  if (cpu_set.empty()) {
    return true;
  }
  if (!IsCpuSetSpecValid(cpu_set)) {
    LOG(ERROR) << "Invalid CPU set spec: " << cpu_set;
    return false;
  }
  args.push_back("--cpu-set=" + cpu_set);
  return true;
}
void AddDex2OatDebugInfo( std::vector<std::string>& args) {
  args.emplace_back("--generate-mini-debug-info");
  args.emplace_back("--strip");
}
void AddDex2OatInstructionSet( std::vector<std::string>& args, InstructionSet isa) {
  const char* isa_str = GetInstructionSetString(isa);
  args.emplace_back(Concatenate({"--instruction-set=", isa_str}));
}
void AddDex2OatProfileAndCompilerFilter(
              std::vector<std::string>& args,
              std::vector<std::unique_ptr<File>>& output_files,
    const std::vector<std::string>& profile_paths) {
  bool has_any_profile = false;
  for (auto& path : profile_paths) {
    std::unique_ptr<File> profile_file(OS::OpenFileForReading(path.c_str()));
    if (profile_file && profile_file->IsOpened()) {
      args.emplace_back(android::base::StringPrintf("--profile-file-fd=%d", profile_file->Fd()));
      output_files.emplace_back(std::move(profile_file));
      has_any_profile = true;
    }
  }
  if (has_any_profile) {
    args.emplace_back("--compiler-filter=speed-profile");
  } else {
    args.emplace_back("--compiler-filter=speed");
  }
}
bool AddBootClasspathFds( std::vector<std::string>& args,
                                   std::vector<std::unique_ptr<File>>& output_files,
                         const std::vector<std::string>& bcp_jars) {
  std::vector<std::string> bcp_fds;
  for (const std::string& jar : bcp_jars) {
    if (StartsWith(jar, "/apex/")) {
      bcp_fds.emplace_back("-1");
    } else {
      std::string actual_path = AndroidRootRewrite(jar);
      std::unique_ptr<File> jar_file(OS::OpenFileForReading(actual_path.c_str()));
      if (!jar_file || !jar_file->IsValid()) {
        LOG(ERROR) << "Failed to open a BCP jar " << actual_path;
        return false;
      }
      bcp_fds.push_back(std::to_string(jar_file->Fd()));
      output_files.push_back(std::move(jar_file));
    }
  }
  args.emplace_back("--runtime-arg");
  args.emplace_back(Concatenate({"-Xbootclasspathfds:", android::base::Join(bcp_fds, ':')}));
  return true;
}
std::string GetBootImageComponentBasename(const std::string& jar_path, bool is_first_jar) {
  if (is_first_jar) {
    return kFirstBootImageBasename;
  }
  const std::string jar_name = android::base::Basename(jar_path);
  return "boot-" + ReplaceFileExtension(jar_name, "art");
}
void AddCompiledBootClasspathFdsIfAny(
              std::vector<std::string>& args,
              std::vector<std::unique_ptr<File>>& output_files,
    const std::vector<std::string>& bcp_jars,
    const InstructionSet isa,
    const std::string& artifact_dir) {
  std::vector<std::string> bcp_image_fds;
  std::vector<std::string> bcp_oat_fds;
  std::vector<std::string> bcp_vdex_fds;
  std::vector<std::unique_ptr<File>> opened_files;
  bool added_any = false;
  for (size_t i = 0; i < bcp_jars.size(); i++) {
    const std::string& jar = bcp_jars[i];
    std::string image_path =
        artifact_dir + "/" + GetBootImageComponentBasename(jar, i == 0);
    image_path = GetSystemImageFilename(image_path.c_str(), isa);
    std::unique_ptr<File> image_file(OS::OpenFileForReading(image_path.c_str()));
    if (image_file && image_file->IsValid()) {
      bcp_image_fds.push_back(std::to_string(image_file->Fd()));
      opened_files.push_back(std::move(image_file));
      added_any = true;
    } else {
      bcp_image_fds.push_back("-1");
    }
    std::string oat_path = ReplaceFileExtension(image_path, "oat");
    std::unique_ptr<File> oat_file(OS::OpenFileForReading(oat_path.c_str()));
    if (oat_file && oat_file->IsValid()) {
      bcp_oat_fds.push_back(std::to_string(oat_file->Fd()));
      opened_files.push_back(std::move(oat_file));
      added_any = true;
    } else {
      bcp_oat_fds.push_back("-1");
    }
    std::string vdex_path = ReplaceFileExtension(image_path, "vdex");
    std::unique_ptr<File> vdex_file(OS::OpenFileForReading(vdex_path.c_str()));
    if (vdex_file && vdex_file->IsValid()) {
      bcp_vdex_fds.push_back(std::to_string(vdex_file->Fd()));
      opened_files.push_back(std::move(vdex_file));
      added_any = true;
    } else {
      bcp_vdex_fds.push_back("-1");
    }
  }
  if (added_any) {
    std::move(opened_files.begin(), opened_files.end(), std::back_inserter(output_files));
    args.emplace_back("--runtime-arg");
    args.emplace_back(
        Concatenate({"-Xbootclasspathimagefds:", android::base::Join(bcp_image_fds, ':')}));
    args.emplace_back("--runtime-arg");
    args.emplace_back(
        Concatenate({"-Xbootclasspathoatfds:", android::base::Join(bcp_oat_fds, ':')}));
    args.emplace_back("--runtime-arg");
    args.emplace_back(
        Concatenate({"-Xbootclasspathvdexfds:", android::base::Join(bcp_vdex_fds, ':')}));
  }
}
std::string GetStagingLocation(const std::string& staging_dir, const std::string& path) {
  return Concatenate({staging_dir, "/", android::base::Basename(path)});
}
WARN_UNUSED bool CheckCompilationSpace() {
  static constexpr uint64_t kMinimumSpaceForCompilation = 48 * 1024 * 1024;
  uint64_t bytes_available;
  const std::string& art_apex_data_path = GetArtApexData();
  if (!GetFreeSpace(art_apex_data_path, &bytes_available)) {
    return false;
  }
  if (bytes_available < kMinimumSpaceForCompilation) {
    LOG(WARNING) << "Low space for " << QuotePath(art_apex_data_path) << " (" << bytes_available
                 << " bytes)";
    return false;
  }
  return true;
}
std::string GetSystemBootImageDir() { return GetAndroidRoot() + "/framework"; }
}
OnDeviceRefresh::OnDeviceRefresh(const OdrConfig& config)
    : OnDeviceRefresh(config,
                      Concatenate({config.GetArtifactDirectory(), "/", kCacheInfoFile}),
                      std::make_unique<ExecUtils>()) {}
OnDeviceRefresh::OnDeviceRefresh(const OdrConfig& config,
                                 const std::string& cache_info_filename,
                                 std::unique_ptr<ExecUtils> exec_utils)
    : config_{config},
      cache_info_filename_{cache_info_filename},
      start_time_{time(nullptr)},
      exec_utils_{std::move(exec_utils)} {
  for (const std::string& jar : android::base::Split(config_.GetDex2oatBootClasspath(), ":")) {
    boot_classpath_compilable_jars_.emplace_back(jar);
  }
  all_systemserver_jars_ = android::base::Split(config_.GetSystemServerClasspath(), ":");
  systemserver_classpath_jars_ = {all_systemserver_jars_.begin(), all_systemserver_jars_.end()};
  boot_classpath_jars_ = android::base::Split(config_.GetBootClasspath(), ":");
  std::string standalone_system_server_jars_str = config_.GetStandaloneSystemServerJars();
  if (!standalone_system_server_jars_str.empty()) {
    std::vector<std::string> standalone_systemserver_jars =
        android::base::Split(standalone_system_server_jars_str, ":");
    std::move(standalone_systemserver_jars.begin(),
              standalone_systemserver_jars.end(),
              std::back_inserter(all_systemserver_jars_));
  }
}
time_t OnDeviceRefresh::GetExecutionTimeUsed() const { return time(nullptr) - start_time_; }
time_t OnDeviceRefresh::GetExecutionTimeRemaining() const {
  return std::max(static_cast<time_t>(0),
                  kMaximumExecutionSeconds - GetExecutionTimeUsed());
}
time_t OnDeviceRefresh::GetSubprocessTimeout() const {
  return std::min(GetExecutionTimeRemaining(), kMaxChildProcessSeconds);
}
std::optional<std::vector<apex::ApexInfo>> OnDeviceRefresh::GetApexInfoList() const {
  std::optional<apex::ApexInfoList> info_list =
      apex::readApexInfoList(config_.GetApexInfoListFile().c_str());
  if (!info_list.has_value()) {
    return std::nullopt;
  }
  std::unordered_set<std::string_view> relevant_apexes;
  relevant_apexes.reserve(info_list->getApexInfo().size());
  for (const std::vector<std::string>* jar_list :
       {&boot_classpath_compilable_jars_, &all_systemserver_jars_, &boot_classpath_jars_}) {
    for (auto& jar : *jar_list) {
      std::string_view apex = ApexNameFromLocation(jar);
      if (!apex.empty()) {
        relevant_apexes.insert(apex);
      }
    }
  }
  relevant_apexes.insert("com.android.art");
  std::vector<apex::ApexInfo> filtered_info_list;
  std::copy_if(info_list->getApexInfo().begin(),
               info_list->getApexInfo().end(),
               std::back_inserter(filtered_info_list),
               [&](const apex::ApexInfo& info) {
                 return info.getIsActive() && relevant_apexes.count(info.getModuleName()) != 0;
               });
  return filtered_info_list;
}
std::optional<art_apex::CacheInfo> OnDeviceRefresh::ReadCacheInfo() const {
  return art_apex::read(cache_info_filename_.c_str());
}
Result<void> OnDeviceRefresh::WriteCacheInfo() const {
  if (OS::FileExists(cache_info_filename_.c_str())) {
    if (unlink(cache_info_filename_.c_str()) != 0) {
      return ErrnoErrorf("Failed to unlink() file {}", QuotePath(cache_info_filename_));
    }
  }
  const std::string dir_name = android::base::Dirname(cache_info_filename_);
  if (!EnsureDirectoryExists(dir_name)) {
    return Errorf("Could not create directory {}", QuotePath(dir_name));
  }
  std::optional<std::vector<apex::ApexInfo>> apex_info_list = GetApexInfoList();
  if (!apex_info_list.has_value()) {
    return Errorf("Could not update {}: no APEX info", QuotePath(cache_info_filename_));
  }
  std::optional<apex::ApexInfo> art_apex_info = GetArtApexInfo(apex_info_list.value());
  if (!art_apex_info.has_value()) {
    return Errorf("Could not update {}: no ART APEX info", QuotePath(cache_info_filename_));
  }
  art_apex::ModuleInfo art_module_info = GenerateModuleInfo(art_apex_info.value());
  std::vector<art_apex::ModuleInfo> module_info_list =
      GenerateModuleInfoList(apex_info_list.value());
  std::optional<std::vector<art_apex::Component>> bcp_components =
      GenerateBootClasspathComponents();
  if (!bcp_components.has_value()) {
    return Errorf("No boot classpath components.");
  }
  std::optional<std::vector<art_apex::Component>> bcp_compilable_components =
      GenerateBootClasspathCompilableComponents();
  if (!bcp_compilable_components.has_value()) {
    return Errorf("No boot classpath compilable components.");
  }
  std::optional<std::vector<art_apex::SystemServerComponent>> system_server_components =
      GenerateSystemServerComponents();
  if (!system_server_components.has_value()) {
    return Errorf("No system_server components.");
  }
  std::ofstream out(cache_info_filename_.c_str());
  if (out.fail()) {
    return Errorf("Cannot open {} for writing.", QuotePath(cache_info_filename_));
  }
  art_apex::CacheInfo info(
      {art_module_info},
      {art_apex::ModuleInfoList(module_info_list)},
      {art_apex::Classpath(bcp_components.value())},
      {art_apex::Classpath(bcp_compilable_components.value())},
      {art_apex::SystemServerComponents(system_server_components.value())},
      config_.GetCompilationOsMode() ? std::make_optional(true) : std::nullopt);
  art_apex::write(out, info);
  out.close();
  if (out.fail()) {
    return Errorf("Cannot write to {}", QuotePath(cache_info_filename_));
  }
  return {};
}
static void ReportNextBootAnimationProgress(uint32_t current_compilation,
                                            uint32_t number_of_compilations) {
  uint32_t value = (90 * current_compilation) / number_of_compilations;
  android::base::SetProperty("service.bootanim.progress", std::to_string(value));
}
std::vector<art_apex::Component> OnDeviceRefresh::GenerateBootClasspathComponents() const {
  return GenerateComponents(boot_classpath_jars_);
}
std::vector<art_apex::Component> OnDeviceRefresh::GenerateBootClasspathCompilableComponents()
    const {
  return GenerateComponents(boot_classpath_compilable_jars_);
}
std::vector<art_apex::SystemServerComponent> OnDeviceRefresh::GenerateSystemServerComponents()
    const {
  return GenerateComponents<art_apex::SystemServerComponent>(
      all_systemserver_jars_,
      [&](const std::string& path, uint64_t size, const std::string& checksum) {
        bool isInClasspath = ContainsElement(systemserver_classpath_jars_, path);
        return art_apex::SystemServerComponent{path, size, checksum, isInClasspath};
      });
}
std::string OnDeviceRefresh::GetBootImage(bool on_system, bool minimal) const {
  DCHECK(!on_system || !minimal);
  const char* basename = minimal ? kMinimalBootImageBasename : kFirstBootImageBasename;
  if (on_system) {
    return GetPrebuiltPrimaryBootImageDir() + "/" + basename;
  } else {
    return config_.GetArtifactDirectory() + "/" + basename;
  }
}
std::string OnDeviceRefresh::GetBootImagePath(bool on_system,
                                              bool minimal,
                                              const InstructionSet isa) const {
  return GetSystemImageFilename(GetBootImage(on_system, minimal).c_str(), isa);
}
std::string OnDeviceRefresh::GetSystemBootImageExtension() const {
  std::string art_root = GetArtRoot() + "/";
  auto it = std::find_if_not(
      boot_classpath_compilable_jars_.begin(),
      boot_classpath_compilable_jars_.end(),
      [&](const std::string& jar) { return android::base::StartsWith(jar, art_root); });
  CHECK(it != boot_classpath_compilable_jars_.end());
  return GetSystemBootImageDir() + "/" + GetBootImageComponentBasename(*it, false);
}
std::string OnDeviceRefresh::GetSystemBootImageExtensionPath(const InstructionSet isa) const {
  return GetSystemImageFilename(GetSystemBootImageExtension().c_str(), isa);
}
std::string OnDeviceRefresh::GetSystemServerImagePath(bool on_system,
                                                      const std::string& jar_path) const {
  if (on_system) {
    if (LocationIsOnApex(jar_path)) {
      return GetSystemOdexFilenameForApex(jar_path, config_.GetSystemServerIsa());
    }
    const std::string jar_name = android::base::Basename(jar_path);
    const std::string image_name = ReplaceFileExtension(jar_name, "art");
    const char* isa_str = GetInstructionSetString(config_.GetSystemServerIsa());
    return Concatenate({GetAndroidRoot(), "/framework/oat/", isa_str, "/", image_name});
  } else {
    const std::string image = GetApexDataImage(jar_path.c_str());
    return GetSystemImageFilename(image.c_str(), config_.GetSystemServerIsa());
  }
}
WARN_UNUSED bool OnDeviceRefresh::RemoveArtifactsDirectory() const {
  if (config_.GetDryRun()) {
    LOG(INFO) << "Directory " << QuotePath(config_.GetArtifactDirectory())
              << " and contents would be removed (dry-run).";
    return true;
  }
  return RemoveDirectory(config_.GetArtifactDirectory());
}
WARN_UNUSED bool OnDeviceRefresh::BootClasspathArtifactsExist(
    bool on_system,
    bool minimal,
    const InstructionSet isa,
            std::string* error_msg,
            std::vector<std::string>* checked_artifacts) const {
  std::string path = GetBootImagePath(on_system, minimal, isa);
  OdrArtifacts artifacts = OdrArtifacts::ForBootImage(path);
  if (!ArtifactsExist(artifacts, true, error_msg, checked_artifacts)) {
    return false;
  }
  if (on_system) {
    std::string extension_path = GetSystemBootImageExtensionPath(isa);
    OdrArtifacts extension_artifacts = OdrArtifacts::ForBootImage(extension_path);
    if (!ArtifactsExist(
            extension_artifacts, true, error_msg, checked_artifacts)) {
      return false;
    }
  }
  return true;
}
WARN_UNUSED bool OnDeviceRefresh::SystemServerArtifactsExist(
    bool on_system,
            std::string* error_msg,
            std::set<std::string>* jars_missing_artifacts,
            std::vector<std::string>* checked_artifacts) const {
  for (const std::string& jar_path : all_systemserver_jars_) {
    const std::string image_location = GetSystemServerImagePath(on_system, jar_path);
    const OdrArtifacts artifacts = OdrArtifacts::ForSystemServer(image_location);
    const bool check_art_file = !on_system;
    std::string error_msg_tmp;
    if (!ArtifactsExist(artifacts, check_art_file, &error_msg_tmp, checked_artifacts)) {
      jars_missing_artifacts->insert(jar_path);
      *error_msg = error_msg->empty() ? error_msg_tmp : *error_msg + "\n" + error_msg_tmp;
    }
  }
  return jars_missing_artifacts->empty();
}
WARN_UNUSED bool OnDeviceRefresh::CheckBootClasspathArtifactsAreUpToDate(
    OdrMetrics& metrics,
    const InstructionSet isa,
    const apex::ApexInfo& art_apex_info,
    const std::optional<art_apex::CacheInfo>& cache_info,
            std::vector<std::string>* checked_artifacts) const {
  if (art_apex_info.getIsFactory()) {
    LOG(INFO) << "Factory ART APEX mounted.";
    std::string error_msg;
    if (BootClasspathArtifactsExist( true, false, isa, &error_msg)) {
      return true;
    }
    LOG(INFO) << "Incomplete boot classpath artifacts on /system. " << error_msg;
    LOG(INFO) << "Checking cache.";
  }
  if (!cache_info.has_value()) {
    PLOG(INFO) << "No prior cache-info file: " << QuotePath(cache_info_filename_);
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return false;
  }
  const art_apex::ModuleInfo* cached_art_info = cache_info->getFirstArtModuleInfo();
  if (cached_art_info == nullptr) {
    LOG(INFO) << "Missing ART APEX info from cache-info.";
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return false;
  }
  if (cached_art_info->getVersionCode() != art_apex_info.getVersionCode()) {
    LOG(INFO) << "ART APEX version code mismatch (" << cached_art_info->getVersionCode()
              << " != " << art_apex_info.getVersionCode() << ").";
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return false;
  }
  if (cached_art_info->getVersionName() != art_apex_info.getVersionName()) {
    LOG(INFO) << "ART APEX version name mismatch (" << cached_art_info->getVersionName()
              << " != " << art_apex_info.getVersionName() << ").";
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return false;
  }
  const int64_t cached_art_last_update_millis =
      cached_art_info->hasLastUpdateMillis() ? cached_art_info->getLastUpdateMillis() : -1;
  if (cached_art_last_update_millis != art_apex_info.getLastUpdateMillis()) {
    LOG(INFO) << "ART APEX last update time mismatch (" << cached_art_last_update_millis
              << " != " << art_apex_info.getLastUpdateMillis() << ").";
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return false;
  }
  const std::vector<art_apex::Component> expected_bcp_compilable_components =
      GenerateBootClasspathCompilableComponents();
  if (expected_bcp_compilable_components.size() != 0 &&
      (!cache_info->hasDex2oatBootClasspath() ||
       !cache_info->getFirstDex2oatBootClasspath()->hasComponent())) {
    LOG(INFO) << "Missing Dex2oatBootClasspath components.";
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return false;
  }
  const std::vector<art_apex::Component>& bcp_compilable_components =
      cache_info->getFirstDex2oatBootClasspath()->getComponent();
  Result<void> result =
      CheckComponents(expected_bcp_compilable_components, bcp_compilable_components);
  if (!result.ok()) {
    LOG(INFO) << "Dex2OatClasspath components mismatch: " << result.error();
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return false;
  }
  std::string error_msg;
  if (!BootClasspathArtifactsExist(
                        false, false, isa, &error_msg, checked_artifacts)) {
    LOG(INFO) << "Incomplete boot classpath artifacts. " << error_msg;
    metrics.SetTrigger(OdrMetrics::Trigger::kMissingArtifacts);
    if (BootClasspathArtifactsExist(
                          false, true, isa, &error_msg, checked_artifacts)) {
      LOG(INFO) << "Found minimal boot classpath artifacts.";
    }
    return false;
  }
  return true;
}
bool OnDeviceRefresh::CheckSystemServerArtifactsAreUpToDate(
    OdrMetrics& metrics,
    const std::vector<apex::ApexInfo>& apex_info_list,
    const std::optional<art_apex::CacheInfo>& cache_info,
            std::set<std::string>* jars_to_compile,
            std::vector<std::string>* checked_artifacts) const {
  auto compile_all = [&, this]() {
    *jars_to_compile = AllSystemServerJars();
    return false;
  };
  std::set<std::string> jars_missing_artifacts_on_system;
  bool artifacts_on_system_up_to_date = false;
  if (std::all_of(apex_info_list.begin(),
                  apex_info_list.end(),
                  [](const apex::ApexInfo& apex_info) { return apex_info.getIsFactory(); })) {
    LOG(INFO) << "Factory APEXes mounted.";
    std::string error_msg;
    if (SystemServerArtifactsExist(
                          true, &error_msg, &jars_missing_artifacts_on_system)) {
      return true;
    }
    LOG(INFO) << "Incomplete system server artifacts on /system. " << error_msg;
    LOG(INFO) << "Checking cache.";
    artifacts_on_system_up_to_date = true;
  }
  if (!cache_info.has_value()) {
    PLOG(INFO) << "No prior cache-info file: " << QuotePath(cache_info_filename_);
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    if (artifacts_on_system_up_to_date) {
      *jars_to_compile = jars_missing_artifacts_on_system;
      return false;
    }
    return compile_all();
  }
  const art_apex::ModuleInfoList* cached_module_info_list = cache_info->getFirstModuleInfoList();
  if (cached_module_info_list == nullptr) {
    LOG(INFO) << "Missing APEX info list from cache-info.";
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return compile_all();
  }
  std::unordered_map<std::string, const art_apex::ModuleInfo*> cached_module_info_map;
  for (const art_apex::ModuleInfo& module_info : cached_module_info_list->getModuleInfo()) {
    if (!module_info.hasName()) {
      LOG(INFO) << "Unexpected module info from cache-info. Missing module name.";
      metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
      return compile_all();
    }
    cached_module_info_map[module_info.getName()] = &module_info;
  }
  for (const apex::ApexInfo& current_apex_info : apex_info_list) {
    auto& apex_name = current_apex_info.getModuleName();
    auto it = cached_module_info_map.find(apex_name);
    if (it == cached_module_info_map.end()) {
      LOG(INFO) << "Missing APEX info from cache-info (" << apex_name << ").";
      metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
      return compile_all();
    }
    const art_apex::ModuleInfo* cached_module_info = it->second;
    if (cached_module_info->getVersionCode() != current_apex_info.getVersionCode()) {
      LOG(INFO) << "APEX (" << apex_name << ") version code mismatch ("
                << cached_module_info->getVersionCode()
                << " != " << current_apex_info.getVersionCode() << ").";
      metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
      return compile_all();
    }
    if (cached_module_info->getVersionName() != current_apex_info.getVersionName()) {
      LOG(INFO) << "APEX (" << apex_name << ") version name mismatch ("
                << cached_module_info->getVersionName()
                << " != " << current_apex_info.getVersionName() << ").";
      metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
      return compile_all();
    }
    if (!cached_module_info->hasLastUpdateMillis() ||
        cached_module_info->getLastUpdateMillis() != current_apex_info.getLastUpdateMillis()) {
      LOG(INFO) << "APEX (" << apex_name << ") last update time mismatch ("
                << cached_module_info->getLastUpdateMillis()
                << " != " << current_apex_info.getLastUpdateMillis() << ").";
      metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
      return compile_all();
    }
  }
  const std::vector<art_apex::SystemServerComponent> expected_system_server_components =
      GenerateSystemServerComponents();
  if (expected_system_server_components.size() != 0 &&
      (!cache_info->hasSystemServerComponents() ||
       !cache_info->getFirstSystemServerComponents()->hasComponent())) {
    LOG(INFO) << "Missing SystemServerComponents.";
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return compile_all();
  }
  const std::vector<art_apex::SystemServerComponent>& system_server_components =
      cache_info->getFirstSystemServerComponents()->getComponent();
  Result<void> result =
      CheckSystemServerComponents(expected_system_server_components, system_server_components);
  if (!result.ok()) {
    LOG(INFO) << "SystemServerComponents mismatch: " << result.error();
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return compile_all();
  }
  const std::vector<art_apex::Component> expected_bcp_components =
      GenerateBootClasspathComponents();
  if (expected_bcp_components.size() != 0 &&
      (!cache_info->hasBootClasspath() || !cache_info->getFirstBootClasspath()->hasComponent())) {
    LOG(INFO) << "Missing BootClasspath components.";
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return false;
  }
  const std::vector<art_apex::Component>& bcp_components =
      cache_info->getFirstBootClasspath()->getComponent();
  result = CheckComponents(expected_bcp_components, bcp_components);
  if (!result.ok()) {
    LOG(INFO) << "BootClasspath components mismatch: " << result.error();
    metrics.SetTrigger(OdrMetrics::Trigger::kDexFilesChanged);
    return compile_all();
  }
  std::string error_msg;
  std::set<std::string> jars_missing_artifacts_on_data;
  if (!SystemServerArtifactsExist(
                        false, &error_msg, &jars_missing_artifacts_on_data, checked_artifacts)) {
    if (artifacts_on_system_up_to_date) {
      std::set_intersection(jars_missing_artifacts_on_system.begin(),
                            jars_missing_artifacts_on_system.end(),
                            jars_missing_artifacts_on_data.begin(),
                            jars_missing_artifacts_on_data.end(),
                            std::inserter(*jars_to_compile, jars_to_compile->end()));
      if (!jars_to_compile->empty()) {
        LOG(INFO) << "Incomplete system_server artifacts on /data. " << error_msg;
        metrics.SetTrigger(OdrMetrics::Trigger::kMissingArtifacts);
        return false;
      }
      LOG(INFO) << "Found the remaining system_server artifacts on /data.";
      return true;
    }
    LOG(INFO) << "Incomplete system_server artifacts. " << error_msg;
    metrics.SetTrigger(OdrMetrics::Trigger::kMissingArtifacts);
    *jars_to_compile = jars_missing_artifacts_on_data;
    return false;
  }
  return true;
}
Result<void> OnDeviceRefresh::CleanupArtifactDirectory(
    const std::vector<std::string>& artifacts_to_keep) const {
  const std::string& artifact_dir = config_.GetArtifactDirectory();
  std::unordered_set<std::string> artifact_set{artifacts_to_keep.begin(), artifacts_to_keep.end()};
  auto remove_artifact_dir = android::base::make_scope_guard([&]() {
    if (!RemoveDirectory(artifact_dir)) {
      LOG(ERROR) << "Failed to remove the artifact directory";
    }
  });
  std::vector<std::filesystem::directory_entry> entries;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(artifact_dir, ec)) {
    entries.push_back(entry);
  }
  if (ec) {
    return Errorf("Failed to iterate over entries in the artifact directory: {}", ec.message());
  }
  for (const std::filesystem::directory_entry& entry : entries) {
    std::string path = entry.path().string();
    if (entry.is_regular_file()) {
      if (!ContainsElement(artifact_set, path)) {
        LOG(INFO) << "Removing " << path;
        if (unlink(path.c_str()) != 0) {
          return ErrnoErrorf("Failed to remove file {}", QuotePath(path));
        }
      }
    } else if (!entry.is_directory()) {
      LOG(INFO) << "Removing " << path;
      if (unlink(path.c_str()) != 0) {
        return ErrnoErrorf("Failed to remove file {}", QuotePath(path));
      }
    }
  }
  remove_artifact_dir.Disable();
  return {};
}
Result<void> OnDeviceRefresh::RefreshExistingArtifacts() const {
  const std::string& artifact_dir = config_.GetArtifactDirectory();
  if (!OS::DirectoryExists(artifact_dir.c_str())) {
    return {};
  }
  std::vector<std::filesystem::directory_entry> entries;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(artifact_dir, ec)) {
    entries.push_back(entry);
  }
  if (ec) {
    return Errorf("Failed to iterate over entries in the artifact directory: {}", ec.message());
  }
  for (const std::filesystem::directory_entry& entry : entries) {
    std::string path = entry.path().string();
    if (entry.is_regular_file()) {
      LOG(INFO) << "Refreshing " << path;
      std::string content;
      if (!android::base::ReadFileToString(path, &content)) {
        return Errorf("Failed to read file {}", QuotePath(path));
      }
      if (unlink(path.c_str()) != 0) {
        return ErrnoErrorf("Failed to remove file {}", QuotePath(path));
      }
      if (!android::base::WriteStringToFile(content, path)) {
        return Errorf("Failed to write file {}", QuotePath(path));
      }
      if (chmod(path.c_str(), kFileMode) != 0) {
        return ErrnoErrorf("Failed to chmod file {}", QuotePath(path));
      }
    }
  }
  return {};
}
WARN_UNUSED ExitCode
OnDeviceRefresh::CheckArtifactsAreUpToDate(OdrMetrics& metrics,
                                                   CompilationOptions* compilation_options) const {
  metrics.SetStage(OdrMetrics::Stage::kCheck);
  auto cleanup_and_compile_all = [&, this]() {
    compilation_options->compile_boot_classpath_for_isas = config_.GetBootClasspathIsas();
    compilation_options->system_server_jars_to_compile = AllSystemServerJars();
    return RemoveArtifactsDirectory() ? ExitCode::kCompilationRequired : ExitCode::kCleanupFailed;
  };
  std::optional<std::vector<apex::ApexInfo>> apex_info_list = GetApexInfoList();
  if (!apex_info_list.has_value()) {
    LOG(ERROR) << "Could not get APEX info.";
    metrics.SetTrigger(OdrMetrics::Trigger::kUnknown);
    return cleanup_and_compile_all();
  }
  std::optional<apex::ApexInfo> art_apex_info = GetArtApexInfo(apex_info_list.value());
  if (!art_apex_info.has_value()) {
    LOG(ERROR) << "Could not get ART APEX info.";
    metrics.SetTrigger(OdrMetrics::Trigger::kUnknown);
    return cleanup_and_compile_all();
  }
  metrics.SetArtApexVersion(art_apex_info->getVersionCode());
  LOG(INFO) << "ART APEX version " << art_apex_info->getVersionCode();
  metrics.SetArtApexLastUpdateMillis(art_apex_info->getLastUpdateMillis());
  std::optional<art_apex::CacheInfo> cache_info = ReadCacheInfo();
  if (!cache_info.has_value() && OS::FileExists(cache_info_filename_.c_str())) {
    PLOG(ERROR) << "Failed to parse cache-info file: " << QuotePath(cache_info_filename_);
    metrics.SetTrigger(OdrMetrics::Trigger::kApexVersionMismatch);
    return cleanup_and_compile_all();
  }
  InstructionSet system_server_isa = config_.GetSystemServerIsa();
  std::vector<std::string> checked_artifacts;
  for (const InstructionSet isa : config_.GetBootClasspathIsas()) {
    if (!CheckBootClasspathArtifactsAreUpToDate(
            metrics, isa, art_apex_info.value(), cache_info, &checked_artifacts)) {
      compilation_options->compile_boot_classpath_for_isas.push_back(isa);
      if (isa == system_server_isa) {
        compilation_options->system_server_jars_to_compile = AllSystemServerJars();
      }
    }
  }
  if (compilation_options->system_server_jars_to_compile.empty()) {
    CheckSystemServerArtifactsAreUpToDate(metrics,
                                          apex_info_list.value(),
                                          cache_info,
                                          &compilation_options->system_server_jars_to_compile,
                                          &checked_artifacts);
  }
  bool compilation_required = (!compilation_options->compile_boot_classpath_for_isas.empty() ||
                               !compilation_options->system_server_jars_to_compile.empty());
  if (compilation_required && !config_.GetPartialCompilation()) {
    return cleanup_and_compile_all();
  }
  if (!checked_artifacts.empty()) {
    checked_artifacts.push_back(cache_info_filename_);
  }
  Result<void> result = CleanupArtifactDirectory(checked_artifacts);
  if (!result.ok()) {
    LOG(ERROR) << result.error();
    return ExitCode::kCleanupFailed;
  }
  return compilation_required ? ExitCode::kCompilationRequired : ExitCode::kOkay;
}
WARN_UNUSED bool OnDeviceRefresh::CompileBootClasspathArtifacts(
    const InstructionSet isa,
    const std::string& staging_dir,
    OdrMetrics& metrics,
    const std::function<void()>& on_dex2oat_success,
    bool minimal,
    std::string* error_msg) const {
  ScopedOdrCompilationTimer compilation_timer(metrics);
  std::vector<std::string> args;
  args.push_back(config_.GetDex2Oat());
  AddDex2OatCommonOptions(args);
  AddDex2OatDebugInfo(args);
  AddDex2OatInstructionSet(args, isa);
  if (!AddDex2OatConcurrencyArguments(args)) {
    return false;
  }
  std::vector<std::unique_ptr<File>> readonly_files_raii;
  const std::string art_boot_profile_file = GetArtRoot() + "/etc/boot-image.prof";
  const std::string framework_boot_profile_file = GetAndroidRoot() + "/etc/boot-image.prof";
  AddDex2OatProfileAndCompilerFilter(args, readonly_files_raii,
                                     {art_boot_profile_file, framework_boot_profile_file});
  args.emplace_back("--single-image");
  args.emplace_back(android::base::StringPrintf("--base=0x%08x", ART_BASE_ADDRESS));
  const std::string dirty_image_objects_file(GetAndroidRoot() + "/etc/dirty-image-objects");
  if (OS::FileExists(dirty_image_objects_file.c_str())) {
    std::unique_ptr<File> file(OS::OpenFileForReading(dirty_image_objects_file.c_str()));
    args.emplace_back(android::base::StringPrintf("--dirty-image-objects-fd=%d", file->Fd()));
    readonly_files_raii.push_back(std::move(file));
  } else {
    LOG(WARNING) << "Missing dirty objects file : " << QuotePath(dirty_image_objects_file);
  }
  std::vector<std::string> jars_to_compile = boot_classpath_compilable_jars_;
  if (minimal) {
    auto end =
        std::remove_if(jars_to_compile.begin(), jars_to_compile.end(), [](const std::string& jar) {
          return !android::base::StartsWith(jar, GetArtRoot());
        });
    jars_to_compile.erase(end, jars_to_compile.end());
  }
  for (const std::string& component : jars_to_compile) {
    std::string actual_path = AndroidRootRewrite(component);
    args.emplace_back("--dex-file=" + component);
    std::unique_ptr<File> file(OS::OpenFileForReading(actual_path.c_str()));
    args.emplace_back(android::base::StringPrintf("--dex-fd=%d", file->Fd()));
    readonly_files_raii.push_back(std::move(file));
  }
  args.emplace_back("--runtime-arg");
  args.emplace_back(Concatenate({"-Xbootclasspath:", android::base::Join(jars_to_compile, ":")}));
  if (!AddBootClasspathFds(args, readonly_files_raii, jars_to_compile)) {
    return false;
  }
  const std::string image_location = GetBootImagePath( false, minimal, isa);
  const OdrArtifacts artifacts = OdrArtifacts::ForBootImage(image_location);
  args.emplace_back("--oat-location=" + artifacts.OatPath());
  const std::pair<const std::string, const char*> location_kind_pairs[] = {
      std::make_pair(artifacts.ImagePath(), "image"),
      std::make_pair(artifacts.OatPath(), "oat"),
      std::make_pair(artifacts.VdexPath(), "output-vdex")};
  std::vector<std::unique_ptr<File>> staging_files;
  for (const auto& location_kind_pair : location_kind_pairs) {
    auto& [location, kind] = location_kind_pair;
    const std::string staging_location = GetStagingLocation(staging_dir, location);
    std::unique_ptr<File> staging_file(OS::CreateEmptyFile(staging_location.c_str()));
    if (staging_file == nullptr) {
      PLOG(ERROR) << "Failed to create " << kind << " file: " << staging_location;
      metrics.SetStatus(OdrMetrics::Status::kIoError);
      EraseFiles(staging_files);
      return false;
    }
    if (fchmod(staging_file->Fd(), S_IRUSR | S_IWUSR) != 0) {
      PLOG(ERROR) << "Could not set file mode on " << QuotePath(staging_location);
      metrics.SetStatus(OdrMetrics::Status::kIoError);
      EraseFiles(staging_files);
      return false;
    }
    args.emplace_back(android::base::StringPrintf("--%s-fd=%d", kind, staging_file->Fd()));
    staging_files.emplace_back(std::move(staging_file));
  }
  const std::string install_location = android::base::Dirname(image_location);
  if (!EnsureDirectoryExists(install_location)) {
    metrics.SetStatus(OdrMetrics::Status::kIoError);
    return false;
  }
  const time_t timeout = GetSubprocessTimeout();
  const std::string cmd_line = android::base::Join(args, ' ');
  LOG(INFO) << android::base::StringPrintf("Compiling boot classpath (%s%s): %s [timeout %lds]",
                                           GetInstructionSetString(isa),
                                           minimal ? ", minimal" : "",
                                           cmd_line.c_str(),
                                           timeout);
  if (config_.GetDryRun()) {
    LOG(INFO) << "Compilation skipped (dry-run).";
    return true;
  }
  bool timed_out = false;
  int dex2oat_exit_code = exec_utils_->ExecAndReturnCode(args, timeout, &timed_out, error_msg);
  if (dex2oat_exit_code != 0) {
    if (timed_out) {
      metrics.SetStatus(OdrMetrics::Status::kTimeLimitExceeded);
    } else {
      metrics.SetStatus(OdrMetrics::Status::kDex2OatError);
    }
    EraseFiles(staging_files);
    return false;
  }
  if (!MoveOrEraseFiles(staging_files, install_location)) {
    metrics.SetStatus(OdrMetrics::Status::kInstallFailed);
    return false;
  }
  on_dex2oat_success();
  return true;
}
WARN_UNUSED bool OnDeviceRefresh::CompileSystemServerArtifacts(
    const std::string& staging_dir,
    OdrMetrics& metrics,
    const std::set<std::string>& system_server_jars_to_compile,
    const std::function<void()>& on_dex2oat_success,
    std::string* error_msg) const {
  ScopedOdrCompilationTimer compilation_timer(metrics);
  std::vector<std::string> classloader_context;
  const std::string dex2oat = config_.GetDex2Oat();
  const InstructionSet isa = config_.GetSystemServerIsa();
  for (const std::string& jar : all_systemserver_jars_) {
    auto scope_guard = android::base::make_scope_guard([&]() {
      if (ContainsElement(systemserver_classpath_jars_, jar)) {
        classloader_context.emplace_back(jar);
      }
    });
    if (!ContainsElement(system_server_jars_to_compile, jar)) {
      continue;
    }
    std::vector<std::unique_ptr<File>> readonly_files_raii;
    std::vector<std::string> args;
    args.emplace_back(dex2oat);
    args.emplace_back("--dex-file=" + jar);
    std::string actual_jar_path = AndroidRootRewrite(jar);
    std::unique_ptr<File> dex_file(OS::OpenFileForReading(actual_jar_path.c_str()));
    args.emplace_back(android::base::StringPrintf("--dex-fd=%d", dex_file->Fd()));
    readonly_files_raii.push_back(std::move(dex_file));
    AddDex2OatCommonOptions(args);
    AddDex2OatDebugInfo(args);
    AddDex2OatInstructionSet(args, isa);
    if (!AddDex2OatConcurrencyArguments(args)) {
      return false;
    }
    const std::string jar_name(android::base::Basename(jar));
    const std::string profile = Concatenate({GetAndroidRoot(), "/framework/", jar_name, ".prof"});
    std::string compiler_filter = config_.GetSystemServerCompilerFilter();
    if (compiler_filter == "speed-profile") {
      AddDex2OatProfileAndCompilerFilter(args, readonly_files_raii, {profile});
    } else {
      args.emplace_back("--compiler-filter=" + compiler_filter);
    }
    const std::string image_location = GetSystemServerImagePath( false, jar);
    const std::string install_location = android::base::Dirname(image_location);
    if (!EnsureDirectoryExists(install_location)) {
      metrics.SetStatus(OdrMetrics::Status::kIoError);
      return false;
    }
    OdrArtifacts artifacts = OdrArtifacts::ForSystemServer(image_location);
    CHECK_EQ(artifacts.OatPath(), GetApexDataOdexFilename(jar.c_str(), isa));
    const std::pair<const std::string, const char*> location_kind_pairs[] = {
        std::make_pair(artifacts.ImagePath(), "app-image"),
        std::make_pair(artifacts.OatPath(), "oat"),
        std::make_pair(artifacts.VdexPath(), "output-vdex")};
    std::vector<std::unique_ptr<File>> staging_files;
    for (const auto& location_kind_pair : location_kind_pairs) {
      auto& [location, kind] = location_kind_pair;
      const std::string staging_location = GetStagingLocation(staging_dir, location);
      std::unique_ptr<File> staging_file(OS::CreateEmptyFile(staging_location.c_str()));
      if (staging_file == nullptr) {
        PLOG(ERROR) << "Failed to create " << kind << " file: " << staging_location;
        metrics.SetStatus(OdrMetrics::Status::kIoError);
        EraseFiles(staging_files);
        return false;
      }
      args.emplace_back(android::base::StringPrintf("--%s-fd=%d", kind, staging_file->Fd()));
      staging_files.emplace_back(std::move(staging_file));
    }
    args.emplace_back("--oat-location=" + artifacts.OatPath());
    args.emplace_back("--runtime-arg");
    args.emplace_back(Concatenate({"-Xbootclasspath:", config_.GetBootClasspath()}));
    auto bcp_jars = android::base::Split(config_.GetBootClasspath(), ":");
    if (!AddBootClasspathFds(args, readonly_files_raii, bcp_jars)) {
      return false;
    }
    std::string unused_error_msg;
    bool boot_image_on_system = !BootClasspathArtifactsExist(
                      false, false, isa, &unused_error_msg);
    AddCompiledBootClasspathFdsIfAny(
        args,
        readonly_files_raii,
        bcp_jars,
        isa,
        boot_image_on_system ? GetSystemBootImageDir() : config_.GetArtifactDirectory());
    args.emplace_back(
        Concatenate({"--boot-image=",
                     boot_image_on_system ? GetBootImage( true, false) +
                                                ":" + GetSystemBootImageExtension() :
                                            GetBootImage( false, false)}));
    const std::string context_path = android::base::Join(classloader_context, ':');
    if (art::ContainsElement(systemserver_classpath_jars_, jar)) {
      args.emplace_back("--class-loader-context=PCL[" + context_path + "]");
    } else {
      args.emplace_back("--class-loader-context=PCL[];PCL[" + context_path + "]");
    }
    if (!classloader_context.empty()) {
      std::vector<int> fds;
      for (const std::string& path : classloader_context) {
        std::string actual_path = AndroidRootRewrite(path);
        std::unique_ptr<File> file(OS::OpenFileForReading(actual_path.c_str()));
        if (!file->IsValid()) {
          PLOG(ERROR) << "Failed to open classloader context " << actual_path;
          metrics.SetStatus(OdrMetrics::Status::kIoError);
          return false;
        }
        fds.emplace_back(file->Fd());
        readonly_files_raii.emplace_back(std::move(file));
      }
      const std::string context_fds = android::base::Join(fds, ':');
      args.emplace_back(Concatenate({"--class-loader-context-fds=", context_fds}));
    }
    const time_t timeout = GetSubprocessTimeout();
    const std::string cmd_line = android::base::Join(args, ' ');
    LOG(INFO) << "Compiling " << jar << ": " << cmd_line << " [timeout " << timeout << "s]";
    if (config_.GetDryRun()) {
      LOG(INFO) << "Compilation skipped (dry-run).";
      return true;
    }
    bool timed_out = false;
    int dex2oat_exit_code = exec_utils_->ExecAndReturnCode(args, timeout, &timed_out, error_msg);
    if (dex2oat_exit_code != 0) {
      if (timed_out) {
        metrics.SetStatus(OdrMetrics::Status::kTimeLimitExceeded);
      } else {
        metrics.SetStatus(OdrMetrics::Status::kDex2OatError);
      }
      EraseFiles(staging_files);
      return false;
    }
    if (!MoveOrEraseFiles(staging_files, install_location)) {
      metrics.SetStatus(OdrMetrics::Status::kInstallFailed);
      return false;
    }
    on_dex2oat_success();
  }
  return true;
}
WARN_UNUSED ExitCode OnDeviceRefresh::Compile(OdrMetrics& metrics,
                                              const CompilationOptions& compilation_options) const {
  const char* staging_dir = nullptr;
  metrics.SetStage(OdrMetrics::Stage::kPreparation);
  if (config_.GetRefresh()) {
    Result<void> result = RefreshExistingArtifacts();
    if (!result.ok()) {
      LOG(ERROR) << "Failed to refresh existing artifacts: " << result.error();
      return ExitCode::kCleanupFailed;
    }
  }
  Result<void> result = WriteCacheInfo();
  if (!result.ok()) {
    LOG(ERROR) << result.error();
    return ExitCode::kCleanupFailed;
  }
  if (!config_.GetStagingDir().empty()) {
    staging_dir = config_.GetStagingDir().c_str();
  } else {
    if (PaletteCreateOdrefreshStagingDirectory(&staging_dir) != PALETTE_STATUS_OK) {
      metrics.SetStatus(OdrMetrics::Status::kStagingFailed);
      return ExitCode::kCleanupFailed;
    }
  }
  std::string error_msg;
  uint32_t dex2oat_invocation_count = 0;
  uint32_t total_dex2oat_invocation_count =
      compilation_options.compile_boot_classpath_for_isas.size() +
      compilation_options.system_server_jars_to_compile.size();
  ReportNextBootAnimationProgress(dex2oat_invocation_count, total_dex2oat_invocation_count);
  auto advance_animation_progress = [&]() {
    ReportNextBootAnimationProgress(++dex2oat_invocation_count, total_dex2oat_invocation_count);
  };
  const auto& bcp_instruction_sets = config_.GetBootClasspathIsas();
  DCHECK(!bcp_instruction_sets.empty() && bcp_instruction_sets.size() <= 2);
  bool full_compilation_failed = false;
  for (const InstructionSet isa : compilation_options.compile_boot_classpath_for_isas) {
    auto stage = (isa == bcp_instruction_sets.front()) ? OdrMetrics::Stage::kPrimaryBootClasspath :
                                                         OdrMetrics::Stage::kSecondaryBootClasspath;
    metrics.SetStage(stage);
    if (!config_.GetMinimal()) {
      if (CheckCompilationSpace()) {
        if (CompileBootClasspathArtifacts(isa,
                                          staging_dir,
                                          metrics,
                                          advance_animation_progress,
                                                      false,
                                          &error_msg)) {
          std::string path = GetBootImagePath( false, true, isa);
          OdrArtifacts artifacts = OdrArtifacts::ForBootImage(path);
          unlink(artifacts.ImagePath().c_str());
          unlink(artifacts.OatPath().c_str());
          unlink(artifacts.VdexPath().c_str());
          continue;
        }
        LOG(ERROR) << "Compilation of BCP failed: " << error_msg;
      } else {
        metrics.SetStatus(OdrMetrics::Status::kNoSpace);
      }
    }
    full_compilation_failed = true;
    std::string ignored_error_msg;
    if (BootClasspathArtifactsExist(
                          false, true, isa, &ignored_error_msg)) {
      continue;
    }
    if (CompileBootClasspathArtifacts(isa,
                                      staging_dir,
                                      metrics,
                                      advance_animation_progress,
                                                  true,
                                      &error_msg)) {
      continue;
    }
    LOG(ERROR) << "Compilation of minimal BCP failed: " << error_msg;
    if (!config_.GetDryRun() && !RemoveDirectory(staging_dir)) {
      return ExitCode::kCleanupFailed;
    }
    return ExitCode::kCompilationFailed;
  }
  if (full_compilation_failed) {
    if (!config_.GetDryRun() && !RemoveDirectory(staging_dir)) {
      return ExitCode::kCleanupFailed;
    }
    return ExitCode::kCompilationFailed;
  }
  if (!compilation_options.system_server_jars_to_compile.empty()) {
    metrics.SetStage(OdrMetrics::Stage::kSystemServerClasspath);
    if (!CheckCompilationSpace()) {
      metrics.SetStatus(OdrMetrics::Status::kNoSpace);
      return ExitCode::kCompilationFailed;
    }
    if (!CompileSystemServerArtifacts(staging_dir,
                                      metrics,
                                      compilation_options.system_server_jars_to_compile,
                                      advance_animation_progress,
                                      &error_msg)) {
      LOG(ERROR) << "Compilation of system_server failed: " << error_msg;
      if (!config_.GetDryRun() && !RemoveDirectory(staging_dir)) {
        return ExitCode::kCleanupFailed;
      }
      return ExitCode::kCompilationFailed;
    }
  }
  metrics.SetStage(OdrMetrics::Stage::kComplete);
  return ExitCode::kCompilationSuccess;
}
}
}
