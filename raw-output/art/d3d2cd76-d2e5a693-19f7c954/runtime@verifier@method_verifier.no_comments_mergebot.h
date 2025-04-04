#ifndef ART_RUNTIME_VERIFIER_METHOD_VERIFIER_H_
#define ART_RUNTIME_VERIFIER_METHOD_VERIFIER_H_ 
#include <memory>
#include <set>
#include <vector>
#include "base/casts.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "class_reference.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "handle.h"
#include "instruction_flags.h"
#include "method_reference.h"
#include "reg_type.h"
#include "reg_type_cache.h"
#include "register_line.h"
#include "safe_map.h"
namespace art {
struct ReferenceMap2Visitor;
template<class T> class Handle;
namespace verifier {
class MethodVerifier;
class DexPcToReferenceMap;
enum MethodType {
METHOD_UNKNOWN = 0,METHOD_DIRECT,
METHOD_STATIC,
METHOD_VIRTUAL,
METHOD_INTERFACE
};
std::ostream& operator<<(std::ostream& os, const MethodType& rhs);
enum VerifyError {
VERIFY_ERROR_BAD_CLASS_HARD,
VERIFY_ERROR_BAD_CLASS_SOFT,
VERIFY_ERROR_NO_CLASS,
VERIFY_ERROR_NO_FIELD,
VERIFY_ERROR_NO_METHOD,
VERIFY_ERROR_ACCESS_CLASS,
VERIFY_ERROR_ACCESS_FIELD,
VERIFY_ERROR_ACCESS_METHOD,
VERIFY_ERROR_CLASS_CHANGE,
VERIFY_ERROR_INSTANTIATION,
};
std::ostream& operator<<(std::ostream& os, const VerifyError& rhs);
enum VerifyErrorRefType {
VERIFY_ERROR_REF_CLASS = 0,VERIFY_ERROR_REF_FIELD = 1,VERIFY_ERROR_REF_METHOD = 2,};
const int kVerifyErrorRefTypeShift = 6;
enum RegisterTrackingMode {
kTrackRegsBranches,kTrackCompilerInterestPoints,kTrackRegsAll,};
class PcToRegisterLineTable {
public:
  PcToRegisterLineTable() : size_(0) {}
  ~PcToRegisterLineTable();
  void Init(RegisterTrackingMode mode, InstructionFlags* flags, uint32_t insns_size,
            uint16_t registers_size, MethodVerifier* verifier);
  RegisterLine* GetLine(size_t idx) {
    DCHECK_LT(idx, size_);
    return register_lines_[idx];
  }
private:
  std::unique_ptr<RegisterLine*[]> register_lines_;
  size_t size_;
};
class MethodVerifier {
public:
  enum FailureKind {
    kNoFailure,
    kSoftFailure,
    kHardFailure,
  };
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static MethodVerifier* VerifyMethodAndDump(Thread* self, std::ostream& os, uint32_t method_idx,
                                             const DexFile* dex_file,
                                             Handle<mirror::DexCache> dex_cache,
                                             Handle<mirror::ClassLoader> class_loader,
                                             const DexFile::ClassDef* class_def,
                                             const DexFile::CodeItem* code_item,
                                             Handle<mirror::ArtMethod> method,
                                             uint32_t method_access_flags)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  uint8_t EncodePcToReferenceMapData() const;
  uint32_t DexFileVersion() const {
    return dex_file_->GetVersion();
  }
  RegTypeCache* GetRegTypeCache() {
    return &reg_types_;
  }
  std::ostream& Fail(VerifyError error);
  std::ostream& LogVerifyInfo();
  std::ostream& DumpFailures(std::ostream& os);
                              SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                              SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void Init() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void Shutdown();
  bool CanLoadClasses() const {
    return can_load_classes_;
  }
MethodVerifier(Thread* self, const DexFile* dex_file, Handle<mirror::DexCache> dex_cache,
                 Handle<mirror::ClassLoader> class_loader, const DexFile::ClassDef* class_def,
                 const DexFile::CodeItem* code_item, uint32_t method_idx,
                 Handle<mirror::ArtMethod> method,
                 uint32_t access_flags, bool can_load_classes, bool allow_soft_failures,
                 bool need_precise_constants) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
                                              : MethodVerifier(self, dex_file, dex_cache, class_loader, class_def, code_item, method_idx,
                                              method, access_flags, can_load_classes, allow_soft_failures,
                                              need_precise_constants, false) {}
  ~MethodVerifier();
  bool Verify() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  std::vector<int32_t> DescribeVRegs(uint32_t dex_pc);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const DexFile::CodeItem* CodeItem() const;
  RegisterLine* GetRegLine(uint32_t dex_pc);
  const InstructionFlags& GetInstructionFlags(size_t index) const;
                                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  MethodReference GetMethodReference() const;
  uint32_t GetAccessFlags() const;
  bool HasCheckCasts() const;
  bool HasVirtualOrInterfaceInvokes() const;
  bool HasFailures() const;
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
private:
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PrependToLastFailMessage(std::string);
  void AppendToLastFailMessage(std::string);
  static FailureKind VerifyMethod(Thread* self, uint32_t method_idx, const DexFile* dex_file,
                                  Handle<mirror::DexCache> dex_cache,
                                  Handle<mirror::ClassLoader> class_loader,
                                  const DexFile::ClassDef* class_def_idx,
                                  const DexFile::CodeItem* code_item,
                                  Handle<mirror::ArtMethod> method, uint32_t method_access_flags,
                                  bool allow_soft_failures, bool need_precise_constants)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                          SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                          SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool ComputeWidthsAndCountOps();
                            SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                            SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool VerifyInstructions();
  bool VerifyInstruction(const Instruction* inst, uint32_t code_offset);
  bool CheckRegisterIndex(uint32_t idx);
  bool CheckWideRegisterIndex(uint32_t idx);
  bool CheckFieldIndex(uint32_t idx);
  bool CheckMethodIndex(uint32_t idx);
  bool CheckNewInstance(uint32_t idx);
  bool CheckStringIndex(uint32_t idx);
  bool CheckTypeIndex(uint32_t idx);
  bool CheckNewArray(uint32_t idx);
  bool CheckArrayData(uint32_t cur_offset);
  bool CheckBranchTarget(uint32_t cur_offset);
  bool CheckSwitchTargets(uint32_t cur_offset);
  bool CheckVarArgRegs(uint32_t vA, uint32_t arg[]);
  bool CheckVarArgRangeRegs(uint32_t vA, uint32_t vC);
  bool GetBranchOffset(uint32_t cur_offset, int32_t* pOffset, bool* pConditional,
                       bool* selfOkay);
                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                               SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                               SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                              SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                              SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                                  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CheckNotMoveException(const uint16_t* insns, int insn_idx);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsConstructor() const {
    return (method_access_flags_ & kAccConstructor) != 0;
  }
  bool IsStatic() const {
    return (method_access_flags_ & kAccStatic) != 0;
  }
                                       SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                       SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
                                     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  InstructionFlags* CurrentInsnFlags();
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  Thread* const self_;
  RegTypeCache reg_types_;
  PcToRegisterLineTable reg_table_;
  std::unique_ptr<RegisterLine> work_line_;
  uint32_t work_insn_idx_;
  std::unique_ptr<RegisterLine> saved_line_;
const uint32_t dex_method_idx_;
Handle<mirror::ArtMethod> mirror_method_ GUARDED_BY(Locks::mutator_lock_);
const uint32_t method_access_flags_;
const RegType* return_type_;
const DexFile* const dex_file_;
                                                 GUARDED_BY(Locks::mutator_lock_);
Handle<mirror::ClassLoader> class_loader_ GUARDED_BY(Locks::mutator_lock_);
const DexFile::ClassDef* const class_def_;
const DexFile::CodeItem* const code_item_;
const RegType* declaring_class_;
  std::unique_ptr<InstructionFlags[]> insn_flags_;
  uint32_t interesting_dex_pc_;
  std::vector<uint32_t>* monitor_enter_dex_pcs_;
  std::vector<VerifyError> failures_;
  std::vector<std::ostringstream*> failure_messages_;
  bool have_pending_hard_failure_;
  bool have_pending_runtime_throw_failure_;
  std::ostringstream info_messages_;
  size_t new_instance_count_;
  size_t monitor_enter_count_;
  const bool can_load_classes_;
  const bool allow_soft_failures_;
  const bool need_precise_constants_;
  bool has_check_casts_;
  bool has_virtual_or_interface_invokes_;
  const bool verify_to_dump_;
};
std::ostream& operator<<(std::ostream& os, const MethodVerifier::FailureKind& rhs);
}
}
#endif
