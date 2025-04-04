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
#include "base/logging.h"
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
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
static_assert(sizeof(ProfileCompilationInfo::kProfileVersion) == 4,
              "Invalid profile version size");
static_assert(sizeof(ProfileCompilationInfo::kProfileVersionForBootImage) == 4,
              "Invalid profile version size");
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
static constexpr char kSampleMetadataSeparator = ':';
static constexpr uint16_t kMaxDexFileKeyLength = 1024;
static constexpr uint32_t kMaxSupportedMethodIndex = 0xffffu;
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
std::unique_ptr<uint8_t[]> DeflateBuffer(ArrayRef<const uint8_t> in_buffer, uint32_t* compressed_data_size) {
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
int InflateBuffer(ArrayRef<const uint8_t> in_buffer, ArrayRef<uint8_t> out_buffer) {
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
}
namespace {
std::unique_ptr<uint8_t[]> DeflateBuffer(ArrayRef<const uint8_t> in_buffer, uint32_t* compressed_data_size) {
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
int InflateBuffer(ArrayRef<const uint8_t> in_buffer, ArrayRef<uint8_t> out_buffer) {
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
}
enum class ProfileCompilationInfo::ProfileLoadStatus : uint32_t {
kSuccess,kIOError,kBadMagic,kVersionMismatch,kBadData,kMergeError,
};
enum class ProfileCompilationInfo::FileSectionType : uint32_t {
kDexFiles = 0,
kExtraDescriptors = 1,
kClasses = 2,
kMethods = 3,
  kNumberOfSections = 4
};
class ProfileCompilationInfo::FileSectionInfo {
public:
  FileSectionInfo(){}
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
uint32_t inflated_size_;
};
class ProfileCompilationInfo::FileHeader {
public:
  FileHeader(){
    DCHECK(!IsValid());
  }
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
           file_section_count_ != 0u &&
           file_section_count_ <= kMaxFileSectionCount;
  }
  const uint8_t* GetVersion() const {
    DCHECK(IsValid());
    return version_;
  }
  ProfileLoadStatus InvalidHeaderMessage( std::string* error_msg) const;
  uint32_t GetFileSectionCount() const {
    DCHECK(IsValid());
    return file_section_count_;
  }
private:
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
uint8_t* ptr_end_;
uint8_t* ptr_end_;
};
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
static bool WriteBuffer(int fd, const uint8_t* buffer, const void* buffer, size_t byte_count) {
  while (byte_count > 0) {
    int bytes_written = TEMP_FAILURE_RETRY(write(fd, buffer, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;
    reinterpret_cast<const uint8_t*&>(buffer) += bytes_written;
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
}
