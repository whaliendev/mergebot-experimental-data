#ifndef ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_
#define ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_ 
#include "invoke_type.h"
#include "compiled_method.h"
#include "dex/compiler_enums.h"
#include "dex/compiler_ir.h"
#include "dex/reg_location.h"
#include "dex/reg_storage.h"
#include "dex/backend.h"
#include "dex/quick/resource_mask.h"
#include "driver/compiler_driver.h"
#include "instruction_set.h"
#include "leb128.h"
#include "safe_map.h"
#include "utils/array_ref.h"
#include "utils/arena_allocator.h"
#include "utils/growable_array.h"
#include "utils/stack_checks.h"
namespace art {
typedef uint32_t DexOffset;
typedef uint16_t NarrowDexOffset;
typedef uint32_t CodeOffset;
#define NO_SUSPEND 0
#define IS_BINARY_OP (1ULL << kIsBinaryOp)
#define IS_BRANCH (1ULL << kIsBranch)
#define IS_IT (1ULL << kIsIT)
#define IS_LOAD (1ULL << kMemLoad)
#define IS_QUAD_OP (1ULL << kIsQuadOp)
#define IS_QUIN_OP (1ULL << kIsQuinOp)
#define IS_SEXTUPLE_OP (1ULL << kIsSextupleOp)
#define IS_STORE (1ULL << kMemStore)
#define IS_TERTIARY_OP (1ULL << kIsTertiaryOp)
#define IS_UNARY_OP (1ULL << kIsUnaryOp)
#define NEEDS_FIXUP (1ULL << kPCRelFixup)
#define NO_OPERAND (1ULL << kNoOperand)
#define REG_DEF0 (1ULL << kRegDef0)
#define REG_DEF1 (1ULL << kRegDef1)
#define REG_DEF2 (1ULL << kRegDef2)
#define REG_DEFA (1ULL << kRegDefA)
#define REG_DEFD (1ULL << kRegDefD)
#define REG_DEF_FPCS_LIST0 (1ULL << kRegDefFPCSList0)
#define REG_DEF_FPCS_LIST2 (1ULL << kRegDefFPCSList2)
#define REG_DEF_LIST0 (1ULL << kRegDefList0)
#define REG_DEF_LIST1 (1ULL << kRegDefList1)
#define REG_DEF_LR (1ULL << kRegDefLR)
#define REG_DEF_SP (1ULL << kRegDefSP)
#define REG_USE0 (1ULL << kRegUse0)
#define REG_USE1 (1ULL << kRegUse1)
#define REG_USE2 (1ULL << kRegUse2)
#define REG_USE3 (1ULL << kRegUse3)
#define REG_USE4 (1ULL << kRegUse4)
#define REG_USEA (1ULL << kRegUseA)
#define REG_USEC (1ULL << kRegUseC)
#define REG_USED (1ULL << kRegUseD)
#define REG_USEB (1ULL << kRegUseB)
#define REG_USE_FPCS_LIST0 (1ULL << kRegUseFPCSList0)
#define REG_USE_FPCS_LIST2 (1ULL << kRegUseFPCSList2)
#define REG_USE_LIST0 (1ULL << kRegUseList0)
#define REG_USE_LIST1 (1ULL << kRegUseList1)
#define REG_USE_LR (1ULL << kRegUseLR)
#define REG_USE_PC (1ULL << kRegUsePC)
#define REG_USE_SP (1ULL << kRegUseSP)
#define SETS_CCODES (1ULL << kSetsCCodes)
#define USES_CCODES (1ULL << kUsesCCodes)
#define USE_FP_STACK (1ULL << kUseFpStack)
#define REG_USE_LO (1ULL << kUseLo)
#define REG_USE_HI (1ULL << kUseHi)
#define REG_DEF_LO (1ULL << kDefLo)
#define REG_DEF_HI (1ULL << kDefHi)
#define REG_DEF01 (REG_DEF0 | REG_DEF1)
#define REG_DEF012 (REG_DEF0 | REG_DEF1 | REG_DEF2)
#define REG_DEF01_USE2 (REG_DEF0 | REG_DEF1 | REG_USE2)
#define REG_DEF0_USE01 (REG_DEF0 | REG_USE01)
#define REG_DEF0_USE0 (REG_DEF0 | REG_USE0)
#define REG_DEF0_USE12 (REG_DEF0 | REG_USE12)
#define REG_DEF0_USE123 (REG_DEF0 | REG_USE123)
#define REG_DEF0_USE1 (REG_DEF0 | REG_USE1)
#define REG_DEF0_USE2 (REG_DEF0 | REG_USE2)
#define REG_DEFAD_USEAD (REG_DEFAD_USEA | REG_USED)
#define REG_DEFAD_USEA (REG_DEFA_USEA | REG_DEFD)
#define REG_DEFA_USEA (REG_DEFA | REG_USEA)
#define REG_USE012 (REG_USE01 | REG_USE2)
#define REG_USE014 (REG_USE01 | REG_USE4)
#define REG_USE01 (REG_USE0 | REG_USE1)
#define REG_USE02 (REG_USE0 | REG_USE2)
#define REG_USE12 (REG_USE1 | REG_USE2)
#define REG_USE23 (REG_USE2 | REG_USE3)
#define REG_USE123 (REG_USE1 | REG_USE2 | REG_USE3)
#ifndef INVALID_SREG
#define INVALID_SREG (-1)
#endif
struct BasicBlock;
struct CallInfo;
struct CompilationUnit;
struct InlineMethod;
struct MIR;
struct LIR;
struct RegisterInfo;
class DexFileMethodInliner;
class MIRGraph;
class Mir2Lir;
typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int,
                            const MethodReference& target_method,
                            uint32_t method_idx, uintptr_t direct_code,
                            uintptr_t direct_method, InvokeType type);
typedef std::vector<uint8_t> CodeBuffer;
struct UseDefMasks {
  const ResourceMask* use_mask;
  const ResourceMask* def_mask;
};
struct AssemblyInfo {
  LIR* pcrel_next;
};
struct LIR {
  CodeOffset offset;
  NarrowDexOffset dalvik_offset;
  int16_t opcode;
  LIR* next;
  LIR* prev;
  LIR* target;
  struct {
    unsigned int alias_info:17;
    bool is_nop:1;
    unsigned int size:4;
    bool use_def_invalid:1;
    unsigned int generation:1;
    unsigned int fixup:8;
  } flags;
  union {
    UseDefMasks m;
    AssemblyInfo a;
  } u;
  int32_t operands[5];
};
Mir2Lir* ArmCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* Arm64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                            ArenaAllocator* const arena);
Mir2Lir* MipsCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)
#define DECODE_ALIAS_INFO_REG(X) (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE_FLAG (0x10000)
#define DECODE_ALIAS_INFO_WIDE(X) ((X & DECODE_ALIAS_INFO_WIDE_FLAG) ? 1 : 0)
#define ENCODE_ALIAS_INFO(REG,ISWIDE) (REG | (ISWIDE ? DECODE_ALIAS_INFO_WIDE_FLAG : 0))
#define ENCODE_REG_PAIR(low_reg,high_reg) ((low_reg & 0xff) | ((high_reg & 0xff) << 8))
#define DECODE_REG_PAIR(both_regs,low_reg,high_reg) \
  do { \
    low_reg = both_regs & 0xff; \
    high_reg = (both_regs >> 8) & 0xff; \
  } while (false)
#define STARTING_WIDE_SREG 0x10000
#define SLOW_FIELD_PATH (cu_->enable_debug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cu_->enable_debug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cu_->enable_debug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowestStringPath))
class Mir2Lir : public Backend {
  public:
    static constexpr bool kFailOnSizeError = true && kIsDebugBuild;
    static constexpr bool kReportSizeError = true && kIsDebugBuild;
    struct EmbeddedData {
      CodeOffset offset;
      const uint16_t* table;
      DexOffset vaddr;
    };
    struct FillArrayData : EmbeddedData {
      int32_t size;
    };
    struct SwitchTable : EmbeddedData {
      LIR* anchor;
      LIR** targets;
    };
    struct RefCounts {
      int count;
      int s_reg;
    };
    class RegisterInfo {
     public:
      RegisterInfo(RegStorage r, const ResourceMask& mask = kEncodeAll);
      ~RegisterInfo() {}
      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocRegAlloc);
      }
      static const uint32_t k32SoloStorageMask = 0x00000001;
      static const uint32_t kLowSingleStorageMask = 0x00000001;
      static const uint32_t kHighSingleStorageMask = 0x00000002;
      static const uint32_t k64SoloStorageMask = 0x00000003;
      static const uint32_t k128SoloStorageMask = 0x0000000f;
      static const uint32_t k256SoloStorageMask = 0x000000ff;
      static const uint32_t k512SoloStorageMask = 0x0000ffff;
      static const uint32_t k1024SoloStorageMask = 0xffffffff;
      bool InUse() { return (storage_mask_ & master_->used_storage_) != 0; }
      void MarkInUse() { master_->used_storage_ |= storage_mask_; }
      void MarkFree() { master_->used_storage_ &= ~storage_mask_; }
      bool IsDead() { return (master_->liveness_ & storage_mask_) == 0; }
      bool IsLive() { return (master_->liveness_ & storage_mask_) == storage_mask_; }
      void MarkLive(int s_reg) {
        s_reg_ = s_reg;
        master_->liveness_ |= storage_mask_;
      }
      void MarkDead() {
        if (SReg() != INVALID_SREG) {
          s_reg_ = INVALID_SREG;
          master_->liveness_ &= ~storage_mask_;
          ResetDefBody();
        }
      }
      RegStorage GetReg() { return reg_; }
      void SetReg(RegStorage reg) { reg_ = reg; }
      bool IsTemp() { return is_temp_; }
      void SetIsTemp(bool val) { is_temp_ = val; }
      bool IsWide() { return wide_value_; }
      void SetIsWide(bool val) {
        wide_value_ = val;
        if (!val) {
          SetPartner(GetReg());
        }
      }
      bool IsDirty() { return dirty_; }
      void SetIsDirty(bool val) { dirty_ = val; }
      RegStorage Partner() { return partner_; }
      void SetPartner(RegStorage partner) { partner_ = partner; }
      int SReg() { return (!IsTemp() || IsLive()) ? s_reg_ : INVALID_SREG; }
      const ResourceMask& DefUseMask() { return def_use_mask_; }
      void SetDefUseMask(const ResourceMask& def_use_mask) { def_use_mask_ = def_use_mask; }
      RegisterInfo* Master() { return master_; }
      void SetMaster(RegisterInfo* master) {
        master_ = master;
        if (master != this) {
          master_->aliased_ = true;
          DCHECK(alias_chain_ == nullptr);
          alias_chain_ = master_->alias_chain_;
          master_->alias_chain_ = this;
        }
      }
      bool IsAliased() { return aliased_; }
      RegisterInfo* GetAliasChain() { return alias_chain_; }
      uint32_t StorageMask() { return storage_mask_; }
      void SetStorageMask(uint32_t storage_mask) { storage_mask_ = storage_mask; }
      LIR* DefStart() { return def_start_; }
      void SetDefStart(LIR* def_start) { def_start_ = def_start; }
      LIR* DefEnd() { return def_end_; }
      void SetDefEnd(LIR* def_end) { def_end_ = def_end; }
      void ResetDefBody() { def_start_ = def_end_ = nullptr; }
      RegisterInfo* FindMatchingView(uint32_t storage_used) {
        RegisterInfo* res = Master();
        for (; res != nullptr; res = res->GetAliasChain()) {
          if (res->StorageMask() == storage_used)
            break;
        }
        return res;
      }
     private:
      RegStorage reg_;
      bool is_temp_;
      bool wide_value_;
      bool dirty_;
      bool aliased_;
      RegStorage partner_;
      int s_reg_;
      ResourceMask def_use_mask_;
      uint32_t used_storage_;
      uint32_t liveness_;
      RegisterInfo* master_;
      uint32_t storage_mask_;
      LIR *def_start_;
      LIR *def_end_;
      RegisterInfo* alias_chain_;
    };
    class RegisterPool {
     public:
      RegisterPool(Mir2Lir* m2l, ArenaAllocator* arena,
                   const ArrayRef<const RegStorage>& core_regs,
                   const ArrayRef<const RegStorage>& core64_regs,
                   const ArrayRef<const RegStorage>& sp_regs,
                   const ArrayRef<const RegStorage>& dp_regs,
                   const ArrayRef<const RegStorage>& reserved_regs,
                   const ArrayRef<const RegStorage>& reserved64_regs,
                   const ArrayRef<const RegStorage>& core_temps,
                   const ArrayRef<const RegStorage>& core64_temps,
                   const ArrayRef<const RegStorage>& sp_temps,
                   const ArrayRef<const RegStorage>& dp_temps);
      ~RegisterPool() {}
      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocRegAlloc);
      }
      void ResetNextTemp() {
        next_core_reg_ = 0;
        next_sp_reg_ = 0;
        next_dp_reg_ = 0;
      }
      GrowableArray<RegisterInfo*> core_regs_;
      int next_core_reg_;
      GrowableArray<RegisterInfo*> core64_regs_;
      int next_core64_reg_;
      GrowableArray<RegisterInfo*> sp_regs_;
      int next_sp_reg_;
      GrowableArray<RegisterInfo*> dp_regs_;
      int next_dp_reg_;
      GrowableArray<RegisterInfo*>* ref_regs_;
      int* next_ref_reg_;
     private:
      Mir2Lir* const m2l_;
    };
    struct PromotionMap {
      RegLocationType core_location:3;
      uint8_t core_reg;
      RegLocationType fp_location:3;
      uint8_t fp_reg;
      bool first_in_pair;
    };
    class LIRSlowPath {
     public:
      LIRSlowPath(Mir2Lir* m2l, const DexOffset dexpc, LIR* fromfast,
                  LIR* cont = nullptr) :
        m2l_(m2l), cu_(m2l->cu_), current_dex_pc_(dexpc), fromfast_(fromfast), cont_(cont) {
          m2l->StartSlowPath(this);
      }
      virtual ~LIRSlowPath() {}
      virtual void Compile() = 0;
      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocData);
      }
      LIR *GetContinuationLabel() {
        return cont_;
      }
      LIR *GetFromFast() {
        return fromfast_;
      }
     protected:
      LIR* GenerateTargetLabel(int opcode = kPseudoTargetLabel);
      Mir2Lir* const m2l_;
      CompilationUnit* const cu_;
      const DexOffset current_dex_pc_;
      LIR* const fromfast_;
      LIR* const cont_;
    };
    class ScopedMemRefType {
     public:
      ScopedMemRefType(Mir2Lir* m2l, ResourceMask::ResourceBit new_mem_ref_type)
          : m2l_(m2l),
            old_mem_ref_type_(m2l->mem_ref_type_) {
        m2l_->mem_ref_type_ = new_mem_ref_type;
      }
      ~ScopedMemRefType() {
        m2l_->mem_ref_type_ = old_mem_ref_type_;
      }
     private:
      Mir2Lir* const m2l_;
      ResourceMask::ResourceBit old_mem_ref_type_;
      DISALLOW_COPY_AND_ASSIGN(ScopedMemRefType);
    };
    virtual ~Mir2Lir() {}
    int32_t s4FromSwitchData(const void* switch_data) {
      return *reinterpret_cast<const int32_t*>(switch_data);
    }
    RegisterClass RegClassBySize(OpSize size) {
      if (size == kReference) {
        return kRefReg;
      } else {
        return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte ||
                size == kSignedByte) ? kCoreReg : kAnyReg;
      }
    }
    size_t CodeBufferSizeInBytes() {
      return code_buffer_.size() / sizeof(code_buffer_[0]);
    }
    static bool IsPseudoLirOp(int opcode) {
      return (opcode < 0);
    }
    uint32_t WrapPointer(void* pointer) {
      uint32_t res = pointer_storage_.Size();
      pointer_storage_.Insert(pointer);
      return res;
    }
    void* UnwrapPointer(size_t index) {
      return pointer_storage_.Get(index);
    }
    char* ArenaStrdup(const char* str) {
      size_t len = strlen(str) + 1;
      char* res = reinterpret_cast<char*>(arena_->Alloc(len, kArenaAllocMisc));
      if (res != NULL) {
        strncpy(res, str, len);
      }
      return res;
    }
    void AppendLIR(LIR* lir);
    void InsertLIRBefore(LIR* current_lir, LIR* new_lir);
    void InsertLIRAfter(LIR* current_lir, LIR* new_lir);
    size_t GetMaxPossibleCompilerTemps() const;
    size_t GetNumBytesForCompilerTempSpillRegion();
    DexOffset GetCurrentDexPc() const {
      return current_dalvik_offset_;
    }
    RegisterClass ShortyToRegClass(char shorty_type);
    RegisterClass LocToRegClass(RegLocation loc);
    int ComputeFrameSize();
    virtual void Materialize();
    virtual CompiledMethod* GetCompiledMethod();
    void MarkSafepointPC(LIR* inst);
    void MarkSafepointPCAfter(LIR* after);
    void SetupResourceMasks(LIR* lir);
    void SetMemRefType(LIR* lir, bool is_load, int mem_type);
    void AnnotateDalvikRegAccess(LIR* lir, int reg_id, bool is_load, bool is64bit);
    void SetupRegMask(ResourceMask* mask, int reg);
    void DumpLIRInsn(LIR* arg, unsigned char* base_addr);
    void DumpPromotionMap();
    void CodegenDump();
    LIR* RawLIR(DexOffset dalvik_offset, int opcode, int op0 = 0, int op1 = 0,
                int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
    LIR* NewLIR0(int opcode);
    LIR* NewLIR1(int opcode, int dest);
    LIR* NewLIR2(int opcode, int dest, int src1);
    LIR* NewLIR2NoDest(int opcode, int src, int info);
    LIR* NewLIR3(int opcode, int dest, int src1, int src2);
    LIR* NewLIR4(int opcode, int dest, int src1, int src2, int info);
    LIR* NewLIR5(int opcode, int dest, int src1, int src2, int info1, int info2);
    LIR* ScanLiteralPool(LIR* data_target, int value, unsigned int delta);
    LIR* ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi);
    LIR* ScanLiteralPoolMethod(LIR* data_target, const MethodReference& method);
    LIR* AddWordData(LIR* *constant_list_p, int value);
    LIR* AddWideData(LIR* *constant_list_p, int val_lo, int val_hi);
    void ProcessSwitchTables();
    void DumpSparseSwitchTable(const uint16_t* table);
    void DumpPackedSwitchTable(const uint16_t* table);
    void MarkBoundary(DexOffset offset, const char* inst_str);
    void NopLIR(LIR* lir);
    void UnlinkLIR(LIR* lir);
    bool EvaluateBranch(Instruction::Code opcode, int src1, int src2);
    bool IsInexpensiveConstant(RegLocation rl_src);
    ConditionCode FlipComparisonOrder(ConditionCode before);
    ConditionCode NegateComparison(ConditionCode before);
    virtual void InstallLiteralPools();
    void InstallSwitchTables();
    void InstallFillArrayData();
    bool VerifyCatchEntries();
    void CreateMappingTables();
    void CreateNativeGcMap();
    int AssignLiteralOffset(CodeOffset offset);
    int AssignSwitchTablesOffset(CodeOffset offset);
    int AssignFillArrayDataOffset(CodeOffset offset);
    virtual LIR* InsertCaseLabel(DexOffset vaddr, int keyVal);
    void MarkPackedCaseLabels(Mir2Lir::SwitchTable* tab_rec);
    void MarkSparseCaseLabels(Mir2Lir::SwitchTable* tab_rec);
    virtual void StartSlowPath(LIRSlowPath* slowpath) {}
    virtual void BeginInvoke(CallInfo* info) {}
    virtual void EndInvoke(CallInfo* info) {}
    virtual RegLocation NarrowRegLoc(RegLocation loc);
    void ConvertMemOpIntoMove(LIR* orig_lir, RegStorage dest, RegStorage src);
    void ApplyLoadStoreElimination(LIR* head_lir, LIR* tail_lir);
    void ApplyLoadHoisting(LIR* head_lir, LIR* tail_lir);
    virtual void ApplyLocalOptimizations(LIR* head_lir, LIR* tail_lir);
    int GetSRegHi(int lowSreg);
    bool LiveOut(int s_reg);
    void SimpleRegAlloc();
    void ResetRegPool();
    void CompilerInitPool(RegisterInfo* info, RegStorage* regs, int num);
    void DumpRegPool(GrowableArray<RegisterInfo*>* regs);
    void DumpCoreRegPool();
    void DumpFpRegPool();
    void DumpRegPools();
    void Clobber(RegStorage reg);
    void ClobberSReg(int s_reg);
    void ClobberAliases(RegisterInfo* info, uint32_t clobber_mask);
    int SRegToPMap(int s_reg);
    void RecordCorePromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedCoreReg(int s_reg);
    void RecordFpPromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedFpReg(int s_reg);
    virtual RegStorage AllocPreservedSingle(int s_reg);
    virtual RegStorage AllocPreservedDouble(int s_reg);
    RegStorage AllocTempBody(GrowableArray<RegisterInfo*> &regs, int* next_temp, bool required);
    virtual RegStorage AllocTemp(bool required = true);
    virtual RegStorage AllocTempWide(bool required = true);
    virtual RegStorage AllocTempRef(bool required = true);
    virtual RegStorage AllocTempSingle(bool required = true);
    virtual RegStorage AllocTempDouble(bool required = true);
    virtual RegStorage AllocTypedTemp(bool fp_hint, int reg_class, bool required = true);
    virtual RegStorage AllocTypedTempWide(bool fp_hint, int reg_class, bool required = true);
    void FlushReg(RegStorage reg);
    void FlushRegWide(RegStorage reg);
    RegStorage AllocLiveReg(int s_reg, int reg_class, bool wide);
    RegStorage FindLiveReg(GrowableArray<RegisterInfo*> &regs, int s_reg);
    virtual void FreeTemp(RegStorage reg);
    virtual void FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free);
    virtual bool IsLive(RegStorage reg);
    virtual bool IsTemp(RegStorage reg);
    bool IsPromoted(RegStorage reg);
    bool IsDirty(RegStorage reg);
    virtual void LockTemp(RegStorage reg);
    void ResetDef(RegStorage reg);
    void NullifyRange(RegStorage reg, int s_reg);
    void MarkDef(RegLocation rl, LIR *start, LIR *finish);
    void MarkDefWide(RegLocation rl, LIR *start, LIR *finish);
    void ResetDefLoc(RegLocation rl);
    void ResetDefLocWide(RegLocation rl);
    void ResetDefTracking();
    void ClobberAllTemps();
    void FlushSpecificReg(RegisterInfo* info);
    void FlushAllRegs();
    bool RegClassMatches(int reg_class, RegStorage reg);
    void MarkLive(RegLocation loc);
    void MarkTemp(RegStorage reg);
    void UnmarkTemp(RegStorage reg);
    void MarkWide(RegStorage reg);
    void MarkNarrow(RegStorage reg);
    void MarkClean(RegLocation loc);
    void MarkDirty(RegLocation loc);
    void MarkInUse(RegStorage reg);
    bool CheckCorePoolSanity();
    virtual RegLocation UpdateLoc(RegLocation loc);
    virtual RegLocation UpdateLocWide(RegLocation loc);
    RegLocation UpdateRawLoc(RegLocation loc);
    virtual RegLocation EvalLocWide(RegLocation loc, int reg_class, bool update);
    virtual RegLocation EvalLoc(RegLocation loc, int reg_class, bool update);
    void CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs);
    void DumpCounts(const RefCounts* arr, int size, const char* msg);
    void DoPromotion();
    int VRegOffset(int v_reg);
    int SRegOffset(int s_reg);
    RegLocation GetReturnWide(RegisterClass reg_class);
    RegLocation GetReturn(RegisterClass reg_class);
    RegisterInfo* GetRegInfo(RegStorage reg);
    void AddIntrinsicSlowPath(CallInfo* info, LIR* branch, LIR* resume = nullptr);
    virtual bool HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                  RegLocation rl_src, RegLocation rl_dest, int lit);
    bool HandleEasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit);
    virtual void HandleSlowPaths();
    void GenBarrier();
    void GenDivZeroException();
    void GenDivZeroCheck(ConditionCode c_code);
    void GenDivZeroCheck(RegStorage reg);
    void GenArrayBoundsCheck(RegStorage index, RegStorage length);
    void GenArrayBoundsCheck(int32_t index, RegStorage length);
    LIR* GenNullCheck(RegStorage reg);
    void MarkPossibleNullPointerException(int opt_flags);
    void MarkPossibleNullPointerExceptionAfter(int opt_flags, LIR* after);
    void MarkPossibleStackOverflowException();
    void ForceImplicitNullCheck(RegStorage reg, int opt_flags);
    LIR* GenImmedCheck(ConditionCode c_code, RegStorage reg, int imm_val, ThrowKind kind);
    LIR* GenNullCheck(RegStorage m_reg, int opt_flags);
    LIR* GenExplicitNullCheck(RegStorage m_reg, int opt_flags);
    virtual void GenImplicitNullCheck(RegStorage reg, int opt_flags);
    void GenCompareAndBranch(Instruction::Code opcode, RegLocation rl_src1,
                             RegLocation rl_src2, LIR* taken, LIR* fall_through);
    void GenCompareZeroAndBranch(Instruction::Code opcode, RegLocation rl_src,
                                 LIR* taken, LIR* fall_through);
    virtual void GenIntToLong(RegLocation rl_dest, RegLocation rl_src);
    void GenIntNarrowing(Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src);
    void GenNewArray(uint32_t type_idx, RegLocation rl_dest,
                     RegLocation rl_src);
    void GenFilledNewArray(CallInfo* info);
    void GenSput(MIR* mir, RegLocation rl_src,
                 bool is_long_or_double, bool is_object);
    void GenSget(MIR* mir, RegLocation rl_dest,
                 bool is_long_or_double, bool is_object);
    void GenIGet(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenIPut(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                        RegLocation rl_src);
    void GenConstClass(uint32_t type_idx, RegLocation rl_dest);
    void GenConstString(uint32_t string_idx, RegLocation rl_dest);
    void GenNewInstance(uint32_t type_idx, RegLocation rl_dest);
    void GenThrow(RegLocation rl_src);
    void GenInstanceof(uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);
    void GenCheckCast(uint32_t insn_idx, uint32_t type_idx, RegLocation rl_src);
    void GenLong3Addr(OpKind first_op, OpKind second_op, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_shift);
    void GenArithOpIntLit(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src, int lit);
    void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_src2);
    template <size_t pointer_size>
    void GenConversionCall(ThreadOffset<pointer_size> func_offset, RegLocation rl_dest,
                           RegLocation rl_src);
    virtual void GenSuspendTest(int opt_flags);
    virtual void GenSuspendTestAndBranch(int opt_flags, LIR* target);
    virtual void GenConstWide(RegLocation rl_dest, int64_t value);
    virtual void GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                       RegLocation rl_src1, RegLocation rl_src2);
    template <size_t pointer_size>
    LIR* CallHelper(RegStorage r_tgt, ThreadOffset<pointer_size> helper_offset, bool safepoint_pc,
                    bool use_link = true);
    RegStorage CallHelperSetup(ThreadOffset<4> helper_offset);
    RegStorage CallHelperSetup(ThreadOffset<8> helper_offset);
    template <size_t pointer_size>
    void CallRuntimeHelper(ThreadOffset<pointer_size> helper_offset, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImm(ThreadOffset<pointer_size> helper_offset, int arg0, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperReg(ThreadOffset<pointer_size> helper_offset, RegStorage arg0, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegLocation(ThreadOffset<pointer_size> helper_offset, RegLocation arg0,
                                      bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmImm(ThreadOffset<pointer_size> helper_offset, int arg0, int arg1,
                                 bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmRegLocation(ThreadOffset<pointer_size> helper_offset, int arg0,
                                         RegLocation arg1, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegLocationImm(ThreadOffset<pointer_size> helper_offset, RegLocation arg0,
                                         int arg1, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmReg(ThreadOffset<pointer_size> helper_offset, int arg0, RegStorage arg1,
                                 bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegImm(ThreadOffset<pointer_size> helper_offset, RegStorage arg0, int arg1,
                                 bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmMethod(ThreadOffset<pointer_size> helper_offset, int arg0,
                                    bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegMethod(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                    bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegMethodRegLocation(ThreadOffset<pointer_size> helper_offset,
                                               RegStorage arg0, RegLocation arg2, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                 RegLocation arg0, RegLocation arg1,
                                                 bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegReg(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                 RegStorage arg1, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegRegImm(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                    RegStorage arg1, int arg2, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmMethodRegLocation(ThreadOffset<pointer_size> helper_offset, int arg0,
                                               RegLocation arg2, bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmMethodImm(ThreadOffset<pointer_size> helper_offset, int arg0, int arg2,
                                       bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperImmRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                    int arg0, RegLocation arg1, RegLocation arg2,
                                                    bool safepoint_pc);
    template <size_t pointer_size>
    void CallRuntimeHelperRegLocationRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                            RegLocation arg0, RegLocation arg1,
                                                            RegLocation arg2,
                                                            bool safepoint_pc);
    void GenInvoke(CallInfo* info);
    void GenInvokeNoInline(CallInfo* info);
    virtual void FlushIns(RegLocation* ArgLocs, RegLocation rl_method);
    virtual int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                             NextCallInsn next_call_insn,
                             const MethodReference& target_method,
                             uint32_t vtable_idx,
                             uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                             bool skip_this);
    virtual int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);
    RegLocation InlineTarget(CallInfo* info);
    RegLocation InlineTargetWide(CallInfo* info);
    bool GenInlinedGet(CallInfo* info);
    bool GenInlinedCharAt(CallInfo* info);
    bool GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty);
    virtual bool GenInlinedReverseBits(CallInfo* info, OpSize size);
    bool GenInlinedReverseBytes(CallInfo* info, OpSize size);
    bool GenInlinedAbsInt(CallInfo* info);
    virtual bool GenInlinedAbsLong(CallInfo* info);
    virtual bool GenInlinedAbsFloat(CallInfo* info) = 0;
    virtual bool GenInlinedAbsDouble(CallInfo* info) = 0;
    bool GenInlinedFloatCvt(CallInfo* info);
    bool GenInlinedDoubleCvt(CallInfo* info);
    virtual bool GenInlinedArrayCopyCharArray(CallInfo* info);
    virtual bool GenInlinedIndexOf(CallInfo* info, bool zero_based);
    bool GenInlinedStringCompareTo(CallInfo* info);
    bool GenInlinedCurrentThread(CallInfo* info);
    bool GenInlinedUnsafeGet(CallInfo* info, bool is_long, bool is_volatile);
    bool GenInlinedUnsafePut(CallInfo* info, bool is_long, bool is_object,
                             bool is_volatile, bool is_ordered);
    virtual int LoadArgRegs(CallInfo* info, int call_state,
                    NextCallInsn next_call_insn,
                    const MethodReference& target_method,
                    uint32_t vtable_idx,
                    uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                    bool skip_this);
    RegLocation LoadCurrMethod();
    void LoadCurrMethodDirect(RegStorage r_tgt);
    virtual LIR* LoadConstant(RegStorage r_dest, int value);
    virtual LIR* LoadWordDisp(RegStorage r_base, int displacement, RegStorage r_dest) {
      return LoadBaseDisp(r_base, displacement, r_dest, kWord, kNotVolatile);
    }
    virtual LIR* Load32Disp(RegStorage r_base, int displacement, RegStorage r_dest) {
      return LoadBaseDisp(r_base, displacement, r_dest, k32, kNotVolatile);
    }
    virtual LIR* LoadRefDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                             VolatileKind is_volatile) {
      return LoadBaseDisp(r_base, displacement, r_dest, kReference, is_volatile);
    }
    virtual LIR* LoadRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                int scale) {
      return LoadBaseIndexed(r_base, r_index, r_dest, scale, kReference);
    }
    virtual RegLocation LoadValue(RegLocation rl_src, RegisterClass op_kind);
    virtual RegLocation LoadValue(RegLocation rl_src);
    virtual RegLocation LoadValueWide(RegLocation rl_src, RegisterClass op_kind);
    virtual void LoadValueDirect(RegLocation rl_src, RegStorage r_dest);
    virtual void LoadValueDirectFixed(RegLocation rl_src, RegStorage r_dest);
    virtual void LoadValueDirectWide(RegLocation rl_src, RegStorage r_dest);
    virtual void LoadValueDirectWideFixed(RegLocation rl_src, RegStorage r_dest);
    virtual LIR* StoreWordDisp(RegStorage r_base, int displacement, RegStorage r_src) {
      return StoreBaseDisp(r_base, displacement, r_src, kWord, kNotVolatile);
    }
    virtual LIR* StoreRefDisp(RegStorage r_base, int displacement, RegStorage r_src,
                              VolatileKind is_volatile) {
      return StoreBaseDisp(r_base, displacement, r_src, kReference, is_volatile);
    }
    virtual LIR* StoreRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                 int scale) {
      return StoreBaseIndexed(r_base, r_index, r_src, scale, kReference);
    }
    virtual LIR* Store32Disp(RegStorage r_base, int displacement, RegStorage r_src) {
      return StoreBaseDisp(r_base, displacement, r_src, k32, kNotVolatile);
    }
    virtual void StoreValue(RegLocation rl_dest, RegLocation rl_src);
    virtual void StoreValueWide(RegLocation rl_dest, RegLocation rl_src);
    virtual void StoreFinalValue(RegLocation rl_dest, RegLocation rl_src);
    virtual void StoreFinalValueWide(RegLocation rl_dest, RegLocation rl_src);
    void CompileDalvikInstruction(MIR* mir, BasicBlock* bb, LIR* label_list);
    virtual void HandleExtendedMethodMIR(BasicBlock* bb, MIR* mir);
    bool MethodBlockCodeGen(BasicBlock* bb);
    bool SpecialMIR2LIR(const InlineMethod& special);
    virtual void MethodMIR2LIR();
    void UpdateLIROffsets();
    void LoadCodeAddress(const MethodReference& target_method, InvokeType type,
                         SpecialTargetRegister symbolic_reg);
    virtual void LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                   SpecialTargetRegister symbolic_reg);
    virtual void LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg);
    virtual LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                   int offset, int check_value, LIR* target, LIR** compare);
    virtual bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual LIR* CheckSuspendUsingLoad() = 0;
    virtual RegStorage LoadHelper(ThreadOffset<4> offset) = 0;
    virtual RegStorage LoadHelper(ThreadOffset<8> offset) = 0;
    virtual LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size, VolatileKind is_volatile) = 0;
    virtual LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) = 0;
    virtual LIR* LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                     int displacement, RegStorage r_dest, OpSize size) = 0;
    virtual LIR* LoadConstantNoClobber(RegStorage r_dest, int value) = 0;
    virtual LIR* LoadConstantWide(RegStorage r_dest, int64_t value) = 0;
    virtual LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                               OpSize size, VolatileKind is_volatile) = 0;
    virtual LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                  int scale, OpSize size) = 0;
    virtual LIR* StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                      int displacement, RegStorage r_src, OpSize size) = 0;
    virtual void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) = 0;
    bool IsSameReg(RegStorage reg1, RegStorage reg2) {
      RegisterInfo* info1 = GetRegInfo(reg1);
      RegisterInfo* info2 = GetRegInfo(reg2);
      return (info1->Master() == info2->Master() &&
             (info1->StorageMask() & info2->StorageMask()) != 0);
    }
    virtual RegStorage TargetReg(SpecialTargetRegister reg) = 0;
    virtual RegStorage TargetReg(SpecialTargetRegister reg, WideKind wide_kind) {
      if (wide_kind == kWide) {
        DCHECK((kArg0 <= reg && reg < kArg7) || (kFArg0 <= reg && reg < kFArg7) || (kRet0 == reg));
        COMPILE_ASSERT((kArg1 == kArg0 + 1) && (kArg2 == kArg1 + 1) && (kArg3 == kArg2 + 1) &&
                       (kArg4 == kArg3 + 1) && (kArg5 == kArg4 + 1) && (kArg6 == kArg5 + 1) &&
                       (kArg7 == kArg6 + 1), kargs_range_unexpected);
        COMPILE_ASSERT((kFArg1 == kFArg0 + 1) && (kFArg2 == kFArg1 + 1) && (kFArg3 == kFArg2 + 1) &&
                       (kFArg4 == kFArg3 + 1) && (kFArg5 == kFArg4 + 1) && (kFArg6 == kFArg5 + 1) &&
                       (kFArg7 == kFArg6 + 1), kfargs_range_unexpected);
        COMPILE_ASSERT(kRet1 == kRet0 + 1, kret_range_unexpected);
        return RegStorage::MakeRegPair(TargetReg(reg),
                                       TargetReg(static_cast<SpecialTargetRegister>(reg + 1)));
      } else {
        return TargetReg(reg);
      }
    }
    virtual RegStorage TargetPtrReg(SpecialTargetRegister reg) {
      return TargetReg(reg);
    }
    virtual RegStorage TargetReg(SpecialTargetRegister reg, RegLocation loc) {
      if (loc.ref) {
        return TargetReg(reg, kRef);
      } else {
        return TargetReg(reg, loc.wide ? kWide : kNotWide);
      }
    }
    virtual RegStorage GetArgMappingToPhysicalReg(int arg_num) = 0;
    virtual RegLocation GetReturnAlt() = 0;
    virtual RegLocation GetReturnWideAlt() = 0;
    virtual RegLocation LocCReturn() = 0;
    virtual RegLocation LocCReturnRef() = 0;
    virtual RegLocation LocCReturnDouble() = 0;
    virtual RegLocation LocCReturnFloat() = 0;
    virtual RegLocation LocCReturnWide() = 0;
    virtual ResourceMask GetRegMaskCommon(const RegStorage& reg) const = 0;
    virtual void AdjustSpillMask() = 0;
    virtual void ClobberCallerSave() = 0;
    virtual void FreeCallTemps() = 0;
    virtual void LockCallTemps() = 0;
    virtual void CompilerInitializeRegAlloc() = 0;
    virtual void AssembleLIR() = 0;
    virtual void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) = 0;
    virtual void SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                          ResourceMask* use_mask, ResourceMask* def_mask) = 0;
    virtual const char* GetTargetInstFmt(int opcode) = 0;
    virtual const char* GetTargetInstName(int opcode) = 0;
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) = 0;
    virtual ResourceMask GetPCUseDefEncoding() const = 0;
    virtual uint64_t GetTargetInstFlags(int opcode) = 0;
    virtual size_t GetInsnSize(LIR* lir) = 0;
    virtual bool IsUnconditionalBranch(LIR* lir) = 0;
    virtual RegisterClass RegClassForFieldLoadStore(OpSize size, bool is_volatile) = 0;
    virtual void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenMulLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAddLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenAndLong(Instruction::Code,
                            RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenArithOpDouble(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2) = 0;
    virtual void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenConversion(Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) = 0;
    virtual bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object) = 0;
    virtual bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) = 0;
    virtual bool GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double);
    virtual bool GenInlinedSqrt(CallInfo* info) = 0;
    virtual bool GenInlinedPeek(CallInfo* info, OpSize size) = 0;
    virtual bool GenInlinedPoke(CallInfo* info, OpSize size) = 0;
    virtual void GenNotLong(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenNegLong(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenOrLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                           RegLocation rl_src2) = 0;
    virtual void GenSubLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenXorLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2) = 0;
    virtual void GenDivRemLong(Instruction::Code, RegLocation rl_dest, RegLocation rl_src1,
                            RegLocation rl_src2, bool is_div) = 0;
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi,
                                  bool is_div) = 0;
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit,
                                     bool is_div) = 0;
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2, bool is_div, bool check_zero) = 0;
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit,
                                     bool is_div) = 0;
    virtual void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenDivZeroCheckWide(RegStorage reg) = 0;
    virtual void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) = 0;
    virtual void GenExitSequence() = 0;
    virtual void GenFillArrayData(DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double) = 0;
    virtual void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) = 0;
    virtual void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir);
    virtual void GenSelect(BasicBlock* bb, MIR* mir) = 0;
    virtual void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                  int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                  int dest_reg_class) = 0;
    virtual bool GenMemBarrier(MemBarrierKind barrier_kind) = 0;
    virtual void GenMoveException(RegLocation rl_dest) = 0;
    virtual void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) = 0;
    virtual void GenNegDouble(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenNegFloat(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) = 0;
    virtual void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale,
                             bool card_mark) = 0;
    virtual void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift) = 0;
    virtual LIR* OpUnconditionalBranch(LIR* target) = 0;
    virtual LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) = 0;
    virtual LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value,
                                LIR* target) = 0;
    virtual LIR* OpCondBranch(ConditionCode cc, LIR* target) = 0;
    virtual LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) = 0;
    virtual LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpIT(ConditionCode cond, const char* guide) = 0;
    virtual void OpEndIT(LIR* it) = 0;
    virtual LIR* OpMem(OpKind op, RegStorage r_base, int disp) = 0;
    virtual LIR* OpPcRelLoad(RegStorage reg, LIR* target) = 0;
    virtual LIR* OpReg(OpKind op, RegStorage r_dest_src) = 0;
    virtual void OpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value) = 0;
    virtual LIR* OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset) = 0;
    virtual LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) = 0;
    virtual LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset,
                             MoveType move_type) = 0;
    virtual LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src,
                             MoveType move_type) = 0;
    virtual LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) = 0;
    virtual LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1,
                             RegStorage r_src2) = 0;
    virtual LIR* OpTestSuspend(LIR* target) = 0;
    virtual LIR* OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) = 0;
    virtual LIR* OpThreadMem(OpKind op, ThreadOffset<8> thread_offset) = 0;
    virtual LIR* OpVldm(RegStorage r_base, int count) = 0;
    virtual LIR* OpVstm(RegStorage r_base, int count) = 0;
    virtual void OpLea(RegStorage r_base, RegStorage reg1, RegStorage reg2, int scale,
                       int offset) = 0;
    virtual void OpRegCopyWide(RegStorage dest, RegStorage src) = 0;
    virtual void OpTlsCmp(ThreadOffset<4> offset, int val) = 0;
    virtual void OpTlsCmp(ThreadOffset<8> offset, int val) = 0;
    virtual bool InexpensiveConstantInt(int32_t value) = 0;
    virtual bool InexpensiveConstantFloat(int32_t value) = 0;
    virtual bool InexpensiveConstantLong(int64_t value) = 0;
    virtual bool InexpensiveConstantDouble(int64_t value) = 0;
    virtual void GenMonitorEnter(int opt_flags, RegLocation rl_src);
    virtual void GenMonitorExit(int opt_flags, RegLocation rl_src);
    void Workaround7250540(RegLocation rl_dest, RegStorage zero_reg);
  protected:
    Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);
    CompilationUnit* GetCompilationUnit() {
      return cu_;
    }
    int32_t LowestSetBit(uint64_t x);
    bool IsPowerOfTwo(uint64_t x);
    bool BadOverlap(RegLocation rl_op1, RegLocation rl_op2);
    virtual RegLocation ForceTemp(RegLocation loc);
    virtual RegLocation ForceTempWide(RegLocation loc);
    static constexpr OpSize LoadStoreOpSize(bool wide, bool ref) {
      return wide ? k64 : ref ? kReference : k32;
    }
    virtual void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx,
                                    RegLocation rl_dest, RegLocation rl_src);
    void AddSlowPath(LIRSlowPath* slowpath);
    void GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                    bool type_known_abstract, bool use_declaring_class,
                                    bool can_assume_type_is_in_dex_cache,
                                    uint32_t type_idx, RegLocation rl_dest,
                                    RegLocation rl_src);
    virtual std::vector<uint8_t>* ReturnCallFrameInformation();
    void GenPrintLabel(MIR* mir);
    virtual void GenSpecialExitSequence() = 0;
    virtual bool GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special);
  protected:
    void ClobberBody(RegisterInfo* p);
    void SetCurrentDexPc(DexOffset dexpc) {
      current_dalvik_offset_ = dexpc;
    }
    void LockArg(int in_position, bool wide = false);
    RegStorage LoadArg(int in_position, RegisterClass reg_class, bool wide = false);
    void LoadArgDirect(int in_position, RegLocation rl_dest);
    bool GenSpecialIGet(MIR* mir, const InlineMethod& special);
    bool GenSpecialIPut(MIR* mir, const InlineMethod& special);
    bool GenSpecialIdentity(MIR* mir, const InlineMethod& special);
    void AddDivZeroCheckSlowPath(LIR* branch);
    virtual void CopyToArgumentRegs(RegStorage arg0, RegStorage arg1);
    virtual void GenConst(RegLocation rl_dest, int value);
    virtual bool WideGPRsAreAliases() = 0;
    virtual bool WideFPRsAreAliases() = 0;
    enum class WidenessCheck {
      kIgnoreWide,
      kCheckWide,
      kCheckNotWide
    };
    enum class RefCheck {
      kIgnoreRef,
      kCheckRef,
      kCheckNotRef
    };
    enum class FPCheck {
      kIgnoreFP,
      kCheckFP,
      kCheckNotFP
    };
    void CheckRegStorageImpl(RegStorage rs, WidenessCheck wide, RefCheck ref, FPCheck fp, bool fail,
                             bool report)
        const;
    void CheckRegLocationImpl(RegLocation rl, bool fail, bool report) const;
    void CheckRegStorage(RegStorage rs, WidenessCheck wide, RefCheck ref, FPCheck fp) const;
    void CheckRegLocation(RegLocation rl) const;
  public:
    LIR* literal_list_;
    LIR* method_literal_list_;
    LIR* class_literal_list_;
    LIR* code_literal_list_;
    LIR* first_fixup_;
  protected:
    CompilationUnit* const cu_;
    MIRGraph* const mir_graph_;
    GrowableArray<SwitchTable*> switch_tables_;
    GrowableArray<FillArrayData*> fill_array_data_;
    GrowableArray<RegisterInfo*> tempreg_info_;
    GrowableArray<RegisterInfo*> reginfo_map_;
    GrowableArray<void*> pointer_storage_;
    CodeOffset current_code_offset_;
    CodeOffset data_offset_;
    size_t total_size_;
    LIR* block_label_list_;
    PromotionMap* promotion_map_;
    DexOffset current_dalvik_offset_;
    size_t estimated_native_code_size_;
    RegisterPool* reg_pool_;
    int live_sreg_;
    CodeBuffer code_buffer_;
    std::vector<uint8_t> encoded_mapping_table_;
    std::vector<uint32_t> core_vmap_table_;
    std::vector<uint32_t> fp_vmap_table_;
    std::vector<uint8_t> native_gc_map_;
    int num_core_spills_;
    int num_fp_spills_;
    int frame_size_;
    unsigned int core_spill_mask_;
    unsigned int fp_spill_mask_;
    LIR* first_lir_insn_;
    LIR* last_lir_insn_;
    GrowableArray<LIRSlowPath*> slow_paths_;
    ResourceMask::ResourceBit mem_ref_type_;
    ResourceMaskCache mask_cache_;
};
}
#endif
