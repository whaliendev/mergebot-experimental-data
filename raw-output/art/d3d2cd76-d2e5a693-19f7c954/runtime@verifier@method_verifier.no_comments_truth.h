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
  METHOD_UNKNOWN = 0,
  METHOD_DIRECT,
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
  VERIFY_ERROR_REF_CLASS = 0,
  VERIFY_ERROR_REF_FIELD = 1,
  VERIFY_ERROR_REF_METHOD = 2,
};
const int kVerifyErrorRefTypeShift = 6;
enum RegisterTrackingMode {
  kTrackRegsBranches,
  kTrackCompilerInterestPoints,
  kTrackRegsAll,
};
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
  static FailureKind VerifyClass(Thread* self, mirror::Class* klass, bool allow_soft_failures,
                                 std::string* error)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static FailureKind VerifyClass(Thread* self, const DexFile* dex_file,
                                 Handle<mirror::DexCache> dex_cache,
                                 Handle<mirror::ClassLoader> class_loader,
                                 const DexFile::ClassDef* class_def,
                                 bool allow_soft_failures, std::string* error)
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
  void Dump(std::ostream& os) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void FindLocksAtDexPc(mirror::ArtMethod* m, uint32_t dex_pc,
                               std::vector<uint32_t>* monitor_enter_dex_pcs)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static mirror::ArtField* FindAccessedFieldAtDexPc(mirror::ArtMethod* m, uint32_t dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static mirror::ArtMethod* FindInvokedMethodAtDexPc(mirror::ArtMethod* m, uint32_t dex_pc)
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
  static void VisitStaticRoots(RootCallback* callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VisitRoots(RootCallback* callback, void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const DexFile::CodeItem* CodeItem() const;
  RegisterLine* GetRegLine(uint32_t dex_pc);
  const InstructionFlags& GetInstructionFlags(size_t index) const;
  mirror::ClassLoader* GetClassLoader() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::DexCache* GetDexCache() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  MethodReference GetMethodReference() const;
  uint32_t GetAccessFlags() const;
  bool HasCheckCasts() const;
  bool HasVirtualOrInterfaceInvokes() const;
  bool HasFailures() const;
  const RegType& ResolveCheckedClass(uint32_t class_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
 private:
  MethodVerifier(Thread* self, const DexFile* dex_file, Handle<mirror::DexCache> dex_cache,
                 Handle<mirror::ClassLoader> class_loader, const DexFile::ClassDef* class_def,
                 const DexFile::CodeItem* code_item, uint32_t method_idx,
                 Handle<mirror::ArtMethod> method,
                 uint32_t access_flags, bool can_load_classes, bool allow_soft_failures,
                 bool need_precise_constants, bool verify_to_dump)
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
  void FindLocksAtDexPc() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtField* FindAccessedFieldAtDexPc(uint32_t dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtMethod* FindInvokedMethodAtDexPc(uint32_t dex_pc)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool ComputeWidthsAndCountOps();
  bool ScanTryCatchBlocks() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
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
  bool VerifyCodeFlow() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool SetTypesFromSignature() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CodeFlowVerifyMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CodeFlowVerifyInstruction(uint32_t* start_guess)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyNewArray(const Instruction* inst, bool is_filled, bool is_range)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyPrimitivePut(const RegType& target_type, const RegType& insn_type,
                          const uint32_t vregA) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyAGet(const Instruction* inst, const RegType& insn_type,
                  bool is_primitive) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyAPut(const Instruction* inst, const RegType& insn_type,
                  bool is_primitive) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtField* GetInstanceField(const RegType& obj_type, int field_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtField* GetStaticField(int field_idx) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyISGet(const Instruction* inst, const RegType& insn_type,
                   bool is_primitive, bool is_static)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyISPut(const Instruction* inst, const RegType& insn_type,
                   bool is_primitive, bool is_static)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtField* GetQuickFieldAccess(const Instruction* inst, RegisterLine* reg_line)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyIGetQuick(const Instruction* inst, const RegType& insn_type,
                       bool is_primitive)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyIPutQuick(const Instruction* inst, const RegType& insn_type,
                       bool is_primitive)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& ResolveClassAndCheckAccess(uint32_t class_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& GetCaughtExceptionType()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtMethod* ResolveMethodAndCheckAccess(uint32_t method_idx, MethodType method_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtMethod* VerifyInvocationArgs(const Instruction* inst,
                                          MethodType method_type,
                                          bool is_range, bool is_super)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void VerifyInvocationArgsUnresolvedMethod(const Instruction* inst, MethodType method_type,
                                            bool is_range)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  template <class T>
  mirror::ArtMethod* VerifyInvocationArgsFromIterator(T* it, const Instruction* inst,
                                                      MethodType method_type, bool is_range,
                                                      mirror::ArtMethod* res_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtMethod* GetQuickInvokedMethod(const Instruction* inst,
                                           RegisterLine* reg_line,
                                           bool is_range)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  mirror::ArtMethod* VerifyInvokeVirtualQuickArgs(const Instruction* inst, bool is_range)
  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool CheckNotMoveException(const uint16_t* insns, int insn_idx);
  bool UpdateRegisters(uint32_t next_insn, RegisterLine* merge_line, bool update_merge_line)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool IsConstructor() const {
    return (method_access_flags_ & kAccConstructor) != 0;
  }
  bool IsStatic() const {
    return (method_access_flags_ & kAccStatic) != 0;
  }
  const RegType& GetMethodReturnType() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const RegType& GetDeclaringClass() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  InstructionFlags* CurrentInsnFlags();
  const RegType& DetermineCat1Constant(int32_t value, bool precise)
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
  Handle<mirror::DexCache> dex_cache_ GUARDED_BY(Locks::mutator_lock_);
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
