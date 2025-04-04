#ifndef ART_LIBPROFILE_PROFILE_PROFILE_COMPILATION_INFO_H_
#define ART_LIBPROFILE_PROFILE_PROFILE_COMPILATION_INFO_H_ 
#include <array>
#include <list>
#include <set>
#include <string_view>
#include <vector>
#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "base/array_ref.h"
#include "base/atomic.h"
#include "base/bit_memory_region.h"
#include "base/hash_map.h"
#include "base/hash_set.h"
#include "base/malloc_arena_pool.h"
#include "base/mem_map.h"
#include "base/safe_map.h"
#include "dex/dex_file.h"
#include "dex/dex_file_types.h"
#include "dex/method_reference.h"
#include "dex/type_reference.h"
namespace art {
struct ProfileMethodInfo {
  struct ProfileInlineCache {
    ProfileInlineCache(uint32_t pc,
                       bool missing_types,
                       const std::vector<TypeReference>& profile_classes,
                       bool megamorphic = false)
        : dex_pc(pc),
          is_missing_types(missing_types),
          classes(profile_classes),
          is_megamorphic(megamorphic) {}
    const uint32_t dex_pc;
    const bool is_missing_types;
    const std::vector<TypeReference> classes;
    const bool is_megamorphic;
  };
  explicit ProfileMethodInfo(MethodReference reference) : ref(reference) {}
  ProfileMethodInfo(MethodReference reference, const std::vector<ProfileInlineCache>& caches)
      : ref(reference),
        inline_caches(caches) {}
  MethodReference ref;
  std::vector<ProfileInlineCache> inline_caches;
};
class FlattenProfileData;
class ProfileCompilationInfo {
public:
  static const uint8_t kProfileMagic[];
  static const uint8_t kProfileVersion[];
  static const uint8_t kProfileVersionForBootImage[];
  static const char kDexMetadataProfileEntry[];
  static constexpr size_t kProfileVersionSize = 4;
  static constexpr uint8_t kIndividualInlineCacheSize = 5;
  using ProfileIndexType = uint16_t;
  struct ClassReference : public ValueObject {
    ClassReference(ProfileIndexType dex_profile_idx, const dex::TypeIndex type_idx) :
      dex_profile_index(dex_profile_idx), type_index(type_idx) {}
    bool operator==(const ClassReference& other) const {
      return dex_profile_index == other.dex_profile_index && type_index == other.type_index;
    }
    bool operator<(const ClassReference& other) const {
      return dex_profile_index == other.dex_profile_index
          ? type_index < other.type_index
          : dex_profile_index < other.dex_profile_index;
    }
    ProfileIndexType dex_profile_index;
    dex::TypeIndex type_index;
  };
  struct DexPcData : public ArenaObject<kArenaAllocProfile> {
    explicit DexPcData(ArenaAllocator* allocator)
        : DexPcData(allocator->Adapter(kArenaAllocProfile)) {}
    explicit DexPcData(const ArenaAllocatorAdapter<void>& allocator)
        : is_missing_types(false),
          is_megamorphic(false),
          classes(std::less<dex::TypeIndex>(), allocator) {}
    void AddClass(const dex::TypeIndex& type_idx);
    void SetIsMegamorphic() {
      if (is_missing_types) return;
      is_megamorphic = true;
      classes.clear();
    }
    void SetIsMissingTypes() {
      is_megamorphic = false;
      is_missing_types = true;
      classes.clear();
    }
    bool operator==(const DexPcData& other) const {
      return is_megamorphic == other.is_megamorphic &&
          is_missing_types == other.is_missing_types &&
          classes == other.classes;
    }
    bool is_missing_types;
    bool is_megamorphic;
    ArenaSet<dex::TypeIndex> classes;
  };
  using InlineCacheMap = ArenaSafeMap<uint16_t, DexPcData>;
  using MethodMap = ArenaSafeMap<uint16_t, InlineCacheMap>;
  class MethodHotness {
   public:
    enum Flag {
      kFlagFirst = 1 << 0,
      kFlagHot = 1 << 0,
      kFlagStartup = 1 << 1,
      kFlagPostStartup = 1 << 2,
      kFlagLastRegular = 1 << 2,
      kFlag32bit = 1 << 3,
      kFlag64bit = 1 << 4,
      kFlagSensitiveThread = 1 << 5,
      kFlagAmStartup = 1 << 6,
      kFlagAmPostStartup = 1 << 7,
      kFlagBoot = 1 << 8,
      kFlagPostBoot = 1 << 9,
      kFlagStartupBin = 1 << 10,
      kFlagStartupMaxBin = 1 << 15,
      kFlagLastBoot = 1 << 15,
    };
    bool IsHot() const {
      return (flags_ & kFlagHot) != 0;
    }
    bool IsStartup() const {
      return (flags_ & kFlagStartup) != 0;
    }
    bool IsPostStartup() const {
      return (flags_ & kFlagPostStartup) != 0;
    }
    void AddFlag(Flag flag) {
      flags_ |= flag;
    }
    uint32_t GetFlags() const {
      return flags_;
    }
    bool HasFlagSet(MethodHotness::Flag flag) {
      return (flags_ & flag ) != 0;
    }
    bool IsInProfile() const {
      return flags_ != 0;
    }
    const InlineCacheMap* GetInlineCacheMap() const {
      return inline_cache_map_;
    }
   private:
    const InlineCacheMap* inline_cache_map_ = nullptr;
    uint32_t flags_ = 0;
    void SetInlineCacheMap(const InlineCacheMap* info) {
      inline_cache_map_ = info;
    }
    friend class ProfileCompilationInfo;
  };
  class ProfileSampleAnnotation {
   public:
    explicit ProfileSampleAnnotation(const std::string& package_name) :
        origin_package_name_(package_name) {}
    const std::string& GetOriginPackageName() const { return origin_package_name_; }
    bool operator==(const ProfileSampleAnnotation& other) const {
      return origin_package_name_ == other.origin_package_name_;
    }
    bool operator<(const ProfileSampleAnnotation& other) const {
      return origin_package_name_ < other.origin_package_name_;
    }
    static const ProfileSampleAnnotation kNone;
   private:
    const std::string origin_package_name_;
  };
  struct DexReferenceDumper;
  ProfileCompilationInfo();
  explicit ProfileCompilationInfo(bool for_boot_image);
  explicit ProfileCompilationInfo(ArenaPool* arena_pool);
  ProfileCompilationInfo(ArenaPool* arena_pool, bool for_boot_image);
  ~ProfileCompilationInfo();
  bool AddMethods(const std::vector<ProfileMethodInfo>& methods,
                  MethodHotness::Flag flags,
                  const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone);
  dex::TypeIndex FindOrCreateTypeIndex(const DexFile& dex_file, TypeReference class_ref);
  dex::TypeIndex FindOrCreateTypeIndex(const DexFile& dex_file, const char* descriptor);
  void AddClass(ProfileIndexType profile_index, dex::TypeIndex type_index) {
    DCHECK_LT(profile_index, info_.size());
    DexFileData* const data = info_[profile_index].get();
    DCHECK(type_index.IsValid());
    DCHECK(type_index.index_ <= data->num_type_ids ||
           type_index.index_ - data->num_type_ids < extra_descriptors_.size());
    data->class_set.insert(type_index);
  }
  bool AddClass(const DexFile& dex_file, dex::TypeIndex type_index, const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    DCHECK(type_index.IsValid());
    DCHECK(type_index.index_ <= dex_file.NumTypeIds() ||
           type_index.index_ - dex_file.NumTypeIds() < extra_descriptors_.size());
    DexFileData* const data = GetOrAddDexFileData(&dex_file, annotation);
    if (data == nullptr) {
      return false;
    }
    data->class_set.insert(type_index);
    return true;
  }
  bool AddClass(const DexFile& dex_file,
                dex::TypeIndex type_index,
                const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone);
  bool AddClass(const DexFile& dex_file,
                const char* descriptor,
                const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone);
  bool AddClass(const DexFile& dex_file, const std::string& descriptor, const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    return AddClass(dex_file, descriptor.c_str(), annotation);
  }
  bool AddClass(const DexFile& dex_file, std::string_view descriptor, const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    return AddClass(dex_file, std::string(descriptor).c_str(), annotation);
  }
  template <class Iterator>
  bool AddClassesForDex(
      const DexFile* dex_file,
      Iterator index_begin,
      Iterator index_end,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    DexFileData* data = GetOrAddDexFileData(dex_file, annotation);
    if (data == nullptr) {
      return false;
    }
    data->class_set.insert(index_begin, index_end);
    return true;
  }
  void AddMethod(ProfileIndexType profile_index, uint32_t method_index, MethodHotness::Flag flags) {
    DCHECK_LT(profile_index, info_.size());
    DexFileData* const data = info_[profile_index].get();
    DCHECK_LT(method_index, data->num_method_ids);
    data->AddMethod(flags, method_index);
  }
  bool AddMethod(const ProfileMethodInfo& pmi,
                 MethodHotness::Flag flags,
                 const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone);
  template <class Iterator>
  bool AddMethodsForDex(
      MethodHotness::Flag flags,
      const DexFile* dex_file,
      Iterator index_begin,
      Iterator index_end,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    DexFileData* data = GetOrAddDexFileData(dex_file, annotation);
    if (data == nullptr) {
      return false;
    }
    for (Iterator it = index_begin; it != index_end; ++it) {
      DCHECK_LT(*it, data->num_method_ids);
      if (!data->AddMethod(flags, *it)) {
        return false;
      }
    }
    return true;
  }
  using ProfileLoadFilterFn = std::function<bool(const std::string&, uint32_t)>;
  static bool ProfileFilterFnAcceptAll(const std::string& dex_location, uint32_t checksum);
  bool Load(
      int fd,
      bool merge_classes = true,
      const ProfileLoadFilterFn& filter_fn = ProfileFilterFnAcceptAll);
  bool VerifyProfileData(const std::vector<const DexFile*>& dex_files);
  bool Load(const std::string& filename, bool clear_if_invalid);
  bool MergeWith(const ProfileCompilationInfo& info, bool merge_classes = true);
  bool MergeWith(const std::string& filename);
  bool Save(int fd);
  bool Save(const std::string& filename, uint64_t* bytes_written);
  size_t GetNumberOfDexFiles() const {
    return info_.size();
  }
  uint32_t GetNumberOfMethods() const;
  uint32_t GetNumberOfResolvedClasses() const;
  MethodHotness GetMethodHotness(
      const MethodReference& method_ref,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) const;
  bool ContainsClass(
      const DexFile& dex_file,
      dex::TypeIndex type_idx,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) const;
  template <typename Container>
  const DexFile* FindDexFileForProfileIndex(ProfileIndexType profile_index,
                                            const Container& dex_files) const {
    static_assert(std::is_same_v<typename Container::value_type, const DexFile*> ||
                  std::is_same_v<typename Container::value_type, std::unique_ptr<const DexFile>>);
    DCHECK_LE(profile_index, info_.size());
    const DexFileData* dex_file_data = info_[profile_index].get();
    DCHECK(dex_file_data != nullptr);
    uint32_t dex_checksum = dex_file_data->checksum;
    std::string_view base_key = GetBaseKeyViewFromAugmentedKey(dex_file_data->profile_key);
    for (const auto& dex_file : dex_files) {
      if (dex_checksum == dex_file->GetLocationChecksum() &&
          base_key == GetProfileDexFileBaseKeyView(dex_file->GetLocation())) {
        return std::addressof(*dex_file);
      }
    }
    return nullptr;
  }
  DexReferenceDumper DumpDexReference(ProfileIndexType profile_index) const;
  std::string DumpInfo(const std::vector<const DexFile*>& dex_files,
                       bool print_full_dex_location = true) const;
  bool GetClassesAndMethods(
      const DexFile& dex_file,
             std::set<dex::TypeIndex>* class_set,
             std::set<uint16_t>* hot_method_set,
             std::set<uint16_t>* startup_method_set,
             std::set<uint16_t>* post_startup_method_method_set,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) const;
  bool SameVersion(const ProfileCompilationInfo& other) const;
  bool Equals(const ProfileCompilationInfo& other);
  static std::string GetProfileDexFileBaseKey(const std::string& dex_location);
  static std::string GetBaseKeyFromAugmentedKey(const std::string& profile_key);
  static ProfileSampleAnnotation GetAnnotationFromKey(const std::string& augmented_key);
  static bool GenerateTestProfile(int fd,
                                  uint16_t number_of_dex_files,
                                  uint16_t method_ratio,
                                  uint16_t class_ratio,
                                  uint32_t random_seed);
  static bool GenerateTestProfile(int fd,
                                  std::vector<std::unique_ptr<const DexFile>>& dex_files,
                                  uint16_t method_percentage,
                                  uint16_t class_percentage,
                                  uint32_t random_seed);
  ArenaAllocator* GetAllocator() { return &allocator_; }
  HashSet<std::string> GetClassDescriptors(
      const std::vector<const DexFile*>& dex_files,
      const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone);
  bool IsProfileFile(int fd);
  bool UpdateProfileKeys(const std::vector<std::unique_ptr<const DexFile>>& dex_files);
  bool IsEmpty() const;
  void ClearData();
  void ClearDataAndAdjustVersion(bool for_boot_image);
  void PrepareForAggregationCounters();
  bool IsForBootImage() const;
  const char* GetTypeDescriptor(const DexFile* dex_file, dex::TypeIndex type_index) const {
    DCHECK(type_index.IsValid());
    uint32_t num_type_ids = dex_file->NumTypeIds();
    if (type_index.index_ < num_type_ids) {
      return dex_file->StringByTypeIdx(type_index);
    } else {
      return extra_descriptors_[type_index.index_ - num_type_ids].c_str();
    }
  }
  const uint8_t* GetVersion() const;
  std::unique_ptr<FlattenProfileData> ExtractProfileData(
      const std::vector<std::unique_ptr<const DexFile>>& dex_files) const;
private:
  class FileHeader;
  class FileSectionInfo;
  enum class FileSectionType : uint32_t;
  enum class ProfileLoadStatus : uint32_t;
  class ProfileSource;
  class SafeBuffer;
  using ExtraDescriptorIndex = uint16_t;
  static constexpr ExtraDescriptorIndex kMaxExtraDescriptors = DexFile::kDexNoIndex16;
  class ExtraDescriptorIndexEmpty {
   public:
    void MakeEmpty(ExtraDescriptorIndex& index) const {
      index = kMaxExtraDescriptors;
    }
    bool IsEmpty(const ExtraDescriptorIndex& index) const {
      return index == kMaxExtraDescriptors;
    }
  };
  class ExtraDescriptorHash {
   public:
    explicit ExtraDescriptorHash(const dchecked_vector<std::string>* extra_descriptors)
        : extra_descriptors_(extra_descriptors) {}
    size_t operator()(const ExtraDescriptorIndex& index) const {
      std::string_view str = (*extra_descriptors_)[index];
      return (*this)(str);
    }
    size_t operator()(std::string_view str) const {
      return DataHash()(str);
    }
   private:
    const dchecked_vector<std::string>* extra_descriptors_;
  };
  class ExtraDescriptorEquals {
   public:
    explicit ExtraDescriptorEquals(const dchecked_vector<std::string>* extra_descriptors)
        : extra_descriptors_(extra_descriptors) {}
    size_t operator()(const ExtraDescriptorIndex& lhs, const ExtraDescriptorIndex& rhs) const {
      DCHECK_EQ(lhs == rhs, (*this)(lhs, (*extra_descriptors_)[rhs]));
      return lhs == rhs;
    }
    size_t operator()(const ExtraDescriptorIndex& lhs, std::string_view rhs_str) const {
      std::string_view lhs_str = (*extra_descriptors_)[lhs];
      return lhs_str == rhs_str;
    }
   private:
    const dchecked_vector<std::string>* extra_descriptors_;
  };
  using ExtraDescriptorHashSet = HashSet<ExtraDescriptorIndex,
                                         ExtraDescriptorIndexEmpty,
                                         ExtraDescriptorHash,
                                         ExtraDescriptorEquals>;
  struct DexFileData : public DeletableArenaObject<kArenaAllocProfile> {
    DexFileData(ArenaAllocator* allocator,
                const std::string& key,
                uint32_t location_checksum,
                uint16_t index,
                uint32_t num_types,
                uint32_t num_methods,
                bool for_boot_image)
        : allocator_(allocator),
          profile_key(key),
          profile_index(index),
          checksum(location_checksum),
          method_map(std::less<uint16_t>(), allocator->Adapter(kArenaAllocProfile)),
          class_set(std::less<dex::TypeIndex>(), allocator->Adapter(kArenaAllocProfile)),
          num_type_ids(num_types),
          num_method_ids(num_methods),
          bitmap_storage(allocator->Adapter(kArenaAllocProfile)),
          is_for_boot_image(for_boot_image) {
      bitmap_storage.resize(ComputeBitmapStorage(is_for_boot_image, num_method_ids));
      if (!bitmap_storage.empty()) {
        method_bitmap =
            BitMemoryRegion(MemoryRegion(
                &bitmap_storage[0],
                bitmap_storage.size()),
                0,
                ComputeBitmapBits(is_for_boot_image, num_method_ids));
      }
    }
    static size_t ComputeBitmapBits(bool is_for_boot_image, uint32_t num_method_ids) {
      size_t flag_bitmap_index = FlagBitmapIndex(is_for_boot_image
          ? MethodHotness::kFlagLastBoot
          : MethodHotness::kFlagLastRegular);
      return num_method_ids * (flag_bitmap_index + 1);
    }
    static size_t ComputeBitmapStorage(bool is_for_boot_image, uint32_t num_method_ids) {
      return RoundUp(ComputeBitmapBits(is_for_boot_image, num_method_ids), kBitsPerByte) /
          kBitsPerByte;
    }
    bool operator==(const DexFileData& other) const {
      return checksum == other.checksum &&
          num_method_ids == other.num_method_ids &&
          method_map == other.method_map &&
          class_set == other.class_set &&
          (BitMemoryRegion::Compare(method_bitmap, other.method_bitmap) == 0);
    }
    bool AddMethod(MethodHotness::Flag flags, size_t index);
    void MergeBitmap(const DexFileData& other) {
      DCHECK_EQ(bitmap_storage.size(), other.bitmap_storage.size());
      for (size_t i = 0; i < bitmap_storage.size(); ++i) {
        bitmap_storage[i] |= other.bitmap_storage[i];
      }
    }
    void SetMethodHotness(size_t index, MethodHotness::Flag flags);
    MethodHotness GetHotnessInfo(uint32_t dex_method_index) const;
    bool ContainsClass(dex::TypeIndex type_index) const;
    uint32_t ClassesDataSize() const;
    void WriteClasses(SafeBuffer& buffer) const;
    ProfileLoadStatus ReadClasses(
        SafeBuffer& buffer,
        const dchecked_vector<ExtraDescriptorIndex>& extra_descriptors_remap,
        std::string* error);
    static ProfileLoadStatus SkipClasses(SafeBuffer& buffer, std::string* error);
    uint32_t MethodsDataSize( uint16_t* method_flags = nullptr,
                                     size_t* saved_bitmap_bit_size = nullptr) const;
    void WriteMethods(SafeBuffer& buffer) const;
    ProfileLoadStatus ReadMethods(
        SafeBuffer& buffer,
        const dchecked_vector<ExtraDescriptorIndex>& extra_descriptors_remap,
        std::string* error);
    static ProfileLoadStatus SkipMethods(SafeBuffer& buffer, std::string* error);
    ArenaAllocator* const allocator_;
    std::string profile_key;
    ProfileIndexType profile_index;
    uint32_t checksum;
    MethodMap method_map;
    ArenaSet<dex::TypeIndex> class_set;
    InlineCacheMap* FindOrAddHotMethod(uint16_t method_index);
    uint32_t num_type_ids;
    uint32_t num_method_ids;
    ArenaVector<uint8_t> bitmap_storage;
    BitMemoryRegion method_bitmap;
    bool is_for_boot_image;
   private:
    template <typename Fn>
    void ForMethodBitmapHotnessFlags(Fn fn) const;
    static void WriteClassSet(SafeBuffer& buffer, const ArenaSet<dex::TypeIndex>& class_set);
    size_t MethodFlagBitmapIndex(MethodHotness::Flag flag, size_t method_index) const;
    static size_t FlagBitmapIndex(MethodHotness::Flag flag);
    uint16_t GetUsedBitmapFlags() const;
  };
  DexFileData* GetOrAddDexFileData(const std::string& profile_key,
                                   uint32_t checksum,
                                   uint32_t num_type_ids,
                                   uint32_t num_method_ids);
  DexFileData* GetOrAddDexFileData(const DexFile* dex_file,
                                   const ProfileSampleAnnotation& annotation) {
    return GetOrAddDexFileData(GetProfileDexFileAugmentedKey(dex_file->GetLocation(), annotation),
                               dex_file->GetLocationChecksum(),
                               dex_file->NumTypeIds(),
                               dex_file->NumMethodIds());
  }
  const DexFileData* FindDexData(const std::string& profile_key,
                                 uint32_t checksum,
                                 bool verify_checksum = true) const;
  const DexFileData* FindDexDataUsingAnnotations(
      const DexFile* dex_file,
      const ProfileSampleAnnotation& annotation) const;
  void FindAllDexData(
      const DexFile* dex_file,
              std::vector<const ProfileCompilationInfo::DexFileData*>* result) const;
  ExtraDescriptorIndex AddExtraDescriptor(std::string_view extra_descriptor);
  ProfileLoadStatus OpenSource(int32_t fd,
                                       std::unique_ptr<ProfileSource>* source,
                                       std::string* error);
  ProfileLoadStatus ReadSectionData(ProfileSource& source,
                                    const FileSectionInfo& section_info,
                                            SafeBuffer* buffer,
                                            std::string* error);
  ProfileLoadStatus ReadDexFilesSection(
      ProfileSource& source,
      const FileSectionInfo& section_info,
      const ProfileLoadFilterFn& filter_fn,
              dchecked_vector<ProfileIndexType>* dex_profile_index_remap,
              std::string* error);
  ProfileLoadStatus ReadExtraDescriptorsSection(
      ProfileSource& source,
      const FileSectionInfo& section_info,
              dchecked_vector<ExtraDescriptorIndex>* extra_descriptors_remap,
              std::string* error);
  ProfileLoadStatus ReadClassesSection(
      ProfileSource& source,
      const FileSectionInfo& section_info,
      const dchecked_vector<ProfileIndexType>& dex_profile_index_remap,
      const dchecked_vector<ExtraDescriptorIndex>& extra_descriptors_remap,
              std::string* error);
  ProfileLoadStatus ReadMethodsSection(
      ProfileSource& source,
      const FileSectionInfo& section_info,
      const dchecked_vector<ProfileIndexType>& dex_profile_index_remap,
      const dchecked_vector<ExtraDescriptorIndex>& extra_descriptors_remap,
              std::string* error);
  ProfileLoadStatus LoadInternal(
      int32_t fd,
      std::string* error,
      bool merge_classes = true,
      const ProfileLoadFilterFn& filter_fn = ProfileFilterFnAcceptAll);
  static DexPcData* FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc);
  void InitProfileVersionInternal(const uint8_t version[]);
  size_t GetSizeWarningThresholdBytes() const;
  size_t GetSizeErrorThresholdBytes() const;
  static std::string_view GetProfileDexFileBaseKeyView(std::string_view dex_location);
  static std::string_view GetBaseKeyViewFromAugmentedKey(std::string_view dex_location);
  static std::string GetProfileDexFileAugmentedKey(const std::string& dex_location,
                                                   const ProfileSampleAnnotation& annotation);
  static std::string MigrateAnnotationInfo(const std::string& base_key,
                                           const std::string& augmented_key);
  static constexpr ProfileIndexType MaxProfileIndex() {
    return std::numeric_limits<ProfileIndexType>::max();
  }
public:
  ProfileIndexType FindOrAddDexFile(const DexFile& dex_file, const ProfileSampleAnnotation& annotation = ProfileSampleAnnotation::kNone) {
    DexFileData* data = GetOrAddDexFileData(&dex_file, annotation);
    return (data != nullptr) ? data->profile_index : MaxProfileIndex();
  }
private:
  friend class ProfileCompilationInfoTest;
  friend class CompilerDriverProfileTest;
  friend class ProfileAssistantTest;
  friend class Dex2oatLayoutTest;
  MallocArenaPool default_arena_pool_;
  ArenaAllocator allocator_;
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
  ArenaSafeMap<const std::string_view, ProfileIndexType> profile_key_map_;
  dchecked_vector<std::string> extra_descriptors_;
  ExtraDescriptorHashSet extra_descriptors_indexes_;
const ProfileCompilationInfo::ProfileSampleAnnotation
  ProfileCompilationInfo::ProfileSampleAnnotation::kNone =
      ProfileCompilationInfo::ProfileSampleAnnotation("");
};
class FlattenProfileData {
public:
  class ItemMetadata {
   public:
    ItemMetadata();
    ItemMetadata(const ItemMetadata& other);
    uint16_t GetFlags() const {
      return flags_;
    }
    const std::list<ProfileCompilationInfo::ProfileSampleAnnotation>& GetAnnotations() const {
      return annotations_;
    }
    void AddFlag(ProfileCompilationInfo::MethodHotness::Flag flag) {
      flags_ |= flag;
    }
    bool HasFlagSet(ProfileCompilationInfo::MethodHotness::Flag flag) const {
      return (flags_ & flag) != 0;
    }
   private:
    uint16_t flags_;
    std::list<ProfileCompilationInfo::ProfileSampleAnnotation> annotations_;
    friend class ProfileCompilationInfo;
    friend class FlattenProfileData;
  };
  FlattenProfileData();
  const SafeMap<MethodReference, ItemMetadata>& GetMethodData() const {
    return method_metadata_;
  }
  const SafeMap<TypeReference, ItemMetadata>& GetClassData() const {
    return class_metadata_;
  }
  uint32_t GetMaxAggregationForMethods() const {
    return max_aggregation_for_methods_;
  }
  uint32_t GetMaxAggregationForClasses() const {
    return max_aggregation_for_classes_;
  }
  void MergeData(const FlattenProfileData& other);
private:
  SafeMap<MethodReference, ItemMetadata> method_metadata_;
  SafeMap<TypeReference, ItemMetadata> class_metadata_;
  uint32_t max_aggregation_for_methods_;
  uint32_t max_aggregation_for_classes_;
  friend class ProfileCompilationInfo;
};
struct ProfileCompilationInfo::DexReferenceDumper {
  const std::string& GetProfileKey() {
    return dex_file_data->profile_key;
  }
  uint32_t GetDexChecksum() const {
    return dex_file_data->checksum;
  }
  uint32_t GetNumTypeIds() const {
    return dex_file_data->num_type_ids;
  }
  uint32_t GetNumMethodIds() const {
    return dex_file_data->num_method_ids;
  }
  const DexFileData* dex_file_data;
};
inline ProfileCompilationInfo::DexReferenceDumper ProfileCompilationInfo::DumpDexReference(
    ProfileIndexType profile_index) const {
  return DexReferenceDumper{info_[profile_index].get()};
}
std::ostream& operator<<(std::ostream& stream, ProfileCompilationInfo::DexReferenceDumper dumper);
}
#endif
