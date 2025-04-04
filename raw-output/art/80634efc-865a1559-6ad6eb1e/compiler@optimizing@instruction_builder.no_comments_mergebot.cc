#include "instruction_builder.h"
#include "bytecode_utils.h"
#include "class_linker.h"
#include "driver/compiler_options.h"
#include "scoped_thread_state_change.h"
namespace art {
void HInstructionBuilder::MaybeRecordStat(MethodCompilationStat compilation_stat) {
  if (compilation_stats_ != nullptr) {
    compilation_stats_->RecordStat(compilation_stat);
  }
}
HBasicBlock* HInstructionBuilder::FindBlockStartingAt(uint32_t dex_pc) const {
  return block_builder_->GetBlockAt(dex_pc);
}
ArenaVector<HInstruction*>* HInstructionBuilder::GetLocalsFor(HBasicBlock* block) {
  ArenaVector<HInstruction*>* locals = &locals_for_[block->GetBlockId()];
  const size_t vregs = graph_->GetNumberOfVRegs();
  if (locals->size() != vregs) {
    locals->resize(vregs, nullptr);
    if (block->IsCatchBlock()) {
      for (size_t i = 0; i < vregs; ++i) {
        HInstruction* current_local_value = (*current_locals_)[i];
        if (current_local_value != nullptr) {
          HPhi* phi = new (arena_) HPhi(
              arena_,
              i,
              0,
              current_local_value->GetType());
          block->AddPhi(phi);
          (*locals)[i] = phi;
        }
      }
    }
  }
  return locals;
}
HInstruction* HInstructionBuilder::ValueOfLocalAt(HBasicBlock* block, size_t local) {
  ArenaVector<HInstruction*>* locals = GetLocalsFor(block);
  return (*locals)[local];
}
void HInstructionBuilder::InitializeBlockLocals() {
  current_locals_ = GetLocalsFor(current_block_);
  if (current_block_->IsCatchBlock()) {
    if (kIsDebugBuild) {
      bool catch_block_visited = false;
      for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
        HBasicBlock* current = it.Current();
        if (current == current_block_) {
          catch_block_visited = true;
        } else if (current->IsTryBlock()) {
          const HTryBoundary& try_entry = current->GetTryCatchInformation()->GetTryEntry();
          if (try_entry.HasExceptionHandler(*current_block_)) {
            DCHECK(!catch_block_visited) << "Catch block visited before its try block.";
          }
        }
      }
      DCHECK_EQ(current_locals_->size(), graph_->GetNumberOfVRegs())
          << "No instructions throwing into a live catch block.";
    }
  } else if (current_block_->IsLoopHeader()) {
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      HInstruction* incoming =
          ValueOfLocalAt(current_block_->GetLoopInformation()->GetPreHeader(), local);
      if (incoming != nullptr) {
        HPhi* phi = new (arena_) HPhi(
            arena_,
            local,
            0,
            incoming->GetType());
        current_block_->AddPhi(phi);
        (*current_locals_)[local] = phi;
      }
    }
    loop_headers_.push_back(current_block_);
  } else if (current_block_->GetPredecessors().size() > 0) {
    for (size_t local = 0; local < current_locals_->size(); ++local) {
      bool one_predecessor_has_no_value = false;
      bool is_different = false;
      HInstruction* value = ValueOfLocalAt(current_block_->GetPredecessors()[0], local);
      for (HBasicBlock* predecessor : current_block_->GetPredecessors()) {
        HInstruction* current = ValueOfLocalAt(predecessor, local);
        if (current == nullptr) {
          one_predecessor_has_no_value = true;
          break;
        } else if (current != value) {
          is_different = true;
        }
      }
      if (one_predecessor_has_no_value) {
        continue;
      }
      if (is_different) {
        HInstruction* first_input = ValueOfLocalAt(current_block_->GetPredecessors()[0], local);
        HPhi* phi = new (arena_) HPhi(
            arena_,
            local,
            current_block_->GetPredecessors().size(),
            first_input->GetType());
        for (size_t i = 0; i < current_block_->GetPredecessors().size(); i++) {
          HInstruction* pred_value = ValueOfLocalAt(current_block_->GetPredecessors()[i], local);
          phi->SetRawInputAt(i, pred_value);
        }
        current_block_->AddPhi(phi);
        value = phi;
      }
      (*current_locals_)[local] = value;
    }
  }
}
void HInstructionBuilder::PropagateLocalsToCatchBlocks() {
  const HTryBoundary& try_entry = current_block_->GetTryCatchInformation()->GetTryEntry();
  for (HBasicBlock* catch_block : try_entry.GetExceptionHandlers()) {
    ArenaVector<HInstruction*>* handler_locals = GetLocalsFor(catch_block);
    DCHECK_EQ(handler_locals->size(), current_locals_->size());
    for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
      HInstruction* handler_value = (*handler_locals)[vreg];
      if (handler_value == nullptr) {
        continue;
      }
      DCHECK(handler_value->IsPhi());
      HInstruction* local_value = (*current_locals_)[vreg];
      if (local_value == nullptr) {
        catch_block->RemovePhi(handler_value->AsPhi());
        (*handler_locals)[vreg] = nullptr;
      } else {
        handler_value->AsPhi()->AddInput(local_value);
      }
    }
  }
}
void HInstructionBuilder::AppendInstruction(HInstruction* instruction) {
  current_block_->AddInstruction(instruction);
  InitializeInstruction(instruction);
}
void HInstructionBuilder::InsertInstructionAtTop(HInstruction* instruction) {
  if (current_block_->GetInstructions().IsEmpty()) {
    current_block_->AddInstruction(instruction);
  } else {
    current_block_->InsertInstructionBefore(instruction, current_block_->GetFirstInstruction());
  }
  InitializeInstruction(instruction);
}
void HInstructionBuilder::InitializeInstruction(HInstruction* instruction) {
  if (instruction->NeedsEnvironment()) {
    HEnvironment* environment = new (arena_) HEnvironment(
        arena_,
        current_locals_->size(),
        graph_->GetDexFile(),
        graph_->GetMethodIdx(),
        instruction->GetDexPc(),
        graph_->GetInvokeType(),
        instruction);
    environment->CopyFrom(*current_locals_);
    instruction->SetRawEnvironment(environment);
  }
}
void HInstructionBuilder::SetLoopHeaderPhiInputs() {
  for (size_t i = loop_headers_.size(); i > 0; --i) {
    HBasicBlock* block = loop_headers_[i - 1];
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HPhi* phi = it.Current()->AsPhi();
      size_t vreg = phi->GetRegNumber();
      for (HBasicBlock* predecessor : block->GetPredecessors()) {
        HInstruction* value = ValueOfLocalAt(predecessor, vreg);
        if (value == nullptr) {
          phi->SetDead();
          break;
        } else {
          phi->AddInput(value);
        }
      }
    }
  }
}
static bool IsBlockPopulated(HBasicBlock* block) {
  if (block->IsLoopHeader()) {
    DCHECK(block->GetFirstInstruction()->IsSuspendCheck());
    return block->GetFirstInstruction() != block->GetLastInstruction();
  } else {
    return !block->GetInstructions().IsEmpty();
  }
}
bool HInstructionBuilder::Build() {
  locals_for_.resize(graph_->GetBlocks().size(),
                     ArenaVector<HInstruction*>(arena_->Adapter(kArenaAllocGraphBuilder)));
  const bool native_debuggable = compiler_driver_ != nullptr &&
                                 compiler_driver_->GetCompilerOptions().GetNativeDebuggable();
  ArenaBitVector* native_debug_info_locations = nullptr;
  if (native_debuggable) {
    const uint32_t num_instructions = code_item_.insns_size_in_code_units_;
    native_debug_info_locations = new (arena_) ArenaBitVector (arena_, num_instructions, false);
    FindNativeDebugInfoLocations(native_debug_info_locations);
  }
  for (HReversePostOrderIterator block_it(*graph_); !block_it.Done(); block_it.Advance()) {
    current_block_ = block_it.Current();
    uint32_t block_dex_pc = current_block_->GetDexPc();
    InitializeBlockLocals();
    if (current_block_->IsEntryBlock()) {
      InitializeParameters();
      AppendInstruction(new (arena_) HSuspendCheck(0u));
      AppendInstruction(new (arena_) HGoto(0u));
      continue;
    } else if (current_block_->IsExitBlock()) {
      AppendInstruction(new (arena_) HExit());
      continue;
    } else if (current_block_->IsLoopHeader()) {
      HSuspendCheck* suspend_check = new (arena_) HSuspendCheck(current_block_->GetDexPc());
      current_block_->GetLoopInformation()->SetSuspendCheck(suspend_check);
      InsertInstructionAtTop(suspend_check);
    }
    if (block_dex_pc == kNoDexPc || current_block_ != block_builder_->GetBlockAt(block_dex_pc)) {
      DCHECK(IsBlockPopulated(current_block_));
      continue;
    }
    DCHECK(!IsBlockPopulated(current_block_));
    for (CodeItemIterator it(code_item_, block_dex_pc); !it.Done(); it.Advance()) {
      if (current_block_ == nullptr) {
        break;
      }
      uint32_t dex_pc = it.CurrentDexPc();
      if (dex_pc != block_dex_pc && FindBlockStartingAt(dex_pc) != nullptr) {
        break;
      }
      if (current_block_->IsTryBlock() && IsThrowingDexInstruction(it.CurrentInstruction())) {
        PropagateLocalsToCatchBlocks();
      }
      if (native_debuggable && native_debug_info_locations->IsBitSet(dex_pc)) {
        AppendInstruction(new (arena_) HNativeDebugInfo(dex_pc));
      }
      if (!ProcessDexInstruction(it.CurrentInstruction(), dex_pc)) {
        return false;
      }
    }
    if (current_block_ != nullptr) {
      DCHECK_EQ(current_block_->GetSuccessors().size(), 1u);
      AppendInstruction(new (arena_) HGoto());
    }
  }
  SetLoopHeaderPhiInputs();
  return true;
}
void HInstructionBuilder::FindNativeDebugInfoLocations(ArenaBitVector* locations) {
  struct Callback {
    static bool Position(void* ctx, const DexFile::PositionInfo& entry) {
      static_cast<ArenaBitVector*>(ctx)->SetBit(entry.address_);
      return false;
    }
  };
  dex_file_->DecodeDebugPositionInfo(&code_item_, Callback::Position, locations);
  const Instruction* const begin = Instruction::At(code_item_.insns_);
  const Instruction* const end = begin->RelativeAt(code_item_.insns_size_in_code_units_);
  for (const Instruction* inst = begin; inst < end; inst = inst->Next()) {
    switch (inst->Opcode()) {
      case Instruction::MOVE_EXCEPTION: {
        locations->ClearBit(inst->GetDexPc(code_item_.insns_));
        const Instruction* next = inst->Next();
        if (next < end) {
          locations->SetBit(next->GetDexPc(code_item_.insns_));
        }
        break;
      }
      default:
        break;
    }
  }
}
HInstruction* HInstructionBuilder::LoadLocal(uint32_t reg_number, Primitive::Type type) const {
  HInstruction* value = (*current_locals_)[reg_number];
  DCHECK(value != nullptr);
  if (type != value->GetType()) {
    if (Primitive::IsFloatingPointType(type)) {
      return ssa_builder_->GetFloatOrDoubleEquivalent(value, type);
    } else if (type == Primitive::kPrimNot) {
      return ssa_builder_->GetReferenceTypeEquivalent(value);
    }
  }
  return value;
}
void HInstructionBuilder::UpdateLocal(uint32_t reg_number, HInstruction* stored_value) {
  Primitive::Type stored_type = stored_value->GetType();
  DCHECK_NE(stored_type, Primitive::kPrimVoid);
  if (reg_number != 0) {
    HInstruction* local_low = (*current_locals_)[reg_number - 1];
    if (local_low != nullptr && Primitive::Is64BitType(local_low->GetType())) {
      DCHECK((*current_locals_)[reg_number] == nullptr);
      (*current_locals_)[reg_number - 1] = nullptr;
    }
  }
  (*current_locals_)[reg_number] = stored_value;
  if (Primitive::Is64BitType(stored_type)) {
    (*current_locals_)[reg_number + 1] = nullptr;
  }
}
void HInstructionBuilder::InitializeParameters() {
  DCHECK(current_block_->IsEntryBlock());
  if (dex_compilation_unit_ == nullptr) {
    return;
  }
  const char* shorty = dex_compilation_unit_->GetShorty();
  uint16_t number_of_parameters = graph_->GetNumberOfInVRegs();
  uint16_t locals_index = graph_->GetNumberOfLocalVRegs();
  uint16_t parameter_index = 0;
  const DexFile::MethodId& referrer_method_id =
      dex_file_->GetMethodId(dex_compilation_unit_->GetDexMethodIndex());
  if (!dex_compilation_unit_->IsStatic()) {
    HParameterValue* parameter = new (arena_) HParameterValue(*dex_file_,
                                                              referrer_method_id.class_idx_,
                                                              parameter_index++,
                                                              Primitive::kPrimNot,
                                                              true);
    AppendInstruction(parameter);
    UpdateLocal(locals_index++, parameter);
    number_of_parameters--;
  }
  const DexFile::ProtoId& proto = dex_file_->GetMethodPrototype(referrer_method_id);
  const DexFile::TypeList* arg_types = dex_file_->GetProtoParameters(proto);
  for (int i = 0, shorty_pos = 1; i < number_of_parameters; i++) {
    HParameterValue* parameter = new (arena_) HParameterValue(
        *dex_file_,
        arg_types->GetTypeItem(shorty_pos - 1).type_idx_,
        parameter_index++,
        Primitive::GetType(shorty[shorty_pos]),
        false);
    ++shorty_pos;
    AppendInstruction(parameter);
    UpdateLocal(locals_index++, parameter);
    if (Primitive::Is64BitType(parameter->GetType())) {
      i++;
      locals_index++;
      parameter_index++;
    }
  }
}
template<typename T>
void HInstructionBuilder::If_22t(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), Primitive::kPrimInt);
  HInstruction* second = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  T* comparison = new (arena_) T(first, second, dex_pc);
  AppendInstruction(comparison);
  AppendInstruction(new (arena_) HIf(comparison, dex_pc));
  current_block_ = nullptr;
}
template<typename T>
void HInstructionBuilder::If_21t(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt);
  T* comparison = new (arena_) T(value, graph_->GetIntConstant(0, dex_pc), dex_pc);
  AppendInstruction(comparison);
  AppendInstruction(new (arena_) HIf(comparison, dex_pc));
  current_block_ = nullptr;
}
template<typename T>
void HInstructionBuilder::Unop_12x(const Instruction& instruction,
                                   Primitive::Type type,
                                   uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  AppendInstruction(new (arena_) T(type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
void HInstructionBuilder::Conversion_12x(const Instruction& instruction,
                                         Primitive::Type input_type,
                                         Primitive::Type result_type,
                                         uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), input_type);
  AppendInstruction(new (arena_) HTypeConversion(result_type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_23x(const Instruction& instruction,
                                    Primitive::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), type);
  AppendInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_23x_shift(const Instruction& instruction,
                                          Primitive::Type type,
                                          uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), Primitive::kPrimInt);
  AppendInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
void HInstructionBuilder::Binop_23x_cmp(const Instruction& instruction,
                                        Primitive::Type type,
                                        ComparisonBias bias,
                                        uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), type);
  AppendInstruction(new (arena_) HCompare(type, first, second, bias, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_12x_shift(const Instruction& instruction,
                                          Primitive::Type type,
                                          uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type);
  HInstruction* second = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  AppendInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_12x(const Instruction& instruction,
                                    Primitive::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type);
  HInstruction* second = LoadLocal(instruction.VRegB(), type);
  AppendInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_22s(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22s(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  AppendInstruction(new (arena_) T(Primitive::kPrimInt, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
template<typename T>
void HInstructionBuilder::Binop_22b(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22b(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  AppendInstruction(new (arena_) T(Primitive::kPrimInt, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}
static bool RequiresConstructorBarrier(const DexCompilationUnit* cu, CompilerDriver* driver) {
  Thread* self = Thread::Current();
  return cu->IsConstructor()
      && driver->RequiresConstructorBarrier(self, cu->GetDexFile(), cu->GetClassDefIndex());
}
static bool IsFallthroughInstruction(const Instruction& instruction,
                                     uint32_t dex_pc,
                                     HBasicBlock* block) {
  uint32_t next_dex_pc = dex_pc + instruction.SizeInCodeUnits();
  return block->GetSingleSuccessor()->GetDexPc() == next_dex_pc;
}
void HInstructionBuilder::BuildSwitch(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt);
  DexSwitchTable table(instruction, dex_pc);
  if (table.GetNumEntries() == 0) {
    DCHECK(IsFallthroughInstruction(instruction, dex_pc, current_block_));
    AppendInstruction(new (arena_) HGoto(dex_pc));
  } else if (table.ShouldBuildDecisionTree()) {
    for (DexSwitchTableIterator it(table); !it.Done(); it.Advance()) {
      HInstruction* case_value = graph_->GetIntConstant(it.CurrentKey(), dex_pc);
      HEqual* comparison = new (arena_) HEqual(value, case_value, dex_pc);
      AppendInstruction(comparison);
      AppendInstruction(new (arena_) HIf(comparison, dex_pc));
      if (!it.IsLast()) {
        current_block_ = FindBlockStartingAt(it.GetDexPcForCurrentIndex());
      }
    }
  } else {
    AppendInstruction(
        new (arena_) HPackedSwitch(table.GetEntryAt(0), table.GetNumEntries(), value, dex_pc));
  }
  current_block_ = nullptr;
}
void HInstructionBuilder::BuildReturn(const Instruction& instruction,
                                      Primitive::Type type,
                                      uint32_t dex_pc) {
  if (type == Primitive::kPrimVoid) {
    if (graph_->ShouldGenerateConstructorBarrier()) {
      if (dex_compilation_unit_ != nullptr) {
        DCHECK(RequiresConstructorBarrier(dex_compilation_unit_, compiler_driver_))
          << "Inconsistent use of ShouldGenerateConstructorBarrier. Should not generate a barrier.";
      }
      AppendInstruction(new (arena_) HMemoryBarrier(kStoreStore, dex_pc));
    }
    AppendInstruction(new (arena_) HReturnVoid(dex_pc));
  } else {
    HInstruction* value = LoadLocal(instruction.VRegA(), type);
    AppendInstruction(new (arena_) HReturn(value, dex_pc));
  }
  current_block_ = nullptr;
}
static InvokeType GetInvokeTypeFromOpCode(Instruction::Code opcode) {
  switch (opcode) {
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      return kStatic;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      return kDirect;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      return kVirtual;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      return kInterface;
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_SUPER:
      return kSuper;
    default:
      LOG(FATAL) << "Unexpected invoke opcode: " << opcode;
      UNREACHABLE();
  }
}
ArtMethod* HInstructionBuilder::ResolveMethod(uint16_t method_idx, InvokeType invoke_type) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  ClassLinker* class_linker = dex_compilation_unit_->GetClassLinker();
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  Handle<mirror::Class> compiling_class(hs.NewHandle(GetCompilingClass()));
  ArtMethod* resolved_method = class_linker->ResolveMethod<ClassLinker::kForceICCECheck>(
      *dex_compilation_unit_->GetDexFile(),
      method_idx,
      dex_compilation_unit_->GetDexCache(),
      class_loader,
                     nullptr,
      invoke_type);
  if (UNLIKELY(resolved_method == nullptr)) {
    soa.Self()->ClearException();
    return nullptr;
  }
  if (compiling_class.Get() == nullptr) {
    if (!resolved_method->IsPublic()) {
      return nullptr;
    }
  } else if (!compiling_class->CanAccessResolvedMethod(resolved_method->GetDeclaringClass(),
                                                       resolved_method,
                                                       dex_compilation_unit_->GetDexCache().Get(),
                                                       method_idx)) {
    return nullptr;
  }
  if (invoke_type == kSuper) {
    if (compiling_class.Get() == nullptr) {
      DCHECK(Runtime::Current()->IsAotCompiler());
      return nullptr;
    }
    ArtMethod* current_method = graph_->GetArtMethod();
    DCHECK(current_method != nullptr);
    Handle<mirror::Class> methods_class(hs.NewHandle(
        dex_compilation_unit_->GetClassLinker()->ResolveReferencedClassOfMethod(Thread::Current(),
                                                                                method_idx,
                                                                                current_method)));
    if (methods_class.Get() == nullptr) {
      DCHECK(Runtime::Current()->IsAotCompiler());
      return nullptr;
    } else {
      ArtMethod* actual_method;
      if (methods_class->IsInterface()) {
        actual_method = methods_class->FindVirtualMethodForInterfaceSuper(
            resolved_method, class_linker->GetImagePointerSize());
      } else {
        uint16_t vtable_index = resolved_method->GetMethodIndex();
        actual_method = compiling_class->GetSuperClass()->GetVTableEntry(
            vtable_index, class_linker->GetImagePointerSize());
      }
      if (actual_method != resolved_method &&
          !IsSameDexFile(*actual_method->GetDexFile(), *dex_compilation_unit_->GetDexFile())) {
        return nullptr;
      }
      if (!actual_method->IsInvokable()) {
        return nullptr;
      }
      resolved_method = actual_method;
    }
  }
  if (resolved_method->CheckIncompatibleClassChange(invoke_type)) {
    return nullptr;
  }
  return resolved_method;
}
bool HInstructionBuilder::BuildInvoke(const Instruction& instruction,
                                      uint32_t dex_pc,
                                      uint32_t method_idx,
                                      uint32_t number_of_vreg_arguments,
                                      bool is_range,
                                      uint32_t* args,
                                      uint32_t register_index) {
  InvokeType invoke_type = GetInvokeTypeFromOpCode(instruction.Opcode());
  const char* descriptor = dex_file_->GetMethodShorty(method_idx);
  Primitive::Type return_type = Primitive::GetType(descriptor[0]);
  size_t number_of_arguments = strlen(descriptor) - 1;
  if (invoke_type != kStatic) {
    number_of_arguments++;
  }
  MethodReference target_method(dex_file_, method_idx);
  int32_t string_init_offset = 0;
  bool is_string_init = compiler_driver_->IsStringInit(method_idx,
                                                       dex_file_,
                                                       &string_init_offset);
  if (is_string_init) {
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kStringInit,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        dchecked_integral_cast<uint64_t>(string_init_offset),
        0U
    };
    HInvoke* invoke = new (arena_) HInvokeStaticOrDirect(
        arena_,
        number_of_arguments - 1,
        Primitive::kPrimNot ,
        dex_pc,
        method_idx,
        target_method,
        dispatch_info,
        invoke_type,
        kStatic ,
        HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
    return HandleStringInit(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor);
  }
  ArtMethod* resolved_method = ResolveMethod(method_idx, invoke_type);
  if (UNLIKELY(resolved_method == nullptr)) {
    MaybeRecordStat(MethodCompilationStat::kUnresolvedMethod);
    HInvoke* invoke = new (arena_) HInvokeUnresolved(arena_,
                                                     number_of_arguments,
                                                     return_type,
                                                     dex_pc,
                                                     method_idx,
                                                     invoke_type);
    return HandleInvoke(invoke,
                        number_of_vreg_arguments,
                        args,
                        register_index,
                        is_range,
                        descriptor,
                        nullptr );
  }
  HClinitCheck* clinit_check = nullptr;
  HInvoke* invoke = nullptr;
  if (invoke_type == kDirect || invoke_type == kStatic || invoke_type == kSuper) {
    HInvokeStaticOrDirect::ClinitCheckRequirement clinit_check_requirement
        = HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit;
    ScopedObjectAccess soa(Thread::Current());
    if (invoke_type == kStatic) {
      clinit_check = ProcessClinitCheckForInvoke(
          dex_pc, resolved_method, method_idx, &clinit_check_requirement);
    } else if (invoke_type == kSuper) {
      if (IsSameDexFile(*resolved_method->GetDexFile(), *dex_compilation_unit_->GetDexFile())) {
        method_idx = resolved_method->GetDexMethodIndex();
        target_method = MethodReference(dex_file_, method_idx);
      }
    }
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        0u,
        0U
    };
    invoke = new (arena_) HInvokeStaticOrDirect(arena_,
                                                number_of_arguments,
                                                return_type,
                                                dex_pc,
                                                method_idx,
                                                target_method,
                                                dispatch_info,
                                                invoke_type,
                                                invoke_type,
                                                clinit_check_requirement);
  } else if (invoke_type == kVirtual) {
    ScopedObjectAccess soa(Thread::Current());
    invoke = new (arena_) HInvokeVirtual(arena_,
                                         number_of_arguments,
                                         return_type,
                                         dex_pc,
                                         method_idx,
                                         resolved_method->GetMethodIndex());
  } else {
    DCHECK_EQ(invoke_type, kInterface);
    ScopedObjectAccess soa(Thread::Current());
    invoke = new (arena_) HInvokeInterface(arena_,
                                           number_of_arguments,
                                           return_type,
                                           dex_pc,
                                           method_idx,
                                           resolved_method->GetDexMethodIndex());
  }
  return HandleInvoke(invoke,
                      number_of_vreg_arguments,
                      args,
                      register_index,
                      is_range,
                      descriptor,
                      clinit_check);
}
bool HInstructionBuilder::BuildNewInstance(uint16_t type_index, uint32_t dex_pc) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  Handle<mirror::Class> resolved_class(hs.NewHandle(dex_cache->GetResolvedType(type_index)));
  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> outer_dex_cache = outer_compilation_unit_->GetDexCache();
  bool finalizable;
<<<<<<< HEAD
  bool needs_access_check = NeedsAccessCheck(type_index, dex_cache, &finalizable);
||||||| 6ad6eb1e14
  bool can_throw = NeedsAccessCheck(type_index, &finalizable);
=======
  bool can_throw = NeedsAccessCheck(type_index, dex_cache, &finalizable);
>>>>>>> 865a1559
  QuickEntrypointEnum entrypoint = (finalizable || needs_access_check)
      ? kQuickAllocObject
      : kQuickAllocObjectInitialized;
  if (outer_dex_cache.Get() != dex_cache.Get()) {
    return false;
  }
  HLoadClass* load_class = new (arena_) HLoadClass(
      graph_->GetCurrentMethod(),
      type_index,
      outer_dex_file,
      IsOutermostCompilingClass(type_index),
      dex_pc,
<<<<<<< HEAD
      needs_access_check,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_cache, type_index));
||||||| 6ad6eb1e14
                             can_throw,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_file, type_index));
=======
                             can_throw,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_cache, type_index));
>>>>>>> 865a1559
  AppendInstruction(load_class);
  HInstruction* cls = load_class;
  if (!IsInitialized(resolved_class)) {
    cls = new (arena_) HClinitCheck(load_class, dex_pc);
    AppendInstruction(cls);
  }
  AppendInstruction(new (arena_) HNewInstance(
      cls,
      graph_->GetCurrentMethod(),
      dex_pc,
      type_index,
      *dex_compilation_unit_->GetDexFile(),
      needs_access_check,
      finalizable,
      entrypoint));
  return true;
}
static bool IsSubClass(mirror::Class* to_test, mirror::Class* super_class)
    SHARED_REQUIRES(Locks::mutator_lock_) {
    return to_test != nullptr && !to_test->IsInterface() && to_test->IsSubClass(super_class);
    }
bool HInstructionBuilder::IsInitialized(Handle<mirror::Class> cls) const {
  if (cls.Get() == nullptr) {
    return false;
  }
  if (cls->IsInitialized() &&
      compiler_driver_->CanAssumeClassIsLoaded(cls.Get())) {
    return true;
  }
  if (IsSubClass(GetOutermostCompilingClass(), cls.Get())) {
    return true;
  }
  if (IsSubClass(GetCompilingClass(), cls.Get())) {
    return true;
  }
  return false;
}
HClinitCheck* HInstructionBuilder::ProcessClinitCheckForInvoke(
      uint32_t dex_pc,
      ArtMethod* resolved_method,
      uint32_t method_idx,
      HInvokeStaticOrDirect::ClinitCheckRequirement* clinit_check_requirement) {
  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  Handle<mirror::DexCache> outer_dex_cache = outer_compilation_unit_->GetDexCache();
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));
  Handle<mirror::Class> resolved_method_class(hs.NewHandle(resolved_method->GetDeclaringClass()));
  uint32_t storage_index = DexFile::kDexNoIndex;
  bool is_outer_class = (resolved_method->GetDeclaringClass() == outer_class.Get());
  if (is_outer_class) {
    storage_index = outer_class->GetDexTypeIndex();
  } else if (outer_dex_cache.Get() == dex_cache.Get()) {
    compiler_driver_->IsClassOfStaticMethodAvailableToReferrer(outer_dex_cache.Get(),
                                                               GetCompilingClass(),
                                                               resolved_method,
                                                               method_idx,
                                                               &storage_index);
  }
  HClinitCheck* clinit_check = nullptr;
  if (IsInitialized(resolved_method_class)) {
    *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kNone;
  } else if (storage_index != DexFile::kDexNoIndex) {
    *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit;
    HLoadClass* load_class = new (arena_) HLoadClass(
        graph_->GetCurrentMethod(),
        storage_index,
        outer_dex_file,
        is_outer_class,
        dex_pc,
                               false,
        compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_cache, storage_index));
    AppendInstruction(load_class);
    clinit_check = new (arena_) HClinitCheck(load_class, dex_pc);
    AppendInstruction(clinit_check);
  }
  return clinit_check;
}
bool HInstructionBuilder::SetupInvokeArguments(HInvoke* invoke,
                                               uint32_t number_of_vreg_arguments,
                                               uint32_t* args,
                                               uint32_t register_index,
                                               bool is_range,
                                               const char* descriptor,
                                               size_t start_index,
                                               size_t* argument_index) {
  uint32_t descriptor_index = 1;
  for (size_t i = start_index;
       (i < number_of_vreg_arguments) && (*argument_index < invoke->GetNumberOfArguments());
       i++, (*argument_index)++) {
    Primitive::Type type = Primitive::GetType(descriptor[descriptor_index++]);
    bool is_wide = (type == Primitive::kPrimLong) || (type == Primitive::kPrimDouble);
    if (!is_range
        && is_wide
        && ((i + 1 == number_of_vreg_arguments) || (args[i] + 1 != args[i + 1]))) {
      VLOG(compiler) << "Did not compile "
                     << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                     << " because of non-sequential dex register pair in wide argument";
      MaybeRecordStat(MethodCompilationStat::kNotCompiledMalformedOpcode);
      return false;
    }
    HInstruction* arg = LoadLocal(is_range ? register_index + i : args[i], type);
    invoke->SetArgumentAt(*argument_index, arg);
    if (is_wide) {
      i++;
    }
  }
  if (*argument_index != invoke->GetNumberOfArguments()) {
    VLOG(compiler) << "Did not compile "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << " because of wrong number of arguments in invoke instruction";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledMalformedOpcode);
    return false;
  }
  if (invoke->IsInvokeStaticOrDirect() &&
      HInvokeStaticOrDirect::NeedsCurrentMethodInput(
          invoke->AsInvokeStaticOrDirect()->GetMethodLoadKind())) {
    invoke->SetArgumentAt(*argument_index, graph_->GetCurrentMethod());
    (*argument_index)++;
  }
  return true;
}
bool HInstructionBuilder::HandleInvoke(HInvoke* invoke,
                                       uint32_t number_of_vreg_arguments,
                                       uint32_t* args,
                                       uint32_t register_index,
                                       bool is_range,
                                       const char* descriptor,
                                       HClinitCheck* clinit_check) {
  DCHECK(!invoke->IsInvokeStaticOrDirect() || !invoke->AsInvokeStaticOrDirect()->IsStringInit());
  size_t start_index = 0;
  size_t argument_index = 0;
  if (invoke->GetOriginalInvokeType() != InvokeType::kStatic) {
    HInstruction* arg = LoadLocal(is_range ? register_index : args[0], Primitive::kPrimNot);
    HNullCheck* null_check = new (arena_) HNullCheck(arg, invoke->GetDexPc());
    AppendInstruction(null_check);
    invoke->SetArgumentAt(0, null_check);
    start_index = 1;
    argument_index = 1;
  }
  if (!SetupInvokeArguments(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor,
                            start_index,
                            &argument_index)) {
    return false;
  }
  if (clinit_check != nullptr) {
    DCHECK(invoke->IsInvokeStaticOrDirect());
    DCHECK(invoke->AsInvokeStaticOrDirect()->GetClinitCheckRequirement()
        == HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit);
    invoke->SetArgumentAt(argument_index, clinit_check);
    argument_index++;
  }
  AppendInstruction(invoke);
  latest_result_ = invoke;
  return true;
}
bool HInstructionBuilder::HandleStringInit(HInvoke* invoke,
                                           uint32_t number_of_vreg_arguments,
                                           uint32_t* args,
                                           uint32_t register_index,
                                           bool is_range,
                                           const char* descriptor) {
  DCHECK(invoke->IsInvokeStaticOrDirect());
  DCHECK(invoke->AsInvokeStaticOrDirect()->IsStringInit());
  size_t start_index = 1;
  size_t argument_index = 0;
  if (!SetupInvokeArguments(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor,
                            start_index,
                            &argument_index)) {
    return false;
  }
  AppendInstruction(invoke);
  uint32_t orig_this_reg = is_range ? register_index : args[0];
  HInstruction* arg_this = LoadLocal(orig_this_reg, Primitive::kPrimNot);
  if (arg_this->IsNewInstance()) {
    ssa_builder_->AddUninitializedString(arg_this->AsNewInstance());
  } else {
    DCHECK(arg_this->IsPhi());
  }
  for (size_t vreg = 0, e = current_locals_->size(); vreg < e; ++vreg) {
    if ((*current_locals_)[vreg] == arg_this) {
      (*current_locals_)[vreg] = invoke;
    }
  }
  return true;
}
static Primitive::Type GetFieldAccessType(const DexFile& dex_file, uint16_t field_index) {
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_index);
  const char* type = dex_file.GetFieldTypeDescriptor(field_id);
  return Primitive::GetType(type[0]);
}
bool HInstructionBuilder::BuildInstanceFieldAccess(const Instruction& instruction,
                                                   uint32_t dex_pc,
                                                   bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_22c();
  uint32_t obj_reg = instruction.VRegB_22c();
  uint16_t field_index;
  if (instruction.IsQuickened()) {
    if (!CanDecodeQuickenedInfo()) {
      return false;
    }
    field_index = LookupQuickenedInfo(dex_pc);
  } else {
    field_index = instruction.VRegC_22c();
  }
  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field =
      compiler_driver_->ComputeInstanceFieldInfo(field_index, dex_compilation_unit_, is_put, soa);
  HInstruction* object = LoadLocal(obj_reg, Primitive::kPrimNot);
  HInstruction* null_check = new (arena_) HNullCheck(object, dex_pc);
  AppendInstruction(null_check);
  Primitive::Type field_type = (resolved_field == nullptr)
      ? GetFieldAccessType(*dex_file_, field_index)
      : resolved_field->GetTypeAsPrimitiveType();
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    HInstruction* field_set = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
      field_set = new (arena_) HUnresolvedInstanceFieldSet(null_check,
                                                           value,
                                                           field_type,
                                                           field_index,
                                                           dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_set = new (arena_) HInstanceFieldSet(null_check,
                                                 value,
                                                 field_type,
                                                 resolved_field->GetOffset(),
                                                 resolved_field->IsVolatile(),
                                                 field_index,
                                                 class_def_index,
                                                 *dex_file_,
                                                 dex_compilation_unit_->GetDexCache(),
                                                 dex_pc);
    }
    AppendInstruction(field_set);
  } else {
    HInstruction* field_get = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
      field_get = new (arena_) HUnresolvedInstanceFieldGet(null_check,
                                                           field_type,
                                                           field_index,
                                                           dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_get = new (arena_) HInstanceFieldGet(null_check,
                                                 field_type,
                                                 resolved_field->GetOffset(),
                                                 resolved_field->IsVolatile(),
                                                 field_index,
                                                 class_def_index,
                                                 *dex_file_,
                                                 dex_compilation_unit_->GetDexCache(),
                                                 dex_pc);
    }
    AppendInstruction(field_get);
    UpdateLocal(source_or_dest_reg, field_get);
  }
  return true;
}
static mirror::Class* GetClassFrom(CompilerDriver* driver,
                                   const DexCompilationUnit& compilation_unit) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(compilation_unit.GetClassLoader())));
  Handle<mirror::DexCache> dex_cache = compilation_unit.GetDexCache();
  return driver->ResolveCompilingMethodsClass(soa, dex_cache, class_loader, &compilation_unit);
}
mirror::Class* HInstructionBuilder::GetOutermostCompilingClass() const {
  return GetClassFrom(compiler_driver_, *outer_compilation_unit_);
}
mirror::Class* HInstructionBuilder::GetCompilingClass() const {
  return GetClassFrom(compiler_driver_, *dex_compilation_unit_);
}
bool HInstructionBuilder::IsOutermostCompilingClass(uint16_t type_index) const {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  Handle<mirror::Class> cls(hs.NewHandle(compiler_driver_->ResolveClass(
      soa, dex_cache, class_loader, type_index, dex_compilation_unit_)));
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));
  return (cls.Get() != nullptr) && (outer_class.Get() == cls.Get());
}
void HInstructionBuilder::BuildUnresolvedStaticFieldAccess(const Instruction& instruction,
                                                     uint32_t dex_pc,
                                                     bool is_put,
                                                     Primitive::Type field_type) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    AppendInstruction(
        new (arena_) HUnresolvedStaticFieldSet(value, field_type, field_index, dex_pc));
  } else {
    AppendInstruction(new (arena_) HUnresolvedStaticFieldGet(field_type, field_index, dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
}
bool HInstructionBuilder::BuildStaticFieldAccess(const Instruction& instruction,
                                                 uint32_t dex_pc,
                                                 bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  ArtField* resolved_field = compiler_driver_->ResolveField(
      soa, dex_cache, class_loader, dex_compilation_unit_, field_index, true);
  if (resolved_field == nullptr) {
    MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
    Primitive::Type field_type = GetFieldAccessType(*dex_file_, field_index);
    BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
    return true;
  }
  Primitive::Type field_type = resolved_field->GetTypeAsPrimitiveType();
  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> outer_dex_cache = outer_compilation_unit_->GetDexCache();
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));
  uint32_t storage_index;
  bool is_outer_class = (outer_class.Get() == resolved_field->GetDeclaringClass());
  if (is_outer_class) {
    storage_index = outer_class->GetDexTypeIndex();
  } else if (outer_dex_cache.Get() != dex_cache.Get()) {
    return false;
  } else {
    std::pair<bool, bool> pair = compiler_driver_->IsFastStaticField(
        outer_dex_cache.Get(),
        GetCompilingClass(),
        resolved_field,
        field_index,
        &storage_index);
    bool can_easily_access = is_put ? pair.second : pair.first;
    if (!can_easily_access) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedFieldNotAFastAccess);
      BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
      return true;
    }
  }
  bool is_in_cache =
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_cache, storage_index);
  HLoadClass* constant = new (arena_) HLoadClass(graph_->GetCurrentMethod(),
                                                 storage_index,
                                                 outer_dex_file,
                                                 is_outer_class,
                                                 dex_pc,
                                                                        false,
                                                 is_in_cache);
  AppendInstruction(constant);
  HInstruction* cls = constant;
  Handle<mirror::Class> klass(hs.NewHandle(resolved_field->GetDeclaringClass()));
  if (!IsInitialized(klass)) {
    cls = new (arena_) HClinitCheck(constant, dex_pc);
    AppendInstruction(cls);
  }
  uint16_t class_def_index = klass->GetDexClassDefIndex();
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    DCHECK_EQ(HPhi::ToPhiType(value->GetType()), HPhi::ToPhiType(field_type));
    AppendInstruction(new (arena_) HStaticFieldSet(cls,
                                                   value,
                                                   field_type,
                                                   resolved_field->GetOffset(),
                                                   resolved_field->IsVolatile(),
                                                   field_index,
                                                   class_def_index,
                                                   *dex_file_,
                                                   dex_cache_,
                                                   dex_pc));
  } else {
    AppendInstruction(new (arena_) HStaticFieldGet(cls,
                                                   field_type,
                                                   resolved_field->GetOffset(),
                                                   resolved_field->IsVolatile(),
                                                   field_index,
                                                   class_def_index,
                                                   *dex_file_,
                                                   dex_cache_,
                                                   dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
  return true;
}
void HInstructionBuilder::BuildCheckedDivRem(uint16_t out_vreg,
                                       uint16_t first_vreg,
                                       int64_t second_vreg_or_constant,
                                       uint32_t dex_pc,
                                       Primitive::Type type,
                                       bool second_is_constant,
                                       bool isDiv) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);
  HInstruction* first = LoadLocal(first_vreg, type);
  HInstruction* second = nullptr;
  if (second_is_constant) {
    if (type == Primitive::kPrimInt) {
      second = graph_->GetIntConstant(second_vreg_or_constant, dex_pc);
    } else {
      second = graph_->GetLongConstant(second_vreg_or_constant, dex_pc);
    }
  } else {
    second = LoadLocal(second_vreg_or_constant, type);
  }
  if (!second_is_constant
      || (type == Primitive::kPrimInt && second->AsIntConstant()->GetValue() == 0)
      || (type == Primitive::kPrimLong && second->AsLongConstant()->GetValue() == 0)) {
    second = new (arena_) HDivZeroCheck(second, dex_pc);
    AppendInstruction(second);
  }
  if (isDiv) {
    AppendInstruction(new (arena_) HDiv(type, first, second, dex_pc));
  } else {
    AppendInstruction(new (arena_) HRem(type, first, second, dex_pc));
  }
  UpdateLocal(out_vreg, current_block_->GetLastInstruction());
}
void HInstructionBuilder::BuildArrayAccess(const Instruction& instruction,
                                           uint32_t dex_pc,
                                           bool is_put,
                                           Primitive::Type anticipated_type) {
  uint8_t source_or_dest_reg = instruction.VRegA_23x();
  uint8_t array_reg = instruction.VRegB_23x();
  uint8_t index_reg = instruction.VRegC_23x();
  HInstruction* object = LoadLocal(array_reg, Primitive::kPrimNot);
  object = new (arena_) HNullCheck(object, dex_pc);
  AppendInstruction(object);
  HInstruction* length = new (arena_) HArrayLength(object, dex_pc);
  AppendInstruction(length);
  HInstruction* index = LoadLocal(index_reg, Primitive::kPrimInt);
  index = new (arena_) HBoundsCheck(index, length, dex_pc);
  AppendInstruction(index);
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, anticipated_type);
    HArraySet* aset = new (arena_) HArraySet(object, index, value, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  } else {
    HArrayGet* aget = new (arena_) HArrayGet(object, index, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArrayGet(aget);
    AppendInstruction(aget);
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
  graph_->SetHasBoundsChecks(true);
}
void HInstructionBuilder::BuildFilledNewArray(uint32_t dex_pc,
                                              uint32_t type_index,
                                              uint32_t number_of_vreg_arguments,
                                              bool is_range,
                                              uint32_t* args,
                                              uint32_t register_index) {
  HInstruction* length = graph_->GetIntConstant(number_of_vreg_arguments, dex_pc);
  bool finalizable;
  QuickEntrypointEnum entrypoint = NeedsAccessCheck(type_index, &finalizable)
      ? kQuickAllocArrayWithAccessCheck
      : kQuickAllocArray;
  HInstruction* object = new (arena_) HNewArray(length,
                                                graph_->GetCurrentMethod(),
                                                dex_pc,
                                                type_index,
                                                *dex_compilation_unit_->GetDexFile(),
                                                entrypoint);
  AppendInstruction(object);
  const char* descriptor = dex_file_->StringByTypeIdx(type_index);
  DCHECK_EQ(descriptor[0], '[') << descriptor;
  char primitive = descriptor[1];
  DCHECK(primitive == 'I'
      || primitive == 'L'
      || primitive == '[') << descriptor;
  bool is_reference_array = (primitive == 'L') || (primitive == '[');
  Primitive::Type type = is_reference_array ? Primitive::kPrimNot : Primitive::kPrimInt;
  for (size_t i = 0; i < number_of_vreg_arguments; ++i) {
    HInstruction* value = LoadLocal(is_range ? register_index + i : args[i], type);
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HArraySet* aset = new (arena_) HArraySet(object, index, value, type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
  latest_result_ = object;
}
template <typename T>
void HInstructionBuilder::BuildFillArrayData(HInstruction* object,
                                             const T* data,
                                             uint32_t element_count,
                                             Primitive::Type anticipated_type,
                                             uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetIntConstant(data[i], dex_pc);
    HArraySet* aset = new (arena_) HArraySet(object, index, value, anticipated_type, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
}
void HInstructionBuilder::BuildFillArrayData(const Instruction& instruction, uint32_t dex_pc) {
  HInstruction* array = LoadLocal(instruction.VRegA_31t(), Primitive::kPrimNot);
  HNullCheck* null_check = new (arena_) HNullCheck(array, dex_pc);
  AppendInstruction(null_check);
  HInstruction* length = new (arena_) HArrayLength(null_check, dex_pc);
  AppendInstruction(length);
  int32_t payload_offset = instruction.VRegB_31t() + dex_pc;
  const Instruction::ArrayDataPayload* payload =
      reinterpret_cast<const Instruction::ArrayDataPayload*>(code_item_.insns_ + payload_offset);
  const uint8_t* data = payload->data;
  uint32_t element_count = payload->element_count;
  HInstruction* last_index = graph_->GetIntConstant(payload->element_count - 1, dex_pc);
  AppendInstruction(new (arena_) HBoundsCheck(last_index, length, dex_pc));
  switch (payload->element_width) {
    case 1:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int8_t*>(data),
                         element_count,
                         Primitive::kPrimByte,
                         dex_pc);
      break;
    case 2:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int16_t*>(data),
                         element_count,
                         Primitive::kPrimShort,
                         dex_pc);
      break;
    case 4:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int32_t*>(data),
                         element_count,
                         Primitive::kPrimInt,
                         dex_pc);
      break;
    case 8:
      BuildFillWideArrayData(null_check,
                             reinterpret_cast<const int64_t*>(data),
                             element_count,
                             dex_pc);
      break;
    default:
      LOG(FATAL) << "Unknown element width for " << payload->element_width;
  }
  graph_->SetHasBoundsChecks(true);
}
void HInstructionBuilder::BuildFillWideArrayData(HInstruction* object,
                                                 const int64_t* data,
                                                 uint32_t element_count,
                                                 uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetLongConstant(data[i], dex_pc);
    HArraySet* aset = new (arena_) HArraySet(object, index, value, Primitive::kPrimLong, dex_pc);
    ssa_builder_->MaybeAddAmbiguousArraySet(aset);
    AppendInstruction(aset);
  }
}
static TypeCheckKind ComputeTypeCheckKind(Handle<mirror::Class> cls)
    SHARED_REQUIRES(Locks::mutator_lock_) {
    if (cls.Get() == nullptr) {
    return TypeCheckKind::kUnresolvedCheck;
    } else if (cls->IsInterface()) {
    return TypeCheckKind::kInterfaceCheck;
    } else if (cls->IsArrayClass()) {
    if (cls->GetComponentType()->IsObjectClass()) {
      return TypeCheckKind::kArrayObjectCheck;
    } else if (cls->CannotBeAssignedFromOtherTypes()) {
      return TypeCheckKind::kExactCheck;
    } else {
      return TypeCheckKind::kArrayCheck;
    }
    } else if (cls->IsFinal()) {
    return TypeCheckKind::kExactCheck;
    } else if (cls->IsAbstract()) {
    return TypeCheckKind::kAbstractClassCheck;
    } else {
    return TypeCheckKind::kClassHierarchyCheck;
    }
    }
void HInstructionBuilder::BuildTypeCheck(const Instruction& instruction,
                                         uint8_t destination,
                                         uint8_t reference,
                                         uint16_t type_index,
                                         uint32_t dex_pc) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  Handle<mirror::Class> resolved_class(hs.NewHandle(dex_cache->GetResolvedType(type_index)));
  bool can_access = compiler_driver_->CanAccessTypeWithoutChecks(
      dex_compilation_unit_->GetDexMethodIndex(),
      dex_cache,
      type_index);
  HInstruction* object = LoadLocal(reference, Primitive::kPrimNot);
  HLoadClass* cls = new (arena_) HLoadClass(
      graph_->GetCurrentMethod(),
      type_index,
      dex_file,
      IsOutermostCompilingClass(type_index),
      dex_pc,
      !can_access,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(dex_cache, type_index));
  AppendInstruction(cls);
  TypeCheckKind check_kind = ComputeTypeCheckKind(resolved_class);
  if (instruction.Opcode() == Instruction::INSTANCE_OF) {
    AppendInstruction(new (arena_) HInstanceOf(object, cls, check_kind, dex_pc));
    UpdateLocal(destination, current_block_->GetLastInstruction());
  } else {
    DCHECK_EQ(instruction.Opcode(), Instruction::CHECK_CAST);
    AppendInstruction(new (arena_) HCheckCast(object, cls, check_kind, dex_pc));
    AppendInstruction(new (arena_) HBoundType(object, dex_pc));
    UpdateLocal(reference, current_block_->GetLastInstruction());
  }
}
bool HInstructionBuilder::NeedsAccessCheck(uint32_t type_index, Handle<mirror::DexCache> dex_cache, bool* finalizable) const {
  return !compiler_driver_->CanAccessInstantiableTypeWithoutChecks(
      dex_compilation_unit_->GetDexMethodIndex(), dex_cache, type_index, finalizable);
}
bool HInstructionBuilder::NeedsAccessCheck(uint32_t type_index, bool* finalizable) const {
  ScopedObjectAccess soa(Thread::Current());
  Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
  return NeedsAccessCheck(type_index, dex_cache, finalizable);
}
bool HInstructionBuilder::CanDecodeQuickenedInfo() const {
  return interpreter_metadata_ != nullptr;
}
uint16_t HInstructionBuilder::LookupQuickenedInfo(uint32_t dex_pc) {
  DCHECK(interpreter_metadata_ != nullptr);
  auto it = skipped_interpreter_metadata_.find(dex_pc);
  if (it != skipped_interpreter_metadata_.end()) {
    uint16_t value_in_map = it->second;
    skipped_interpreter_metadata_.erase(it);
    return value_in_map;
  }
  while (true) {
    uint32_t dex_pc_in_map = DecodeUnsignedLeb128(&interpreter_metadata_);
    uint16_t value_in_map = DecodeUnsignedLeb128(&interpreter_metadata_);
    DCHECK_LE(dex_pc_in_map, dex_pc);
    if (dex_pc_in_map == dex_pc) {
      return value_in_map;
    } else {
      skipped_interpreter_metadata_.Put(dex_pc_in_map, value_in_map);
    }
  }
}
bool HInstructionBuilder::ProcessDexInstruction(const Instruction& instruction, uint32_t dex_pc) {
  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_11n(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21s(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_31i(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_HIGH16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21h() << 16, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_WIDE_16: {
      int32_t register_index = instruction.VRegA();
      int64_t value = instruction.VRegB_21s();
      value <<= 48;
      value >>= 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_WIDE_32: {
      int32_t register_index = instruction.VRegA();
      int64_t value = instruction.VRegB_31i();
      value <<= 32;
      value >>= 32;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_WIDE: {
      int32_t register_index = instruction.VRegA();
      HLongConstant* constant = graph_->GetLongConstant(instruction.VRegB_51l(), dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::CONST_WIDE_HIGH16: {
      int32_t register_index = instruction.VRegA();
      int64_t value = static_cast<int64_t>(instruction.VRegB_21h()) << 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant);
      break;
    }
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimLong);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_OBJECT_FROM16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimNot);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }
    case Instruction::RETURN_VOID_NO_BARRIER:
    case Instruction::RETURN_VOID: {
      BuildReturn(instruction, Primitive::kPrimVoid, dex_pc);
      break;
    }
#define IF_XX(comparison,cond) \
    case Instruction::IF_##cond: If_22t<comparison>(instruction, dex_pc); break; \
    case Instruction::IF_##cond##Z: If_21t<comparison>(instruction, dex_pc); break
    IF_XX(HEqual, EQ);
    IF_XX(HNotEqual, NE);
    IF_XX(HLessThan, LT);
    IF_XX(HLessThanOrEqual, LE);
    IF_XX(HGreaterThan, GT);
    IF_XX(HGreaterThanOrEqual, GE);
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      AppendInstruction(new (arena_) HGoto(dex_pc));
      current_block_ = nullptr;
      break;
    }
    case Instruction::RETURN: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }
    case Instruction::RETURN_OBJECT: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }
    case Instruction::RETURN_WIDE: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          return false;
        }
        method_idx = LookupQuickenedInfo(dex_pc);
      } else {
        method_idx = instruction.VRegB_35c();
      }
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      if (!BuildInvoke(instruction, dex_pc, method_idx,
                       number_of_vreg_arguments, false, args, -1)) {
        return false;
      }
      break;
    }
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          return false;
        }
        method_idx = LookupQuickenedInfo(dex_pc);
      } else {
        method_idx = instruction.VRegB_3rc();
      }
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t register_index = instruction.VRegC();
      if (!BuildInvoke(instruction, dex_pc, method_idx,
                       number_of_vreg_arguments, true, nullptr, register_index)) {
        return false;
      }
      break;
    }
    case Instruction::NEG_INT: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::NEG_LONG: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::NEG_FLOAT: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::NEG_DOUBLE: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::NOT_INT: {
      Unop_12x<HNot>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::NOT_LONG: {
      Unop_12x<HNot>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::INT_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::INT_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::INT_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::LONG_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::LONG_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::LONG_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::FLOAT_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::FLOAT_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::FLOAT_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::DOUBLE_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::DOUBLE_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::DOUBLE_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::INT_TO_BYTE: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimByte, dex_pc);
      break;
    }
    case Instruction::INT_TO_SHORT: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimShort, dex_pc);
      break;
    }
    case Instruction::INT_TO_CHAR: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimChar, dex_pc);
      break;
    }
    case Instruction::ADD_INT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::ADD_LONG: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::ADD_DOUBLE: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::ADD_FLOAT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::SUB_INT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SUB_LONG: {
      Binop_23x<HSub>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::SUB_FLOAT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::SUB_DOUBLE: {
      Binop_23x<HSub>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::ADD_INT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::MUL_INT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::MUL_LONG: {
      Binop_23x<HMul>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::MUL_FLOAT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::MUL_DOUBLE: {
      Binop_23x<HMul>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::DIV_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, false, true);
      break;
    }
    case Instruction::DIV_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimLong, false, true);
      break;
    }
    case Instruction::DIV_FLOAT: {
      Binop_23x<HDiv>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::DIV_DOUBLE: {
      Binop_23x<HDiv>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::REM_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, false, false);
      break;
    }
    case Instruction::REM_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimLong, false, false);
      break;
    }
    case Instruction::REM_FLOAT: {
      Binop_23x<HRem>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::REM_DOUBLE: {
      Binop_23x<HRem>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::AND_INT: {
      Binop_23x<HAnd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::AND_LONG: {
      Binop_23x<HAnd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::SHL_INT: {
      Binop_23x_shift<HShl>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SHL_LONG: {
      Binop_23x_shift<HShl>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::SHR_INT: {
      Binop_23x_shift<HShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SHR_LONG: {
      Binop_23x_shift<HShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::USHR_INT: {
      Binop_23x_shift<HUShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::USHR_LONG: {
      Binop_23x_shift<HUShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::OR_INT: {
      Binop_23x<HOr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::OR_LONG: {
      Binop_23x<HOr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::XOR_INT: {
      Binop_23x<HXor>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::XOR_LONG: {
      Binop_23x<HXor>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::ADD_LONG_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::ADD_DOUBLE_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::ADD_FLOAT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::SUB_INT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SUB_LONG_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::SUB_FLOAT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::SUB_DOUBLE_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::MUL_INT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::MUL_LONG_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::MUL_FLOAT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::MUL_DOUBLE_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::DIV_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimInt, false, true);
      break;
    }
    case Instruction::DIV_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimLong, false, true);
      break;
    }
    case Instruction::REM_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimInt, false, false);
      break;
    }
    case Instruction::REM_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimLong, false, false);
      break;
    }
    case Instruction::REM_FLOAT_2ADDR: {
      Binop_12x<HRem>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::REM_DOUBLE_2ADDR: {
      Binop_12x<HRem>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::SHL_INT_2ADDR: {
      Binop_12x_shift<HShl>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SHL_LONG_2ADDR: {
      Binop_12x_shift<HShl>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::SHR_INT_2ADDR: {
      Binop_12x_shift<HShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::SHR_LONG_2ADDR: {
      Binop_12x_shift<HShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::USHR_INT_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::USHR_LONG_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::DIV_FLOAT_2ADDR: {
      Binop_12x<HDiv>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }
    case Instruction::DIV_DOUBLE_2ADDR: {
      Binop_12x<HDiv>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }
    case Instruction::AND_INT_2ADDR: {
      Binop_12x<HAnd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::AND_LONG_2ADDR: {
      Binop_12x<HAnd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::OR_INT_2ADDR: {
      Binop_12x<HOr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::OR_LONG_2ADDR: {
      Binop_12x<HOr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::XOR_INT_2ADDR: {
      Binop_12x<HXor>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }
    case Instruction::XOR_LONG_2ADDR: {
      Binop_12x<HXor>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }
    case Instruction::ADD_INT_LIT16: {
      Binop_22s<HAdd>(instruction, false, dex_pc);
      break;
    }
    case Instruction::AND_INT_LIT16: {
      Binop_22s<HAnd>(instruction, false, dex_pc);
      break;
    }
    case Instruction::OR_INT_LIT16: {
      Binop_22s<HOr>(instruction, false, dex_pc);
      break;
    }
    case Instruction::XOR_INT_LIT16: {
      Binop_22s<HXor>(instruction, false, dex_pc);
      break;
    }
    case Instruction::RSUB_INT: {
      Binop_22s<HSub>(instruction, true, dex_pc);
      break;
    }
    case Instruction::MUL_INT_LIT16: {
      Binop_22s<HMul>(instruction, false, dex_pc);
      break;
    }
    case Instruction::ADD_INT_LIT8: {
      Binop_22b<HAdd>(instruction, false, dex_pc);
      break;
    }
    case Instruction::AND_INT_LIT8: {
      Binop_22b<HAnd>(instruction, false, dex_pc);
      break;
    }
    case Instruction::OR_INT_LIT8: {
      Binop_22b<HOr>(instruction, false, dex_pc);
      break;
    }
    case Instruction::XOR_INT_LIT8: {
      Binop_22b<HXor>(instruction, false, dex_pc);
      break;
    }
    case Instruction::RSUB_INT_LIT8: {
      Binop_22b<HSub>(instruction, true, dex_pc);
      break;
    }
    case Instruction::MUL_INT_LIT8: {
      Binop_22b<HMul>(instruction, false, dex_pc);
      break;
    }
    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, true, true);
      break;
    }
    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, true, false);
      break;
    }
    case Instruction::SHL_INT_LIT8: {
      Binop_22b<HShl>(instruction, false, dex_pc);
      break;
    }
    case Instruction::SHR_INT_LIT8: {
      Binop_22b<HShr>(instruction, false, dex_pc);
      break;
    }
    case Instruction::USHR_INT_LIT8: {
      Binop_22b<HUShr>(instruction, false, dex_pc);
      break;
    }
    case Instruction::NEW_INSTANCE: {
      if (!BuildNewInstance(instruction.VRegB_21c(), dex_pc)) {
        return false;
      }
      UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::NEW_ARRAY: {
      uint16_t type_index = instruction.VRegC_22c();
      HInstruction* length = LoadLocal(instruction.VRegB_22c(), Primitive::kPrimInt);
      bool finalizable;
      QuickEntrypointEnum entrypoint = NeedsAccessCheck(type_index, &finalizable)
          ? kQuickAllocArrayWithAccessCheck
          : kQuickAllocArray;
      AppendInstruction(new (arena_) HNewArray(length,
                                               graph_->GetCurrentMethod(),
                                               dex_pc,
                                               type_index,
                                               *dex_compilation_unit_->GetDexFile(),
                                               entrypoint));
      UpdateLocal(instruction.VRegA_22c(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::FILLED_NEW_ARRAY: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t type_index = instruction.VRegB_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      BuildFilledNewArray(dex_pc, type_index, number_of_vreg_arguments, false, args, 0);
      break;
    }
    case Instruction::FILLED_NEW_ARRAY_RANGE: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t type_index = instruction.VRegB_3rc();
      uint32_t register_index = instruction.VRegC_3rc();
      BuildFilledNewArray(
          dex_pc, type_index, number_of_vreg_arguments, true, nullptr, register_index);
      break;
    }
    case Instruction::FILL_ARRAY_DATA: {
      BuildFillArrayData(instruction, dex_pc);
      break;
    }
    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT: {
      DCHECK(latest_result_ != nullptr);
      UpdateLocal(instruction.VRegA(), latest_result_);
      latest_result_ = nullptr;
      break;
    }
    case Instruction::CMP_LONG: {
      Binop_23x_cmp(instruction, Primitive::kPrimLong, ComparisonBias::kNoBias, dex_pc);
      break;
    }
    case Instruction::CMPG_FLOAT: {
      Binop_23x_cmp(instruction, Primitive::kPrimFloat, ComparisonBias::kGtBias, dex_pc);
      break;
    }
    case Instruction::CMPG_DOUBLE: {
      Binop_23x_cmp(instruction, Primitive::kPrimDouble, ComparisonBias::kGtBias, dex_pc);
      break;
    }
    case Instruction::CMPL_FLOAT: {
      Binop_23x_cmp(instruction, Primitive::kPrimFloat, ComparisonBias::kLtBias, dex_pc);
      break;
    }
    case Instruction::CMPL_DOUBLE: {
      Binop_23x_cmp(instruction, Primitive::kPrimDouble, ComparisonBias::kLtBias, dex_pc);
      break;
    }
    case Instruction::NOP:
      break;
    case Instruction::IGET:
    case Instruction::IGET_QUICK:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_WIDE_QUICK:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_OBJECT_QUICK:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BOOLEAN_QUICK:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_BYTE_QUICK:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_CHAR_QUICK:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, false)) {
        return false;
      }
      break;
    }
    case Instruction::IPUT:
    case Instruction::IPUT_QUICK:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_WIDE_QUICK:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_BYTE_QUICK:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_CHAR_QUICK:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, true)) {
        return false;
      }
      break;
    }
    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT: {
      if (!BuildStaticFieldAccess(instruction, dex_pc, false)) {
        return false;
      }
      break;
    }
    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
      if (!BuildStaticFieldAccess(instruction, dex_pc, true)) {
        return false;
      }
      break;
    }
#define ARRAY_XX(kind,anticipated_type) \
    case Instruction::AGET##kind: { \
      BuildArrayAccess(instruction, dex_pc, false, anticipated_type); \
      break; \
    } \
    case Instruction::APUT##kind: { \
      BuildArrayAccess(instruction, dex_pc, true, anticipated_type); \
      break; \
    }
    ARRAY_XX(, Primitive::kPrimInt);
    ARRAY_XX(_WIDE, Primitive::kPrimLong);
    ARRAY_XX(_OBJECT, Primitive::kPrimNot);
    ARRAY_XX(_BOOLEAN, Primitive::kPrimBoolean);
    ARRAY_XX(_BYTE, Primitive::kPrimByte);
    ARRAY_XX(_CHAR, Primitive::kPrimChar);
    ARRAY_XX(_SHORT, Primitive::kPrimShort);
    case Instruction::ARRAY_LENGTH: {
      HInstruction* object = LoadLocal(instruction.VRegB_12x(), Primitive::kPrimNot);
      object = new (arena_) HNullCheck(object, dex_pc);
      AppendInstruction(object);
      AppendInstruction(new (arena_) HArrayLength(object, dex_pc));
      UpdateLocal(instruction.VRegA_12x(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::CONST_STRING: {
      uint32_t string_index = instruction.VRegB_21c();
      AppendInstruction(
          new (arena_) HLoadString(graph_->GetCurrentMethod(), string_index, *dex_file_, dex_pc));
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::CONST_STRING_JUMBO: {
      uint32_t string_index = instruction.VRegB_31c();
      AppendInstruction(
          new (arena_) HLoadString(graph_->GetCurrentMethod(), string_index, *dex_file_, dex_pc));
      UpdateLocal(instruction.VRegA_31c(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::CONST_CLASS: {
      uint16_t type_index = instruction.VRegB_21c();
      ScopedObjectAccess soa(Thread::Current());
      Handle<mirror::DexCache> dex_cache = dex_compilation_unit_->GetDexCache();
      bool can_access = compiler_driver_->CanAccessTypeWithoutChecks(
          dex_compilation_unit_->GetDexMethodIndex(), dex_cache, type_index);
      bool is_in_dex_cache =
          compiler_driver_->CanAssumeTypeIsPresentInDexCache(dex_cache, type_index);
      AppendInstruction(new (arena_) HLoadClass(
          graph_->GetCurrentMethod(),
          type_index,
          *dex_file_,
          IsOutermostCompilingClass(type_index),
          dex_pc,
          !can_access,
          is_in_dex_cache));
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction());
      break;
    }
    case Instruction::MOVE_EXCEPTION: {
      AppendInstruction(new (arena_) HLoadException(dex_pc));
      UpdateLocal(instruction.VRegA_11x(), current_block_->GetLastInstruction());
      AppendInstruction(new (arena_) HClearException(dex_pc));
      break;
    }
    case Instruction::THROW: {
      HInstruction* exception = LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot);
      AppendInstruction(new (arena_) HThrow(exception, dex_pc));
      current_block_ = nullptr;
      break;
    }
    case Instruction::INSTANCE_OF: {
      uint8_t destination = instruction.VRegA_22c();
      uint8_t reference = instruction.VRegB_22c();
      uint16_t type_index = instruction.VRegC_22c();
      BuildTypeCheck(instruction, destination, reference, type_index, dex_pc);
      break;
    }
    case Instruction::CHECK_CAST: {
      uint8_t reference = instruction.VRegA_21c();
      uint16_t type_index = instruction.VRegB_21c();
      BuildTypeCheck(instruction, -1, reference, type_index, dex_pc);
      break;
    }
    case Instruction::MONITOR_ENTER: {
      AppendInstruction(new (arena_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot),
          HMonitorOperation::OperationKind::kEnter,
          dex_pc));
      break;
    }
    case Instruction::MONITOR_EXIT: {
      AppendInstruction(new (arena_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot),
          HMonitorOperation::OperationKind::kExit,
          dex_pc));
      break;
    }
    case Instruction::SPARSE_SWITCH:
    case Instruction::PACKED_SWITCH: {
      BuildSwitch(instruction, dex_pc);
      break;
    }
    default:
      VLOG(compiler) << "Did not compile "
                     << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                     << " because of unhandled instruction "
                     << instruction.Name();
      MaybeRecordStat(MethodCompilationStat::kNotCompiledUnhandledInstruction);
      return false;
  }
  return true;
}
}
