/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

// Last profile version: fix profman merges. Update profile version to force
// regeneration of possibly faulty profiles.
const uint8_t ProfileCompilationInfo::kProfileVersion[] = { '0', '0', '5', '\0' };

// Last profile version: fix profman merges. Update profile version to force
// regeneration of possibly faulty profiles.
const uint8_t ProfileCompilationInfo::kProfileVersion[] = { '0', '0', '5', '\0' };

static constexpr uint16_t kMaxDexFileKeyLength = PATH_MAX;

// Debug flag to ignore checksums when testing if a method or a class is present in the profile.
// Used to facilitate testing profile guided compilation across a large number of apps
// using the same test profile.
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

  ProfileCompilationInfo(): default_arena_pool_(/*use_malloc*/true, /*low_4gb*/false, "ProfileCompilationInfo"), arena_(&default_arena_pool_), info_(arena_.Adapter(kArenaAllocProfile)), profile_key_map_(std::less<const std::string>(), arena_.Adapter(kArenaAllocProfile)) {
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

  // Perform an explicit lookup for the type instead of directly emplacing the
  // element. We do this because emplace() allocates the node before doing the
  // lookup and if it then finds an identical element, it shall deallocate the
  // node. For Arena allocations, that's essentially a leak.
  ClassReference ref(dex_profile_idx, type_idx);
  auto it = classes.find(ref);
  if (it != classes.end()) {
    // The type index exists.
    return;
  }

  // Check if the adding the type will cause the cache to become megamorphic.
  if (classes.size() + 1 >= InlineCache::kIndividualCacheSize) {
    is_megamorphic = true;
    classes.clear();
    return;
  }

  // The type does not exist and the inline cache will not be megamorphic.
  classes.insert(ref);
}

// Transform the actual dex location into relative paths.
// Note: this is OK because we don't store profiles of different apps into the same file.
// Apps with split apks don't cause trouble because each split has a different name and will not
// collide with other entries.
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
  // There's no need to fsync profile data right away. We get many chances
  // to write it again in case something goes wrong. We can rely on a simple
  // close(), no sync, and let to the kernel decide when to write to disk.
  if (!flock.Init(filename.c_str(), flags, /*block*/false, /*flush_on_close*/false, &error)) {
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
  // There's no need to fsync profile data right away. We get many chances
  // to write it again in case something goes wrong. We can rely on a simple
  // close(), no sync, and let to the kernel decide when to write to disk.
  if (!flock.Init(filename.c_str(), flags, /*block*/false, /*flush_on_close*/false, &error)) {
    LOG(WARNING) << "Couldn't lock the profile file " << filename << ": " << error;
    return false;
  }

  int fd = flock.GetFile()->Fd();

  // We need to clear the data because we don't support appending to the profiles yet.
  if (!flock.GetFile()->ClearContent()) {
    PLOG(WARNING) << "Could not clear profile file: " << filename;
    return false;
  }

  // This doesn't need locking because we are trying to lock the file for exclusive
  // access and fail immediately if we can't.
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

// Returns true if all the bytes were successfully written to the file descriptor.
static bool WriteBuffer(int fd, const uint8_t* buffer, size_t byte_count) {
  while (byte_count > 0) {
    int bytes_written = TEMP_FAILURE_RETRY(write(fd, buffer, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;  // Reduce the number of remaining bytes.
    buffer += bytes_written;  // Move the buffer forward.
  }
  return true;
}

// Add the string bytes to the buffer.
static void AddStringToBuffer(std::vector<uint8_t>* buffer, const std::string& value) {
  buffer->insert(buffer->end(), value.begin(), value.end());
}

ProfileCompilationInfo::DexPcData*
ProfileCompilationInfo::FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc) {
  return &(inline_cache->FindOrAdd(dex_pc, DexPcData(&arena_))->second);
}

static constexpr size_t kLineHeaderSize =
    2 * sizeof(uint16_t) +  // class_set.size + dex_location.size
    2 * sizeof(uint32_t);                            // method_map.size + checksum
                            /**
                            * Serialization format:
                            *    magic,version,number_of_dex_files,uncompressed_size_of_zipped_data,compressed_data_size,
                            *    zipped[dex_location1,number_of_classes1,methods_region_size,dex_location_checksum1, \
                            *        method_encoding_11,method_encoding_12...,class_id1,class_id2...
                            *    dex_location2,number_of_classes2,methods_region_size,dex_location_checksum2, \
                            *        method_encoding_21,method_encoding_22...,,class_id1,class_id2...
                            *    .....]
                            * The method_encoding is:
                            *    method_id,number_of_inline_caches,inline_cache1,inline_cache2...
                            * The inline_cache is:
                            *    dex_pc,[M|dex_map_size], dex_profile_index,class_id1,class_id2...,dex_profile_index2,...
                            *    dex_map_size is the number of dex_indeces that follows.
                            *       Classes are grouped per their dex files and the line
                            *       `dex_profile_index,class_id1,class_id2...,dex_profile_index2,...` encodes the
                            *       mapping from `dex_profile_index` to the set of classes `class_id1,class_id2...`
                            *    M stands for megamorphic or missing types and it's encoded as either
                            *    the byte kIsMegamorphicEncoding or kIsMissingTypesEncoding.
                            *    When present, there will be no class ids following.
                            **/
bool ProfileCompilationInfo::Save(int fd) {
  uint64_t start = NanoTime();
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);

  // Use a vector wrapper to avoid keeping track of offsets when we add elements.
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
  // Make sure that the buffer has enough capacity to avoid repeated resizings
  // while we add data.
  buffer.reserve(required_capacity);
  buffer.clear();

  // Dex files must be written in the order of their profile index. This
  // avoids writing the index in the output file and simplifies the parsing logic.
  for (const DexFileData* dex_data_ptr : info_) {
    const DexFileData& dex_data = *dex_data_ptr;

    // Note that we allow dex files without any methods or classes, so that
    // inline caches can refer valid dex files.

    if (dex_data.profile_key.size() >= kMaxDexFileKeyLength) {
      LOG(WARNING) << "DexFileKey exceeds allocated limit";
      return false;
    }

    uint32_t methods_region_size = GetMethodsRegionSize(dex_data);

    DCHECK_LE(dex_data.profile_key.size(), std::numeric_limits<uint16_t>::max());
    DCHECK_LE(dex_data.class_set.size(), std::numeric_limits<uint16_t>::max());
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.profile_key.size()));
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.class_set.size()));
    AddUintToBuffer(&buffer, methods_region_size);  // uint32_t
    AddUintToBuffer(&buffer, dex_data.checksum);  // uint32_t

    AddStringToBuffer(&buffer, dex_data.profile_key);

    uint16_t last_method_index = 0;
    for (const auto& method_it : dex_data.method_map) {
      // Store the difference between the method indices. The SafeMap is ordered by
      // method_id, so the difference will always be non negative.
      DCHECK_GE(method_it.first, last_method_index);
      uint16_t diff_with_last_method_index = method_it.first - last_method_index;
      last_method_index = method_it.first;
      AddUintToBuffer(&buffer, diff_with_last_method_index);
      AddInlineCacheToBuffer(&buffer, method_it.second);
    }

    uint16_t last_class_index = 0;
    for (const auto& class_id : dex_data.class_set) {
      // Store the difference between the class indices. The set is ordered by
      // class_id, so the difference will always be non negative.
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
  // Add inline cache map size.
  AddUintToBuffer(buffer, static_cast<uint16_t>(inline_cache_map.size()));
  if (inline_cache_map.size() == 0) {
    return;
  }
  for (const auto& inline_cache_it : inline_cache_map) {
    uint16_t dex_pc = inline_cache_it.first;
    const DexPcData dex_pc_data = inline_cache_it.second;
    const ClassSet& classes = dex_pc_data.classes;

    // Add the dex pc.
    AddUintToBuffer(buffer, dex_pc);

    // Add the megamorphic/missing_types encoding if needed and continue.
    // In either cases we don't add any classes to the profiles and so there's
    // no point to continue.
    // TODO(calin): in case we miss types there is still value to add the
    // rest of the classes. They can be added without bumping the profile version.
    if (dex_pc_data.is_missing_types) {
      DCHECK(!dex_pc_data.is_megamorphic);  // at this point the megamorphic flag should not be set.
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
    // Group the classes by dex. We expect that most of the classes will come from
    // the same dex, so this will be more efficient than encoding the dex index
    // for each class reference.
    GroupClassesByDex(classes, &dex_to_classes_map);
    // Add the dex map size.
    AddUintToBuffer(buffer, static_cast<uint8_t>(dex_to_classes_map.size()));
    for (const auto& dex_it : dex_to_classes_map) {
      uint8_t dex_profile_index = dex_it.first;
      const std::vector<dex::TypeIndex>& dex_classes = dex_it.second;
      // Add the dex profile index.
      AddUintToBuffer(buffer, dex_profile_index);
      // Add the the number of classes for each dex profile index.
      AddUintToBuffer(buffer, static_cast<uint8_t>(dex_classes.size()));
      for (size_t i = 0; i < dex_classes.size(); i++) {
        // Add the type index of the classes.
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

} // namespace art
