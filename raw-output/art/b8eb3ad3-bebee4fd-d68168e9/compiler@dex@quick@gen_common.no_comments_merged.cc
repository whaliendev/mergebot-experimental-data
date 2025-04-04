#include "dex/compiler_ir.h"
#include "dex/compiler_internals.h"
#include "dex/quick/arm/arm_lir.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "verifier/method_verifier.h"
#include <functional>
namespace art {
typedef mirror::ObjectArray<mirror::Object> ObjArray;
typedef mirror::ObjectArray<mirror::Class> ClassArray;
void Mir2Lir::GenBarrier() {
  LIR* barrier = NewLIR0(kPseudoBarrier);
  DCHECK(!barrier->flags.use_def_invalid);
  barrier->u.m.def_mask = &kEncodeAll;
}
void Mir2Lir::GenDivZeroException() {
  LIR* branch = OpUnconditionalBranch(nullptr);
  AddDivZeroCheckSlowPath(branch);
}
void Mir2Lir::GenDivZeroCheck(ConditionCode c_code) {
  LIR* branch = OpCondBranch(c_code, nullptr);
  AddDivZeroCheckSlowPath(branch);
}
void Mir2Lir::GenDivZeroCheck(RegStorage reg) {
  LIR* branch = OpCmpImmBranch(kCondEq, reg, 0, nullptr);
  AddDivZeroCheckSlowPath(branch);
}
void Mir2Lir::AddDivZeroCheckSlowPath(LIR* branch) {
  class DivZeroCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    DivZeroCheckSlowPath(Mir2Lir* m2l, LIR* branch)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch) {
    }
    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      if (m2l_->cu_->target64) {
        m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(8, pThrowDivZero), true);
      } else {
        m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(4, pThrowDivZero), true);
      }
    }
  };
  AddSlowPath(new (arena_) DivZeroCheckSlowPath(this, branch));
}
void Mir2Lir::GenArrayBoundsCheck(RegStorage index, RegStorage length) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch, RegStorage index, RegStorage length)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch),
          index_(index), length_(length) {
    }
    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      if (m2l_->cu_->target64) {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(8, pThrowArrayBounds),
                                      index_, length_, true);
      } else {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pThrowArrayBounds),
                                      index_, length_, true);
      }
    }
   private:
    const RegStorage index_;
    const RegStorage length_;
  };
  LIR* branch = OpCmpBranch(kCondUge, index, length, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch, index, length));
}
void Mir2Lir::GenArrayBoundsCheck(int index, RegStorage length) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch, int index, RegStorage length)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch),
          index_(index), length_(length) {
    }
    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      RegStorage arg1_32 = m2l_->TargetReg(kArg1, kNotWide);
      RegStorage arg0_32 = m2l_->TargetReg(kArg0, kNotWide);
      m2l_->OpRegCopy(arg1_32, length_);
      m2l_->LoadConstant(arg0_32, index_);
      if (m2l_->cu_->target64) {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(8, pThrowArrayBounds),
                                      arg0_32, arg1_32, true);
      } else {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pThrowArrayBounds),
                                      arg0_32, arg1_32, true);
      }
    }
   private:
    const int32_t index_;
    const RegStorage length_;
  };
  LIR* branch = OpCmpImmBranch(kCondLs, length, index, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch, index, length));
}
LIR* Mir2Lir::GenNullCheck(RegStorage reg) {
  class NullCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    NullCheckSlowPath(Mir2Lir* m2l, LIR* branch)
        : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch) {
    }
    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      if (m2l_->cu_->target64) {
        m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(8, pThrowNullPointer), true);
      } else {
        m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(4, pThrowNullPointer), true);
      }
    }
  };
  LIR* branch = OpCmpImmBranch(kCondEq, reg, 0, nullptr);
  AddSlowPath(new (arena_) NullCheckSlowPath(this, branch));
  return branch;
}
LIR* Mir2Lir::GenNullCheck(RegStorage m_reg, int opt_flags) {
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    return GenExplicitNullCheck(m_reg, opt_flags);
  }
  return nullptr;
}
LIR* Mir2Lir::GenExplicitNullCheck(RegStorage m_reg, int opt_flags) {
  if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
    return NULL;
  }
  return GenNullCheck(m_reg);
}
void Mir2Lir::MarkPossibleNullPointerException(int opt_flags) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    MarkSafepointPC(last_lir_insn_);
  }
}
void Mir2Lir::MarkPossibleNullPointerExceptionAfter(int opt_flags, LIR* after) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    MarkSafepointPCAfter(after);
  }
}
void Mir2Lir::MarkPossibleStackOverflowException() {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks()) {
    MarkSafepointPC(last_lir_insn_);
  }
}
void Mir2Lir::ForceImplicitNullCheck(RegStorage reg, int opt_flags) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    RegStorage tmp = AllocTemp();
    LIR* load = Load32Disp(reg, 0, tmp);
    FreeTemp(tmp);
    MarkSafepointPC(load);
  }
}
void Mir2Lir::GenCompareAndBranch(Instruction::Code opcode, RegLocation rl_src1,
                                  RegLocation rl_src2, LIR* taken,
                                  LIR* fall_through) {
  DCHECK(!rl_src1.fp);
  DCHECK(!rl_src2.fp);
  ConditionCode cond;
  switch (opcode) {
    case Instruction::IF_EQ:
      cond = kCondEq;
      break;
    case Instruction::IF_NE:
      cond = kCondNe;
      break;
    case Instruction::IF_LT:
      cond = kCondLt;
      break;
    case Instruction::IF_GE:
      cond = kCondGe;
      break;
    case Instruction::IF_GT:
      cond = kCondGt;
      break;
    case Instruction::IF_LE:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  if (rl_src1.is_const) {
    RegLocation rl_temp = rl_src1;
    rl_src1 = rl_src2;
    rl_src2 = rl_temp;
    cond = FlipComparisonOrder(cond);
  }
  rl_src1 = LoadValue(rl_src1);
  if (rl_src2.is_const) {
    RegLocation rl_temp = UpdateLoc(rl_src2);
    if ((rl_temp.location == kLocDalvikFrame) &&
        InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src2))) {
      OpCmpImmBranch(cond, rl_src1.reg, mir_graph_->ConstantValue(rl_src2), taken);
      return;
    }
  }
  rl_src2 = LoadValue(rl_src2);
  OpCmpBranch(cond, rl_src1.reg, rl_src2.reg, taken);
}
void Mir2Lir::GenCompareZeroAndBranch(Instruction::Code opcode, RegLocation rl_src, LIR* taken,
                                      LIR* fall_through) {
  ConditionCode cond;
  DCHECK(!rl_src.fp);
  rl_src = LoadValue(rl_src);
  switch (opcode) {
    case Instruction::IF_EQZ:
      cond = kCondEq;
      break;
    case Instruction::IF_NEZ:
      cond = kCondNe;
      break;
    case Instruction::IF_LTZ:
      cond = kCondLt;
      break;
    case Instruction::IF_GEZ:
      cond = kCondGe;
      break;
    case Instruction::IF_GTZ:
      cond = kCondGt;
      break;
    case Instruction::IF_LEZ:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  OpCmpImmBranch(cond, rl_src.reg, 0, taken);
}
void Mir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopy(rl_result.reg, rl_src.reg);
  } else {
    LoadValueDirect(rl_src, rl_result.reg.GetLow());
  }
  OpRegRegImm(kOpAsr, rl_result.reg.GetHigh(), rl_result.reg.GetLow(), 31);
  StoreValueWide(rl_dest, rl_result);
}
void Mir2Lir::GenIntNarrowing(Instruction::Code opcode, RegLocation rl_dest,
                              RegLocation rl_src) {
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpKind op = kOpInvalid;
  switch (opcode) {
    case Instruction::INT_TO_BYTE:
      op = kOp2Byte;
      break;
    case Instruction::INT_TO_SHORT:
       op = kOp2Short;
       break;
    case Instruction::INT_TO_CHAR:
       op = kOp2Char;
       break;
    default:
      LOG(ERROR) << "Bad int conversion type";
  }
  OpRegReg(op, rl_result.reg, rl_src.reg);
  StoreValue(rl_dest, rl_result);
}
template <size_t pointer_size>
static void GenNewArrayImpl(Mir2Lir* mir_to_lir, CompilationUnit* cu,
                            uint32_t type_idx, RegLocation rl_dest,
                            RegLocation rl_src) {
  mir_to_lir->FlushAllRegs();
  ThreadOffset<pointer_size> func_offset(-1);
  const DexFile* dex_file = cu->dex_file;
  CompilerDriver* driver = cu->compiler_driver;
  if (cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx, *dex_file,
                                                      type_idx)) {
    bool is_type_initialized;
    bool use_direct_type_ptr;
    uintptr_t direct_type_ptr;
    bool is_finalizable;
    if (kEmbedClassInCode &&
        driver->CanEmbedTypeInCode(*dex_file, type_idx, &is_type_initialized, &use_direct_type_ptr,
                                   &direct_type_ptr, &is_finalizable)) {
      if (!use_direct_type_ptr) {
        mir_to_lir->LoadClassType(type_idx, kArg0);
        func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocArrayResolved);
        mir_to_lir->CallRuntimeHelperRegMethodRegLocation(func_offset,
                                                          mir_to_lir->TargetReg(kArg0, kNotWide),
                                                          rl_src, true);
      } else {
        func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocArrayResolved);
        mir_to_lir->CallRuntimeHelperImmMethodRegLocation(func_offset, direct_type_ptr, rl_src,
                                                          true);
      }
    } else {
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocArray);
      mir_to_lir->CallRuntimeHelperImmMethodRegLocation(func_offset, type_idx, rl_src, true);
    }
    DCHECK_NE(func_offset.Int32Value(), -1);
  } else {
    func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocArrayWithAccessCheck);
    mir_to_lir->CallRuntimeHelperImmMethodRegLocation(func_offset, type_idx, rl_src, true);
  }
  RegLocation rl_result = mir_to_lir->GetReturn(kRefReg);
  mir_to_lir->StoreValue(rl_dest, rl_result);
}
void Mir2Lir::GenNewArray(uint32_t type_idx, RegLocation rl_dest,
                          RegLocation rl_src) {
  if (cu_->target64) {
    GenNewArrayImpl<8>(this, cu_, type_idx, rl_dest, rl_src);
  } else {
    GenNewArrayImpl<4>(this, cu_, type_idx, rl_dest, rl_src);
  }
}
template <size_t pointer_size>
static void GenFilledNewArrayCall(Mir2Lir* mir_to_lir, CompilationUnit* cu, int elems, int type_idx) {
  ThreadOffset<pointer_size> func_offset(-1);
  if (cu->compiler_driver->CanAccessTypeWithoutChecks(cu->method_idx, *cu->dex_file,
                                                      type_idx)) {
    func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pCheckAndAllocArray);
  } else {
    func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pCheckAndAllocArrayWithAccessCheck);
  }
  mir_to_lir->CallRuntimeHelperImmMethodImm(func_offset, type_idx, elems, true);
}
void Mir2Lir::GenFilledNewArray(CallInfo* info) {
  int elems = info->num_arg_words;
  int type_idx = info->index;
  FlushAllRegs();
  if (cu_->target64) {
    GenFilledNewArrayCall<8>(this, cu_, elems, type_idx);
  } else {
    GenFilledNewArrayCall<4>(this, cu_, elems, type_idx);
  }
  FreeTemp(TargetReg(kArg2, kNotWide));
  FreeTemp(TargetReg(kArg1, kNotWide));
  RegStorage ref_reg = TargetReg(kRet0, kRef);
  LockTemp(ref_reg);
  size_t component_size = sizeof(int32_t);
  if (info->is_range && (elems > 0)) {
    for (int i = 0; i < elems; i++) {
      RegLocation loc = UpdateLoc(info->args[i]);
      if (loc.location == kLocPhysReg) {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        Store32Disp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg);
      }
    }
    RegStorage r_src = AllocTempRef();
    RegStorage r_dst = AllocTempRef();
    RegStorage r_idx = AllocTempRef();
    RegStorage r_val;
    switch (cu_->instruction_set) {
      case kThumb2:
      case kArm64:
        r_val = TargetReg(kLr, kNotWide);
        break;
      case kX86:
      case kX86_64:
        FreeTemp(ref_reg);
        r_val = AllocTemp();
        break;
      case kMips:
        r_val = AllocTemp();
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu_->instruction_set;
    }
    RegLocation rl_first = info->args[0];
    OpRegRegImm(kOpAdd, r_src, TargetPtrReg(kSp), SRegOffset(rl_first.s_reg_low));
    OpRegRegImm(kOpAdd, r_dst, ref_reg,
                mirror::Array::DataOffset(component_size).Int32Value());
    LoadConstant(r_idx, elems - 1);
    LIR* target = NewLIR0(kPseudoTargetLabel);
    {
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      LoadBaseIndexed(r_src, r_idx, r_val, 2, k32);
    }
    StoreBaseIndexed(r_dst, r_idx, r_val, 2, k32);
    FreeTemp(r_val);
    OpDecAndBranch(kCondGe, r_idx, target);
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
      OpRegRegImm(kOpAdd, ref_reg, r_dst,
                  -mirror::Array::DataOffset(component_size).Int32Value());
    }
  } else if (!info->is_range) {
    for (int i = 0; i < elems; i++) {
      RegLocation rl_arg = LoadValue(info->args[i], kCoreReg);
      Store32Disp(ref_reg,
                  mirror::Array::DataOffset(component_size).Int32Value() + i * 4, rl_arg.reg);
      if (IsTemp(rl_arg.reg)) {
        FreeTemp(rl_arg.reg);
      }
    }
  }
  if (info->result.location != kLocInvalid) {
    StoreValue(info->result, GetReturn(kRefReg));
  }
}
class StaticFieldSlowPath : public Mir2Lir::LIRSlowPath {
 public:
  StaticFieldSlowPath(Mir2Lir* m2l, LIR* unresolved, LIR* uninit, LIR* cont, int storage_index,
                      RegStorage r_base) :
    LIRSlowPath(m2l, m2l->GetCurrentDexPc(), unresolved, cont), uninit_(uninit),
               storage_index_(storage_index), r_base_(r_base) {
  }
  void Compile() {
    LIR* unresolved_target = GenerateTargetLabel();
    uninit_->target = unresolved_target;
    if (cu_->target64) {
      m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeStaticStorage),
                                 storage_index_, true);
    } else {
      m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeStaticStorage),
                                 storage_index_, true);
    }
    m2l_->OpRegCopy(r_base_, m2l_->TargetReg(kRet0, kRef));
    m2l_->OpUnconditionalBranch(cont_);
  }
 private:
  LIR* const uninit_;
  const int storage_index_;
  const RegStorage r_base_;
};
template <size_t pointer_size>
static void GenSputCall(Mir2Lir* mir_to_lir, bool is_long_or_double, bool is_object,
                        const MirSFieldLoweringInfo* field_info, RegLocation rl_src) {
  ThreadOffset<pointer_size> setter_offset =
      is_long_or_double ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pSet64Static)
          : (is_object ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pSetObjStatic)
              : QUICK_ENTRYPOINT_OFFSET(pointer_size, pSet32Static));
  mir_to_lir->CallRuntimeHelperImmRegLocation(setter_offset, field_info->FieldIndex(), rl_src,
                                              true);
}
void Mir2Lir::GenSput(MIR* mir, RegLocation rl_src, bool is_long_or_double,
                      bool is_object) {
  const MirSFieldLoweringInfo& field_info = mir_graph_->GetSFieldLoweringInfo(mir);
  cu_->compiler_driver->ProcessedStaticField(field_info.FastPut(), field_info.IsReferrersClass());
  OpSize store_size = LoadStoreOpSize(is_long_or_double, is_object);
  if (!SLOW_FIELD_PATH && field_info.FastPut()) {
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    RegStorage r_base;
    if (field_info.IsReferrersClass()) {
      RegLocation rl_method = LoadCurrMethod();
      r_base = AllocTempRef();
      LoadRefDisp(rl_method.reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(), r_base,
                  kNotVolatile);
      if (IsTemp(rl_method.reg)) {
        FreeTemp(rl_method.reg);
      }
    } else {
      DCHECK_NE(field_info.StorageIndex(), DexFile::kDexNoIndex);
      FlushAllRegs();
      RegStorage r_method = TargetReg(kArg1, kRef);
      LockTemp(r_method);
      LoadCurrMethodDirect(r_method);
      r_base = TargetReg(kArg0, kRef);
      LockTemp(r_base);
      LoadRefDisp(r_method, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(), r_base,
                  kNotVolatile);
      int32_t offset_of_field = ObjArray::OffsetOfElement(field_info.StorageIndex()).Int32Value();
      LoadRefDisp(r_base, offset_of_field, r_base, kNotVolatile);
      if (!field_info.IsInitialized() &&
          (mir->optimization_flags & MIR_IGNORE_CLINIT_CHECK) == 0) {
        LIR* unresolved_branch = OpCmpImmBranch(kCondEq, r_base, 0, NULL);
        RegStorage r_tmp = TargetReg(kArg2, kNotWide);
        LockTemp(r_tmp);
        LIR* uninit_branch = OpCmpMemImmBranch(kCondLt, r_tmp, r_base,
                                          mirror::Class::StatusOffset().Int32Value(),
                                          mirror::Class::kStatusInitialized, nullptr, nullptr);
        LIR* cont = NewLIR0(kPseudoTargetLabel);
        AddSlowPath(new (arena_) StaticFieldSlowPath(this, unresolved_branch, uninit_branch, cont,
                                                     field_info.StorageIndex(), r_base));
        FreeTemp(r_tmp);
        GenMemBarrier(kLoadAny);
      }
      FreeTemp(r_method);
    }
    RegisterClass reg_class = RegClassForFieldLoadStore(store_size, field_info.IsVolatile());
    if (is_long_or_double) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    if (is_object) {
      StoreRefDisp(r_base, field_info.FieldOffset().Int32Value(), rl_src.reg,
                   field_info.IsVolatile() ? kVolatile : kNotVolatile);
    } else {
      StoreBaseDisp(r_base, field_info.FieldOffset().Int32Value(), rl_src.reg, store_size,
                    field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    if (is_object && !mir_graph_->IsConstantNullRef(rl_src)) {
      MarkGCCard(rl_src.reg, r_base);
    }
    FreeTemp(r_base);
  } else {
    FlushAllRegs();
    if (cu_->target64) {
      GenSputCall<8>(this, is_long_or_double, is_object, &field_info, rl_src);
    } else {
      GenSputCall<4>(this, is_long_or_double, is_object, &field_info, rl_src);
    }
  }
}
template <size_t pointer_size>
static void GenSgetCall(Mir2Lir* mir_to_lir, bool is_long_or_double, bool is_object,
                        const MirSFieldLoweringInfo* field_info) {
  ThreadOffset<pointer_size> getter_offset =
      is_long_or_double ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pGet64Static)
          : (is_object ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pGetObjStatic)
              : QUICK_ENTRYPOINT_OFFSET(pointer_size, pGet32Static));
  mir_to_lir->CallRuntimeHelperImm(getter_offset, field_info->FieldIndex(), true);
}
void Mir2Lir::GenSget(MIR* mir, RegLocation rl_dest,
                      bool is_long_or_double, bool is_object) {
  const MirSFieldLoweringInfo& field_info = mir_graph_->GetSFieldLoweringInfo(mir);
  cu_->compiler_driver->ProcessedStaticField(field_info.FastGet(), field_info.IsReferrersClass());
  OpSize load_size = LoadStoreOpSize(is_long_or_double, is_object);
  if (!SLOW_FIELD_PATH && field_info.FastGet()) {
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    RegStorage r_base;
    if (field_info.IsReferrersClass()) {
      RegLocation rl_method = LoadCurrMethod();
      r_base = AllocTempRef();
      LoadRefDisp(rl_method.reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(), r_base,
                  kNotVolatile);
    } else {
      DCHECK_NE(field_info.StorageIndex(), DexFile::kDexNoIndex);
      FlushAllRegs();
      RegStorage r_method = TargetReg(kArg1, kRef);
      LockTemp(r_method);
      LoadCurrMethodDirect(r_method);
      r_base = TargetReg(kArg0, kRef);
      LockTemp(r_base);
      LoadRefDisp(r_method, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(), r_base,
                  kNotVolatile);
      int32_t offset_of_field = ObjArray::OffsetOfElement(field_info.StorageIndex()).Int32Value();
      LoadRefDisp(r_base, offset_of_field, r_base, kNotVolatile);
      if (!field_info.IsInitialized() &&
          (mir->optimization_flags & MIR_IGNORE_CLINIT_CHECK) == 0) {
        LIR* unresolved_branch = OpCmpImmBranch(kCondEq, r_base, 0, NULL);
        RegStorage r_tmp = TargetReg(kArg2, kNotWide);
        LockTemp(r_tmp);
        LIR* uninit_branch = OpCmpMemImmBranch(kCondLt, r_tmp, r_base,
                                          mirror::Class::StatusOffset().Int32Value(),
                                          mirror::Class::kStatusInitialized, nullptr, nullptr);
        LIR* cont = NewLIR0(kPseudoTargetLabel);
        AddSlowPath(new (arena_) StaticFieldSlowPath(this, unresolved_branch, uninit_branch, cont,
                                                     field_info.StorageIndex(), r_base));
        FreeTemp(r_tmp);
        GenMemBarrier(kLoadAny);
      }
      FreeTemp(r_method);
    }
    RegisterClass reg_class = RegClassForFieldLoadStore(load_size, field_info.IsVolatile());
    RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
    int field_offset = field_info.FieldOffset().Int32Value();
    if (is_object) {
      LoadRefDisp(r_base, field_offset, rl_result.reg, field_info.IsVolatile() ? kVolatile :
          kNotVolatile);
    } else {
      LoadBaseDisp(r_base, field_offset, rl_result.reg, load_size, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    }
    FreeTemp(r_base);
    if (is_long_or_double) {
      StoreValueWide(rl_dest, rl_result);
    } else {
      StoreValue(rl_dest, rl_result);
    }
  } else {
    FlushAllRegs();
    if (cu_->target64) {
      GenSgetCall<8>(this, is_long_or_double, is_object, &field_info);
    } else {
      GenSgetCall<4>(this, is_long_or_double, is_object, &field_info);
    }
    if (is_long_or_double) {
      RegLocation rl_result = GetReturnWide(kCoreReg);
      StoreValueWide(rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(rl_dest.ref ? kRefReg : kCoreReg);
      StoreValue(rl_dest, rl_result);
    }
  }
}
void Mir2Lir::HandleSlowPaths() {
  for (size_t i = 0; i < slow_paths_.Size(); ++i) {
    LIRSlowPath* slowpath = slow_paths_.Get(i);
    slowpath->Compile();
  }
  slow_paths_.Reset();
}
template <size_t pointer_size>
static void GenIgetCall(Mir2Lir* mir_to_lir, bool is_long_or_double, bool is_object,
                        const MirIFieldLoweringInfo* field_info, RegLocation rl_obj) {
  ThreadOffset<pointer_size> getter_offset =
      is_long_or_double ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pGet64Instance)
          : (is_object ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pGetObjInstance)
              : QUICK_ENTRYPOINT_OFFSET(pointer_size, pGet32Instance));
  DCHECK_EQ(static_cast<unsigned int>(rl_obj.wide), 0U);
  mir_to_lir->CallRuntimeHelperImmRegLocation(getter_offset, field_info->FieldIndex(), rl_obj,
                                              true);
}
void Mir2Lir::GenIGet(MIR* mir, int opt_flags, OpSize size,
                      RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double,
                      bool is_object) {
  const MirIFieldLoweringInfo& field_info = mir_graph_->GetIFieldLoweringInfo(mir);
  cu_->compiler_driver->ProcessedInstanceField(field_info.FastGet());
  OpSize load_size = LoadStoreOpSize(is_long_or_double, is_object);
  if (!SLOW_FIELD_PATH && field_info.FastGet()) {
    RegisterClass reg_class = RegClassForFieldLoadStore(load_size, field_info.IsVolatile());
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    rl_obj = LoadValue(rl_obj, kRefReg);
    GenNullCheck(rl_obj.reg, opt_flags);
    RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
    int field_offset = field_info.FieldOffset().Int32Value();
    LIR* load_lir;
    if (is_object) {
      load_lir = LoadRefDisp(rl_obj.reg, field_offset, rl_result.reg, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    } else {
      load_lir = LoadBaseDisp(rl_obj.reg, field_offset, rl_result.reg, load_size,
                              field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    MarkPossibleNullPointerExceptionAfter(opt_flags, load_lir);
    if (is_long_or_double) {
      StoreValueWide(rl_dest, rl_result);
    } else {
      StoreValue(rl_dest, rl_result);
    }
  } else {
    if (cu_->target64) {
      GenIgetCall<8>(this, is_long_or_double, is_object, &field_info, rl_obj);
    } else {
      GenIgetCall<4>(this, is_long_or_double, is_object, &field_info, rl_obj);
    }
    if (is_long_or_double) {
      RegLocation rl_result = GetReturnWide(kCoreReg);
      StoreValueWide(rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(rl_dest.ref ? kRefReg : kCoreReg);
      StoreValue(rl_dest, rl_result);
    }
  }
}
template <size_t pointer_size>
static void GenIputCall(Mir2Lir* mir_to_lir, bool is_long_or_double, bool is_object,
                        const MirIFieldLoweringInfo* field_info, RegLocation rl_obj,
                        RegLocation rl_src) {
  ThreadOffset<pointer_size> setter_offset =
      is_long_or_double ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pSet64Instance)
          : (is_object ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pSetObjInstance)
              : QUICK_ENTRYPOINT_OFFSET(pointer_size, pSet32Instance));
  mir_to_lir->CallRuntimeHelperImmRegLocationRegLocation(setter_offset, field_info->FieldIndex(),
                                                         rl_obj, rl_src, true);
}
void Mir2Lir::GenIPut(MIR* mir, int opt_flags, OpSize size,
                      RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double,
                      bool is_object) {
  const MirIFieldLoweringInfo& field_info = mir_graph_->GetIFieldLoweringInfo(mir);
  cu_->compiler_driver->ProcessedInstanceField(field_info.FastPut());
  OpSize store_size = LoadStoreOpSize(is_long_or_double, is_object);
  if (!SLOW_FIELD_PATH && field_info.FastPut()) {
    RegisterClass reg_class = RegClassForFieldLoadStore(store_size, field_info.IsVolatile());
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    rl_obj = LoadValue(rl_obj, kRefReg);
    if (is_long_or_double) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    GenNullCheck(rl_obj.reg, opt_flags);
    int field_offset = field_info.FieldOffset().Int32Value();
    LIR* store;
    if (is_object) {
      store = StoreRefDisp(rl_obj.reg, field_offset, rl_src.reg, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    } else {
      store = StoreBaseDisp(rl_obj.reg, field_offset, rl_src.reg, store_size,
                            field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    MarkPossibleNullPointerExceptionAfter(opt_flags, store);
    if (is_object && !mir_graph_->IsConstantNullRef(rl_src)) {
      MarkGCCard(rl_src.reg, rl_obj.reg);
    }
  } else {
    if (cu_->target64) {
      GenIputCall<8>(this, is_long_or_double, is_object, &field_info, rl_obj, rl_src);
    } else {
      GenIputCall<4>(this, is_long_or_double, is_object, &field_info, rl_obj, rl_src);
    }
  }
}
template <size_t pointer_size>
static void GenArrayObjPutCall(Mir2Lir* mir_to_lir, bool needs_range_check, bool needs_null_check,
                               RegLocation rl_array, RegLocation rl_index, RegLocation rl_src) {
  ThreadOffset<pointer_size> helper = needs_range_check
        ? (needs_null_check ? QUICK_ENTRYPOINT_OFFSET(pointer_size, pAputObjectWithNullAndBoundCheck)
                            : QUICK_ENTRYPOINT_OFFSET(pointer_size, pAputObjectWithBoundCheck))
        : QUICK_ENTRYPOINT_OFFSET(pointer_size, pAputObject);
  mir_to_lir->CallRuntimeHelperRegLocationRegLocationRegLocation(helper, rl_array, rl_index, rl_src,
                                                                 true);
}
void Mir2Lir::GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                             RegLocation rl_src) {
  bool needs_range_check = !(opt_flags & MIR_IGNORE_RANGE_CHECK);
  bool needs_null_check = !((cu_->disable_opt & (1 << kNullCheckElimination)) &&
      (opt_flags & MIR_IGNORE_NULL_CHECK));
  if (cu_->target64) {
    GenArrayObjPutCall<8>(this, needs_range_check, needs_null_check, rl_array, rl_index, rl_src);
  } else {
    GenArrayObjPutCall<4>(this, needs_range_check, needs_null_check, rl_array, rl_index, rl_src);
  }
}
void Mir2Lir::GenConstClass(uint32_t type_idx, RegLocation rl_dest) {
  RegLocation rl_method = LoadCurrMethod();
  CheckRegLocation(rl_method);
  RegStorage res_reg = AllocTempRef();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  if (!cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                        *cu_->dex_file,
                                                        type_idx)) {
    if (cu_->target64) {
      CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(8, pInitializeTypeAndVerifyAccess),
                              type_idx, rl_method.reg, true);
    } else {
      CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(4, pInitializeTypeAndVerifyAccess),
                              type_idx, rl_method.reg, true);
    }
    RegLocation rl_result = GetReturn(kRefReg);
    StoreValue(rl_dest, rl_result);
  } else {
    int32_t dex_cache_offset =
        mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value();
    LoadRefDisp(rl_method.reg, dex_cache_offset, res_reg, kNotVolatile);
    int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
    LoadRefDisp(res_reg, offset_of_type, rl_result.reg, kNotVolatile);
    if (!cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file,
        type_idx) || SLOW_TYPE_PATH) {
      FlushAllRegs();
      LIR* branch = OpCmpImmBranch(kCondEq, rl_result.reg, 0, NULL);
      LIR* cont = NewLIR0(kPseudoTargetLabel);
      class SlowPath : public LIRSlowPath {
       public:
        SlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont, const int type_idx,
                 const RegLocation& rl_method, const RegLocation& rl_result) :
                   LIRSlowPath(m2l, m2l->GetCurrentDexPc(), fromfast, cont), type_idx_(type_idx),
                   rl_method_(rl_method), rl_result_(rl_result) {
        }
        void Compile() {
          GenerateTargetLabel();
          if (cu_->target64) {
            m2l_->CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx_,
                                          rl_method_.reg, true);
          } else {
            m2l_->CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx_,
                                                      rl_method_.reg, true);
          }
          m2l_->OpRegCopy(rl_result_.reg, m2l_->TargetReg(kRet0, kRef));
          m2l_->OpUnconditionalBranch(cont_);
        }
       private:
        const int type_idx_;
        const RegLocation rl_method_;
        const RegLocation rl_result_;
      };
      AddSlowPath(new (arena_) SlowPath(this, branch, cont, type_idx, rl_method, rl_result));
      StoreValue(rl_dest, rl_result);
     } else {
      StoreValue(rl_dest, rl_result);
    }
  }
}
void Mir2Lir::GenConstString(uint32_t string_idx, RegLocation rl_dest) {
  int32_t offset_of_string = mirror::ObjectArray<mirror::String>::OffsetOfElement(string_idx).
                                                                                      Int32Value();
  if (!cu_->compiler_driver->CanAssumeStringIsPresentInDexCache(
      *cu_->dex_file, string_idx) || SLOW_STRING_PATH) {
    FlushAllRegs();
    LockCallTemps();
    RegLocation rl_method = mir_graph_->GetMethodLoc();
    RegStorage r_method;
    if (rl_method.location == kLocPhysReg) {
      DCHECK(!IsTemp(rl_method.reg));
      r_method = rl_method.reg;
    } else {
      r_method = TargetReg(kArg2, kRef);
      LoadCurrMethodDirect(r_method);
    }
    LoadRefDisp(r_method, mirror::ArtMethod::DexCacheStringsOffset().Int32Value(),
                TargetReg(kArg0, kRef), kNotVolatile);
    LoadRefDisp(TargetReg(kArg0, kRef), offset_of_string, TargetReg(kRet0, kRef), kNotVolatile);
    LIR* fromfast = OpCmpImmBranch(kCondEq, TargetReg(kRet0, kRef), 0, NULL);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    {
      class SlowPath : public LIRSlowPath {
       public:
        SlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont, RegStorage r_method, int32_t string_idx) :
          LIRSlowPath(m2l, m2l->GetCurrentDexPc(), fromfast, cont),
          r_method_(r_method), string_idx_(string_idx) {
        }
        void Compile() {
          GenerateTargetLabel();
          if (cu_->target64) {
            m2l_->CallRuntimeHelperRegImm(QUICK_ENTRYPOINT_OFFSET(8, pResolveString),
                                          r_method_, string_idx_, true);
          } else {
            m2l_->CallRuntimeHelperRegImm(QUICK_ENTRYPOINT_OFFSET(4, pResolveString),
                                          r_method_, string_idx_, true);
          }
          m2l_->OpUnconditionalBranch(cont_);
        }
       private:
         const RegStorage r_method_;
         const int32_t string_idx_;
      };
      AddSlowPath(new (arena_) SlowPath(this, fromfast, cont, r_method, string_idx));
    }
    GenBarrier();
    StoreValue(rl_dest, GetReturn(kRefReg));
  } else {
    RegLocation rl_method = LoadCurrMethod();
    RegStorage res_reg = AllocTempRef();
    RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
    LoadRefDisp(rl_method.reg, mirror::ArtMethod::DexCacheStringsOffset().Int32Value(), res_reg,
                kNotVolatile);
    LoadRefDisp(res_reg, offset_of_string, rl_result.reg, kNotVolatile);
    StoreValue(rl_dest, rl_result);
  }
}
template <size_t pointer_size>
static void GenNewInstanceImpl(Mir2Lir* mir_to_lir, CompilationUnit* cu, uint32_t type_idx,
                               RegLocation rl_dest) {
  mir_to_lir->FlushAllRegs();
  ThreadOffset<pointer_size> func_offset(-1);
  const DexFile* dex_file = cu->dex_file;
  CompilerDriver* driver = cu->compiler_driver;
  if (driver->CanAccessInstantiableTypeWithoutChecks(
      cu->method_idx, *dex_file, type_idx)) {
    bool is_type_initialized;
    bool use_direct_type_ptr;
    uintptr_t direct_type_ptr;
    bool is_finalizable;
    if (kEmbedClassInCode &&
        driver->CanEmbedTypeInCode(*dex_file, type_idx, &is_type_initialized, &use_direct_type_ptr,
                                   &direct_type_ptr, &is_finalizable) &&
                                   !is_finalizable) {
      if (!use_direct_type_ptr) {
        mir_to_lir->LoadClassType(type_idx, kArg0);
        if (!is_type_initialized) {
          func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObjectResolved);
          mir_to_lir->CallRuntimeHelperRegMethod(func_offset, mir_to_lir->TargetReg(kArg0, kRef),
                                                 true);
        } else {
          func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObjectInitialized);
          mir_to_lir->CallRuntimeHelperRegMethod(func_offset, mir_to_lir->TargetReg(kArg0, kRef),
                                                 true);
        }
      } else {
        if (!is_type_initialized) {
          func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObjectResolved);
          mir_to_lir->CallRuntimeHelperImmMethod(func_offset, direct_type_ptr, true);
        } else {
          func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObjectInitialized);
          mir_to_lir->CallRuntimeHelperImmMethod(func_offset, direct_type_ptr, true);
        }
      }
    } else {
      DCHECK_EQ(func_offset.Int32Value(), -1);
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObject);
      mir_to_lir->CallRuntimeHelperImmMethod(func_offset, type_idx, true);
    }
    DCHECK_NE(func_offset.Int32Value(), -1);
  } else {
    func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pAllocObjectWithAccessCheck);
    mir_to_lir->CallRuntimeHelperImmMethod(func_offset, type_idx, true);
  }
  RegLocation rl_result = mir_to_lir->GetReturn(kRefReg);
  mir_to_lir->StoreValue(rl_dest, rl_result);
}
void Mir2Lir::GenNewInstance(uint32_t type_idx, RegLocation rl_dest) {
  if (cu_->target64) {
    GenNewInstanceImpl<8>(this, cu_, type_idx, rl_dest);
  } else {
    GenNewInstanceImpl<4>(this, cu_, type_idx, rl_dest);
  }
}
void Mir2Lir::GenThrow(RegLocation rl_src) {
  FlushAllRegs();
  if (cu_->target64) {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(8, pDeliverException), rl_src, true);
  } else {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(4, pDeliverException), rl_src, true);
  }
}
void Mir2Lir::GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx, RegLocation rl_dest,
                                 RegLocation rl_src) {
  DCHECK(cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64);
  RegLocation object = LoadValue(rl_src, kRefReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage result_reg = rl_result.reg;
  if (IsSameReg(result_reg, object.reg)) {
    result_reg = AllocTypedTemp(false, kCoreReg);
    DCHECK(!IsSameReg(result_reg, object.reg));
  }
  LoadConstant(result_reg, 0);
  LIR* null_branchover = OpCmpImmBranch(kCondEq, object.reg, 0, NULL);
  RegStorage check_class = AllocTypedTemp(false, kRefReg);
  RegStorage object_class = AllocTypedTemp(false, kRefReg);
  LoadCurrMethodDirect(check_class);
  if (use_declaring_class) {
    LoadRefDisp(check_class, mirror::ArtMethod::DeclaringClassOffset().Int32Value(), check_class,
                kNotVolatile);
    LoadRefDisp(object.reg, mirror::Object::ClassOffset().Int32Value(), object_class,
                kNotVolatile);
  } else {
    LoadRefDisp(check_class, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                check_class, kNotVolatile);
    LoadRefDisp(object.reg, mirror::Object::ClassOffset().Int32Value(), object_class,
                kNotVolatile);
    int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
    LoadRefDisp(check_class, offset_of_type, check_class, kNotVolatile);
  }
  if (cu_->instruction_set == kThumb2) {
    OpRegReg(kOpCmp, check_class, object_class);
    LIR* it = OpIT(kCondEq, "");
    LoadConstant(result_reg, 1);
    OpEndIT(it);
  } else {
    GenSelectConst32(check_class, object_class, kCondEq, 1, 0, result_reg, kCoreReg);
  }
  LIR* target = NewLIR0(kPseudoTargetLabel);
  null_branchover->target = target;
  FreeTemp(object_class);
  FreeTemp(check_class);
  if (IsTemp(result_reg)) {
    OpRegCopy(rl_result.reg, result_reg);
    FreeTemp(result_reg);
  }
  StoreValue(rl_dest, rl_result);
}
void Mir2Lir::GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                         bool type_known_abstract, bool use_declaring_class,
                                         bool can_assume_type_is_in_dex_cache,
                                         uint32_t type_idx, RegLocation rl_dest,
                                         RegLocation rl_src) {
  FlushAllRegs();
  LockCallTemps();
  RegStorage method_reg = TargetReg(kArg1, kRef);
  LoadCurrMethodDirect(method_reg);
  RegStorage class_reg = TargetReg(kArg2, kRef);
  RegStorage ref_reg = TargetReg(kArg0, kRef);
  RegStorage ret_reg = GetReturn(kRefReg).reg;
  if (needs_access_check) {
    if (cu_->target64) {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    } else {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    }
    OpRegCopy(class_reg, ret_reg);
    LoadValueDirectFixed(rl_src, ref_reg);
  } else if (use_declaring_class) {
    LoadValueDirectFixed(rl_src, ref_reg);
    LoadRefDisp(method_reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                class_reg, kNotVolatile);
  } else {
<<<<<<< HEAD
    if (can_assume_type_is_in_dex_cache) {
      LoadValueDirectFixed(rl_src, ref_reg);
    }
||||||| d68168e9f0
=======
    if (can_assume_type_is_in_dex_cache) {
      LoadValueDirectFixed(rl_src, TargetReg(kArg0, kRef));
    }
>>>>>>> bebee4fd
    LoadRefDisp(method_reg, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                class_reg, kNotVolatile);
    int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
    LoadRefDisp(class_reg, offset_of_type, class_reg, kNotVolatile);
    if (!can_assume_type_is_in_dex_cache) {
<<<<<<< HEAD
      LIR* slow_path_branch = OpCmpImmBranch(kCondEq, class_reg, 0, NULL);
      LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
      LoadValueDirectFixed(rl_src, ref_reg);
      class InitTypeSlowPath : public Mir2Lir::LIRSlowPath {
       public:
        InitTypeSlowPath(Mir2Lir* m2l, LIR* branch, LIR* cont, uint32_t type_idx,
                         RegLocation rl_src)
            : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch, cont), type_idx_(type_idx),
              rl_src_(rl_src) {
        }
        void Compile() OVERRIDE {
          GenerateTargetLabel();
          if (cu_->target64) {
            m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx_,
                                       true);
          } else {
            m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx_,
                                       true);
          }
          m2l_->OpRegCopy(m2l_->TargetReg(kArg2, kRef),
                          m2l_->TargetReg(kRet0, kRef));
          m2l_->OpUnconditionalBranch(cont_);
        }
       private:
        uint32_t type_idx_;
        RegLocation rl_src_;
      };
      AddSlowPath(new (arena_) InitTypeSlowPath(this, slow_path_branch, slow_path_target,
                                                type_idx, rl_src));
||||||| d68168e9f0
      LIR* hop_branch = OpCmpImmBranch(kCondNe, class_reg, 0, NULL);
      if (cu_->target64) {
        CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx, true);
      } else {
        CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx, true);
      }
      OpRegCopy(TargetReg(kArg2, kRef), TargetReg(kRet0, kRef));
      LoadValueDirectFixed(rl_src, TargetReg(kArg0, kRef));
      LIR* hop_target = NewLIR0(kPseudoTargetLabel);
      hop_branch->target = hop_target;
=======
      LIR* slow_path_branch = OpCmpImmBranch(kCondEq, class_reg, 0, NULL);
      LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
      LoadValueDirectFixed(rl_src, TargetReg(kArg0, kRef));
      class InitTypeSlowPath : public Mir2Lir::LIRSlowPath {
       public:
        InitTypeSlowPath(Mir2Lir* m2l, LIR* branch, LIR* cont, uint32_t type_idx,
                         RegLocation rl_src)
            : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch, cont), type_idx_(type_idx),
              rl_src_(rl_src) {
        }
        void Compile() OVERRIDE {
          GenerateTargetLabel();
          if (cu_->target64) {
            m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx_,
                                       true);
          } else {
            m2l_->CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx_,
                                       true);
          }
          m2l_->OpRegCopy(m2l_->TargetReg(kArg2, kRef),
                          m2l_->TargetReg(kRet0, kRef));
          m2l_->OpUnconditionalBranch(cont_);
        }
       private:
        uint32_t type_idx_;
        RegLocation rl_src_;
      };
      AddSlowPath(new (arena_) InitTypeSlowPath(this, slow_path_branch, slow_path_target,
                                                type_idx, rl_src));
>>>>>>> bebee4fd
    }
  }
  RegLocation rl_result = GetReturn(kCoreReg);
  if (!IsSameReg(rl_result.reg, ref_reg)) {
    LoadConstant(rl_result.reg, 0);
  }
  LIR* branch1 = OpCmpImmBranch(kCondEq, ref_reg, 0, NULL);
  RegStorage ref_class_reg = TargetReg(kArg1, kRef);
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadRefDisp(ref_reg, mirror::Object::ClassOffset().Int32Value(),
              ref_class_reg, kNotVolatile);
  LIR* branchover = NULL;
  if (type_known_final) {
<<<<<<< HEAD
    GenSelectConst32(ref_class_reg, class_reg, kCondEq, 1, 0, rl_result.reg,
                     kCoreReg);
||||||| d68168e9f0
    if (cu_->instruction_set == kThumb2) {
      OpRegReg(kOpCmp, TargetReg(kArg1, kRef), TargetReg(kArg2, kRef));
      LIR* it = OpIT(kCondEq, "E");
      LoadConstant(rl_result.reg, 1);
      LoadConstant(rl_result.reg, 0);
      OpEndIT(it);
    } else {
      LoadConstant(rl_result.reg, 0);
      branchover = OpCmpBranch(kCondNe, TargetReg(kArg1, kRef), TargetReg(kArg2, kRef), NULL);
      LoadConstant(rl_result.reg, 1);
    }
=======
    GenSelectConst32(TargetReg(kArg1, kRef), TargetReg(kArg2, kRef), kCondEq, 1, 0, rl_result.reg,
                     kCoreReg);
>>>>>>> bebee4fd
  } else {
    if (cu_->instruction_set == kThumb2) {
      RegStorage r_tgt = cu_->target64 ?
          LoadHelper(QUICK_ENTRYPOINT_OFFSET(8, pInstanceofNonTrivial)) :
          LoadHelper(QUICK_ENTRYPOINT_OFFSET(4, pInstanceofNonTrivial));
      LIR* it = nullptr;
      if (!type_known_abstract) {
        OpRegReg(kOpCmp, ref_class_reg, class_reg);
        it = OpIT(kCondEq, "EE");
        LoadConstant(rl_result.reg, 1);
      }
      OpRegCopy(ref_reg, class_reg);
      OpReg(kOpBlx, r_tgt);
      if (it != nullptr) {
        OpEndIT(it);
      }
      FreeTemp(r_tgt);
    } else {
      if (!type_known_abstract) {
        LoadConstant(rl_result.reg, 1);
        branchover = OpCmpBranch(kCondEq, TargetReg(kArg1, kRef), TargetReg(kArg2, kRef), NULL);
      }
<<<<<<< HEAD
      OpRegCopy(TargetReg(kArg0, kRef), class_reg);
      if (cu_->target64) {
        CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(8, pInstanceofNonTrivial), false);
      } else {
        CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(4, pInstanceofNonTrivial), false);
      }
||||||| d68168e9f0
      RegStorage r_tgt = cu_->target64 ?
          LoadHelper(QUICK_ENTRYPOINT_OFFSET(8, pInstanceofNonTrivial)) :
          LoadHelper(QUICK_ENTRYPOINT_OFFSET(4, pInstanceofNonTrivial));
      OpRegCopy(TargetReg(kArg0, kRef), TargetReg(kArg2, kRef));
      OpReg(kOpBlx, r_tgt);
      FreeTemp(r_tgt);
=======
      OpRegCopy(TargetReg(kArg0, kRef), TargetReg(kArg2, kRef));
      if (cu_->target64) {
        CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(8, pInstanceofNonTrivial), false);
      } else {
        CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(4, pInstanceofNonTrivial), false);
      }
>>>>>>> bebee4fd
    }
  }
  ClobberCallerSave();
  LIR* target = NewLIR0(kPseudoTargetLabel);
  StoreValue(rl_dest, rl_result);
  branch1->target = target;
  if (branchover != NULL) {
    branchover->target = target;
  }
}
void Mir2Lir::GenInstanceof(uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src) {
  bool type_known_final, type_known_abstract, use_declaring_class;
  bool needs_access_check = !cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                                              *cu_->dex_file,
                                                                              type_idx,
                                                                              &type_known_final,
                                                                              &type_known_abstract,
                                                                              &use_declaring_class);
  bool can_assume_type_is_in_dex_cache = !needs_access_check &&
      cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file, type_idx);
  if ((use_declaring_class || can_assume_type_is_in_dex_cache) && type_known_final) {
    GenInstanceofFinal(use_declaring_class, type_idx, rl_dest, rl_src);
  } else {
    GenInstanceofCallingHelper(needs_access_check, type_known_final, type_known_abstract,
                               use_declaring_class, can_assume_type_is_in_dex_cache,
                               type_idx, rl_dest, rl_src);
  }
}
void Mir2Lir::GenCheckCast(uint32_t insn_idx, uint32_t type_idx, RegLocation rl_src) {
  bool type_known_final, type_known_abstract, use_declaring_class;
  bool needs_access_check = !cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                                              *cu_->dex_file,
                                                                              type_idx,
                                                                              &type_known_final,
                                                                              &type_known_abstract,
                                                                              &use_declaring_class);
  DexCompilationUnit* cu = mir_graph_->GetCurrentDexCompilationUnit();
  if (!needs_access_check && cu_->compiler_driver->IsSafeCast(cu, insn_idx)) {
    return;
  }
  FlushAllRegs();
  LockCallTemps();
  RegStorage method_reg = TargetReg(kArg1, kRef);
  LoadCurrMethodDirect(method_reg);
  RegStorage class_reg = TargetReg(kArg2, kRef);
  if (needs_access_check) {
    if (cu_->target64) {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(8, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    } else {
      CallRuntimeHelperImm(QUICK_ENTRYPOINT_OFFSET(4, pInitializeTypeAndVerifyAccess),
                           type_idx, true);
    }
    OpRegCopy(class_reg, TargetReg(kRet0, kRef));
  } else if (use_declaring_class) {
    LoadRefDisp(method_reg, mirror::ArtMethod::DeclaringClassOffset().Int32Value(),
                class_reg, kNotVolatile);
  } else {
    LoadRefDisp(method_reg, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                class_reg, kNotVolatile);
    int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
    LoadRefDisp(class_reg, offset_of_type, class_reg, kNotVolatile);
    if (!cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file, type_idx)) {
      LIR* hop_branch = OpCmpImmBranch(kCondEq, class_reg, 0, NULL);
      LIR* cont = NewLIR0(kPseudoTargetLabel);
      class SlowPath : public LIRSlowPath {
       public:
        SlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont, const int type_idx,
                 const RegStorage class_reg) :
                   LIRSlowPath(m2l, m2l->GetCurrentDexPc(), fromfast, cont), type_idx_(type_idx),
                   class_reg_(class_reg) {
        }
        void Compile() {
          GenerateTargetLabel();
          if (m2l_->cu_->target64) {
            m2l_->CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(8, pInitializeType), type_idx_,
                                          m2l_->TargetReg(kArg1, kRef), true);
          } else {
            m2l_->CallRuntimeHelperImmReg(QUICK_ENTRYPOINT_OFFSET(4, pInitializeType), type_idx_,
                                          m2l_->TargetReg(kArg1, kRef), true);
          }
          m2l_->OpRegCopy(class_reg_, m2l_->TargetReg(kRet0, kRef));
          m2l_->OpUnconditionalBranch(cont_);
        }
       public:
        const int type_idx_;
        const RegStorage class_reg_;
      };
      AddSlowPath(new (arena_) SlowPath(this, hop_branch, cont, type_idx, class_reg));
    }
  }
  LoadValueDirectFixed(rl_src, TargetReg(kArg0, kRef));
  class SlowPath : public LIRSlowPath {
   public:
    SlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont, bool load):
               LIRSlowPath(m2l, m2l->GetCurrentDexPc(), fromfast, cont), load_(load) {
    }
    void Compile() {
      GenerateTargetLabel();
      if (load_) {
        m2l_->LoadRefDisp(m2l_->TargetReg(kArg0, kRef), mirror::Object::ClassOffset().Int32Value(),
                          m2l_->TargetReg(kArg1, kRef), kNotVolatile);
      }
      if (m2l_->cu_->target64) {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(8, pCheckCast),
                                      m2l_->TargetReg(kArg2, kRef), m2l_->TargetReg(kArg1, kRef),
                                      true);
      } else {
        m2l_->CallRuntimeHelperRegReg(QUICK_ENTRYPOINT_OFFSET(4, pCheckCast),
                                      m2l_->TargetReg(kArg2, kRef), m2l_->TargetReg(kArg1, kRef),
                                      true);
      }
      m2l_->OpUnconditionalBranch(cont_);
    }
   private:
    const bool load_;
  };
  if (type_known_abstract) {
    LIR* branch = OpCmpImmBranch(kCondNe, TargetReg(kArg0, kRef), 0, nullptr);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) SlowPath(this, branch, cont, true));
  } else {
    LIR* branch1 = OpCmpImmBranch(kCondEq, TargetReg(kArg0, kRef), 0, nullptr);
    DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
    LoadRefDisp(TargetReg(kArg0, kRef), mirror::Object::ClassOffset().Int32Value(),
                TargetReg(kArg1, kRef), kNotVolatile);
    LIR* branch2 = OpCmpBranch(kCondNe, TargetReg(kArg1, kRef), class_reg, nullptr);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) SlowPath(this, branch2, cont, false));
    branch1->target = cont;
  }
}
void Mir2Lir::GenLong3Addr(OpKind first_op, OpKind second_op, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2) {
  RegLocation rl_result;
  if (cu_->instruction_set == kThumb2) {
    MarkTemp(TargetReg(kLr, kNotWide));
    FreeTemp(TargetReg(kLr, kNotWide));
  }
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if ((rl_result.reg.GetLowReg() == rl_src1.reg.GetHighReg()) || (rl_result.reg.GetLowReg() == rl_src2.reg.GetHighReg())) {
    RegStorage t_reg = AllocTemp();
    OpRegRegReg(first_op, t_reg, rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
    OpRegRegReg(second_op, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
    OpRegCopy(rl_result.reg.GetLow(), t_reg);
    FreeTemp(t_reg);
  } else {
    OpRegRegReg(first_op, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
    OpRegRegReg(second_op, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  }
  FreeRegLocTemps(rl_result, rl_src1);
  FreeRegLocTemps(rl_result, rl_src2);
  StoreValueWide(rl_dest, rl_result);
  if (cu_->instruction_set == kThumb2) {
    Clobber(TargetReg(kLr, kNotWide));
    UnmarkTemp(TargetReg(kLr, kNotWide));
  }
}
template <size_t pointer_size>
static void GenShiftOpLongCall(Mir2Lir* mir_to_lir, Instruction::Code opcode, RegLocation rl_src1,
                               RegLocation rl_shift) {
  ThreadOffset<pointer_size> func_offset(-1);
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pShlLong);
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pShrLong);
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pUshrLong);
      break;
    default:
      LOG(FATAL) << "Unexpected case";
  }
  mir_to_lir->FlushAllRegs();
  mir_to_lir->CallRuntimeHelperRegLocationRegLocation(func_offset, rl_src1, rl_shift, false);
}
void Mir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_shift) {
  if (cu_->target64) {
    GenShiftOpLongCall<8>(this, opcode, rl_src1, rl_shift);
  } else {
    GenShiftOpLongCall<4>(this, opcode, rl_src1, rl_shift);
  }
  RegLocation rl_result = GetReturnWide(kCoreReg);
  StoreValueWide(rl_dest, rl_result);
}
void Mir2Lir::GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2) {
  DCHECK(cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64);
  OpKind op = kOpBkpt;
  bool is_div_rem = false;
  bool check_zero = false;
  bool unary = false;
  RegLocation rl_result;
  bool shift_op = false;
  switch (opcode) {
    case Instruction::NEG_INT:
      op = kOpNeg;
      unary = true;
      break;
    case Instruction::NOT_INT:
      op = kOpMvn;
      unary = true;
      break;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      op = kOpAdd;
      break;
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      op = kOpSub;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      op = kOpMul;
      break;
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      check_zero = true;
      op = kOpDiv;
      is_div_rem = true;
      break;
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      check_zero = true;
      op = kOpRem;
      is_div_rem = true;
      break;
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      op = kOpXor;
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      shift_op = true;
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Invalid word arith op: " << opcode;
  }
  if (!is_div_rem) {
    if (unary) {
      rl_src1 = LoadValue(rl_src1, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      OpRegReg(op, rl_result.reg, rl_src1.reg);
    } else {
      if ((shift_op) && (cu_->instruction_set != kArm64)) {
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        RegStorage t_reg = AllocTemp();
        OpRegRegImm(kOpAnd, t_reg, rl_src2.reg, 31);
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        OpRegRegReg(op, rl_result.reg, rl_src1.reg, t_reg);
        FreeTemp(t_reg);
      } else {
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        OpRegRegReg(op, rl_result.reg, rl_src1.reg, rl_src2.reg);
      }
    }
    StoreValue(rl_dest, rl_result);
  } else {
    bool done = false;
    if (cu_->instruction_set == kMips || cu_->instruction_set == kArm64) {
      rl_src1 = LoadValue(rl_src1, kCoreReg);
      rl_src2 = LoadValue(rl_src2, kCoreReg);
      if (check_zero) {
        GenDivZeroCheck(rl_src2.reg);
      }
      rl_result = GenDivRem(rl_dest, rl_src1.reg, rl_src2.reg, op == kOpDiv);
      done = true;
    } else if (cu_->instruction_set == kThumb2) {
      if (cu_->GetInstructionSetFeatures().HasDivideInstruction()) {
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        if (check_zero) {
          GenDivZeroCheck(rl_src2.reg);
        }
        rl_result = GenDivRem(rl_dest, rl_src1.reg, rl_src2.reg, op == kOpDiv);
        done = true;
      }
    }
    if (!done) {
      FlushAllRegs();
      LoadValueDirectFixed(rl_src2, TargetReg(kArg1, kNotWide));
      RegStorage r_tgt = cu_->target64 ?
          CallHelperSetup(QUICK_ENTRYPOINT_OFFSET(8, pIdivmod)) :
          CallHelperSetup(QUICK_ENTRYPOINT_OFFSET(4, pIdivmod));
      LoadValueDirectFixed(rl_src1, TargetReg(kArg0, kNotWide));
      if (check_zero) {
        GenDivZeroCheck(TargetReg(kArg1, kNotWide));
      }
      if (cu_->target64) {
        CallHelper(r_tgt, QUICK_ENTRYPOINT_OFFSET(8, pIdivmod), false );
      } else {
        CallHelper(r_tgt, QUICK_ENTRYPOINT_OFFSET(4, pIdivmod), false );
      }
      if (op == kOpDiv)
        rl_result = GetReturn(kCoreReg);
      else
        rl_result = GetReturnAlt();
    }
    StoreValue(rl_dest, rl_result);
  }
}
static bool IsPopCountLE2(unsigned int x) {
  x &= x - 1;
  return (x & (x - 1)) == 0;
}
bool Mir2Lir::HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                               RegLocation rl_src, RegLocation rl_dest, int lit) {
  if ((lit < 2) || ((cu_->instruction_set != kThumb2) && !IsPowerOfTwo(lit))) {
    return false;
  }
  if ((cu_->instruction_set == kThumb2) && !IsPowerOfTwo(lit)) {
    return SmallLiteralDivRem(dalvik_opcode, is_div, rl_src, rl_dest, lit);
  }
  int k = LowestSetBit(lit);
  if (k >= 30) {
    return false;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    RegStorage t_reg = AllocTemp();
    if (lit == 2) {
      OpRegRegImm(kOpLsr, t_reg, rl_src.reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg, t_reg, rl_src.reg);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    } else {
      OpRegRegImm(kOpAsr, t_reg, rl_src.reg, 31);
      OpRegRegImm(kOpLsr, t_reg, t_reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg, t_reg, rl_src.reg);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    }
  } else {
    RegStorage t_reg1 = AllocTemp();
    RegStorage t_reg2 = AllocTemp();
    if (lit == 2) {
      OpRegRegImm(kOpLsr, t_reg1, rl_src.reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg2, t_reg1, rl_src.reg);
      OpRegRegImm(kOpAnd, t_reg2, t_reg2, lit -1);
      OpRegRegReg(kOpSub, rl_result.reg, t_reg2, t_reg1);
    } else {
      OpRegRegImm(kOpAsr, t_reg1, rl_src.reg, 31);
      OpRegRegImm(kOpLsr, t_reg1, t_reg1, 32 - k);
      OpRegRegReg(kOpAdd, t_reg2, t_reg1, rl_src.reg);
      OpRegRegImm(kOpAnd, t_reg2, t_reg2, lit - 1);
      OpRegRegReg(kOpSub, rl_result.reg, t_reg2, t_reg1);
    }
  }
  StoreValue(rl_dest, rl_result);
  return true;
}
bool Mir2Lir::HandleEasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  if (lit < 0) {
    return false;
  }
  if (lit == 0) {
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    LoadConstant(rl_result.reg, 0);
    StoreValue(rl_dest, rl_result);
    return true;
  }
  if (lit == 1) {
    rl_src = LoadValue(rl_src, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegCopy(rl_result.reg, rl_src.reg);
    StoreValue(rl_dest, rl_result);
    return true;
  }
  if (cu_->instruction_set == kThumb2) {
    return EasyMultiply(rl_src, rl_dest, lit);
  }
  bool power_of_two = false;
  bool pop_count_le2 = false;
  bool power_of_two_minus_one = false;
  if (IsPowerOfTwo(lit)) {
    power_of_two = true;
  } else if (IsPopCountLE2(lit)) {
    pop_count_le2 = true;
  } else if (IsPowerOfTwo(lit + 1)) {
    power_of_two_minus_one = true;
  } else {
    return false;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (power_of_two) {
    OpRegRegImm(kOpLsl, rl_result.reg, rl_src.reg, LowestSetBit(lit));
  } else if (pop_count_le2) {
    int first_bit = LowestSetBit(lit);
    int second_bit = LowestSetBit(lit ^ (1 << first_bit));
    GenMultiplyByTwoBitMultiplier(rl_src, rl_result, lit, first_bit, second_bit);
  } else {
    DCHECK(power_of_two_minus_one);
    RegStorage t_reg = AllocTemp();
    OpRegRegImm(kOpLsl, t_reg, rl_src.reg, LowestSetBit(lit + 1));
    OpRegRegReg(kOpSub, rl_result.reg, t_reg, rl_src.reg);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}
void Mir2Lir::GenArithOpIntLit(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src,
                               int lit) {
  RegLocation rl_result;
  OpKind op = static_cast<OpKind>(0);
  int shift_op = false;
  bool is_div = false;
  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      rl_src = LoadValue(rl_src, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      if (cu_->instruction_set == kThumb2) {
        OpRegRegImm(kOpRsub, rl_result.reg, rl_src.reg, lit);
      } else {
        OpRegReg(kOpNeg, rl_result.reg, rl_src.reg);
        OpRegImm(kOpAdd, rl_result.reg, lit);
      }
      StoreValue(rl_dest, rl_result);
      return;
    }
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      lit = -lit;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (HandleEasyMultiply(rl_src, rl_dest, lit)) {
        return;
      }
      op = kOpMul;
      break;
    }
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::AND_INT_LIT8:
    case Instruction::AND_INT_LIT16:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::OR_INT_LIT8:
    case Instruction::OR_INT_LIT16:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::XOR_INT_LIT8:
    case Instruction::XOR_INT_LIT16:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_LIT8:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_LIT8:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsr;
      break;
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      if (lit == 0) {
        GenDivZeroException();
        return;
      }
      if ((opcode == Instruction::DIV_INT) ||
          (opcode == Instruction::DIV_INT_2ADDR) ||
          (opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT_LIT16)) {
        is_div = true;
      } else {
        is_div = false;
      }
      if (HandleEasyDivRem(opcode, is_div, rl_src, rl_dest, lit)) {
        return;
      }
      bool done = false;
      if (cu_->instruction_set == kMips || cu_->instruction_set == kArm64) {
        rl_src = LoadValue(rl_src, kCoreReg);
        rl_result = GenDivRemLit(rl_dest, rl_src.reg, lit, is_div);
        done = true;
      } else if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
        rl_result = GenDivRemLit(rl_dest, rl_src, lit, is_div);
        done = true;
      } else if (cu_->instruction_set == kThumb2) {
        if (cu_->GetInstructionSetFeatures().HasDivideInstruction()) {
          rl_src = LoadValue(rl_src, kCoreReg);
          rl_result = GenDivRemLit(rl_dest, rl_src.reg, lit, is_div);
          done = true;
        }
      }
      if (!done) {
        FlushAllRegs();
        LoadValueDirectFixed(rl_src, TargetReg(kArg0, kNotWide));
        Clobber(TargetReg(kArg0, kNotWide));
        if (cu_->target64) {
          CallRuntimeHelperRegImm(QUICK_ENTRYPOINT_OFFSET(8, pIdivmod), TargetReg(kArg0, kNotWide),
                                  lit, false);
        } else {
          CallRuntimeHelperRegImm(QUICK_ENTRYPOINT_OFFSET(4, pIdivmod), TargetReg(kArg0, kNotWide),
                                  lit, false);
        }
        if (is_div)
          rl_result = GetReturn(kCoreReg);
        else
          rl_result = GetReturnAlt();
      }
      StoreValue(rl_dest, rl_result);
      return;
    }
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (shift_op && (lit == 0)) {
    OpRegCopy(rl_result.reg, rl_src.reg);
  } else {
    OpRegRegImm(op, rl_result.reg, rl_src.reg, lit);
  }
  StoreValue(rl_dest, rl_result);
}
template <size_t pointer_size>
static void GenArithOpLongImpl(Mir2Lir* mir_to_lir, CompilationUnit* cu, Instruction::Code opcode,
                               RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) {
  RegLocation rl_result;
  OpKind first_op = kOpBkpt;
  OpKind second_op = kOpBkpt;
  bool call_out = false;
  bool check_zero = false;
  ThreadOffset<pointer_size> func_offset(-1);
  int ret_reg = mir_to_lir->TargetReg(kRet0, kNotWide).GetReg();
  switch (opcode) {
    case Instruction::NOT_LONG:
      if (cu->instruction_set == kArm64 || cu->instruction_set == kX86_64) {
        mir_to_lir->GenNotLong(rl_dest, rl_src2);
        return;
      }
      rl_src2 = mir_to_lir->LoadValueWide(rl_src2, kCoreReg);
      rl_result = mir_to_lir->EvalLoc(rl_dest, kCoreReg, true);
      if (rl_result.reg.GetLowReg() == rl_src2.reg.GetHighReg()) {
        RegStorage t_reg = mir_to_lir->AllocTemp();
        mir_to_lir->OpRegCopy(t_reg, rl_src2.reg.GetHigh());
        mir_to_lir->OpRegReg(kOpMvn, rl_result.reg.GetLow(), rl_src2.reg.GetLow());
        mir_to_lir->OpRegReg(kOpMvn, rl_result.reg.GetHigh(), t_reg);
        mir_to_lir->FreeTemp(t_reg);
      } else {
        mir_to_lir->OpRegReg(kOpMvn, rl_result.reg.GetLow(), rl_src2.reg.GetLow());
        mir_to_lir->OpRegReg(kOpMvn, rl_result.reg.GetHigh(), rl_src2.reg.GetHigh());
      }
      mir_to_lir->StoreValueWide(rl_dest, rl_result);
      return;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        mir_to_lir->GenAddLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpAdd;
      second_op = kOpAdc;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      if (cu->instruction_set != kThumb2) {
        mir_to_lir->GenSubLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpSub;
      second_op = kOpSbc;
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      if (cu->instruction_set != kMips) {
        mir_to_lir->GenMulLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      } else {
        call_out = true;
        ret_reg = mir_to_lir->TargetReg(kRet0, kNotWide).GetReg();
        func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pLmul);
      }
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      if (cu->instruction_set == kArm64 || cu->instruction_set == kX86_64) {
        mir_to_lir->GenDivRemLong(opcode, rl_dest, rl_src1, rl_src2, true);
        return;
      }
      call_out = true;
      check_zero = true;
      ret_reg = mir_to_lir->TargetReg(kRet0, kNotWide).GetReg();
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pLdiv);
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      if (cu->instruction_set == kArm64 || cu->instruction_set == kX86_64) {
        mir_to_lir->GenDivRemLong(opcode, rl_dest, rl_src1, rl_src2, false);
        return;
      }
      call_out = true;
      check_zero = true;
      func_offset = QUICK_ENTRYPOINT_OFFSET(pointer_size, pLmod);
      ret_reg = (cu->instruction_set == kThumb2) ? mir_to_lir->TargetReg(kArg2, kNotWide).GetReg() :
          mir_to_lir->TargetReg(kRet0, kNotWide).GetReg();
      break;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      if (cu->instruction_set == kX86 || cu->instruction_set == kX86_64 ||
          cu->instruction_set == kArm64) {
        return mir_to_lir->GenAndLong(opcode, rl_dest, rl_src1, rl_src2);
      }
      first_op = kOpAnd;
      second_op = kOpAnd;
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      if (cu->instruction_set == kX86 || cu->instruction_set == kX86_64 ||
          cu->instruction_set == kArm64) {
        mir_to_lir->GenOrLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpOr;
      second_op = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      if (cu->instruction_set == kX86 || cu->instruction_set == kX86_64 ||
          cu->instruction_set == kArm64) {
        mir_to_lir->GenXorLong(opcode, rl_dest, rl_src1, rl_src2);
        return;
      }
      first_op = kOpXor;
      second_op = kOpXor;
      break;
    case Instruction::NEG_LONG: {
      mir_to_lir->GenNegLong(rl_dest, rl_src2);
      return;
    }
    default:
      LOG(FATAL) << "Invalid long arith op";
  }
  if (!call_out) {
    mir_to_lir->GenLong3Addr(first_op, second_op, rl_dest, rl_src1, rl_src2);
  } else {
    mir_to_lir->FlushAllRegs();
    if (check_zero) {
      RegStorage r_tmp1 = mir_to_lir->TargetReg(kArg0, kWide);
      RegStorage r_tmp2 = mir_to_lir->TargetReg(kArg2, kWide);
      mir_to_lir->LoadValueDirectWideFixed(rl_src2, r_tmp2);
      RegStorage r_tgt = mir_to_lir->CallHelperSetup(func_offset);
      mir_to_lir->GenDivZeroCheckWide(r_tmp2);
      mir_to_lir->LoadValueDirectWideFixed(rl_src1, r_tmp1);
      mir_to_lir->CallHelper(r_tgt, func_offset, false );
    } else {
      mir_to_lir->CallRuntimeHelperRegLocationRegLocation(func_offset, rl_src1, rl_src2, false);
    }
    if (ret_reg == mir_to_lir->TargetReg(kRet0, kNotWide).GetReg())
      rl_result = mir_to_lir->GetReturnWide(kCoreReg);
    else
      rl_result = mir_to_lir->GetReturnWideAlt();
    mir_to_lir->StoreValueWide(rl_dest, rl_result);
  }
}
void Mir2Lir::GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2) {
  if (cu_->target64) {
    GenArithOpLongImpl<8>(this, cu_, opcode, rl_dest, rl_src1, rl_src2);
  } else {
    GenArithOpLongImpl<4>(this, cu_, opcode, rl_dest, rl_src1, rl_src2);
  }
}
void Mir2Lir::GenConst(RegLocation rl_dest, int value) {
  RegLocation rl_result = EvalLoc(rl_dest, kAnyReg, true);
  LoadConstantNoClobber(rl_result.reg, value);
  StoreValue(rl_dest, rl_result);
  if (value == 0) {
    Workaround7250540(rl_dest, rl_result.reg);
  }
}
template <size_t pointer_size>
void Mir2Lir::GenConversionCall(ThreadOffset<pointer_size> func_offset,
                                RegLocation rl_dest, RegLocation rl_src) {
  DCHECK_EQ(pointer_size, GetInstructionSetPointerSize(cu_->instruction_set));
  FlushAllRegs();
  CallRuntimeHelperRegLocation(func_offset, rl_src, false);
  if (rl_dest.wide) {
    RegLocation rl_result;
    rl_result = GetReturnWide(LocToRegClass(rl_dest));
    StoreValueWide(rl_dest, rl_result);
  } else {
    RegLocation rl_result;
    rl_result = GetReturn(LocToRegClass(rl_dest));
    StoreValue(rl_dest, rl_result);
  }
}
template void Mir2Lir::GenConversionCall(ThreadOffset<4> func_offset,
                                         RegLocation rl_dest, RegLocation rl_src);
template void Mir2Lir::GenConversionCall(ThreadOffset<8> func_offset,
                                         RegLocation rl_dest, RegLocation rl_src);
class SuspendCheckSlowPath : public Mir2Lir::LIRSlowPath {
 public:
  SuspendCheckSlowPath(Mir2Lir* m2l, LIR* branch, LIR* cont)
      : LIRSlowPath(m2l, m2l->GetCurrentDexPc(), branch, cont) {
  }
  void Compile() OVERRIDE {
    m2l_->ResetRegPool();
    m2l_->ResetDefTracking();
    GenerateTargetLabel(kPseudoSuspendTarget);
    if (cu_->target64) {
      m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(8, pTestSuspend), true);
    } else {
      m2l_->CallRuntimeHelper(QUICK_ENTRYPOINT_OFFSET(4, pTestSuspend), true);
    }
    if (cont_ != nullptr) {
      m2l_->OpUnconditionalBranch(cont_);
    }
  }
};
void Mir2Lir::GenSuspendTest(int opt_flags) {
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitSuspendChecks()) {
    if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
      return;
    }
    FlushAllRegs();
    LIR* branch = OpTestSuspend(NULL);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) SuspendCheckSlowPath(this, branch, cont));
  } else {
    if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
      return;
    }
    FlushAllRegs();
    LIR* inst = CheckSuspendUsingLoad();
    MarkSafepointPC(inst);
  }
}
void Mir2Lir::GenSuspendTestAndBranch(int opt_flags, LIR* target) {
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitSuspendChecks()) {
    if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
      OpUnconditionalBranch(target);
      return;
    }
    OpTestSuspend(target);
    FlushAllRegs();
    LIR* branch = OpUnconditionalBranch(nullptr);
    AddSlowPath(new (arena_) SuspendCheckSlowPath(this, branch, target));
  } else {
    if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK)) {
      OpUnconditionalBranch(target);
      return;
    }
    FlushAllRegs();
    LIR* inst = CheckSuspendUsingLoad();
    MarkSafepointPC(inst);
    OpUnconditionalBranch(target);
  }
}
void Mir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  if (cu_->target64) {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(8, pLockObject), rl_src, true);
  } else {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(4, pLockObject), rl_src, true);
  }
}
void Mir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  FlushAllRegs();
  if (cu_->target64) {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(8, pUnlockObject), rl_src, true);
  } else {
    CallRuntimeHelperRegLocation(QUICK_ENTRYPOINT_OFFSET(4, pUnlockObject), rl_src, true);
  }
}
void Mir2Lir::GenConstWide(RegLocation rl_dest, int64_t value) {
  RegLocation rl_result = EvalLoc(rl_dest, kAnyReg, true);
  LoadConstantWide(rl_result.reg, value);
  StoreValueWide(rl_dest, rl_result);
}
}
