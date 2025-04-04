#ifndef ART_ODREFRESH_ODR_METRICS_H_
#define ART_ODREFRESH_ODR_METRICS_H_ 
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include "base/macros.h"
#include "exec_utils.h"
#include "odr_metrics_record.h"
namespace art {
namespace odrefresh {
class OdrMetrics final {
public:
  enum class Stage : uint8_t {
    kUnknown = 0,
    kCheck = 10,
    kPreparation = 20,
    kPrimaryBootClasspath = 30,
    kSecondaryBootClasspath = 40,
    kSystemServerClasspath = 50,
    kComplete = 60,
  };
  enum class Status : uint8_t {
    kUnknown = 0,
    kOK = 1,
    kNoSpace = 2,
    kIoError = 3,
    kDex2OatError = 4,
    kStagingFailed = 6,
    kInstallFailed = 7,
    kDalvikCachePermissionDenied = 8,
  };
  enum class Trigger : uint8_t {
    kUnknown = 0,
    kApexVersionMismatch = 1,
    kDexFilesChanged = 2,
    kMissingArtifacts = 3,
  };
  explicit OdrMetrics(const std::string& cache_directory,
                      const std::string& metrics_file = kOdrefreshMetricsFile);
  ~OdrMetrics();
  void SetEnabled(bool value) { enabled_ = value; }
  int64_t GetArtApexVersion() const { return art_apex_version_; }
  void SetArtApexVersion(int64_t version) { art_apex_version_ = version; }
  int64_t GetArtApexLastUpdateMillis() const { return art_apex_last_update_millis_; }
  void SetArtApexLastUpdateMillis(int64_t last_update_millis) {
    art_apex_last_update_millis_ = last_update_millis;
  }
  Trigger GetTrigger() const { return trigger_; }
  void SetTrigger(const Trigger trigger) { trigger_ = trigger; }
  void SetStatus(const Status status) { status_ = status; }
  void SetDex2OatResult(const ExecResult& dex2oat_result);
  void CaptureSpaceFreeEnd();
  OdrMetricsRecord ToRecord() const;
private:
  OdrMetrics(const OdrMetrics&)
  OdrMetrics operator=(const OdrMetrics&) = delete
  static int32_t GetFreeSpaceMiB(const std::string& path);
  static void WriteToFile(const std::string& path, const OdrMetrics* metrics);
  void SetCompilationTime(int32_t millis);
  const std::string cache_directory_;
  const std::string metrics_file_;
  bool enabled_ = false;
  int64_t art_apex_version_ = 0;
  int64_t art_apex_last_update_millis_ = 0;
Trigger trigger_ = Trigger::kUnknown; Stage stage_ = Stage::kUnknown;
  Status status_ = Status::kUnknown;
  int32_t cache_space_free_start_mib_ = 0;
  int32_t cache_space_free_end_mib_ = 0;
  int32_t primary_bcp_compilation_millis_ = 0;
  std::optional<ExecResult> primary_bcp_dex2oat_result_;
  int32_t secondary_bcp_compilation_millis_ = 0;
  std::optional<ExecResult> secondary_bcp_dex2oat_result_;
  int32_t system_server_compilation_millis_ = 0;
  std::optional<ExecResult> system_server_dex2oat_result_;
  friend class ScopedOdrCompilationTimer;
};
class ScopedOdrCompilationTimer final {
public:
  explicit ScopedOdrCompilationTimer(OdrMetrics& metrics) :
    metrics_(metrics), start_(std::chrono::steady_clock::now()) {}
  ~ScopedOdrCompilationTimer() {
    auto elapsed_time = std::chrono::steady_clock::now() - start_;
    auto elapsed_millis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time);
    metrics_.SetCompilationTime(static_cast<int32_t>(elapsed_millis.count()));
  }
private:
  OdrMetrics& metrics_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
  DISALLOW_ALLOCATION();
};
std::ostream& operator<<(std::ostream& os, OdrMetrics::Status status);
std::ostream& operator<<(std::ostream& os, OdrMetrics::Stage stage);
std::ostream& operator<<(std::ostream& os, OdrMetrics::Trigger trigger);
}
}
#endif
