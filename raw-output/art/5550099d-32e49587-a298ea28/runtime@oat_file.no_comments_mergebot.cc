#include "oat_file.h"
#include <dlfcn.h>
#include <sstream>
#include <string.h>
#include <unistd.h>
#include "base/bit_vector.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "oat.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "runtime.h"
#include "utils.h"
#include "vmap_table.h"
namespace art {
void OatFile::CheckLocation(const std::string& location) {
  CHECK(!location.empty());
}
OatFile* OatFile::OpenWithElfFile(ElfFile* elf_file,
                                  const std::string& location,
                                  std::string* error_msg) {
  std::unique_ptr<OatFile> oat_file(new OatFile(location, false));
  oat_file->elf_file_.reset(elf_file);
  Elf32_Shdr* hdr = elf_file->FindSectionByName(".rodata");
  oat_file->begin_ = elf_file->Begin() + hdr->sh_offset;
  oat_file->end_ = elf_file->Begin() + hdr->sh_size + hdr->sh_offset;
  return oat_file->Setup(error_msg) ? oat_file.release() : nullptr;
}
OatFile* OatFile::OpenMemory(std::vector<uint8_t>& oat_contents,
                             const std::string& location,
                             std::string* error_msg) {
  CHECK(!oat_contents.empty()) << location;
  CheckLocation(location);
  std::unique_ptr<OatFile> oat_file(new OatFile(location, false));
  oat_file->begin_ = &oat_contents[0];
  oat_file->end_ = &oat_contents[oat_contents.size()];
  return oat_file->Setup(error_msg) ? oat_file.release() : nullptr;
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
OatFile::OatClass::OatClass(const OatFile* oat_file,
                            mirror::Class::Status status,
                            OatClassType type,
                            uint32_t bitmap_size,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file), status_(status), type_(type),
      bitmap_(bitmap_pointer), methods_pointer_(methods_pointer) {
    switch (type_) {
      case kOatClassAllCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassSomeCompiled: {
        CHECK_NE(0U, bitmap_size);
        CHECK(bitmap_pointer != nullptr);
        CHECK(methods_pointer != nullptr);
        break;
      }
      case kOatClassNoneCompiled: {
        CHECK_EQ(0U, bitmap_size);
        CHECK(bitmap_pointer == nullptr);
        CHECK(methods_pointer_ == nullptr);
        break;
      }
      case kOatClassMax: {
        LOG(FATAL) << "Invalid OatClassType " << type_;
        break;
      }
    }
}
void OatFile::OatMethod::LinkMethod(mirror::ArtMethod* method) const {
  CHECK(method != NULL);
  method->SetEntryPointFromPortableCompiledCode(GetPortableCode());
  method->SetEntryPointFromQuickCompiledCode(GetQuickCode());
  method->SetNativeGcMap(GetNativeGcMap());
}
}
