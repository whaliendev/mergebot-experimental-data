#ifndef ART_ODREFRESH_ODREFRESH_H_
#define ART_ODREFRESH_ODREFRESH_H_ 
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#include "android-base/result.h"
#include "com_android_apex.h"
#include "com_android_art.h"
#include "exec_utils.h"
#include "odr_artifacts.h"
#include "odr_config.h"
#include "odr_metrics.h"
#include "odrefresh/odrefresh.h"
namespace art {
namespace odrefresh {
struct CompilationOptions {
  std::vector<InstructionSet> compile_boot_classpath_for_isas;
  std::set<std::string> system_server_jars_to_compile;
};
class PreconditionCheckResult {
 public:
  static PreconditionCheckResult NoneOk(OdrMetrics::Trigger trigger) {
    return PreconditionCheckResult(trigger,
                                                         false,
                                                        false);
  }
  static PreconditionCheckResult SystemServerNotOk(OdrMetrics::Trigger trigger) {
    return PreconditionCheckResult(trigger,
                                                         true,
                                                        false);
  }
  static PreconditionCheckResult AllOk() {
    return PreconditionCheckResult( std::nullopt,
                                                         true,
                                                        true);
  }
  bool IsAllOk() const { return !trigger_.has_value(); }
  OdrMetrics::Trigger GetTrigger() const { return trigger_.value(); }
  bool IsBootClasspathOk() const { return boot_classpath_ok_; }
  bool IsSystemServerOk() const { return system_server_ok_; }
 private:
  PreconditionCheckResult(std::optional<OdrMetrics::Trigger> trigger,
                          bool boot_classpath_ok,
                          bool system_server_ok)
      : trigger_(trigger),
        boot_classpath_ok_(boot_classpath_ok),
        system_server_ok_(system_server_ok) {}
  std::optional<OdrMetrics::Trigger> trigger_;
  bool boot_classpath_ok_;
  bool system_server_ok_;
};
class OnDeviceRefresh final {
 public:
  explicit OnDeviceRefresh(const OdrConfig& config);
  OnDeviceRefresh(const OdrConfig& config,
                  const std::string& cache_info_filename,
                  std::unique_ptr<ExecUtils> exec_utils);
  WARN_UNUSED ExitCode
  CheckArtifactsAreUpToDate(OdrMetrics& metrics,
                                    CompilationOptions* compilation_options) const;
  WARN_UNUSED ExitCode Compile(OdrMetrics& metrics,
                               const CompilationOptions& compilation_options) const;
  WARN_UNUSED bool RemoveArtifactsDirectory() const;
  std::set<std::string> AllSystemServerJars() const {
    return {all_systemserver_jars_.begin(), all_systemserver_jars_.end()};
  }
 private:
  time_t GetExecutionTimeUsed() const;
  time_t GetExecutionTimeRemaining() const;
  time_t GetSubprocessTimeout() const;
  std::optional<std::vector<com::android::apex::ApexInfo>> GetApexInfoList() const;
  android::base::Result<com::android::art::CacheInfo> ReadCacheInfo() const;
  android::base::Result<void> WriteCacheInfo() const;
  std::vector<com::android::art::Component> GenerateBootClasspathComponents() const;
  std::vector<com::android::art::Component> GenerateBootClasspathCompilableComponents() const;
  std::vector<com::android::art::SystemServerComponent> GenerateSystemServerComponents() const;
  std::string GetBootImage(bool on_system, bool minimal) const;
  std::string GetBootImagePath(bool on_system, bool minimal, const InstructionSet isa) const;
  std::string GetSystemBootImageExtension() const;
  std::string GetSystemBootImageExtensionPath(const InstructionSet isa) const;
  std::string GetSystemServerImagePath(bool on_system, const std::string& jar_path) const;
  android::base::Result<void> CleanupArtifactDirectory(
      OdrMetrics& metrics, const std::vector<std::string>& artifacts_to_keep) const;
  android::base::Result<void> RefreshExistingArtifacts() const;
  WARN_UNUSED bool BootClasspathArtifactsExist(
      bool on_system,
      bool minimal,
      const InstructionSet isa,
              std::string* error_msg,
              std::vector<std::string>* checked_artifacts = nullptr) const;
  bool SystemServerArtifactsExist(
      bool on_system,
              std::string* error_msg,
              std::set<std::string>* jars_missing_artifacts,
              std::vector<std::string>* checked_artifacts = nullptr) const;
  WARN_UNUSED bool CheckSystemPropertiesAreDefault() const;
  WARN_UNUSED bool CheckSystemPropertiesHaveNotChanged(
      const com::android::art::CacheInfo& cache_info) const;
  WARN_UNUSED bool CheckBuildUserfaultFdGc() const;
  WARN_UNUSED PreconditionCheckResult
  CheckPreconditionForSystem(const std::vector<com::android::apex::ApexInfo>& apex_info_list) const;
  WARN_UNUSED PreconditionCheckResult
  CheckPreconditionForData(const std::vector<com::android::apex::ApexInfo>& apex_info_list) const;
  WARN_UNUSED bool CheckBootClasspathArtifactsAreUpToDate(
      OdrMetrics& metrics,
      const InstructionSet isa,
      const PreconditionCheckResult& system_result,
      const PreconditionCheckResult& data_result,
              std::vector<std::string>* checked_artifacts) const;
  bool CheckSystemServerArtifactsAreUpToDate(
      OdrMetrics& metrics,
      const PreconditionCheckResult& system_result,
      const PreconditionCheckResult& data_result,
              std::set<std::string>* jars_to_compile,
              std::vector<std::string>* checked_artifacts) const;
  WARN_UNUSED bool CompileBootClasspathArtifacts(const InstructionSet isa,
                                                 const std::string& staging_dir,
                                                 OdrMetrics& metrics,
                                                 const std::function<void()>& on_dex2oat_success,
                                                 bool minimal,
                                                 std::string* error_msg) const;
  WARN_UNUSED bool CompileSystemServerArtifacts(
      const std::string& staging_dir,
      OdrMetrics& metrics,
      const std::set<std::string>& system_server_jars_to_compile,
      const std::function<void()>& on_dex2oat_success,
      std::string* error_msg) const;
  const OdrConfig& config_;
  const std::string cache_info_filename_;
  std::vector<std::string> boot_classpath_compilable_jars_;
  std::unordered_set<std::string> systemserver_classpath_jars_;
  std::vector<std::string> boot_classpath_jars_;
  std::vector<std::string> all_systemserver_jars_;
  const time_t start_time_;
  std::unique_ptr<ExecUtils> exec_utils_;
  DISALLOW_COPY_AND_ASSIGN(OnDeviceRefresh);
};
}
}
#endif
