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
#include "base/os.h"
#include "com_android_apex.h"
#include "com_android_art.h"
#include "exec_utils.h"
#include "odr_artifacts.h"
#include "odr_config.h"
#include "odr_metrics.h"
#include "odrefresh/odrefresh.h"
namespace art {
namespace odrefresh {
void TestOnlyEnableMainlineExtension();
class OnDeviceRefresh;
struct BootImages {
  static constexpr int kMaxCount = 2;
  bool primary_boot_image : 1;
  bool boot_image_mainline_extension : 1;
  int Count() const;
};
struct CompilationOptions {
  std::vector<std::pair<InstructionSet, BootImages>> boot_images_to_generate_for_isas;
  std::set<std::string> system_server_jars_to_compile;
  static CompilationOptions CompileAll(const OnDeviceRefresh& odr);
  int CompilationUnitCount() const;
};
struct CompilationResult {
  OdrMetrics::Status status = OdrMetrics::Status::kOK;
  std::string error_msg;
  int64_t elapsed_time_ms = 0;
  std::optional<ExecResult> dex2oat_result;
  static CompilationResult Ok() { return {}; }
  static CompilationResult Dex2oatOk(int64_t elapsed_time_ms, const ExecResult& dex2oat_result) {
    return {.elapsed_time_ms = elapsed_time_ms, .dex2oat_result = dex2oat_result};
  }
  static CompilationResult Error(OdrMetrics::Status status, const std::string& error_msg) {
    return {.status = status, .error_msg = error_msg};
  }
  static CompilationResult Dex2oatError(const std::string& error_msg,
                                        int64_t elapsed_time_ms,
                                        const ExecResult& dex2oat_result) {
    return {.status = OdrMetrics::Status::kDex2OatError,
            .error_msg = error_msg,
            .elapsed_time_ms = elapsed_time_ms,
            .dex2oat_result = dex2oat_result};
  }
  bool IsOk() { return status == OdrMetrics::Status::kOK; }
  void Merge(const CompilationResult& other) {
    elapsed_time_ms += other.elapsed_time_ms;
    if (status == OdrMetrics::Status::kOK) {
      status = other.status;
      error_msg = other.error_msg;
      dex2oat_result = other.dex2oat_result;
    }
  }
 private:
  CompilationResult()
};
class PreconditionCheckResult {
 private:
  PreconditionCheckResult(std::optional<OdrMetrics::Trigger> trigger,
                          bool primary_boot_image_ok,
                          bool boot_image_mainline_extension_ok,
                          bool system_server_ok)
      : trigger_(trigger),
        primary_boot_image_ok_(primary_boot_image_ok),
        boot_image_mainline_extension_ok_(boot_image_mainline_extension_ok),
        system_server_ok_(system_server_ok) {}
 public:
  static PreconditionCheckResult NoneOk(OdrMetrics::Trigger trigger) {
    return PreconditionCheckResult(trigger,
                                                             false,
                                                                        false,
                                                        false);
  }
  static PreconditionCheckResult BootImageMainlineExtensionNotOk(OdrMetrics::Trigger trigger) {
    return PreconditionCheckResult(trigger,
                                                             true,
                                                                        false,
                                                        false);
  }
  static PreconditionCheckResult SystemServerNotOk(OdrMetrics::Trigger trigger) {
    return PreconditionCheckResult(trigger,
                                                             true,
                                                                        true,
                                                        false);
  }
  static PreconditionCheckResult AllOk() {
    return PreconditionCheckResult( std::nullopt,
                                                             true,
                                                                        true,
                                                        true);
  }
  bool IsAllOk() const { return !trigger_.has_value(); }
  OdrMetrics::Trigger GetTrigger() const { return trigger_.value(); }
  bool IsPrimaryBootImageOk() const { return primary_boot_image_ok_; }
  bool IsBootImageMainlineExtensionOk() const { return boot_image_mainline_extension_ok_; }
  bool IsSystemServerOk() const { return system_server_ok_; }
 private:
  std::optional<OdrMetrics::Trigger> trigger_;
  bool primary_boot_image_ok_;
  bool boot_image_mainline_extension_ok_;
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
  const OdrConfig& Config() const { return config_; }
 private:
  time_t GetExecutionTimeUsed() const;
  time_t GetExecutionTimeRemaining() const;
  time_t GetSubprocessTimeout() const;
  std::optional<std::vector<com::android::apex::ApexInfo>> GetApexInfoList() const;
  android::base::Result<com::android::art::CacheInfo> ReadCacheInfo() const;
  android::base::Result<void> WriteCacheInfo() const;
  std::vector<com::android::art::Component> GenerateBootClasspathComponents() const;
  std::vector<com::android::art::Component> GenerateDex2oatBootClasspathComponents() const;
  std::vector<com::android::art::SystemServerComponent> GenerateSystemServerComponents() const;
  std::vector<std::string> GetArtBcpJars() const;
  std::vector<std::string> GetFrameworkBcpJars() const;
  std::vector<std::string> GetMainlineBcpJars() const;
  std::string GetPrimaryBootImage(bool on_system, bool minimal) const;
  std::string GetPrimaryBootImagePath(bool on_system, bool minimal, InstructionSet isa) const;
  std::string GetSystemBootImageFrameworkExtension() const;
  std::string GetSystemBootImageFrameworkExtensionPath(InstructionSet isa) const;
  std::string GetBootImageMainlineExtension(bool on_system) const;
  std::string GetBootImageMainlineExtensionPath(bool on_system, InstructionSet isa) const;
  std::vector<std::string> GetBestBootImages(InstructionSet isa,
                                             bool include_mainline_extension) const;
  std::string GetSystemServerImagePath(bool on_system, const std::string& jar_path) const;
  android::base::Result<void> CleanupArtifactDirectory(
      OdrMetrics& metrics, const std::vector<std::string>& artifacts_to_keep) const;
  android::base::Result<void> RefreshExistingArtifacts() const;
  WARN_UNUSED bool PrimaryBootImageExist(
      bool on_system,
      bool minimal,
      InstructionSet isa,
              std::string* error_msg,
              std::vector<std::string>* checked_artifacts = nullptr) const;
  WARN_UNUSED bool BootImageMainlineExtensionExist(
      bool on_system,
      InstructionSet isa,
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
  WARN_UNUSED BootImages
  CheckBootClasspathArtifactsAreUpToDate(OdrMetrics& metrics,
                                         InstructionSet isa,
                                         const PreconditionCheckResult& system_result,
                                         const PreconditionCheckResult& data_result,
                                                 std::vector<std::string>* checked_artifacts) const;
  WARN_UNUSED std::set<std::string> CheckSystemServerArtifactsAreUpToDate(
      OdrMetrics& metrics,
      const PreconditionCheckResult& system_result,
      const PreconditionCheckResult& data_result,
              std::vector<std::string>* checked_artifacts) const;
  WARN_UNUSED CompilationResult
  RunDex2oat(const std::string& staging_dir,
             const std::string& debug_message,
             InstructionSet isa,
             const std::vector<std::string>& dex_files,
             const std::vector<std::string>& boot_classpath,
             const std::vector<std::string>& input_boot_images,
             const OdrArtifacts& artifacts,
             const std::vector<std::string>& extra_args,
                       std::vector<std::unique_ptr<File>>& readonly_files_raii) const;
  WARN_UNUSED CompilationResult
  RunDex2oatForBootClasspath(const std::string& staging_dir,
                             const std::string& debug_name,
                             InstructionSet isa,
                             const std::vector<std::string>& dex_files,
                             const std::vector<std::string>& boot_classpath,
                             const std::vector<std::string>& input_boot_images,
                             const std::string& output_path) const;
  WARN_UNUSED CompilationResult
  CompileBootClasspath(const std::string& staging_dir,
                       InstructionSet isa,
                       BootImages boot_images,
                       const std::function<void()>& on_dex2oat_success) const;
  WARN_UNUSED CompilationResult
  RunDex2oatForSystemServer(const std::string& staging_dir,
                            const std::string& dex_file,
                            const std::vector<std::string>& classloader_context) const;
  WARN_UNUSED CompilationResult
  CompileSystemServer(const std::string& staging_dir,
                      const std::set<std::string>& system_server_jars_to_compile,
                      const std::function<void()>& on_dex2oat_success) const;
  const OdrConfig& config_;
  const std::string cache_info_filename_;
  std::vector<std::string> dex2oat_boot_classpath_jars_;
  std::vector<std::string> boot_classpath_jars_;
  std::unordered_set<std::string> systemserver_classpath_jars_;
  std::vector<std::string> all_systemserver_jars_;
  const time_t start_time_;
  std::unique_ptr<ExecUtils> exec_utils_;
  DISALLOW_COPY_AND_ASSIGN(OnDeviceRefresh);
};
}
}
#endif
