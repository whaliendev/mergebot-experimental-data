#include "odr_metrics.h"
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iosfwd>
#include <optional>
#include <ostream>
#include <string>
#include <android-base/logging.h>
#include <base/os.h>
#include <base/string_view_cpp20.h>
#include <odr_fs_utils.h>
#include <odr_metrics_record.h>
namespace art {
namespace odrefresh {
OdrMetrics::OdrMetrics(const std::string& cache_directory, const std::string& metrics_file)
    : cache_directory_(cache_directory), metrics_file_(metrics_file) {
  DCHECK(StartsWith(metrics_file_, "/"));
  if (OS::FileExists(metrics_file.c_str())) {
    if (unlink(metrics_file.c_str()) != 0) {
      PLOG(ERROR) << "Failed to remove metrics file '" << metrics_file << "'";
    }
  }
  if (!EnsureDirectoryExists(cache_directory)) {
    LOG(WARNING) << "Cache directory '" << cache_directory << "' could not be created.";
  }
  cache_space_free_start_mib_ = GetFreeSpaceMiB(cache_directory);
}
OdrMetrics::~OdrMetrics() {
  CaptureSpaceFreeEnd();
  if (enabled_) {
    WriteToFile(metrics_file_, this);
  }
}
void OdrMetrics::CaptureSpaceFreeEnd() {
  cache_space_free_end_mib_ = GetFreeSpaceMiB(cache_directory_);
}
void OdrMetrics::SetCompilationTime(int32_t millis) {
  switch (stage_) {
    case Stage::kPrimaryBootClasspath:
      primary_bcp_compilation_millis_ = millis;
      break;
    case Stage::kSecondaryBootClasspath:
      secondary_bcp_compilation_millis_ = millis;
      break;
    case Stage::kSystemServerClasspath:
      system_server_compilation_millis_ = millis;
      break;
    case Stage::kCheck:
    case Stage::kComplete:
    case Stage::kPreparation:
    case Stage::kUnknown:
      LOG(FATAL) << "Unexpected stage " << stage_ << " when setting compilation time";
  }
}
void OdrMetrics::SetDex2OatResult(const ExecResult& dex2oat_result) {
  switch (stage_) {
    case Stage::kPrimaryBootClasspath:
      primary_bcp_dex2oat_result_ = dex2oat_result;
      break;
    case Stage::kSecondaryBootClasspath:
      secondary_bcp_dex2oat_result_ = dex2oat_result;
      break;
    case Stage::kSystemServerClasspath:
      system_server_dex2oat_result_ = dex2oat_result;
      break;
    case Stage::kCheck:
    case Stage::kComplete:
    case Stage::kPreparation:
    case Stage::kUnknown:
      LOG(FATAL) << "Unexpected stage " << stage_ << " when setting dex2oat result";
  }
}
public:
void SetStage(Stage stage) { stage_ = stage; }
int32_t OdrMetrics::GetFreeSpaceMiB(const std::string& path) {
  static constexpr uint32_t kBytesPerMiB = 1024 * 1024;
  static constexpr uint64_t kNominalMaximumCacheBytes = 1024 * kBytesPerMiB;
  uint64_t used_space_bytes;
  if (!GetUsedSpace(path, &used_space_bytes)) {
    used_space_bytes = 0;
  }
  uint64_t nominal_free_space_bytes = kNominalMaximumCacheBytes - used_space_bytes;
  uint64_t free_space_bytes;
  if (!GetFreeSpace(path, &free_space_bytes)) {
    free_space_bytes = kNominalMaximumCacheBytes;
  }
  uint64_t free_space_mib = std::min(free_space_bytes, nominal_free_space_bytes) / kBytesPerMiB;
  return static_cast<int32_t>(free_space_mib);
}
OdrMetricsRecord OdrMetrics::ToRecord() const {
  return {
      .odrefresh_metrics_version = kOdrefreshMetricsVersion,
      .art_apex_version = art_apex_version_,
      .trigger = static_cast<int32_t>(trigger_),
      .stage_reached = static_cast<int32_t>(stage_),
      .status = static_cast<int32_t>(status_),
      .cache_space_free_start_mib = cache_space_free_start_mib_,
      .cache_space_free_end_mib = cache_space_free_end_mib_,
      .primary_bcp_compilation_millis = primary_bcp_compilation_millis_,
      .secondary_bcp_compilation_millis = secondary_bcp_compilation_millis_,
      .system_server_compilation_millis = system_server_compilation_millis_,
  };
}
void OdrMetrics::WriteToFile(const std::string& path, const OdrMetrics* metrics) {
  OdrMetricsRecord record = metrics->ToRecord();
  const android::base::Result<void>& result = record.WriteToFile(path);
  if (!result.ok()) {
    LOG(ERROR) << "Failed to report metrics to file: " << path
               << ", error: " << result.error().message();
  }
}
}
}
