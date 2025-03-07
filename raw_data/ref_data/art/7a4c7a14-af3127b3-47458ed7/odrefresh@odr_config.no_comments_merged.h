#ifndef ART_ODREFRESH_ODR_CONFIG_H_
#define ART_ODREFRESH_ODR_CONFIG_H_ 
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "android-base/file.h"
#include "android-base/no_destructor.h"
#include "android-base/strings.h"
#include "arch/instruction_set.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "log/log.h"
#include "odr_common.h"
#include "odrefresh/odrefresh.h"
namespace art {
namespace odrefresh {
constexpr const char* kCheckedSystemPropertyPrefixes[]{"dalvik.vm.", "ro.dalvik.vm."};
static constexpr char kSystemPropertySystemServerCompilerFilterOverride[] =
    "persist.device_config.runtime_native_boot.systemservercompilerfilter_override";
const std::unordered_set<std::string> kIgnoredSystemProperties{
    "dalvik.vm.dex2oat-cpu-set",
    "dalvik.vm.dex2oat-threads",
    "dalvik.vm.boot-dex2oat-cpu-set",
    "dalvik.vm.boot-dex2oat-threads",
    "dalvik.vm.restore-dex2oat-cpu-set",
    "dalvik.vm.restore-dex2oat-threads",
    "dalvik.vm.background-dex2oat-cpu-set",
    "dalvik.vm.background-dex2oat-threads"};
struct SystemPropertyConfig {
  const char* name;
  const char* default_value;
};
const android::base::NoDestructor<std::vector<SystemPropertyConfig>> kSystemProperties{
    {SystemPropertyConfig{.name = "persist.device_config.runtime_native_boot.enable_uffd_gc",
                          .default_value = ""},
     SystemPropertyConfig{.name = kPhDisableCompactDex, .default_value = "false"},
     SystemPropertyConfig{.name = kSystemPropertySystemServerCompilerFilterOverride,
                          .default_value = ""}}};
enum class ZygoteKind : uint8_t {
  kZygote32 = 0,
  kZygote32_64 = 1,
  kZygote64_32 = 2,
  kZygote64 = 3
};
class OdrConfig final {
 private:
  std::string apex_info_list_file_;
  std::string art_bin_dir_;
  std::string dex2oat_;
  std::string dex2oat_boot_classpath_;
  bool dry_run_;
  std::optional<bool> refresh_;
  std::optional<bool> partial_compilation_;
  InstructionSet isa_;
  std::string program_name_;
  std::string system_server_classpath_;
  std::string boot_image_compiler_filter_;
  std::string system_server_compiler_filter_;
  ZygoteKind zygote_kind_;
  std::string boot_classpath_;
  std::string artifact_dir_;
  std::string standalone_system_server_jars_;
  bool compilation_os_mode_ = false;
  bool minimal_ = false;
  std::unordered_map<std::string, std::string> system_properties_;
  std::string staging_dir_;
 public:
  explicit OdrConfig(const char* program_name)
    : dry_run_(false),
      isa_(InstructionSet::kNone),
      program_name_(android::base::Basename(program_name)),
      artifact_dir_(GetApexDataDalvikCacheDirectory(InstructionSet::kNone)) {
  }
  const std::string& GetApexInfoListFile() const { return apex_info_list_file_; }
  std::vector<InstructionSet> GetBootClasspathIsas() const {
    const auto [isa32, isa64] = GetPotentialInstructionSets();
    switch (zygote_kind_) {
      case ZygoteKind::kZygote32:
        return {isa32};
      case ZygoteKind::kZygote32_64:
      case ZygoteKind::kZygote64_32:
        return {isa32, isa64};
      case ZygoteKind::kZygote64:
        return {isa64};
    }
  }
  InstructionSet GetSystemServerIsa() const {
    const auto [isa32, isa64] = GetPotentialInstructionSets();
    switch (zygote_kind_) {
      case ZygoteKind::kZygote32:
      case ZygoteKind::kZygote32_64:
        return isa32;
      case ZygoteKind::kZygote64_32:
      case ZygoteKind::kZygote64:
        return isa64;
    }
  }
  const std::string& GetDex2oatBootClasspath() const { return dex2oat_boot_classpath_; }
  const std::string& GetArtifactDirectory() const { return artifact_dir_; }
  std::string GetDex2Oat() const {
    const char* prefix = UseDebugBinaries() ? "dex2oatd" : "dex2oat";
    const char* suffix = "";
    if (kIsTargetBuild) {
      switch (zygote_kind_) {
        case ZygoteKind::kZygote32:
          suffix = "32";
          break;
        case ZygoteKind::kZygote32_64:
        case ZygoteKind::kZygote64_32:
        case ZygoteKind::kZygote64:
          suffix = "64";
          break;
      }
    }
    return art_bin_dir_ + '/' + prefix + suffix;
  }
  bool GetDryRun() const { return dry_run_; }
  bool HasPartialCompilation() const {
    return partial_compilation_.has_value();
  }
  bool GetPartialCompilation() const {
    return partial_compilation_.value_or(true);
  }
  bool GetRefresh() const {
    return refresh_.value_or(true);
  }
  const std::string& GetSystemServerClasspath() const {
    return system_server_classpath_;
  }
  const std::string& GetBootImageCompilerFilter() const {
    return boot_image_compiler_filter_;
  }
  const std::string& GetSystemServerCompilerFilter() const {
    return system_server_compiler_filter_;
  }
  const std::string& GetStagingDir() const {
    return staging_dir_;
  }
  bool GetCompilationOsMode() const { return compilation_os_mode_; }
  bool GetMinimal() const { return minimal_; }
  const std::unordered_map<std::string, std::string>& GetSystemProperties() const {
    return system_properties_;
  }
  void SetApexInfoListFile(const std::string& file_path) { apex_info_list_file_ = file_path; }
  void SetArtBinDir(const std::string& art_bin_dir) { art_bin_dir_ = art_bin_dir; }
  void SetDex2oatBootclasspath(const std::string& classpath) {
    dex2oat_boot_classpath_ = classpath;
  }
  void SetArtifactDirectory(const std::string& artifact_dir) {
    artifact_dir_ = artifact_dir;
  }
  void SetDryRun() { dry_run_ = true; }
  void SetPartialCompilation(bool value) {
    partial_compilation_ = value;
  }
  void SetRefresh(bool value) {
    refresh_ = value;
  }
  void SetIsa(const InstructionSet isa) { isa_ = isa; }
  void SetSystemServerClasspath(const std::string& classpath) {
    system_server_classpath_ = classpath;
  }
  void SetBootImageCompilerFilter(const std::string& filter) {
    boot_image_compiler_filter_ = filter;
  }
  void SetSystemServerCompilerFilter(const std::string& filter) {
    system_server_compiler_filter_ = filter;
  }
  void SetZygoteKind(ZygoteKind zygote_kind) { zygote_kind_ = zygote_kind; }
  const std::string& GetBootClasspath() const { return boot_classpath_; }
  void SetBootClasspath(const std::string& classpath) { boot_classpath_ = classpath; }
  void SetStagingDir(const std::string& staging_dir) {
    staging_dir_ = staging_dir;
  }
  const std::string& GetStandaloneSystemServerJars() const {
    return standalone_system_server_jars_;
  }
  void SetStandaloneSystemServerJars(const std::string& jars) {
    standalone_system_server_jars_ = jars;
  }
  void SetCompilationOsMode(bool value) { compilation_os_mode_ = value; }
  void SetMinimal(bool value) { minimal_ = value; }
  std::unordered_map<std::string, std::string>* MutableSystemProperties() {
    return &system_properties_;
  }
 private:
  std::pair<InstructionSet, InstructionSet> GetPotentialInstructionSets() const {
    switch (isa_) {
      case art::InstructionSet::kArm:
      case art::InstructionSet::kArm64:
        return std::make_pair(art::InstructionSet::kArm, art::InstructionSet::kArm64);
      case art::InstructionSet::kX86:
      case art::InstructionSet::kX86_64:
        return std::make_pair(art::InstructionSet::kX86, art::InstructionSet::kX86_64);
      case art::InstructionSet::kRiscv64:
      case art::InstructionSet::kThumb2:
      case art::InstructionSet::kNone:
        LOG(FATAL) << "Invalid instruction set " << isa_;
        return std::make_pair(art::InstructionSet::kNone, art::InstructionSet::kNone);
    }
  }
  bool UseDebugBinaries() const { return program_name_ == "odrefreshd"; }
  OdrConfig() = delete;
  OdrConfig(const OdrConfig&) = delete;
  OdrConfig& operator=(const OdrConfig&) = delete;
};
}
}
#endif
