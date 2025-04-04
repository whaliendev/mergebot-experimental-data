#ifndef ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_
#define ART_COMPILER_DEX_QUICK_X86_CODEGEN_X86_H_ 
#include "dex/compiler_internals.h"
#include "x86_lir.h"
#include <map>
namespace art {
class X86Mir2Lir : public Mir2Lir {
protected:
  class InToRegStorageMapper {
   public:
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref) = 0;
    virtual ~InToRegStorageMapper() {}
  };
  class InToRegStorageX86_64Mapper : public InToRegStorageMapper {
   public:
    explicit InToRegStorageX86_64Mapper(Mir2Lir* ml) : ml_(ml), cur_core_reg_(0), cur_fp_reg_(0) {}
    virtual ~InToRegStorageX86_64Mapper() {}
    virtual RegStorage GetNextReg(bool is_double_or_float, bool is_wide, bool is_ref);
   protected:
    Mir2Lir* ml_;
   private:
    int cur_core_reg_;
    int cur_fp_reg_;
  };
  class InToRegStorageMapping {
   public:
    InToRegStorageMapping() : max_mapped_in_(0), is_there_stack_mapped_(false),
    initialized_(false) {}
    void Initialize(RegLocation* arg_locs, int count, InToRegStorageMapper* mapper);
    int GetMaxMappedIn() { return max_mapped_in_; }
    bool IsThereStackMapped() { return is_there_stack_mapped_; }
    RegStorage Get(int in_position);
    bool IsInitialized() { return initialized_; }
   private:
    std::map<int, RegStorage> mapping_;
    int max_mapped_in_;
    bool is_there_stack_mapped_;
    bool initialized_;
  };
public:
  X86Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);
  bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div, RegLocation rl_src,
                          RegLocation rl_dest, int lit);
  bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) OVERRIDE;
  LIR* CheckSuspendUsingLoad() OVERRIDE;
  RegStorage LoadHelper(ThreadOffset<4> offset) OVERRIDE;
  RegStorage LoadHelper(ThreadOffset<8> offset) OVERRIDE;
  LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                    OpSize size, VolatileKind is_volatile) OVERRIDE;
  LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest, int scale,
                       OpSize size) OVERRIDE;
  LIR* LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                           RegStorage r_dest, OpSize size) OVERRIDE;
  LIR* LoadConstantNoClobber(RegStorage r_dest, int value);
  LIR* LoadConstantWide(RegStorage r_dest, int64_t value);
  LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                     OpSize size, VolatileKind is_volatile) OVERRIDE;
  LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src, int scale,
                        OpSize size) OVERRIDE;
  LIR* StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale, int displacement,
                            RegStorage r_src, OpSize size) OVERRIDE;
  void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg);
  void GenImplicitNullCheck(RegStorage reg, int opt_flags);
  RegStorage TargetReg(SpecialTargetRegister reg) OVERRIDE;
  RegStorage TargetReg32(SpecialTargetRegister reg);
  RegStorage TargetReg(SpecialTargetRegister symbolic_reg, WideKind wide_kind) OVERRIDE {
    if (wide_kind == kWide) {
      if (cu_->target64) {
        return As64BitReg(TargetReg32(symbolic_reg));
      } else {
        DCHECK((kArg0 <= symbolic_reg && symbolic_reg < kArg3) ||
               (kFArg0 <= symbolic_reg && symbolic_reg < kFArg3) ||
               (kRet0 == symbolic_reg));
        return RegStorage::MakeRegPair(TargetReg32(symbolic_reg),
                                 TargetReg32(static_cast<SpecialTargetRegister>(symbolic_reg + 1)));
      }
    } else if (wide_kind == kRef && cu_->target64) {
      return As64BitReg(TargetReg32(symbolic_reg));
    } else {
      return TargetReg32(symbolic_reg);
    }
  }
  RegStorage TargetPtrReg(SpecialTargetRegister symbolic_reg) OVERRIDE {
    return TargetReg(symbolic_reg, cu_->target64 ? kWide : kNotWide);
  }
  RegStorage GetArgMappingToPhysicalReg(int arg_num);
  RegStorage GetCoreArgMappingToPhysicalReg(int core_arg_num);
  RegLocation GetReturnAlt();
  RegLocation GetReturnWideAlt();
  RegLocation LocCReturn();
  RegLocation LocCReturnRef();
  RegLocation LocCReturnDouble();
  RegLocation LocCReturnFloat();
  RegLocation LocCReturnWide();
  ResourceMask GetRegMaskCommon(const RegStorage& reg) const OVERRIDE;
  void AdjustSpillMask();
  void ClobberCallerSave();
  void FreeCallTemps();
  void LockCallTemps();
  void CompilerInitializeRegAlloc();
  int VectorRegisterSize();
  int NumReservableVectorRegisters(bool fp_used);
  void AssembleLIR();
  int AssignInsnOffsets();
  void AssignOffsets();
  AssemblerStatus AssembleInstructions(CodeOffset start_addr);
  void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) OVERRIDE;
  void SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                ResourceMask* use_mask, ResourceMask* def_mask) OVERRIDE;
  const char* GetTargetInstFmt(int opcode);
  const char* GetTargetInstName(int opcode);
  std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr);
  ResourceMask GetPCUseDefEncoding() const OVERRIDE;
  uint64_t GetTargetInstFlags(int opcode);
  size_t GetInsnSize(LIR* lir) OVERRIDE;
  bool IsUnconditionalBranch(LIR* lir);
  RegisterClass RegClassForFieldLoadStore(OpSize size, bool is_volatile) OVERRIDE;
  void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                         RegLocation rl_src2);
  void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array, RegLocation rl_index,
                   RegLocation rl_dest, int scale);
  void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                   RegLocation rl_index, RegLocation rl_src, int scale, bool card_mark);
  void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src1, RegLocation rl_shift);
  void GenMulLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenAddLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenAndLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenArithOpDouble(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                        RegLocation rl_src2);
  void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                       RegLocation rl_src2);
  void GenRemFP(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2, bool is_double);
  void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                RegLocation rl_src2);
  void GenConversion(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src);
  bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object);
  bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long);
  bool GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double);
  bool GenInlinedSqrt(CallInfo* info);
  bool GenInlinedAbsFloat(CallInfo* info) OVERRIDE;
  bool GenInlinedAbsDouble(CallInfo* info) OVERRIDE;
  bool GenInlinedPeek(CallInfo* info, OpSize size);
  bool GenInlinedPoke(CallInfo* info, OpSize size);
  void GenNotLong(RegLocation rl_dest, RegLocation rl_src);
  void GenNegLong(RegLocation rl_dest, RegLocation rl_src);
  void GenOrLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                 RegLocation rl_src2);
  void GenSubLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenXorLong(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src1,
                  RegLocation rl_src2);
  void GenDivRemLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                     RegLocation rl_src2, bool is_div);
  RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi, bool is_div);
  RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit, bool is_div);
  void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2);
  void GenDivZeroCheckWide(RegStorage reg);
  void GenArrayBoundsCheck(RegStorage index, RegStorage array_base, int32_t len_offset);
  void GenArrayBoundsCheck(int32_t index, RegStorage array_base, int32_t len_offset);
  void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method);
  void GenExitSequence();
  void GenSpecialExitSequence();
  void GenFillArrayData(DexOffset table_offset, RegLocation rl_src);
  void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double);
  void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir);
  void GenSelect(BasicBlock* bb, MIR* mir);
  void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                        int32_t true_val, int32_t false_val, RegStorage rs_dest,
                        int dest_reg_class) OVERRIDE;
  void GenSelectConst01(RegStorage left_op, RegStorage right_op, ConditionCode code, bool true_val,
                        RegStorage rs_dest);
  bool GenMemBarrier(MemBarrierKind barrier_kind);
  void GenMoveException(RegLocation rl_dest);
  void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                     int first_bit, int second_bit);
  void GenNegDouble(RegLocation rl_dest, RegLocation rl_src);
  void GenNegFloat(RegLocation rl_dest, RegLocation rl_src);
  void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
  void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
  void GenIntToLong(RegLocation rl_dest, RegLocation rl_src);
  bool GenLongImm(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);
  bool GenLongLongImm(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                      Instruction::Code op);
  virtual void GenLongArith(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                            Instruction::Code op, bool is_commutative);
  void GenLongArith(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);
  virtual void GenLongRegOrMemOp(RegLocation rl_dest, RegLocation rl_src, Instruction::Code op);
  void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx, RegLocation rl_dest,
                          RegLocation rl_src);
  void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_shift);
  LIR* OpUnconditionalBranch(LIR* target);
  LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target);
  LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value, LIR* target);
  LIR* OpCondBranch(ConditionCode cc, LIR* target);
  LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target);
  LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src);
  LIR* OpIT(ConditionCode cond, const char* guide);
  void OpEndIT(LIR* it);
  LIR* OpMem(OpKind op, RegStorage r_base, int disp);
  LIR* OpPcRelLoad(RegStorage reg, LIR* target);
  LIR* OpReg(OpKind op, RegStorage r_dest_src);
  void OpRegCopy(RegStorage r_dest, RegStorage r_src);
  LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src);
  LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value);
  LIR* OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset);
  LIR* OpRegMem(OpKind op, RegStorage r_dest, RegLocation value);
  LIR* OpMemReg(OpKind op, RegLocation rl_dest, int value);
  LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2);
  LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type);
  LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type);
  LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src);
  LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value);
  LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2);
  LIR* OpTestSuspend(LIR* target);
  LIR* OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) OVERRIDE;
  LIR* OpThreadMem(OpKind op, ThreadOffset<8> thread_offset) OVERRIDE;
  LIR* OpVldm(RegStorage r_base, int count);
  LIR* OpVstm(RegStorage r_base, int count);
  void OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale, int offset);
  void OpRegCopyWide(RegStorage dest, RegStorage src);
  void OpTlsCmp(ThreadOffset<4> offset, int val) OVERRIDE;
  void OpTlsCmp(ThreadOffset<8> offset, int val) OVERRIDE;
  void OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<4> thread_offset);
  void OpRegThreadMem(OpKind op, RegStorage r_dest, ThreadOffset<8> thread_offset);
  void SpillCoreRegs();
  void UnSpillCoreRegs();
  void UnSpillFPRegs();
  void SpillFPRegs();
  static const X86EncodingMap EncodingMap[kX86Last];
  bool InexpensiveConstantInt(int32_t value);
  bool InexpensiveConstantFloat(int32_t value);
  bool InexpensiveConstantLong(int64_t value);
  bool InexpensiveConstantDouble(int64_t value);
  virtual bool GenerateTwoOperandInstructions() const { return true; }
  void GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_lhs,
                     RegLocation rl_rhs);
  static void DumpRegLocation(RegLocation loc);
  void LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                         SpecialTargetRegister symbolic_reg);
  void LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg);
  void FlushIns(RegLocation* ArgLocs, RegLocation rl_method);
  int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);
  int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx,
                         uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                         bool skip_this);
  virtual LIR * CallWithLinkerFixup(const MethodReference& target_method, InvokeType type);
  void InstallLiteralPools();
  static std::vector<uint8_t>* ReturnCommonCallFrameInformation();
  std::vector<uint8_t>* ReturnCallFrameInformation();
protected:
  RegStorage As32BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is64Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 64b register " << reg.GetReg();
      } else {
        LOG(WARNING) << "Expected 64b register " << reg.GetReg();
        return reg;
      }
    }
    RegStorage ret_val = RegStorage(RegStorage::k32BitSolo,
                                    reg.GetRawBits() & RegStorage::kRegTypeMask);
    DCHECK_EQ(GetRegInfo(reg)->FindMatchingView(RegisterInfo::k32SoloStorageMask)
                             ->GetReg().GetReg(),
              ret_val.GetReg());
    return ret_val;
  }
  RegStorage As64BitReg(RegStorage reg) {
    DCHECK(!reg.IsPair());
    if ((kFailOnSizeError || kReportSizeError) && !reg.Is32Bit()) {
      if (kFailOnSizeError) {
        LOG(FATAL) << "Expected 32b register " << reg.GetReg();
      } else {
        LOG(WARNING) << "Expected 32b register " << reg.GetReg();
        return reg;
      }
    }
    RegStorage ret_val = RegStorage(RegStorage::k64BitSolo,
                                    reg.GetRawBits() & RegStorage::kRegTypeMask);
    DCHECK_EQ(GetRegInfo(reg)->FindMatchingView(RegisterInfo::k64SoloStorageMask)
                             ->GetReg().GetReg(),
              ret_val.GetReg());
    return ret_val;
  }
  size_t ComputeSize(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_index,
                     int32_t raw_base, int32_t displacement);
  void CheckValidByteRegister(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitPrefix(const X86EncodingMap* entry,
                  int32_t raw_reg_r, int32_t raw_reg_x, int32_t raw_reg_b);
  void EmitOpcode(const X86EncodingMap* entry);
  void EmitPrefixAndOpcode(const X86EncodingMap* entry,
                           int32_t reg_r, int32_t reg_x, int32_t reg_b);
  void EmitDisp(uint8_t base, int32_t disp);
  void EmitModrmThread(uint8_t reg_or_opcode);
  void EmitModrmDisp(uint8_t reg_or_opcode, uint8_t base, int32_t disp);
  void EmitModrmSibDisp(uint8_t reg_or_opcode, uint8_t base, uint8_t index, int scale,
                        int32_t disp);
  void EmitImm(const X86EncodingMap* entry, int64_t imm);
  void EmitNullary(const X86EncodingMap* entry);
  void EmitOpRegOpcode(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitOpReg(const X86EncodingMap* entry, int32_t raw_reg);
  void EmitOpMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp);
  void EmitOpArray(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                   int32_t disp);
  void EmitMemReg(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t raw_reg);
  void EmitRegMem(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base, int32_t disp);
  void EmitRegArray(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base,
                    int32_t raw_index, int scale, int32_t disp);
  void EmitArrayReg(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                    int32_t disp, int32_t raw_reg);
  void EmitMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t imm);
  void EmitArrayImm(const X86EncodingMap* entry, int32_t raw_base, int32_t raw_index, int scale,
                    int32_t raw_disp, int32_t imm);
  void EmitRegThread(const X86EncodingMap* entry, int32_t raw_reg, int32_t disp);
  void EmitRegReg(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2);
  void EmitRegRegImm(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2, int32_t imm);
  void EmitRegMemImm(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_base, int32_t disp,
                     int32_t imm);
  void EmitMemRegImm(const X86EncodingMap* entry, int32_t base, int32_t disp, int32_t raw_reg1,
                     int32_t imm);
  void EmitRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm);
  void EmitThreadImm(const X86EncodingMap* entry, int32_t disp, int32_t imm);
  void EmitMovRegImm(const X86EncodingMap* entry, int32_t raw_reg, int64_t imm);
  void EmitShiftRegImm(const X86EncodingMap* entry, int32_t raw_reg, int32_t imm);
  void EmitShiftRegCl(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_cl);
  void EmitShiftMemCl(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t raw_cl);
  void EmitShiftMemImm(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t imm);
  void EmitRegCond(const X86EncodingMap* entry, int32_t raw_reg, int32_t cc);
  void EmitMemCond(const X86EncodingMap* entry, int32_t raw_base, int32_t disp, int32_t cc);
  void EmitRegRegCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_reg2, int32_t cc);
  void EmitRegMemCond(const X86EncodingMap* entry, int32_t raw_reg1, int32_t raw_base, int32_t disp,
                      int32_t cc);
  void EmitJmp(const X86EncodingMap* entry, int32_t rel);
  void EmitJcc(const X86EncodingMap* entry, int32_t rel, int32_t cc);
  void EmitCallMem(const X86EncodingMap* entry, int32_t raw_base, int32_t disp);
  void EmitCallImmediate(const X86EncodingMap* entry, int32_t disp);
  void EmitCallThread(const X86EncodingMap* entry, int32_t disp);
  void EmitPcRel(const X86EncodingMap* entry, int32_t raw_reg, int32_t raw_base_or_table,
                 int32_t raw_index, int scale, int32_t table_or_disp);
  void EmitMacro(const X86EncodingMap* entry, int32_t raw_reg, int32_t offset);
  void EmitUnimplemented(const X86EncodingMap* entry, LIR* lir);
  void GenFusedLongCmpImmBranch(BasicBlock* bb, RegLocation rl_src1,
                                int64_t val, ConditionCode ccode);
  void GenConstWide(RegLocation rl_dest, int64_t value);
  void GenMultiplyVectorSignedByte(BasicBlock *bb, MIR *mir);
  void GenShiftByteVector(BasicBlock *bb, MIR *mir);
  void AndMaskVectorRegister(RegStorage rs_src1, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4);
  void MaskVectorRegister(X86OpCode opcode, RegStorage rs_src1, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4);
  void AppendOpcodeWithConst(X86OpCode opcode, int reg, MIR* mir);
  static bool ProvidesFullMemoryBarrier(X86OpCode opcode);
  virtual RegStorage AllocateByteRegister();
  virtual RegStorage Get128BitRegister(RegStorage reg);
  bool IsByteRegister(RegStorage reg);
  bool GenInlinedArrayCopyCharArray(CallInfo* info) OVERRIDE;
  bool GenInlinedIndexOf(CallInfo* info, bool zero_based);
  void ReserveVectorRegisters(MIR* mir);
  void ReturnVectorRegisters();
  void GenConst128(BasicBlock* bb, MIR* mir);
  void GenMoveVector(BasicBlock *bb, MIR *mir);
  void GenMultiplyVector(BasicBlock *bb, MIR *mir);
  void GenAddVector(BasicBlock *bb, MIR *mir);
  void GenSubtractVector(BasicBlock *bb, MIR *mir);
  void GenShiftLeftVector(BasicBlock *bb, MIR *mir);
  void GenSignedShiftRightVector(BasicBlock *bb, MIR *mir);
  void GenUnsignedShiftRightVector(BasicBlock *bb, MIR *mir);
  void GenAndVector(BasicBlock *bb, MIR *mir);
  void GenOrVector(BasicBlock *bb, MIR *mir);
  void GenXorVector(BasicBlock *bb, MIR *mir);
  void GenAddReduceVector(BasicBlock *bb, MIR *mir);
  void GenReduceVector(BasicBlock *bb, MIR *mir);
  void GenSetVector(BasicBlock *bb, MIR *mir);
  void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir);
  X86OpCode GetOpcode(Instruction::Code op, RegLocation loc, bool is_high_op, int32_t value);
  X86OpCode GetOpcode(Instruction::Code op, RegLocation dest, RegLocation rhs,
                      bool is_high_op);
  bool IsNoOp(Instruction::Code op, int32_t value);
  void CalculateMagicAndShift(int divisor, int& magic, int& shift);
  RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2,
                        bool is_div, bool check_zero);
  RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src, int lit, bool is_div);
  RegLocation GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                RegLocation rl_src, int shift_amount);
  void GenImulRegImm(RegStorage dest, RegStorage src, int val);
  void GenImulMemImm(RegStorage dest, int sreg, int displacement, int val);
  LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                         int offset, int check_value, LIR* target, LIR** compare);
  bool IsOperationSafeWithoutTemps(RegLocation rl_lhs, RegLocation rl_rhs);
  virtual void GenLongToFP(RegLocation rl_dest, RegLocation rl_src, bool is_double);
  void Materialize();
  RegLocation UpdateLocTyped(RegLocation loc, int reg_class);
  RegLocation UpdateLocWideTyped(RegLocation loc, int reg_class);
  void AnalyzeMIR();
  void AnalyzeBB(BasicBlock * bb);
  void AnalyzeExtendedMIR(int opcode, BasicBlock * bb, MIR *mir);
  virtual void AnalyzeMIR(int opcode, BasicBlock * bb, MIR *mir);
  void AnalyzeFPInstruction(int opcode, BasicBlock * bb, MIR *mir);
  void AnalyzeDoubleUse(RegLocation rl_use);
  void AnalyzeInvokeStatic(int opcode, BasicBlock * bb, MIR *mir);
  CompilerTemp *base_of_code_;
  bool store_method_addr_;
  bool store_method_addr_used_;
  LIR* setup_method_address_[2];
  GrowableArray<LIR*> method_address_insns_;
  GrowableArray<LIR*> class_type_address_insns_;
  GrowableArray<LIR*> call_method_insns_;
  LIR* stack_decrement_;
  LIR* stack_increment_;
  LIR *const_vectors_;
  LIR *ScanVectorLiteral(MIR *mir);
  LIR *AddVectorLiteral(MIR *mir);
  InToRegStorageMapping in_to_reg_storage_mapping_;
  bool WideGPRsAreAliases() OVERRIDE {
    return cu_->target64;
  }
  bool WideFPRsAreAliases() OVERRIDE {
    return true;
  }
private:
  int num_reserved_vector_regs_;
};
}
#endif
