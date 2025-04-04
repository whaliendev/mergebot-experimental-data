#ifndef ART_RUNTIME_OAT_FILE_H_
#define ART_RUNTIME_OAT_FILE_H_ 
#include <list>
#include <string>
#include <vector>
#include "base/mutex.h"
#include "base/stringpiece.h"
#include "dex_file.h"
#include "invoke_type.h"
#include "mem_map.h"
#include "mirror/class.h"
#include "oat.h"
#include "os.h"
namespace art {
class BitVector;
class ElfFile;
class MemMap;
class OatMethodOffsets;
class OatHeader;
class OatFile {
 public:
  static OatFile* OpenWithElfFile(ElfFile* elf_file, const std::string& location,
                                  std::string* error_msg);
  static OatFile* Open(const std::string& filename,
                       const std::string& location,
                       byte* requested_base,
                       bool executable,
                       std::string* error_msg);
  static OatFile* OpenWritable(File* file, const std::string& location, std::string* error_msg);
  static OatFile* OpenReadable(File* file, const std::string& location, std::string* error_msg);
  static OatFile* OpenMemory(std::vector<uint8_t>& oat_contents,
                             const std::string& location,
                             std::string* error_msg);
  ~OatFile();
  bool IsExecutable() const {
    return is_executable_;
  }
  ElfFile* GetElfFile() const {
    CHECK_NE(reinterpret_cast<uintptr_t>(elf_file_.get()), reinterpret_cast<uintptr_t>(nullptr))
        << "Cannot get an elf file from " << GetLocation();
    return elf_file_.get();
  }
  const std::string& GetLocation() const {
    return location_;
  }
  const OatHeader& GetOatHeader() const;
  class OatDexFile;
  class OatMethod {
   public:
    void LinkMethod(mirror::ArtMethod* method) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
    uint32_t GetCodeOffset() const {
      return code_offset_;
    }
    uint32_t GetNativeGcMapOffset() const {
      return native_gc_map_offset_;
    }
    const void* GetPortableCode() const {
      if (kUsePortableCompiler) {
        return GetOatPointer<const void*>(code_offset_);
      } else {
        return nullptr;
      }
    }
    const void* GetQuickCode() const {
      if (kUsePortableCompiler) {
        return nullptr;
      } else {
        return GetOatPointer<const void*>(code_offset_);
      }
    }
    uint32_t GetPortableCodeSize() const {
      return 0;
    }
    uint32_t GetQuickCodeSize() const;
    const uint8_t* GetNativeGcMap() const {
      return GetOatPointer<const uint8_t*>(native_gc_map_offset_);
    }
    size_t GetFrameSizeInBytes() const;
    uint32_t GetCoreSpillMask() const;
    uint32_t GetFpSpillMask() const;
    uint32_t GetMappingTableOffset() const;
    uint32_t GetVmapTableOffset() const;
    const uint8_t* GetMappingTable() const;
    const uint8_t* GetVmapTable() const;
    OatMethod(const byte* base, const uint32_t code_offset, const uint32_t gc_map_offset)
      : begin_(base),
        code_offset_(code_offset),
        native_gc_map_offset_(gc_map_offset) {
    }
    ~OatMethod() {}
    static const OatMethod Invalid() {
      return OatMethod(nullptr, -1, -1);
    }
   private:
    template<class T>
    T GetOatPointer(uint32_t offset) const {
      if (offset == 0) {
        return NULL;
      }
      return reinterpret_cast<T>(begin_ + offset);
    }
    const byte* const begin_;
    const uint32_t code_offset_;
    const uint32_t native_gc_map_offset_;
    friend class OatClass;
  };
  class OatClass {
   public:
    mirror::Class::Status GetStatus() const {
      return status_;
    }
    OatClassType GetType() const {
      return type_;
    }
    const OatMethod GetOatMethod(uint32_t method_index) const;
    static OatClass Invalid() {
      return OatClass(nullptr, mirror::Class::kStatusError, kOatClassNoneCompiled, 0, nullptr,
                      nullptr);
    }
   private:
    OatClass(const OatFile* oat_file,
             mirror::Class::Status status,
             OatClassType type,
             uint32_t bitmap_size,
             const uint32_t* bitmap_pointer,
             const OatMethodOffsets* methods_pointer);
    const OatFile* const oat_file_;
    const mirror::Class::Status status_;
    const OatClassType type_;
    const uint32_t* const bitmap_;
    const OatMethodOffsets* const methods_pointer_;
    friend class OatDexFile;
  };
  class OatDexFile {
   public:
    const DexFile* OpenDexFile(std::string* error_msg) const;
    const OatFile* GetOatFile() const {
      return oat_file_;
    }
    size_t FileSize() const;
    const std::string& GetDexFileLocation() const {
      return dex_file_location_;
    }
    const std::string& GetCanonicalDexFileLocation() const {
      return canonical_dex_file_location_;
    }
    uint32_t GetDexFileLocationChecksum() const {
      return dex_file_location_checksum_;
    }
    OatClass GetOatClass(uint16_t class_def_index) const;
    ~OatDexFile();
   private:
    OatDexFile(const OatFile* oat_file,
               const std::string& dex_file_location,
               const std::string& canonical_dex_file_location,
               uint32_t dex_file_checksum,
               const byte* dex_file_pointer,
               const uint32_t* oat_class_offsets_pointer);
    const OatFile* const oat_file_;
    const std::string dex_file_location_;
    const std::string canonical_dex_file_location_;
    const uint32_t dex_file_location_checksum_;
    const byte* const dex_file_pointer_;
    const uint32_t* const oat_class_offsets_pointer_;
    friend class OatFile;
    DISALLOW_COPY_AND_ASSIGN(OatDexFile);
  };
  const OatDexFile* GetOatDexFile(const char* dex_location,
                                  const uint32_t* const dex_location_checksum,
                                  bool exception_if_not_found = true) const
      LOCKS_EXCLUDED(secondary_lookup_lock_);
  const std::vector<const OatDexFile*>& GetOatDexFiles() const {
    return oat_dex_files_storage_;
  }
  size_t Size() const {
    return End() - Begin();
  }
  const byte* Begin() const;
  const byte* End() const;
 private:
  static void CheckLocation(const std::string& location);
  static OatFile* OpenDlopen(const std::string& elf_filename,
                             const std::string& location,
                             byte* requested_base,
                             std::string* error_msg);
  static OatFile* OpenElfFile(File* file,
                              const std::string& location,
                              byte* requested_base,
                              bool writable,
                              bool executable,
                              std::string* error_msg);
  explicit OatFile(const std::string& filename, bool executable);
  bool Dlopen(const std::string& elf_filename, byte* requested_base, std::string* error_msg);
  bool ElfFileOpen(File* file, byte* requested_base, bool writable, bool executable,
                   std::string* error_msg);
  bool Setup(std::string* error_msg);
  const std::string location_;
  const byte* begin_;
  const byte* end_;
  const bool is_executable_;
  std::unique_ptr<MemMap> mem_map_;
  std::unique_ptr<ElfFile> elf_file_;
  void* dlopen_handle_;
  std::vector<const OatDexFile*> oat_dex_files_storage_;
  typedef AllocationTrackingSafeMap<StringPiece, const OatDexFile*, kAllocatorTagOatFile> Table;
  Table oat_dex_files_;
  mutable Mutex secondary_lookup_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  mutable Table secondary_oat_dex_files_ GUARDED_BY(secondary_lookup_lock_);
  mutable std::list<std::string> string_cache_ GUARDED_BY(secondary_lookup_lock_);
  friend class OatClass;
  friend class OatDexFile;
  friend class OatDumper;
  DISALLOW_COPY_AND_ASSIGN(OatFile);
};
}
#endif
