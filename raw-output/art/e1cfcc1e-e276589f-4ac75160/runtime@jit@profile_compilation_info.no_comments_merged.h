#ifndef ART_RUNTIME_JIT_PROFILE_COMPILATION_INFO_H_
#define ART_RUNTIME_JIT_PROFILE_COMPILATION_INFO_H_ 
#include <set>
#include <vector>
#include "atomic.h"
#include "base/arena_object.h"
#include "base/arena_containers.h"
#include "dex_cache_resolved_classes.h"
#include "dex_file.h"
#include "dex_file_types.h"
#include "method_reference.h"
#include "safe_map.h"
#include "type_reference.h"
namespace art {
struct ProfileMethodInfo {
<<<<<<< HEAD
||||||| 4ac7516066
  struct ProfileClassReference {
    ProfileClassReference() : dex_file(nullptr) {}
    ProfileClassReference(const DexFile* dex, const dex::TypeIndex& index)
        : dex_file(dex), type_index(index) {}
    const DexFile* dex_file;
    dex::TypeIndex type_index;
  };
=======
  struct ProfileClassReference {
    ProfileClassReference() : dex_file(nullptr) {}
    ProfileClassReference(const DexFile* dex, const dex::TypeIndex index)
        : dex_file(dex), type_index(index) {}
    const DexFile* dex_file;
    dex::TypeIndex type_index;
  };
>>>>>>> e276589f
  struct ProfileInlineCache {
    ProfileInlineCache(uint32_t pc,
                       bool missing_types,
                       const std::vector<TypeReference>& profile_classes)
        : dex_pc(pc), is_missing_types(missing_types), classes(profile_classes) {}
    const uint32_t dex_pc;
    const bool is_missing_types;
    const std::vector<TypeReference> classes;
  };
  ProfileMethodInfo(const DexFile* dex, uint32_t method_index)
      : dex_file(dex), dex_method_index(method_index) {}
  ProfileMethodInfo(const DexFile* dex,
                    uint32_t method_index,
                    const std::vector<ProfileInlineCache>& caches)
      : dex_file(dex), dex_method_index(method_index), inline_caches(caches) {}
  const DexFile* dex_file;
  const uint32_t dex_method_index;
  const std::vector<ProfileInlineCache> inline_caches;
};
class ProfileCompilationInfo {
 public:
  static const uint8_t kProfileMagic[];
  static const uint8_t kProfileVersion[];
  struct DexReference {
    DexReference() : dex_checksum(0) {}
    DexReference(const std::string& location, uint32_t checksum)
        : dex_location(location), dex_checksum(checksum) {}
    bool operator==(const DexReference& other) const {
      return dex_checksum == other.dex_checksum && dex_location == other.dex_location;
    }
    bool MatchesDex(const DexFile* dex_file) const {
      return dex_checksum == dex_file->GetLocationChecksum() &&
           dex_location == GetProfileDexFileKey(dex_file->GetLocation());
    }
    std::string dex_location;
    uint32_t dex_checksum;
  };
  struct ClassReference : public ValueObject {
    ClassReference(uint8_t dex_profile_idx, const dex::TypeIndex type_idx) :
      dex_profile_index(dex_profile_idx), type_index(type_idx) {}
    bool operator==(const ClassReference& other) const {
      return dex_profile_index == other.dex_profile_index && type_index == other.type_index;
    }
    bool operator<(const ClassReference& other) const {
      return dex_profile_index == other.dex_profile_index
          ? type_index < other.type_index
          : dex_profile_index < other.dex_profile_index;
    }
    uint8_t dex_profile_index;
    dex::TypeIndex type_index;
  };
  using ClassSet = ArenaSet<ClassReference>;
  struct DexPcData : public ArenaObject<kArenaAllocProfile> {
    explicit DexPcData(ArenaAllocator* arena)
        : is_missing_types(false),
          is_megamorphic(false),
          classes(std::less<ClassReference>(), arena->Adapter(kArenaAllocProfile)) {}
    void AddClass(uint16_t dex_profile_idx, const dex::TypeIndex& type_idx);
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
    ClassSet classes;
  };
  using InlineCacheMap = ArenaSafeMap<uint16_t, DexPcData>;
  using MethodMap = ArenaSafeMap<uint16_t, InlineCacheMap>;
  struct OfflineProfileMethodInfo {
    explicit OfflineProfileMethodInfo(const InlineCacheMap* inline_cache_map)
        : inline_caches(inline_cache_map) {}
    bool operator==(const OfflineProfileMethodInfo& other) const;
    const InlineCacheMap* const inline_caches;
    std::vector<DexReference> dex_references;
  };
  ProfileCompilationInfo();
  explicit ProfileCompilationInfo(ArenaPool* arena_pool);
  ~ProfileCompilationInfo();
  bool AddMethodsAndClasses(const std::vector<ProfileMethodInfo>& methods,
                            const std::set<DexCacheResolvedClasses>& resolved_classes);
  bool Load(int fd);
  bool Load(const std::string& filename, bool clear_if_invalid);
  bool MergeWith(const ProfileCompilationInfo& info);
  bool Save(int fd);
  bool Save(const std::string& filename, uint64_t* bytes_written);
  uint32_t GetNumberOfMethods() const;
  uint32_t GetNumberOfResolvedClasses() const;
  bool ContainsMethod(const MethodReference& method_ref) const;
  bool ContainsClass(const DexFile& dex_file, dex::TypeIndex type_idx) const;
  std::unique_ptr<OfflineProfileMethodInfo> GetMethod(const std::string& dex_location,
                                                      uint32_t dex_checksum,
                                                      uint16_t dex_method_index) const;
  std::string DumpInfo(const std::vector<std::unique_ptr<const DexFile>>* dex_files,
                       bool print_full_dex_location = true) const;
  std::string DumpInfo(const std::vector<const DexFile*>* dex_files,
                       bool print_full_dex_location = true) const;
  bool GetClassesAndMethods(const DexFile& dex_file,
                                   std::set<dex::TypeIndex>* class_set,
                                   std::set<uint16_t>* method_set) const;
  bool Equals(const ProfileCompilationInfo& other);
  std::set<DexCacheResolvedClasses> GetResolvedClasses(
      const std::unordered_set<std::string>& dex_files_locations) const;
  static std::string GetProfileDexFileKey(const std::string& dex_location);
  static bool GenerateTestProfile(int fd,
                                  uint16_t number_of_dex_files,
                                  uint16_t method_ratio,
                                  uint16_t class_ratio,
                                  uint32_t random_seed);
  static bool GenerateTestProfile(int fd,
                                  std::vector<std::unique_ptr<const DexFile>>& dex_files,
                                  uint32_t random_seed);
  static bool Equals(const ProfileCompilationInfo::OfflineProfileMethodInfo& pmi1,
                     const ProfileCompilationInfo::OfflineProfileMethodInfo& pmi2);
  ArenaAllocator* GetArena() { return &arena_; }
 private:
  enum ProfileLoadSatus {
    kProfileLoadWouldOverwiteData,
    kProfileLoadIOError,
    kProfileLoadVersionMismatch,
    kProfileLoadBadData,
    kProfileLoadSuccess
  };
  const uint32_t kProfileSizeWarningThresholdInBytes = 500000U;
  const uint32_t kProfileSizeErrorThresholdInBytes = 1000000U;
  struct DexFileData : public DeletableArenaObject<kArenaAllocProfile> {
    DexFileData(ArenaAllocator* arena,
                const std::string& key,
                uint32_t location_checksum,
                uint16_t index)
        : arena_(arena),
          profile_key(key),
          profile_index(index),
          checksum(location_checksum),
          method_map(std::less<uint16_t>(), arena->Adapter(kArenaAllocProfile)),
          class_set(std::less<dex::TypeIndex>(), arena->Adapter(kArenaAllocProfile)) {}
    ArenaAllocator* arena_;
    std::string profile_key;
    uint8_t profile_index;
    uint32_t checksum;
    MethodMap method_map;
    ArenaSet<dex::TypeIndex> class_set;
    bool operator==(const DexFileData& other) const {
      return checksum == other.checksum && method_map == other.method_map;
    }
    InlineCacheMap* FindOrAddMethod(uint16_t method_index);
  };
  DexFileData* GetOrAddDexFileData(const std::string& profile_key, uint32_t checksum);
  bool AddMethodIndex(const std::string& dex_location, uint32_t checksum, uint16_t method_idx);
  bool AddMethod(const ProfileMethodInfo& pmi);
  bool AddMethod(const std::string& dex_location,
                 uint32_t dex_checksum,
                 uint16_t method_index,
                 const OfflineProfileMethodInfo& pmi);
  bool AddClassIndex(const std::string& dex_location, uint32_t checksum, dex::TypeIndex type_idx);
  bool AddResolvedClasses(const DexCacheResolvedClasses& classes);
  const InlineCacheMap* FindMethod(const std::string& dex_location,
                                   uint32_t dex_checksum,
                                   uint16_t dex_method_index) const;
  void DexFileToProfileIndex( std::vector<DexReference>* dex_references) const;
  const DexFileData* FindDexData(const std::string& profile_key) const;
  bool IsEmpty() const;
  std::unique_ptr<uint8_t[]> DeflateBuffer(const uint8_t* in_buffer,
                                           uint32_t in_size,
                                                  uint32_t* compressed_data_size);
  int InflateBuffer(const uint8_t* in_buffer,
                    uint32_t in_size,
                    uint32_t out_size,
                           uint8_t* out_buffer);
  struct ProfileLineHeader {
    std::string dex_location;
    uint16_t class_set_size;
    uint32_t method_region_size_bytes;
    uint32_t checksum;
  };
  struct SafeBuffer {
   public:
    explicit SafeBuffer(size_t size) : storage_(new uint8_t[size]) {
      ptr_current_ = storage_.get();
      ptr_end_ = ptr_current_ + size;
    }
    ProfileLoadSatus FillFromFd(int fd,
                                const std::string& source,
                                       std::string* error);
    ProfileLoadSatus FillFromBuffer(uint8_t* buffer_ptr,
                                    const std::string& source,
                                           std::string* error);
    template <typename T> bool ReadUintAndAdvance( T* value);
    bool CompareAndAdvance(const uint8_t* data, size_t data_size);
    void Advance(size_t data_size);
    size_t CountUnreadBytes();
    const uint8_t* GetCurrentPtr();
    uint8_t* Get() { return storage_.get(); }
   private:
    std::unique_ptr<uint8_t[]> storage_;
    uint8_t* ptr_end_;
    uint8_t* ptr_current_;
  };
  ProfileLoadSatus LoadInternal(int fd, std::string* error);
  ProfileLoadSatus ReadProfileHeader(int fd,
                                            uint8_t* number_of_dex_files,
                                            uint32_t* size_uncompressed_data,
                                            uint32_t* size_compressed_data,
                                            std::string* error);
  ProfileLoadSatus ReadProfileLineHeader(SafeBuffer& buffer,
                                                ProfileLineHeader* line_header,
                                                std::string* error);
  bool ReadProfileLineHeaderElements(SafeBuffer& buffer,
                                            uint16_t* dex_location_size,
                                            ProfileLineHeader* line_header,
                                            std::string* error);
  ProfileLoadSatus ReadProfileLine(SafeBuffer& buffer,
                                   uint8_t number_of_dex_files,
                                   const ProfileLineHeader& line_header,
                                          std::string* error);
  bool ReadClasses(SafeBuffer& buffer,
                   const ProfileLineHeader& line_header,
                          std::string* error);
  bool ReadMethods(SafeBuffer& buffer,
                   uint8_t number_of_dex_files,
                   const ProfileLineHeader& line_header,
                          std::string* error);
  bool ReadInlineCache(SafeBuffer& buffer,
                       uint8_t number_of_dex_files,
                              InlineCacheMap* inline_cache,
                              std::string* error);
  void AddInlineCacheToBuffer(std::vector<uint8_t>* buffer,
                              const InlineCacheMap& inline_cache);
  uint32_t GetMethodsRegionSize(const DexFileData& dex_data);
  void GroupClassesByDex(
      const ClassSet& classes,
             SafeMap<uint8_t, std::vector<dex::TypeIndex>>* dex_to_classes_map);
  DexPcData* FindOrAddDexPc(InlineCacheMap* inline_cache, uint32_t dex_pc);
  friend class ProfileCompilationInfoTest;
  friend class CompilerDriverProfileTest;
  friend class ProfileAssistantTest;
  friend class Dex2oatLayoutTest;
  ArenaPool default_arena_pool_;
  ArenaAllocator arena_;
  ArenaVector<DexFileData*> info_;
  ArenaSafeMap<const std::string, uint8_t> profile_key_map_;
};
}
#endif
