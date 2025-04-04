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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include "android-base/file.h"
#include "base/arena_allocator.h"
#include "base/bit_utils.h"
#include "base/dumpable.h"
#include "base/file_utils.h"
#include "base/logging.h"  // For VLOG.
#include "base/malloc_arena_pool.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file_loader.h"

namespace art {

// A synthetic annotations that can be used to denote that no annotation should
// be associated with the profile samples. We use the empty string for the package name
// because that's an invalid package name and should never occur in practice.
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");

// A synthetic annotations that can be used to denote that no annotation should
// be associated with the profile samples. We use the empty string for the package name
// because that's an invalid package name and should never occur in practice.
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");

// A synthetic annotations that can be used to denote that no annotation should
// be associated with the profile samples. We use the empty string for the package name
// because that's an invalid package name and should never occur in practice.
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");

static_assert(sizeof(ProfileCompilationInfo::kProfileVersion) == 4,
              "Invalid profile version size");
static_assert(sizeof(ProfileCompilationInfo::kProfileVersionForBootImage) == 4,
              "Invalid profile version size");

// A synthetic annotations that can be used to denote that no annotation should
// be associated with the profile samples. We use the empty string for the package name
// because that's an invalid package name and should never occur in practice.
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");

// A synthetic annotations that can be used to denote that no annotation should
// be associated with the profile samples. We use the empty string for the package name
// because that's an invalid package name and should never occur in practice.
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");

static constexpr char kSampleMetadataSeparator = ':';

// Note: This used to be PATH_MAX (usually 4096) but that seems excessive
// and we do not want to rely on that external constant anyway.
static constexpr uint16_t kMaxDexFileKeyLength = 1024;

// According to dex file specification, there can be more than 2^16 valid method indexes
// but bytecode uses only 16 bits, so higher method indexes are not very useful (though
// such methods could be reached through virtual or interface dispatch). Consequently,
// dex files with more than 2^16 method indexes are not really used and the profile file
// format does not support higher method indexes.
static constexpr uint32_t kMaxSupportedMethodIndex = 0xffffu;

// Debug flag to ignore checksums when testing if a method or a class is present in the profile.
// Used to facilitate testing profile guided compilation across a large number of apps
// using the same test profile.
static constexpr bool kDebugIgnoreChecksum = false;

static constexpr uint8_t kIsMissingTypesEncoding = 6;
static constexpr uint8_t kIsMegamorphicEncoding = 7;

static_assert(sizeof(ProfileCompilationInfo::kIndividualInlineCacheSize) == sizeof(uint8_t),
              "InlineCache::kIndividualInlineCacheSize does not have the expect type size");
static_assert(ProfileCompilationInfo::kIndividualInlineCacheSize < kIsMegamorphicEncoding,
              "InlineCache::kIndividualInlineCacheSize is larger than expected");
static_assert(ProfileCompilationInfo::kIndividualInlineCacheSize < kIsMissingTypesEncoding,
              "InlineCache::kIndividualInlineCacheSize is larger than expected");

static constexpr uint32_t kSizeWarningThresholdBytes = 500000U;
static constexpr uint32_t kSizeErrorThresholdBytes = 1500000U;

static constexpr uint32_t kSizeWarningThresholdBootBytes = 25000000U;
static constexpr uint32_t kSizeErrorThresholdBootBytes = 100000000U;

static bool ChecksumMatch(uint32_t dex_file_checksum, uint32_t checksum) {
  return kDebugIgnoreChecksum || dex_file_checksum == checksum;
}

namespace {

// Deflate the input buffer `in_buffer`. It returns a buffer of
// compressed data for the input buffer of `*compressed_data_size` size.
std::unique_ptr<uint8_t[]> DeflateBuffer(ArrayRef<const uint8_t> in_buffer, /*out*/ uint32_t* compressed_data_size) {
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  int init_ret = deflateInit(&strm, 1);
  if (init_ret != Z_OK) {
    return nullptr;
  }

  uint32_t out_size = dchecked_integral_cast<uint32_t>(deflateBound(&strm, in_buffer.size()));

  std::unique_ptr<uint8_t[]> compressed_buffer(new uint8_t[out_size]);
  strm.avail_in = in_buffer.size();
  strm.next_in = const_cast<uint8_t*>(in_buffer.data());
  strm.avail_out = out_size;
  strm.next_out = &compressed_buffer[0];
  int ret = deflate(&strm, Z_FINISH);
  if (ret == Z_STREAM_ERROR) {
    return nullptr;
  }
  *compressed_data_size = out_size - strm.avail_out;

  int end_ret = deflateEnd(&strm);
  if (end_ret != Z_OK) {
    return nullptr;
  }

  return compressed_buffer;
}

// Inflate the data from `in_buffer` into `out_buffer`. The `out_buffer.size()`
// is the expected output size of the buffer. It returns Z_STREAM_END on success.
// On error, it returns Z_STREAM_ERROR if the compressed data is inconsistent
// and Z_DATA_ERROR if the stream ended prematurely or the stream has extra data.
int InflateBuffer(ArrayRef<const uint8_t> in_buffer, /*out*/ ArrayRef<uint8_t> out_buffer) {
  /* allocate inflate state */
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = in_buffer.size();
  strm.next_in = const_cast<uint8_t*>(in_buffer.data());
  strm.avail_out = out_buffer.size();
  strm.next_out = out_buffer.data();

  int init_ret = inflateInit(&strm);
  if (init_ret != Z_OK) {
    return init_ret;
  }

  int ret = inflate(&strm, Z_NO_FLUSH);
  if (strm.avail_in != 0 || strm.avail_out != 0) {
    return Z_DATA_ERROR;
  }

  int end_ret = inflateEnd(&strm);
  if (end_ret != Z_OK) {
    return end_ret;
  }

  return ret;
}

} // anonymous namespace

namespace {

// Deflate the input buffer `in_buffer`. It returns a buffer of
// compressed data for the input buffer of `*compressed_data_size` size.
std::unique_ptr<uint8_t[]> DeflateBuffer(ArrayRef<const uint8_t> in_buffer, /*out*/ uint32_t* compressed_data_size) {
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  int init_ret = deflateInit(&strm, 1);
  if (init_ret != Z_OK) {
    return nullptr;
  }

  uint32_t out_size = dchecked_integral_cast<uint32_t>(deflateBound(&strm, in_buffer.size()));

  std::unique_ptr<uint8_t[]> compressed_buffer(new uint8_t[out_size]);
  strm.avail_in = in_buffer.size();
  strm.next_in = const_cast<uint8_t*>(in_buffer.data());
  strm.avail_out = out_size;
  strm.next_out = &compressed_buffer[0];
  int ret = deflate(&strm, Z_FINISH);
  if (ret == Z_STREAM_ERROR) {
    return nullptr;
  }
  *compressed_data_size = out_size - strm.avail_out;

  int end_ret = deflateEnd(&strm);
  if (end_ret != Z_OK) {
    return nullptr;
  }

  return compressed_buffer;
}

// Inflate the data from `in_buffer` into `out_buffer`. The `out_buffer.size()`
// is the expected output size of the buffer. It returns Z_STREAM_END on success.
// On error, it returns Z_STREAM_ERROR if the compressed data is inconsistent
// and Z_DATA_ERROR if the stream ended prematurely or the stream has extra data.
int InflateBuffer(ArrayRef<const uint8_t> in_buffer, /*out*/ ArrayRef<uint8_t> out_buffer) {
  /* allocate inflate state */
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = in_buffer.size();
  strm.next_in = const_cast<uint8_t*>(in_buffer.data());
  strm.avail_out = out_buffer.size();
  strm.next_out = out_buffer.data();

  int init_ret = inflateInit(&strm);
  if (init_ret != Z_OK) {
    return init_ret;
  }

  int ret = inflate(&strm, Z_NO_FLUSH);
  if (strm.avail_in != 0 || strm.avail_out != 0) {
    return Z_DATA_ERROR;
  }

  int end_ret = inflateEnd(&strm);
  if (end_ret != Z_OK) {
    return end_ret;
  }

  return ret;
}

} // anonymous namespace

enum class ProfileCompilationInfo::ProfileLoadStatus : uint32_t {
kSuccess,kIOError,kBadMagic,kVersionMismatch,kBadData,kMergeError,                // Merging failed. There are too many extra descriptors
                // or classes without TypeId referenced by a dex file.
};

enum class ProfileCompilationInfo::FileSectionType : uint32_t {
// The values of section enumerators and data format for individual sections
// must not be changed without changing the profile file version. New sections
// can be added at the end and they shall be ignored by old versions of ART.
// The list of the dex files included in the profile.
// There must be exactly one dex file section and it must be first.
kDexFiles = 0,// Extra descriptors for referencing classes that do not have a `dex::TypeId`
// in the referencing dex file, such as classes from a different dex file
// (even outside of the dex files in the profile) or array classes that were
// used from other dex files or created through reflection.
kExtraDescriptors = 1,// Classes included in the profile.
kClasses = 2,// Methods included in the profile, their hotness flags and inline caches.
kMethods = 3,  // The number of known sections.
  kNumberOfSections = 4
};

class ProfileCompilationInfo::FileSectionInfo {
public:
  // Constructor for reading from a `ProfileSource`. Data shall be filled from the source.
  FileSectionInfo(){}
  
  // Constructor for writing to a file.
  FileSectionInfo(FileSectionType type, uint32_t file_offset, uint32_t file_size, uint32_t inflated_size): type_(type), file_offset_(file_offset), file_size_(file_size), inflated_size_(inflated_size) {}
  
  void SetFileOffset(uint32_t file_offset) {
    DCHECK_EQ(file_offset_, 0u);
    DCHECK_NE(file_offset, 0u);
    file_offset_ = file_offset;
  }
  
  FileSectionType GetType() const {
    return type_;
  }
  
  uint32_t GetFileOffset() const {
    return file_offset_;
  }
  
  uint32_t GetFileSize() const {
    return file_size_;
  }
  
  uint32_t GetInflatedSize() const {
    return inflated_size_;
  }
  
  uint32_t GetMemSize() const {
    return inflated_size_ != 0u ? inflated_size_ : file_size_;
  }
  
private:
  FileSectionType type_;
  uint32_t file_offset_;
  uint32_t file_size_;
uint32_t inflated_size_;                            // If 0, do not inflate and use data from file directly.
};

// The file header.
class ProfileCompilationInfo::FileHeader {
public:
  // Constructor for reading from a `ProfileSource`. Data shall be filled from the source.
  FileHeader(){
    DCHECK(!IsValid());
  }
  
  // Constructor for writing to a file.
  FileHeader(const uint8_t* version, uint32_t file_section_count): file_section_count_(file_section_count) {
    static_assert(sizeof(magic_) == sizeof(kProfileMagic));
    static_assert(sizeof(version_) == sizeof(kProfileVersion));
    static_assert(sizeof(version_) == sizeof(kProfileVersionForBootImage));
    memcpy(magic_, kProfileMagic, sizeof(kProfileMagic));
    memcpy(version_, version, sizeof(version_));
    DCHECK_LE(file_section_count, kMaxFileSectionCount);
    DCHECK(IsValid());
  }
  
  bool IsValid() const {
    return memcmp(magic_, kProfileMagic, sizeof(kProfileMagic)) == 0 &&
           (memcmp(version_, kProfileVersion, kProfileVersionSize) == 0 ||
            memcmp(version_, kProfileVersionForBootImage, kProfileVersionSize) == 0) &&
           file_section_count_ != 0u &&  // The dex files section is mandatory.
           file_section_count_ <= kMaxFileSectionCount;
  }
  
  const uint8_t* GetVersion() const {
    DCHECK(IsValid());
    return version_;
  }
  
  ProfileLoadStatus InvalidHeaderMessage(/*out*/ std::string* error_msg) const;
  
  uint32_t GetFileSectionCount() const {
    DCHECK(IsValid());
    return file_section_count_;
  }
  
private:
  // The upper bound for file section count is used to ensure that there
  // shall be no arithmetic overflow when calculating size of the header
  // with section information.
  static const uint32_t kMaxFileSectionCount;
  
  uint8_t magic_[4] = {0, 0, 0, 0};
  uint8_t version_[4] = {0, 0, 0, 0};
  uint32_t file_section_count_ = 0u;
};

private:
uint8_t* ptr_end_;
void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

/**
 * Encapsulate the source of profile data for loading.
 * The source can be either a plain file or a zip file.
 * For zip files, the profile entry will be extracted to
 * the memory map.
 */
class ProfileCompilationInfo::ProfileSource {
void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

private:
uint8_t* ptr_end_;
uint8_t* ptr_end_;
uint8_t* ptr_end_;
FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

uint8_t* ptr_end_;
                // The fd is not owned by this class.
uint8_t* ptr_end_;
uint8_t* ptr_end_;
                        // Current position in the map to read from.
};

// A helper structure to make sure we don't read past our buffers in the loops.
// Also used for writing but the buffer should be pre-sized correctly for that, so we
// DCHECK() we do not write beyond the end, rather than returning `false` on failure.
class ProfileCompilationInfo::SafeBuffer {
FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

private:
uint8_t* ptr_end_;
uint8_t* ptr_end_;
uint8_t* ptr_end_;
};

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

// Returns true if all the bytes were successfully written to the file descriptor.
static bool WriteBuffer(int fd, const uint8_t* buffer, const void* buffer, size_t byte_count) {
  while (byte_count > 0) {
    int bytes_written = TEMP_FAILURE_RETRY(write(fd, buffer, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;  // Reduce the number of remaining bytes.
    reinterpret_cast<const uint8_t*&>(buffer) += bytes_written;  // Move the buffer forward.
  }
  return true;
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

// To simplify the implementation we use the MethodHotness flag values as indexes into the internal
// bitmap representation. As such, they should never change unless the profile version is updated
// and the implementation changed accordingly.
static_assert(ProfileCompilationInfo::MethodHotness::kFlagFirst == 1 << 0);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagHot == 1 << 0);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagStartup == 1 << 1);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagPostStartup == 1 << 2);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagLastRegular == 1 << 2);
static_assert(ProfileCompilationInfo::MethodHotness::kFlag32bit == 1 << 3);
static_assert(ProfileCompilationInfo::MethodHotness::kFlag64bit == 1 << 4);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagSensitiveThread == 1 << 5);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagAmStartup == 1 << 6);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagAmPostStartup == 1 << 7);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagBoot == 1 << 8);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagPostBoot == 1 << 9);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagStartupBin == 1 << 10);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagStartupMaxBin == 1 << 15);
static_assert(ProfileCompilationInfo::MethodHotness::kFlagLastBoot == 1 << 15);

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

std::ostream& operator<<(std::ostream& stream,
                         ProfileCompilationInfo::DexReferenceDumper dumper) {
  stream << "[profile_key=" << dumper.GetProfileKey()
         << ",dex_checksum=" << std::hex << dumper.GetDexChecksum() << std::dec
         << ",num_type_ids=" << dumper.GetNumTypeIds()
         << ",num_method_ids=" << dumper.GetNumMethodIds()
         << "]";
  return stream;
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

FlattenProfileData::ItemMetadata::ItemMetadata(const ItemMetadata& other) :
    flags_(other.flags_),
    annotations_(other.annotations_) {
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

void FlattenProfileData::MergeData(const FlattenProfileData& other) {
  auto create_metadata_fn = []() { return FlattenProfileData::ItemMetadata(); };
  for (const auto& it : other.method_metadata_) {
    const MethodReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        method_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_methods_ = std::max(
          max_aggregation_for_methods_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
  for (const auto& it : other.class_metadata_) {
    const TypeReference& otherRef = it.first;
    const FlattenProfileData::ItemMetadata otherData = it.second;
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& other_annotations =
        otherData.GetAnnotations();

    FlattenProfileData::ItemMetadata& metadata =
        class_metadata_.GetOrCreate(otherRef, create_metadata_fn);
    metadata.flags_ |= otherData.GetFlags();
    metadata.annotations_.insert(
        metadata.annotations_.end(), other_annotations.begin(), other_annotations.end());

    max_aggregation_for_classes_ = std::max(
          max_aggregation_for_classes_,
          static_cast<uint32_t>(metadata.annotations_.size()));
  }
}

} // namespace art
