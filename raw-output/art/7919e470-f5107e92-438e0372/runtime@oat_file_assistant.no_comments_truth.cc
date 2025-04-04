#include "oat_file_assistant.h"
#include <sys/stat.h>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/properties.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "arch/instruction_set.h"
#include "base/array_ref.h"
#include "base/compiler_filter.h"
#include "base/file_utils.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/string_view_cpp20.h"
#include "base/systrace.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file_loader.h"
#include "exec_utils.h"
#include "fmt/format.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "image.h"
#include "oat.h"
#include "oat_file_assistant_context.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "vdex_file.h"
#include "zlib.h"
namespace art {
using ::android::base::ConsumePrefix;
using ::android::base::StringPrintf;
using ::fmt::literals::operator""_format;
static constexpr const char* kAnonymousDexPrefix = "Anonymous-DexFile@";
static constexpr const char* kVdexExtension = ".vdex";
static constexpr const char* kDmExtension = ".dm";
std::ostream& operator<<(std::ostream& stream, const OatFileAssistant::OatStatus status) {
  switch (status) {
    case OatFileAssistant::kOatCannotOpen:
      stream << "kOatCannotOpen";
      break;
    case OatFileAssistant::kOatDexOutOfDate:
      stream << "kOatDexOutOfDate";
      break;
    case OatFileAssistant::kOatBootImageOutOfDate:
      stream << "kOatBootImageOutOfDate";
      break;
    case OatFileAssistant::kOatUpToDate:
      stream << "kOatUpToDate";
      break;
    case OatFileAssistant::kOatContextOutOfDate:
      stream << "kOaContextOutOfDate";
      break;
  }
  return stream;
}
OatFileAssistant::OatFileAssistant(const char* dex_location,
                                   const InstructionSet isa,
                                   ClassLoaderContext* context,
                                   bool load_executable,
                                   bool only_load_trusted_executable,
                                   OatFileAssistantContext* ofa_context)
    : OatFileAssistant(dex_location,
                       isa,
                       context,
                       load_executable,
                       only_load_trusted_executable,
                       ofa_context,
                                   -1,
                                  -1,
                                  -1) {}
OatFileAssistant::OatFileAssistant(const char* dex_location,
                                   const InstructionSet isa,
                                   ClassLoaderContext* context,
                                   bool load_executable,
                                   bool only_load_trusted_executable,
                                   OatFileAssistantContext* ofa_context,
                                   int vdex_fd,
                                   int oat_fd,
                                   int zip_fd)
    : context_(context),
      isa_(isa),
      load_executable_(load_executable),
      only_load_trusted_executable_(only_load_trusted_executable),
      odex_(this, false),
      oat_(this, true),
      vdex_for_odex_(this, false),
      vdex_for_oat_(this, true),
      dm_for_odex_(this, false),
      dm_for_oat_(this, true),
      zip_fd_(zip_fd) {
  CHECK(dex_location != nullptr) << "OatFileAssistant: null dex location";
  CHECK_IMPLIES(load_executable, context != nullptr) << "Loading executable without a context";
  if (zip_fd < 0) {
    CHECK_LE(oat_fd, 0) << "zip_fd must be provided with valid oat_fd. zip_fd=" << zip_fd
                        << " oat_fd=" << oat_fd;
    CHECK_LE(vdex_fd, 0) << "zip_fd must be provided with valid vdex_fd. zip_fd=" << zip_fd
                         << " vdex_fd=" << vdex_fd;
    CHECK(!UseFdToReadFiles());
  } else {
    CHECK(UseFdToReadFiles());
  }
  dex_location_.assign(dex_location);
  Runtime* runtime = Runtime::Current();
  if (load_executable_ && runtime == nullptr) {
    LOG(WARNING) << "OatFileAssistant: Load executable specified, "
                 << "but no active runtime is found. Will not attempt to load executable.";
    load_executable_ = false;
  }
  if (load_executable_ && isa != kRuntimeISA) {
    LOG(WARNING) << "OatFileAssistant: Load executable specified, "
                 << "but isa is not kRuntimeISA. Will not attempt to load executable.";
    load_executable_ = false;
  }
  if (ofa_context == nullptr) {
    CHECK(runtime != nullptr) << "runtime_options is not provided, and no active runtime is found.";
    ofa_context_ = std::make_unique<OatFileAssistantContext>(runtime);
  } else {
    ofa_context_ = ofa_context;
  }
  if (runtime == nullptr) {
    MemMap::Init();
  }
  std::string error_msg;
  std::string odex_file_name;
  if (DexLocationToOdexFilename(dex_location_, isa_, &odex_file_name, &error_msg)) {
    odex_.Reset(odex_file_name, UseFdToReadFiles(), zip_fd, vdex_fd, oat_fd);
    std::string vdex_file_name = GetVdexFilename(odex_file_name);
    vdex_for_odex_.Reset(vdex_file_name,
                         UseFdToReadFiles(),
                         DupCloexec(zip_fd),
                         DupCloexec(vdex_fd),
                         DupCloexec(oat_fd));
    std::string dm_file_name = GetDmFilename(dex_location_);
    dm_for_odex_.Reset(dm_file_name,
                       UseFdToReadFiles(),
                       DupCloexec(zip_fd),
                       DupCloexec(vdex_fd),
                       DupCloexec(oat_fd));
  } else {
    LOG(WARNING) << "Failed to determine odex file name: " << error_msg;
  }
  if (!UseFdToReadFiles()) {
    std::string oat_file_name;
    if (DexLocationToOatFilename(dex_location_,
                                 isa_,
                                 GetRuntimeOptions().deny_art_apex_data_files,
                                 &oat_file_name,
                                 &error_msg)) {
      oat_.Reset(oat_file_name, false);
      std::string vdex_file_name = GetVdexFilename(oat_file_name);
      vdex_for_oat_.Reset(vdex_file_name, UseFdToReadFiles(), zip_fd, vdex_fd, oat_fd);
      std::string dm_file_name = GetDmFilename(dex_location);
      dm_for_oat_.Reset(dm_file_name, UseFdToReadFiles(), zip_fd, vdex_fd, oat_fd);
    } else {
      LOG(WARNING) << "Failed to determine oat file name for dex location " << dex_location_ << ": "
                   << error_msg;
    }
  }
  size_t pos = dex_location_.rfind('/');
  if (pos == std::string::npos) {
    LOG(WARNING) << "Failed to determine dex file parent directory: " << dex_location_;
  } else if (!UseFdToReadFiles()) {
    std::string parent = dex_location_.substr(0, pos);
    if (access(parent.c_str(), W_OK) == 0) {
      dex_parent_writable_ = true;
    } else {
      VLOG(oat) << "Dex parent of " << dex_location_ << " is not writable: " << strerror(errno);
    }
  }
}
std::unique_ptr<OatFileAssistant> OatFileAssistant::Create(
    const std::string& filename,
    const std::string& isa_str,
    const std::optional<std::string>& context_str,
    bool load_executable,
    bool only_load_trusted_executable,
    OatFileAssistantContext* ofa_context,
            std::unique_ptr<ClassLoaderContext>* context,
            std::string* error_msg) {
  InstructionSet isa = GetInstructionSetFromString(isa_str.c_str());
  if (isa == InstructionSet::kNone) {
    *error_msg = StringPrintf("Instruction set '%s' is invalid", isa_str.c_str());
    return nullptr;
  }
  std::unique_ptr<ClassLoaderContext> tmp_context = nullptr;
  if (context_str.has_value()) {
    tmp_context = ClassLoaderContext::Create(context_str.value());
    if (tmp_context == nullptr) {
      *error_msg = StringPrintf("Class loader context '%s' is invalid", context_str->c_str());
      return nullptr;
    }
    if (!tmp_context->OpenDexFiles(android::base::Dirname(filename),
                                                   {},
                                                           true)) {
      *error_msg =
          StringPrintf("Failed to load class loader context files for '%s' with context '%s'",
                       filename.c_str(),
                       context_str->c_str());
      return nullptr;
    }
  }
  auto assistant = std::make_unique<OatFileAssistant>(filename.c_str(),
                                                      isa,
                                                      tmp_context.get(),
                                                      load_executable,
                                                      only_load_trusted_executable,
                                                      ofa_context);
  *context = std::move(tmp_context);
  return assistant;
}
bool OatFileAssistant::UseFdToReadFiles() { return zip_fd_ >= 0; }
bool OatFileAssistant::IsInBootClassPath() {
  for (const std::string& boot_class_path_location :
       GetRuntimeOptions().boot_class_path_locations) {
    if (boot_class_path_location == dex_location_) {
      VLOG(oat) << "Dex location " << dex_location_ << " is in boot class path";
      return true;
    }
  }
  return false;
}
OatFileAssistant::DexOptTrigger OatFileAssistant::GetDexOptTrigger(
    CompilerFilter::Filter target_compiler_filter, bool profile_changed, bool downgrade) {
  if (downgrade) {
    return DexOptTrigger{.targetFilterIsWorse = true};
  }
  DexOptTrigger dexopt_trigger{
      .targetFilterIsBetter = true, .primaryBootImageBecomesUsable = true, .needExtraction = true};
  if (profile_changed && CompilerFilter::DependsOnProfile(target_compiler_filter)) {
    dexopt_trigger.targetFilterIsSame = true;
  }
  return dexopt_trigger;
}
int OatFileAssistant::GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                                      bool profile_changed,
                                      bool downgrade) {
  OatFileInfo& info = GetBestInfo();
  if (info.CheckDisableCompactDexExperiment()) {
    return kDex2OatFromScratch;
  }
  DexOptNeeded dexopt_needed = info.GetDexOptNeeded(
      target_compiler_filter, GetDexOptTrigger(target_compiler_filter, profile_changed, downgrade));
  if (dexopt_needed != kNoDexOptNeeded && (&info == &dm_for_oat_ || &info == &dm_for_odex_)) {
    return kDex2OatFromScratch;
  }
  if (info.IsOatLocation() || dexopt_needed == kDex2OatFromScratch) {
    return dexopt_needed;
  }
  return -dexopt_needed;
}
bool OatFileAssistant::GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                                       DexOptTrigger dexopt_trigger,
                                               DexOptStatus* dexopt_status) {
  OatFileInfo& info = GetBestInfo();
  if (info.CheckDisableCompactDexExperiment()) {
    dexopt_status->location_ = kLocationNoneOrError;
    return true;
  }
  DexOptNeeded dexopt_needed = info.GetDexOptNeeded(target_compiler_filter, dexopt_trigger);
  if (info.IsUseable()) {
    if (&info == &dm_for_oat_ || &info == &dm_for_odex_) {
      dexopt_status->location_ = kLocationDm;
    } else if (info.IsOatLocation()) {
      dexopt_status->location_ = kLocationOat;
    } else {
      dexopt_status->location_ = kLocationOdex;
    }
  } else {
    dexopt_status->location_ = kLocationNoneOrError;
  }
  return dexopt_needed != kNoDexOptNeeded;
}
bool OatFileAssistant::IsUpToDate() { return GetBestInfo().Status() == kOatUpToDate; }
std::unique_ptr<OatFile> OatFileAssistant::GetBestOatFile() {
  return GetBestInfo().ReleaseFileForUse();
}
std::string OatFileAssistant::GetStatusDump() {
  std::ostringstream status;
  bool oat_file_exists = false;
  bool odex_file_exists = false;
  if (oat_.Status() != kOatCannotOpen) {
    CHECK(oat_.Filename() != nullptr);
    oat_file_exists = true;
    status << *oat_.Filename() << "[status=" << oat_.Status() << ", ";
    const OatFile* file = oat_.GetFile();
    if (file == nullptr) {
      status << "vdex-only";
    } else {
      status << "compilation_filter=" << CompilerFilter::NameOfFilter(file->GetCompilerFilter());
    }
  }
  if (odex_.Status() != kOatCannotOpen) {
    CHECK(odex_.Filename() != nullptr);
    odex_file_exists = true;
    if (oat_file_exists) {
      status << "] ";
    }
    status << *odex_.Filename() << "[status=" << odex_.Status() << ", ";
    const OatFile* file = odex_.GetFile();
    if (file == nullptr) {
      status << "vdex-only";
    } else {
      status << "compilation_filter=" << CompilerFilter::NameOfFilter(file->GetCompilerFilter());
    }
  }
  if (!oat_file_exists && !odex_file_exists) {
    status << "invalid[";
  }
  status << "]";
  return status.str();
}
std::vector<std::unique_ptr<const DexFile>> OatFileAssistant::LoadDexFiles(
    const OatFile& oat_file, const char* dex_location) {
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (LoadDexFiles(oat_file, dex_location, &dex_files)) {
    return dex_files;
  } else {
    return std::vector<std::unique_ptr<const DexFile>>();
  }
}
bool OatFileAssistant::LoadDexFiles(const OatFile& oat_file,
                                    const std::string& dex_location,
                                    std::vector<std::unique_ptr<const DexFile>>* out_dex_files) {
  std::string error_msg;
  const OatDexFile* oat_dex_file =
      oat_file.GetOatDexFile(dex_location.c_str(), nullptr, &error_msg);
  if (oat_dex_file == nullptr) {
    LOG(WARNING) << error_msg;
    return false;
  }
  std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
  if (dex_file.get() == nullptr) {
    LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
    return false;
  }
  out_dex_files->push_back(std::move(dex_file));
  for (size_t i = 1;; i++) {
    std::string multidex_dex_location = DexFileLoader::GetMultiDexLocation(i, dex_location.c_str());
    oat_dex_file = oat_file.GetOatDexFile(multidex_dex_location.c_str(), nullptr);
    if (oat_dex_file == nullptr) {
      break;
    }
    dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      LOG(WARNING) << "Failed to open dex file from oat dex file: " << error_msg;
      return false;
    }
    out_dex_files->push_back(std::move(dex_file));
  }
  return true;
}
std::optional<bool> OatFileAssistant::HasDexFiles(std::string* error_msg) {
  ScopedTrace trace("HasDexFiles");
  const std::vector<std::uint32_t>* checksums = GetRequiredDexChecksums(error_msg);
  if (checksums == nullptr) {
    return std::nullopt;
  }
  return !checksums->empty();
}
OatFileAssistant::OatStatus OatFileAssistant::OdexFileStatus() { return odex_.Status(); }
OatFileAssistant::OatStatus OatFileAssistant::OatFileStatus() { return oat_.Status(); }
bool OatFileAssistant::DexChecksumUpToDate(const OatFile& file, std::string* error_msg) {
  if (!file.ContainsDexCode()) {
    return true;
  }
  ScopedTrace trace("DexChecksumUpToDate");
  const std::vector<uint32_t>* required_dex_checksums = GetRequiredDexChecksums(error_msg);
  if (required_dex_checksums == nullptr) {
    return false;
  }
  if (required_dex_checksums->empty()) {
    LOG(WARNING) << "Required dex checksums not found. Assuming dex checksums are up to date.";
    return true;
  }
  uint32_t number_of_dex_files = file.GetOatHeader().GetDexFileCount();
  if (required_dex_checksums->size() != number_of_dex_files) {
    *error_msg = StringPrintf(
        "expected %zu dex files but found %u", required_dex_checksums->size(), number_of_dex_files);
    return false;
  }
  for (uint32_t i = 0; i < number_of_dex_files; i++) {
    std::string dex = DexFileLoader::GetMultiDexLocation(i, dex_location_.c_str());
    uint32_t expected_checksum = (*required_dex_checksums)[i];
    const OatDexFile* oat_dex_file = file.GetOatDexFile(dex.c_str(), nullptr);
    if (oat_dex_file == nullptr) {
      *error_msg = StringPrintf("failed to find %s in %s", dex.c_str(), file.GetLocation().c_str());
      return false;
    }
    uint32_t actual_checksum = oat_dex_file->GetDexFileLocationChecksum();
    if (expected_checksum != actual_checksum) {
      VLOG(oat) << "Dex checksum does not match for dex: " << dex
                << ". Expected: " << expected_checksum << ", Actual: " << actual_checksum;
      return false;
    }
  }
  return true;
}
OatFileAssistant::OatStatus OatFileAssistant::GivenOatFileStatus(const OatFile& file) {
  if (file.GetOatHeader().IsConcurrentCopying() != gUseReadBarrier) {
    return kOatCannotOpen;
  }
  std::string error_msg;
  if (!DexChecksumUpToDate(file, &error_msg)) {
    LOG(ERROR) << error_msg;
    return kOatDexOutOfDate;
  }
  CompilerFilter::Filter current_compiler_filter = file.GetCompilerFilter();
  if (file.IsBackedByVdexOnly()) {
    VLOG(oat) << "Image checksum test skipped for vdex file " << file.GetLocation();
  } else if (CompilerFilter::DependsOnImageChecksum(current_compiler_filter)) {
    if (!ValidateBootClassPathChecksums(file)) {
      VLOG(oat) << "Oat image checksum does not match image checksum.";
      return kOatBootImageOutOfDate;
    }
    if (!gc::space::ImageSpace::ValidateApexVersions(
            file.GetOatHeader(),
            GetOatFileAssistantContext()->GetApexVersions(),
            file.GetLocation(),
            &error_msg)) {
      VLOG(oat) << error_msg;
      return kOatBootImageOutOfDate;
    }
  } else {
    VLOG(oat) << "Image checksum test skipped for compiler filter " << current_compiler_filter;
  }
  if (only_load_trusted_executable_ &&
      !LocationIsTrusted(file.GetLocation(), !GetRuntimeOptions().deny_art_apex_data_files) &&
      file.ContainsDexCode() && ZipFileOnlyContainsUncompressedDex()) {
    LOG(ERROR) << "Not loading " << dex_location_
               << ": oat file has dex code, but APK has uncompressed dex code";
    return kOatDexOutOfDate;
  }
  if (!ClassLoaderContextIsOkay(file)) {
    return kOatContextOutOfDate;
  }
  return kOatUpToDate;
}
bool OatFileAssistant::AnonymousDexVdexLocation(const std::vector<const DexFile::Header*>& headers,
                                                InstructionSet isa,
                                                          std::string* dex_location,
                                                          std::string* vdex_filename) {
  DCHECK(Runtime::Current() != nullptr);
  uint32_t checksum = adler32(0L, Z_NULL, 0);
  for (const DexFile::Header* header : headers) {
    checksum = adler32_combine(
        checksum, header->checksum_, header->file_size_ - DexFile::kNumNonChecksumBytes);
  }
  const std::string& data_dir = Runtime::Current()->GetProcessDataDirectory();
  if (data_dir.empty() || Runtime::Current()->IsZygote()) {
    *dex_location = StringPrintf("%s%u", kAnonymousDexPrefix, checksum);
    return false;
  }
  *dex_location = StringPrintf("%s/%s%u.jar", data_dir.c_str(), kAnonymousDexPrefix, checksum);
  std::string odex_filename;
  std::string error_msg;
  if (!DexLocationToOdexFilename(*dex_location, isa, &odex_filename, &error_msg)) {
    LOG(WARNING) << "Could not get odex filename for " << *dex_location << ": " << error_msg;
    return false;
  }
  *vdex_filename = GetVdexFilename(odex_filename);
  return true;
}
bool OatFileAssistant::IsAnonymousVdexBasename(const std::string& basename) {
  DCHECK(basename.find('/') == std::string::npos);
  if (basename.size() < strlen(kAnonymousDexPrefix) + strlen(kVdexExtension) + 1 ||
      !android::base::StartsWith(basename, kAnonymousDexPrefix) ||
      !android::base::EndsWith(basename, kVdexExtension)) {
    return false;
  }
  for (size_t i = strlen(kAnonymousDexPrefix); i < basename.size() - strlen(kVdexExtension); ++i) {
    if (!std::isdigit(basename[i])) {
      return false;
    }
  }
  return true;
}
bool OatFileAssistant::DexLocationToOdexFilename(const std::string& location,
                                                 InstructionSet isa,
                                                 std::string* odex_filename,
                                                 std::string* error_msg) {
  CHECK(odex_filename != nullptr);
  CHECK(error_msg != nullptr);
  if (LocationIsOnApex(location)) {
    const std::string system_file = GetSystemOdexFilenameForApex(location, isa);
    if (OS::FileExists(system_file.c_str(), true)) {
      *odex_filename = system_file;
      return true;
    } else if (errno != ENOENT) {
      PLOG(ERROR) << "Could not check odex file " << system_file;
    }
  }
  size_t pos = location.rfind('/');
  if (pos == std::string::npos) {
    *error_msg = "Dex location " + location + " has no directory.";
    return false;
  }
  std::string dir = location.substr(0, pos + 1);
  dir += "oat";
  dir += "/" + std::string(GetInstructionSetString(isa));
  std::string file = location.substr(pos + 1);
  pos = file.rfind('.');
  if (pos == std::string::npos) {
    *error_msg = "Dex location " + location + " has no extension.";
    return false;
  }
  std::string base = file.substr(0, pos);
  *odex_filename = dir + "/" + base + ".odex";
  return true;
}
bool OatFileAssistant::DexLocationToOatFilename(const std::string& location,
                                                InstructionSet isa,
                                                std::string* oat_filename,
                                                std::string* error_msg) {
  DCHECK(Runtime::Current() != nullptr);
  return DexLocationToOatFilename(
      location, isa, Runtime::Current()->DenyArtApexDataFiles(), oat_filename, error_msg);
}
bool OatFileAssistant::DexLocationToOatFilename(const std::string& location,
                                                InstructionSet isa,
                                                bool deny_art_apex_data_files,
                                                std::string* oat_filename,
                                                std::string* error_msg) {
  CHECK(oat_filename != nullptr);
  CHECK(error_msg != nullptr);
  const std::string apex_data_file = GetApexDataOdexFilename(location, isa);
  if (!apex_data_file.empty() && !deny_art_apex_data_files) {
    if (OS::FileExists(apex_data_file.c_str(), true)) {
      *oat_filename = apex_data_file;
      return true;
    } else if (errno != ENOENT) {
      PLOG(ERROR) << "Could not check odex file " << apex_data_file;
    }
  }
  if (GetAndroidDataSafe(error_msg).empty()) {
    *error_msg = "GetAndroidDataSafe failed: " + *error_msg;
    return false;
  }
  std::string dalvik_cache;
  bool have_android_data = false;
  bool dalvik_cache_exists = false;
  bool is_global_cache = false;
  GetDalvikCache(GetInstructionSetString(isa),
                                      true,
                 &dalvik_cache,
                 &have_android_data,
                 &dalvik_cache_exists,
                 &is_global_cache);
  if (!dalvik_cache_exists) {
    *error_msg = "Dalvik cache directory does not exist";
    return false;
  }
  return GetDalvikCacheFilename(location.c_str(), dalvik_cache.c_str(), oat_filename, error_msg);
}
const std::vector<uint32_t>* OatFileAssistant::GetRequiredDexChecksums(std::string* error_msg) {
  if (!required_dex_checksums_attempted_) {
    required_dex_checksums_attempted_ = true;
    std::vector<uint32_t> checksums;
    std::vector<std::string> dex_locations_ignored;
    if (ArtDexFileLoader::GetMultiDexChecksums(dex_location_.c_str(),
                                               &checksums,
                                               &dex_locations_ignored,
                                               &cached_required_dex_checksums_error_,
                                               zip_fd_,
                                               &zip_file_only_contains_uncompressed_dex_)) {
      if (checksums.empty()) {
        VLOG(oat) << "No dex file found in " << dex_location_;
      }
      cached_required_dex_checksums_ = std::move(checksums);
    }
  }
  if (cached_required_dex_checksums_.has_value()) {
    return &cached_required_dex_checksums_.value();
  } else {
    *error_msg = cached_required_dex_checksums_error_;
    DCHECK(!error_msg->empty());
    return nullptr;
  }
}
bool OatFileAssistant::ValidateBootClassPathChecksums(OatFileAssistantContext* ofa_context,
                                                      InstructionSet isa,
                                                      std::string_view oat_checksums,
                                                      std::string_view oat_boot_class_path,
                                                              std::string* error_msg) {
  const std::vector<std::string>& bcp_locations =
      ofa_context->GetRuntimeOptions().boot_class_path_locations;
  if (oat_checksums.empty() || oat_boot_class_path.empty()) {
    *error_msg = oat_checksums.empty() ? "Empty checksums" : "Empty boot class path";
    return false;
  }
  size_t oat_bcp_size = gc::space::ImageSpace::CheckAndCountBCPComponents(
      oat_boot_class_path, ArrayRef<const std::string>(bcp_locations), error_msg);
  if (oat_bcp_size == static_cast<size_t>(-1)) {
    DCHECK(!error_msg->empty());
    return false;
  }
  DCHECK_LE(oat_bcp_size, bcp_locations.size());
  size_t bcp_index = 0;
  size_t boot_image_index = 0;
  bool found_d = false;
  while (bcp_index < oat_bcp_size) {
    static_assert(gc::space::ImageSpace::kImageChecksumPrefix == 'i', "Format prefix check");
    static_assert(gc::space::ImageSpace::kDexFileChecksumPrefix == 'd', "Format prefix check");
    if (StartsWith(oat_checksums, "i") && !found_d) {
      const std::vector<OatFileAssistantContext::BootImageInfo>& boot_image_info_list =
          ofa_context->GetBootImageInfoList(isa);
      if (boot_image_index >= boot_image_info_list.size()) {
        *error_msg = StringPrintf("Missing boot image for %s, remaining checksums: %s",
                                  bcp_locations[bcp_index].c_str(),
                                  std::string(oat_checksums).c_str());
        return false;
      }
      const OatFileAssistantContext::BootImageInfo& boot_image_info =
          boot_image_info_list[boot_image_index];
      if (!ConsumePrefix(&oat_checksums, boot_image_info.checksum)) {
        *error_msg = StringPrintf("Image checksum mismatch, expected %s to start with %s",
                                  std::string(oat_checksums).c_str(),
                                  boot_image_info.checksum.c_str());
        return false;
      }
      bcp_index += boot_image_info.component_count;
      boot_image_index++;
    } else if (StartsWith(oat_checksums, "d")) {
      found_d = true;
      const std::vector<std::string>* bcp_checksums =
          ofa_context->GetBcpChecksums(bcp_index, error_msg);
      if (bcp_checksums == nullptr) {
        return false;
      }
      oat_checksums.remove_prefix(1u);
      for (const std::string& checksum : *bcp_checksums) {
        if (!ConsumePrefix(&oat_checksums, checksum)) {
          *error_msg = StringPrintf(
              "Dex checksum mismatch for bootclasspath file %s, expected %s to start with %s",
              bcp_locations[bcp_index].c_str(),
              std::string(oat_checksums).c_str(),
              checksum.c_str());
          return false;
        }
      }
      bcp_index++;
    } else {
      *error_msg = StringPrintf("Unexpected checksums, expected %s to start with %s",
                                std::string(oat_checksums).c_str(),
                                found_d ? "'d'" : "'i' or 'd'");
      return false;
    }
    if (bcp_index < oat_bcp_size) {
      if (!ConsumePrefix(&oat_checksums, ":")) {
        if (oat_checksums.empty()) {
          *error_msg =
              StringPrintf("Checksum too short, missing %zu components", oat_bcp_size - bcp_index);
        } else {
          *error_msg = StringPrintf("Missing ':' separator at start of %s",
                                    std::string(oat_checksums).c_str());
        }
        return false;
      }
    }
  }
  if (!oat_checksums.empty()) {
    *error_msg =
        StringPrintf("Checksum too long, unexpected tail: %s", std::string(oat_checksums).c_str());
    return false;
  }
  return true;
}
bool OatFileAssistant::ValidateBootClassPathChecksums(const OatFile& oat_file) {
  const char* oat_boot_class_path_checksums =
      oat_file.GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
  const char* oat_boot_class_path =
      oat_file.GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathKey);
  if (oat_boot_class_path_checksums == nullptr || oat_boot_class_path == nullptr) {
    return false;
  }
  std::string error_msg;
  bool result = ValidateBootClassPathChecksums(GetOatFileAssistantContext(),
                                               isa_,
                                               oat_boot_class_path_checksums,
                                               oat_boot_class_path,
                                               &error_msg);
  if (!result) {
    VLOG(oat) << "Failed to verify checksums of oat file " << oat_file.GetLocation()
              << " error: " << error_msg;
    return false;
  }
  return true;
}
bool OatFileAssistant::IsPrimaryBootImageUsable() {
  return !GetOatFileAssistantContext()->GetBootImageInfoList(isa_).empty();
}
OatFileAssistant::OatFileInfo& OatFileAssistant::GetBestInfo() {
  ScopedTrace trace("GetBestInfo");
  if (dex_parent_writable_ || UseFdToReadFiles()) {
    VLOG(oat) << "GetBestInfo checking odex next to the dex file ({})"_format(
        odex_.DisplayFilename());
    if (!odex_.IsUseable()) {
      VLOG(oat) << "GetBestInfo checking vdex next to the dex file ({})"_format(
          vdex_for_odex_.DisplayFilename());
      if (vdex_for_odex_.IsUseable()) {
        return vdex_for_odex_;
      }
      VLOG(oat) << "GetBestInfo checking dm ({})"_format(dm_for_odex_.DisplayFilename());
      if (dm_for_odex_.IsUseable()) {
        return dm_for_odex_;
      }
    }
    return odex_;
  }
  VLOG(oat) << "GetBestInfo checking odex in dalvik-cache ({})"_format(oat_.DisplayFilename());
  if (oat_.IsUseable()) {
    return oat_;
  }
  VLOG(oat) << "GetBestInfo checking odex next to the dex file ({})"_format(
      odex_.DisplayFilename());
  if (odex_.IsUseable()) {
    return odex_;
  }
  VLOG(oat) << "GetBestInfo checking vdex in dalvik-cache ({})"_format(
      vdex_for_oat_.DisplayFilename());
  if (vdex_for_oat_.IsUseable()) {
    return vdex_for_oat_;
  }
  VLOG(oat) << "GetBestInfo checking vdex next to the dex file ({})"_format(
      vdex_for_odex_.DisplayFilename());
  if (vdex_for_odex_.IsUseable()) {
    return vdex_for_odex_;
  }
  VLOG(oat) << "GetBestInfo checking dm ({})"_format(dm_for_oat_.DisplayFilename());
  if (dm_for_oat_.IsUseable()) {
    return dm_for_oat_;
  }
  VLOG(oat) << "GetBestInfo checking dm ({})"_format(dm_for_odex_.DisplayFilename());
  if (dm_for_odex_.IsUseable()) {
    return dm_for_odex_;
  }
  VLOG(oat) << "GetBestInfo no usable artifacts";
  return (odex_.Status() == kOatCannotOpen) ? oat_ : odex_;
}
std::unique_ptr<gc::space::ImageSpace> OatFileAssistant::OpenImageSpace(const OatFile* oat_file) {
  DCHECK(oat_file != nullptr);
  std::string art_file = ReplaceFileExtension(oat_file->GetLocation(), "art");
  if (art_file.empty()) {
    return nullptr;
  }
  std::string error_msg;
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<gc::space::ImageSpace> ret =
      gc::space::ImageSpace::CreateFromAppImage(art_file.c_str(), oat_file, &error_msg);
  if (ret == nullptr && (VLOG_IS_ON(image) || OS::FileExists(art_file.c_str()))) {
    LOG(INFO) << "Failed to open app image " << art_file.c_str() << " " << error_msg;
  }
  return ret;
}
OatFileAssistant::OatFileInfo::OatFileInfo(OatFileAssistant* oat_file_assistant,
                                           bool is_oat_location)
    : oat_file_assistant_(oat_file_assistant), is_oat_location_(is_oat_location) {}
bool OatFileAssistant::OatFileInfo::IsOatLocation() { return is_oat_location_; }
const std::string* OatFileAssistant::OatFileInfo::Filename() {
  return filename_provided_ ? &filename_ : nullptr;
}
const char* OatFileAssistant::OatFileInfo::DisplayFilename() {
  return filename_provided_ ? filename_.c_str() : "unknown";
}
bool OatFileAssistant::OatFileInfo::IsUseable() {
  ScopedTrace trace("IsUseable");
  switch (Status()) {
    case kOatCannotOpen:
    case kOatDexOutOfDate:
    case kOatContextOutOfDate:
    case kOatBootImageOutOfDate:
      return false;
    case kOatUpToDate:
      return true;
  }
  UNREACHABLE();
}
OatFileAssistant::OatStatus OatFileAssistant::OatFileInfo::Status() {
  ScopedTrace trace("Status");
  if (!status_attempted_) {
    status_attempted_ = true;
    const OatFile* file = GetFile();
    if (file == nullptr) {
      status_ = kOatCannotOpen;
    } else {
      status_ = oat_file_assistant_->GivenOatFileStatus(*file);
      VLOG(oat) << file->GetLocation() << " is " << status_ << " with filter "
                << file->GetCompilerFilter();
    }
  }
  return status_;
}
OatFileAssistant::DexOptNeeded OatFileAssistant::OatFileInfo::GetDexOptNeeded(
    CompilerFilter::Filter target_compiler_filter, const DexOptTrigger dexopt_trigger) {
  if (IsUseable()) {
    return ShouldRecompileForFilter(target_compiler_filter, dexopt_trigger) ? kDex2OatForFilter :
                                                                              kNoDexOptNeeded;
  }
  if (!dexopt_trigger.targetFilterIsBetter) {
    return kNoDexOptNeeded;
  }
  if (Status() == kOatBootImageOutOfDate) {
    return kDex2OatForBootImage;
  }
  std::string error_msg;
  std::optional<bool> has_dex_files = oat_file_assistant_->HasDexFiles(&error_msg);
  if (has_dex_files.has_value()) {
    if (*has_dex_files) {
      return kDex2OatFromScratch;
    } else {
      return kNoDexOptNeeded;
    }
  } else {
    LOG(WARNING) << error_msg;
    return kNoDexOptNeeded;
  }
}
const OatFile* OatFileAssistant::OatFileInfo::GetFile() {
  CHECK(!file_released_) << "GetFile called after oat file released.";
  if (load_attempted_) {
    return file_.get();
  }
  load_attempted_ = true;
  if (!filename_provided_) {
    return nullptr;
  }
  if (LocationIsOnArtApexData(filename_) &&
      oat_file_assistant_->GetRuntimeOptions().deny_art_apex_data_files) {
    LOG(WARNING) << "OatFileAssistant rejected file " << filename_
                 << ": ART apexdata is untrusted.";
    return nullptr;
  }
  std::string error_msg;
  bool executable = oat_file_assistant_->load_executable_;
  if (android::base::EndsWith(filename_, kVdexExtension)) {
    executable = false;
    std::unique_ptr<VdexFile> vdex;
    if (use_fd_) {
      if (vdex_fd_ >= 0) {
        struct stat s;
        int rc = TEMP_FAILURE_RETRY(fstat(vdex_fd_, &s));
        if (rc == -1) {
          error_msg = StringPrintf("Failed getting length of the vdex file %s.", strerror(errno));
        } else {
          vdex = VdexFile::Open(vdex_fd_,
                                s.st_size,
                                filename_,
                                             false,
                                            false,
                                &error_msg);
        }
      }
    } else {
      vdex = VdexFile::Open(filename_,
                                         false,
                                        false,
                            &error_msg);
    }
    if (vdex == nullptr) {
      VLOG(oat) << "unable to open vdex file " << filename_ << ": " << error_msg;
    } else {
      file_.reset(OatFile::OpenFromVdex(zip_fd_,
                                        std::move(vdex),
                                        oat_file_assistant_->dex_location_,
                                        oat_file_assistant_->context_,
                                        &error_msg));
    }
  } else if (android::base::EndsWith(filename_, kDmExtension)) {
    executable = false;
    std::unique_ptr<ZipArchive> dm_file(ZipArchive::Open(filename_.c_str(), &error_msg));
    if (dm_file != nullptr) {
      std::unique_ptr<VdexFile> vdex(VdexFile::OpenFromDm(filename_, *dm_file));
      if (vdex != nullptr) {
        file_.reset(OatFile::OpenFromVdex(zip_fd_,
                                          std::move(vdex),
                                          oat_file_assistant_->dex_location_,
                                          oat_file_assistant_->context_,
                                          &error_msg));
      }
    }
  } else {
    if (executable && oat_file_assistant_->only_load_trusted_executable_) {
      executable = LocationIsTrusted(filename_, true);
    }
    VLOG(oat) << "Loading " << filename_ << " with executable: " << executable;
    if (use_fd_) {
      if (oat_fd_ >= 0 && vdex_fd_ >= 0) {
        ArrayRef<const std::string> dex_locations(&oat_file_assistant_->dex_location_,
                                                           1u);
        file_.reset(OatFile::Open(zip_fd_,
                                  vdex_fd_,
                                  oat_fd_,
                                  filename_,
                                  executable,
                                              false,
                                  dex_locations,
                                              ArrayRef<const int>(),
                                                  nullptr,
                                  &error_msg));
      }
    } else {
      file_.reset(OatFile::Open( -1,
                                filename_,
                                filename_,
                                executable,
                                            false,
                                oat_file_assistant_->dex_location_,
                                &error_msg));
    }
  }
  if (file_.get() == nullptr) {
    VLOG(oat) << "OatFileAssistant test for existing oat file " << filename_ << ": " << error_msg;
  } else {
    VLOG(oat) << "Successfully loaded " << filename_ << " with executable: " << executable;
  }
  return file_.get();
}
bool OatFileAssistant::OatFileInfo::ShouldRecompileForFilter(CompilerFilter::Filter target,
                                                             const DexOptTrigger dexopt_trigger) {
  const OatFile* file = GetFile();
  DCHECK(file != nullptr);
  CompilerFilter::Filter current = file->GetCompilerFilter();
  if (dexopt_trigger.targetFilterIsBetter && CompilerFilter::IsBetter(target, current)) {
    VLOG(oat) << "Should recompile: targetFilterIsBetter (current: {}, target: {})"_format(
        CompilerFilter::NameOfFilter(current), CompilerFilter::NameOfFilter(target));
    return true;
  }
  if (dexopt_trigger.targetFilterIsSame && current == target) {
    VLOG(oat) << "Should recompile: targetFilterIsSame (current: {}, target: {})"_format(
        CompilerFilter::NameOfFilter(current), CompilerFilter::NameOfFilter(target));
    return true;
  }
  if (dexopt_trigger.targetFilterIsWorse && CompilerFilter::IsBetter(current, target)) {
    VLOG(oat) << "Should recompile: targetFilterIsWorse (current: {}, target: {})"_format(
        CompilerFilter::NameOfFilter(current), CompilerFilter::NameOfFilter(target));
    return true;
  }
  if (dexopt_trigger.primaryBootImageBecomesUsable &&
      CompilerFilter::DependsOnImageChecksum(current)) {
    const char* oat_boot_class_path_checksums =
        file->GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
    if (oat_boot_class_path_checksums != nullptr &&
        !StartsWith(oat_boot_class_path_checksums, "i") &&
        oat_file_assistant_->IsPrimaryBootImageUsable()) {
      DCHECK(!file->GetOatHeader().RequiresImage());
      VLOG(oat) << "Should recompile: primaryBootImageBecomesUsable";
      return true;
    }
  }
  if (dexopt_trigger.needExtraction && !file->ContainsDexCode() &&
      !oat_file_assistant_->ZipFileOnlyContainsUncompressedDex()) {
    VLOG(oat) << "Should recompile: needExtraction";
    return true;
  }
  VLOG(oat) << "Should not recompile";
  return false;
}
bool OatFileAssistant::ClassLoaderContextIsOkay(const OatFile& oat_file) const {
  if (context_ == nullptr) {
    return true;
  }
  if (oat_file.IsBackedByVdexOnly()) {
    return true;
  }
  if (!CompilerFilter::IsVerificationEnabled(oat_file.GetCompilerFilter())) {
    return true;
  }
  ClassLoaderContext::VerificationResult matches =
      context_->VerifyClassLoaderContextMatch(oat_file.GetClassLoaderContext(),
                                                               true,
                                                                   true);
  if (matches == ClassLoaderContext::VerificationResult::kMismatch) {
    VLOG(oat) << "ClassLoaderContext check failed. Context was " << oat_file.GetClassLoaderContext()
              << ". The expected context is "
              << context_->EncodeContextForOatFile(android::base::Dirname(dex_location_));
    return false;
  }
  return true;
}
bool OatFileAssistant::OatFileInfo::IsExecutable() {
  const OatFile* file = GetFile();
  return (file != nullptr && file->IsExecutable());
}
void OatFileAssistant::OatFileInfo::Reset() {
  load_attempted_ = false;
  file_.reset();
  status_attempted_ = false;
}
void OatFileAssistant::OatFileInfo::Reset(
    const std::string& filename, bool use_fd, int zip_fd, int vdex_fd, int oat_fd) {
  filename_provided_ = true;
  filename_ = filename;
  use_fd_ = use_fd;
  zip_fd_ = zip_fd;
  vdex_fd_ = vdex_fd;
  oat_fd_ = oat_fd;
  Reset();
}
std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFile() {
  file_released_ = true;
  return std::move(file_);
}
std::unique_ptr<OatFile> OatFileAssistant::OatFileInfo::ReleaseFileForUse() {
  ScopedTrace trace("ReleaseFileForUse");
  if (Status() == kOatUpToDate) {
    return ReleaseFile();
  }
  return std::unique_ptr<OatFile>();
}
bool OatFileAssistant::OatFileInfo::CheckDisableCompactDexExperiment() {
  std::string ph_disable_compact_dex = android::base::GetProperty(kPhDisableCompactDex, "false");
  if (ph_disable_compact_dex != "true") {
    return false;
  }
  const OatFile* oat_file = GetFile();
  if (oat_file == nullptr) {
    return false;
  }
  const VdexFile* vdex_file = oat_file->GetVdexFile();
  return vdex_file != nullptr && vdex_file->HasDexSection() &&
         !vdex_file->HasOnlyStandardDexFiles();
}
void OatFileAssistant::GetOptimizationStatus(const std::string& filename,
                                             InstructionSet isa,
                                             std::string* out_compilation_filter,
                                             std::string* out_compilation_reason,
                                             OatFileAssistantContext* ofa_context) {
  OatFileAssistant oat_file_assistant(filename.c_str(),
                                      isa,
                                                  nullptr,
                                                          false,
                                                                       false,
                                      ofa_context);
  std::string out_odex_location;
  std::string out_odex_status;
  oat_file_assistant.GetOptimizationStatus(
      &out_odex_location, out_compilation_filter, out_compilation_reason, &out_odex_status);
}
void OatFileAssistant::GetOptimizationStatus(std::string* out_odex_location,
                                             std::string* out_compilation_filter,
                                             std::string* out_compilation_reason,
                                             std::string* out_odex_status) {
  OatFileInfo& oat_file_info = GetBestInfo();
  const OatFile* oat_file = GetBestInfo().GetFile();
  if (oat_file == nullptr) {
    std::string error_msg;
    std::optional<bool> has_dex_files = HasDexFiles(&error_msg);
    if (!has_dex_files.has_value()) {
      *out_odex_location = "error";
      *out_compilation_filter = "unknown";
      *out_compilation_reason = "unknown";
      *out_odex_status = "io-error-no-apk";
    } else if (!has_dex_files.value()) {
      *out_odex_location = "none";
      *out_compilation_filter = "unknown";
      *out_compilation_reason = "unknown";
      *out_odex_status = "no-dex-code";
    } else {
      *out_odex_location = "error";
      *out_compilation_filter = "run-from-apk";
      *out_compilation_reason = "unknown";
      *out_odex_status = "io-error-no-oat";
    }
    return;
  }
  *out_odex_location = oat_file->GetLocation();
  OatStatus status = oat_file_info.Status();
  const char* reason = oat_file->GetCompilationReason();
  *out_compilation_reason = reason == nullptr ? "unknown" : reason;
  DCHECK(status == kOatUpToDate || status == kOatDexOutOfDate);
  switch (status) {
    case kOatUpToDate:
      *out_compilation_filter = CompilerFilter::NameOfFilter(oat_file->GetCompilerFilter());
      *out_odex_status = "up-to-date";
      return;
    case kOatCannotOpen:
    case kOatBootImageOutOfDate:
    case kOatContextOutOfDate:
      *out_compilation_filter = "unexpected";
      *out_compilation_reason = "unexpected";
      *out_odex_status = "unexpected";
      return;
    case kOatDexOutOfDate:
      *out_compilation_filter = "run-from-apk-fallback";
      *out_odex_status = "apk-more-recent";
      return;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}
bool OatFileAssistant::ZipFileOnlyContainsUncompressedDex() {
  std::string error_msg;
  if (GetRequiredDexChecksums(&error_msg) == nullptr) {
    LOG(ERROR) << error_msg;
  }
  return zip_file_only_contains_uncompressed_dex_;
}
}
