#include "profile_compilation_info.h"
#include "errno.h"
#include <limits.h>
#include <string>
#include <vector>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <base/time_utils.h>
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/mutex.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "jit/profiling_info.h"
#include "os.h"
#include "safe_map.h"
#include "utils.h"
#include "android-base/file.h"
namespace art {
const uint8_t ProfileCompilationInfo::kProfileVersion[] = { '0', '0', '5', '\0' };
const uint8_t ProfileCompilationInfo::kProfileVersion[] = { '0', '0', '5', '\0' };
static constexpr uint16_t kMaxDexFileKeyLength = PATH_MAX;
static constexpr bool kDebugIgnoreChecksum = false;
static constexpr uint8_t kIsMissingTypesEncoding = 6;
static constexpr uint8_t kIsMegamorphicEncoding = 7;
static_assert(sizeof(InlineCache::kIndividualCacheSize) == sizeof(uint8_t),
              "InlineCache::kIndividualCacheSize does not have the expect type size");
static_assert(InlineCache::kIndividualCacheSize < kIsMegamorphicEncoding,
              "InlineCache::kIndividualCacheSize is larger than expected");
static_assert(InlineCache::kIndividualCacheSize < kIsMissingTypesEncoding,
              "InlineCache::kIndividualCacheSize is larger than expected");
ProfileCompilationInfo::ProfileCompilationInfo(ArenaPool* custom_arena_pool): default_arena_pool_(), arena_(custom_arena_pool), info_(arena_.Adapter(kArenaAllocProfile)), profile_key_map_(std::less<const std::string>(), arena_.Adapter(kArenaAllocProfile)) {
}
  ProfileCompilationInfo(): default_arena_pool_( true, false, "ProfileCompilationInfo"), arena_(&default_arena_pool_), info_(arena_.Adapter(kArenaAllocProfile)), profile_key_map_(std::less<const std::string>(), arena_.Adapter(kArenaAllocProfile)) {
  }
~ProfileCompilationInfo(){
  VLOG(profiler) << Dumpable<MemStats>(arena_.GetMemStats());
  for (DexFileData* data : info_) {
    delete data;
  }
}
void ProfileCompilationInfo::DexPcData::AddClass(uint16_t dex_profile_idx,
                                                 const dex::TypeIndex& type_idx) {
  if (is_megamorphic || is_missing_types) {
    return;
  }
  ClassReference ref(dex_profile_idx, type_idx);
  auto it = classes.find(ref);
  if (it != classes.end()) {
    return;
  }
  if (classes.size() + 1 >= InlineCache::kIndividualCacheSize) {
    is_megamorphic = true;
    classes.clear();
    return;
  }
  classes.insert(ref);
}
std::string ProfileCompilationInfo::GetProfileDexFileKey(const std::string& dex_location) {
  DCHECK(!dex_location.empty());
  size_t last_sep_index = dex_location.find_last_of('/');
  if (last_sep_index == std::string::npos) {
    return dex_location;
  } else {
    DCHECK(last_sep_index < dex_location.size());
    return dex_location.substr(last_sep_index + 1);
  }
}
bool ProfileCompilationInfo::AddMethodsAndClasses(
    const std::vector<ProfileMethodInfo>& methods,
    const std::set<DexCacheResolvedClasses>& resolved_classes) {
  for (const ProfileMethodInfo& method : methods) {
    if (!AddMethod(method)) {
      return false;
    }
  }
  for (const DexCacheResolvedClasses& dex_cache : resolved_classes) {
    if (!AddResolvedClasses(dex_cache)) {
      return false;
    }
  }
  return true;
}
bool ProfileCompilationInfo::Load(const std::string& filename, bool clear_if_invalid) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  ScopedFlock flock;
  std::string error;
  int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
  if (!flock.Init(filename.c_str(), flags, false, false, &error)) {
    LOG(WARNING) << "Couldn't lock the profile file " << filename << ": " << error;
    return false;
  }
  int fd = flock.GetFile()->Fd();
  ProfileLoadSatus status = LoadInternal(fd, &error);
  if (status == kProfileLoadSuccess) {
    return true;
  }
  if (clear_if_invalid &&
      ((status == kProfileLoadVersionMismatch) || (status == kProfileLoadBadData))) {
    LOG(WARNING) << "Clearing bad or obsolete profile data from file "
                 << filename << ": " << error;
    if (flock.GetFile()->ClearContent()) {
      return true;
    } else {
      PLOG(WARNING) << "Could not clear profile file: " << filename;
      return false;
    }
  }
  LOG(WARNING) << "Could not load profile data from file " << filename << ": " << error;
  return false;
}
bool ProfileCompilationInfo::Save(const std::string& filename, uint64_t* bytes_written) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  ScopedFlock flock;
  std::string error;
  int flags = O_WRONLY | O_NOFOLLOW | O_CLOEXEC;
  if (!flock.Init(filename.c_str(), flags, false, false, &error)) {
    LOG(WARNING) << "Couldn't lock the profile file " << filename << ": " << error;
    return false;
  }
  int fd = flock.GetFile()->Fd();
  if (!flock.GetFile()->ClearContent()) {
    PLOG(WARNING) << "Could not clear profile file: " << filename;
    return false;
  }
  bool result = Save(fd);
  if (result) {
    int64_t size = GetFileSizeBytes(filename);
    if (size != -1) {
      VLOG(profiler)
        << "Successfully saved profile info to " << filename << " Size: "
        << size;
      if (bytes_written != nullptr) {
        *bytes_written = static_cast<uint64_t>(size);
      }
    }
  } else {
    VLOG(profiler) << "Failed to save profile info to " << filename;
  }
  return result;
}
static bool WriteBuffer(int fd, const uint8_t* buffer, size_t byte_count) {
  while (byte_count > 0) {
    int bytes_written = TEMP_FAILURE_RETRY(write(fd, buffer, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;
    buffer += bytes_written;
  }
  return true;
}
static void AddStringToBuffer(std::vector<uint8_t>* buffer, const std::string& value) {
  buffer->insert(buffer->end(), value.begin(), value.end());
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
static constexpr size_t kLineHeaderSize =
    2 * sizeof(uint16_t) +
    2 * sizeof(uint32_t);
bool ProfileCompilationInfo::Save(int fd) {
  uint64_t start = NanoTime();
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);
  std::vector<uint8_t> buffer;
  if (!WriteBuffer(fd, kProfileMagic, sizeof(kProfileMagic))) {
    return false;
  }
  if (!WriteBuffer(fd, kProfileVersion, sizeof(kProfileVersion))) {
    return false;
  }
  DCHECK_LE(info_.size(), std::numeric_limits<uint8_t>::max());
  AddUintToBuffer(&buffer, static_cast<uint8_t>(info_.size()));
  uint32_t required_capacity = 0;
  for (const DexFileData* dex_data_ptr : info_) {
    const DexFileData& dex_data = *dex_data_ptr;
    uint32_t methods_region_size = GetMethodsRegionSize(dex_data);
    required_capacity += kLineHeaderSize +
        dex_data.profile_key.size() +
        sizeof(uint16_t) * dex_data.class_set.size() +
        methods_region_size;
  }
  if (required_capacity > kProfileSizeErrorThresholdInBytes) {
    LOG(ERROR) << "Profile data size exceeds "
               << std::to_string(kProfileSizeErrorThresholdInBytes)
               << " bytes. Profile will not be written to disk.";
    return false;
  }
  if (required_capacity > kProfileSizeWarningThresholdInBytes) {
    LOG(WARNING) << "Profile data size exceeds "
                 << std::to_string(kProfileSizeWarningThresholdInBytes);
  }
  AddUintToBuffer(&buffer, required_capacity);
  if (!WriteBuffer(fd, buffer.data(), buffer.size())) {
    return false;
  }
  buffer.reserve(required_capacity);
  buffer.clear();
  for (const DexFileData* dex_data_ptr : info_) {
    const DexFileData& dex_data = *dex_data_ptr;
    if (dex_data.profile_key.size() >= kMaxDexFileKeyLength) {
      LOG(WARNING) << "DexFileKey exceeds allocated limit";
      return false;
    }
    uint32_t methods_region_size = GetMethodsRegionSize(dex_data);
    DCHECK_LE(dex_data.profile_key.size(), std::numeric_limits<uint16_t>::max());
    DCHECK_LE(dex_data.class_set.size(), std::numeric_limits<uint16_t>::max());
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.profile_key.size()));
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.class_set.size()));
    AddUintToBuffer(&buffer, methods_region_size);
    AddUintToBuffer(&buffer, dex_data.checksum);
    AddStringToBuffer(&buffer, dex_data.profile_key);
    uint16_t last_method_index = 0;
    for (const auto& method_it : dex_data.method_map) {
      DCHECK_GE(method_it.first, last_method_index);
      uint16_t diff_with_last_method_index = method_it.first - last_method_index;
      last_method_index = method_it.first;
      AddUintToBuffer(&buffer, diff_with_last_method_index);
      AddInlineCacheToBuffer(&buffer, method_it.second);
    }
    uint16_t last_class_index = 0;
    for (const auto& class_id : dex_data.class_set) {
      DCHECK_GE(class_id.index_, last_class_index);
      uint16_t diff_with_last_class_index = class_id.index_ - last_class_index;
      last_class_index = class_id.index_;
      AddUintToBuffer(&buffer, diff_with_last_class_index);
    }
  }
  uint32_t output_size = 0;
  std::unique_ptr<uint8_t[]> compressed_buffer = DeflateBuffer(buffer.data(),
                                                               required_capacity,
                                                               &output_size);
  buffer.clear();
  AddUintToBuffer(&buffer, output_size);
  if (!WriteBuffer(fd, buffer.data(), buffer.size())) {
    return false;
  }
  if (!WriteBuffer(fd, compressed_buffer.get(), output_size)) {
    return false;
  }
  uint64_t total_time = NanoTime() - start;
  VLOG(profiler) << "Compressed from "
                 << std::to_string(required_capacity)
                 << " to "
                 << std::to_string(output_size);
  VLOG(profiler) << "Time to save profile: " << std::to_string(total_time);
  return true;
}
void ProfileCompilationInfo::AddInlineCacheToBuffer(std::vector<uint8_t>* buffer,
                                                    const InlineCacheMap& inline_cache_map) {
  AddUintToBuffer(buffer, static_cast<uint16_t>(inline_cache_map.size()));
  if (inline_cache_map.size() == 0) {
    return;
  }
  for (const auto& inline_cache_it : inline_cache_map) {
    uint16_t dex_pc = inline_cache_it.first;
    const DexPcData dex_pc_data = inline_cache_it.second;
    const ClassSet& classes = dex_pc_data.classes;
    AddUintToBuffer(buffer, dex_pc);
    if (dex_pc_data.is_missing_types) {
      DCHECK(!dex_pc_data.is_megamorphic);
      DCHECK_EQ(classes.size(), 0u);
      AddUintToBuffer(buffer, kIsMissingTypesEncoding);
      continue;
    } else if (dex_pc_data.is_megamorphic) {
      DCHECK_EQ(classes.size(), 0u);
      AddUintToBuffer(buffer, kIsMegamorphicEncoding);
      continue;
    }
    DCHECK_LT(classes.size(), InlineCache::kIndividualCacheSize);
    DCHECK_NE(classes.size(), 0u) << "InlineCache contains a dex_pc with 0 classes";
    SafeMap<uint8_t, std::vector<dex::TypeIndex>> dex_to_classes_map;
    GroupClassesByDex(classes, &dex_to_classes_map);
    AddUintToBuffer(buffer, static_cast<uint8_t>(dex_to_classes_map.size()));
    for (const auto& dex_it : dex_to_classes_map) {
      uint8_t dex_profile_index = dex_it.first;
      const std::vector<dex::TypeIndex>& dex_classes = dex_it.second;
      AddUintToBuffer(buffer, dex_profile_index);
      AddUintToBuffer(buffer, static_cast<uint8_t>(dex_classes.size()));
      for (size_t i = 0; i < dex_classes.size(); i++) {
        AddUintToBuffer(buffer, dex_classes[i].index_);
      }
    }
  }
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}
}
