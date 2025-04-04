#include "nterp.h"
#include "base/quasi_atomic.h"
#include "class_linker-inl.h"
#include "dex/dex_instruction_utils.h"
#include "debugger.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "interpreter/interpreter_cache-inl.h"
#include "interpreter/interpreter_common.h"
#include "interpreter/shadow_frame-inl.h"
#include "mirror/string-alloc-inl.h"
#include "nterp_helpers.h"
namespace art {
namespace interpreter {
bool IsNterpSupported() {
<<<<<<< HEAD
  return !kPoisonHeapReferences && kReserveMarkingRegister;
||||||| a991d42711
  return !kPoisonHeapReferences && gUseReadBarrier;
=======
  return !kPoisonHeapReferences;
>>>>>>> 7758eaad
}
bool CanRuntimeUseNterp() REQUIRES_SHARED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  return IsNterpSupported() &&
      !runtime->IsJavaDebuggable() &&
      !instr->AreExitStubsInstalled() &&
      !instr->InterpretOnly() &&
      !runtime->IsAotCompiler() &&
      !instr->NeedsSlowInterpreterForListeners() &&
      !runtime->AreAsyncExceptionsThrown() &&
      (runtime->GetJit() == nullptr || !runtime->GetJit()->JitAtFirstUse());
}
extern "C" void ExecuteNterpImpl() REQUIRES_SHARED(Locks::mutator_lock_);
extern "C" void ExecuteNterpWithClinitImpl() REQUIRES_SHARED(Locks::mutator_lock_);
const void* GetNterpEntryPoint() {
  return reinterpret_cast<const void*>(interpreter::ExecuteNterpImpl);
}
const void* GetNterpWithClinitEntryPoint() {
  return reinterpret_cast<const void*>(interpreter::ExecuteNterpWithClinitImpl);
}
void CheckNterpAsmConstants() {
  const int width = kNterpHandlerSize;
  ptrdiff_t interp_size = reinterpret_cast<uintptr_t>(artNterpAsmInstructionEnd) -
                          reinterpret_cast<uintptr_t>(artNterpAsmInstructionStart);
  if ((interp_size == 0) || (interp_size != (art::kNumPackedOpcodes * width))) {
      LOG(FATAL) << "ERROR: unexpected asm interp size " << interp_size
                 << "(did an instruction handler exceed " << width << " bytes?)";
  }
}
inline void UpdateHotness(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_) {
  constexpr uint16_t kNterpHotnessLookup = 0xf;
  method->UpdateCounter(kNterpHotnessLookup);
}
template<typename T>
inline void UpdateCache(Thread* self, const uint16_t* dex_pc_ptr, T value) {
  self->GetInterpreterCache()->Set(self, dex_pc_ptr, value);
}
template<typename T>
inline void UpdateCache(Thread* self, const uint16_t* dex_pc_ptr, T* value) {
  UpdateCache(self, dex_pc_ptr, reinterpret_cast<size_t>(value));
}
#ifdef __arm__
extern "C" void NterpStoreArm32Fprs(const char* shorty,
                                    uint32_t* registers,
                                    uint32_t* stack_args,
                                    const uint32_t* fprs) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  uint32_t arg_index = 0;
  uint32_t fpr_double_index = 0;
  uint32_t fpr_index = 0;
  for (uint32_t shorty_index = 0; shorty[shorty_index] != '\0'; ++shorty_index) {
    char arg_type = shorty[shorty_index];
    switch (arg_type) {
      case 'D': {
        fpr_double_index = std::max(fpr_double_index, RoundUp(fpr_index, 2));
        if (fpr_double_index < 16) {
          registers[arg_index] = fprs[fpr_double_index++];
          registers[arg_index + 1] = fprs[fpr_double_index++];
        } else {
          registers[arg_index] = stack_args[arg_index];
          registers[arg_index + 1] = stack_args[arg_index + 1];
        }
        arg_index += 2;
        break;
      }
      case 'F': {
        if (fpr_index % 2 == 0) {
          fpr_index = std::max(fpr_double_index, fpr_index);
        }
        if (fpr_index < 16) {
          registers[arg_index] = fprs[fpr_index++];
        } else {
          registers[arg_index] = stack_args[arg_index];
        }
        arg_index++;
        break;
      }
      case 'J': {
        arg_index += 2;
        break;
      }
      default: {
        arg_index++;
        break;
      }
    }
  }
}
extern "C" void NterpSetupArm32Fprs(const char* shorty,
                                    uint32_t dex_register,
                                    uint32_t stack_index,
                                    uint32_t* fprs,
                                    uint32_t* registers,
                                    uint32_t* stack_args) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  uint32_t fpr_double_index = 0;
  uint32_t fpr_index = 0;
  for (uint32_t shorty_index = 0; shorty[shorty_index] != '\0'; ++shorty_index) {
    char arg_type = shorty[shorty_index];
    switch (arg_type) {
      case 'D': {
        fpr_double_index = std::max(fpr_double_index, RoundUp(fpr_index, 2));
        if (fpr_double_index < 16) {
          fprs[fpr_double_index++] = registers[dex_register++];
          fprs[fpr_double_index++] = registers[dex_register++];
          stack_index += 2;
        } else {
          stack_args[stack_index++] = registers[dex_register++];
          stack_args[stack_index++] = registers[dex_register++];
        }
        break;
      }
      case 'F': {
        if (fpr_index % 2 == 0) {
          fpr_index = std::max(fpr_double_index, fpr_index);
        }
        if (fpr_index < 16) {
          fprs[fpr_index++] = registers[dex_register++];
          stack_index++;
        } else {
          stack_args[stack_index++] = registers[dex_register++];
        }
        break;
      }
      case 'J': {
        stack_index += 2;
        dex_register += 2;
        break;
      }
      default: {
        stack_index++;
        dex_register++;
        break;
      }
    }
  }
}
#endif
extern "C" const dex::CodeItem* NterpGetCodeItem(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  return method->GetCodeItem();
}
extern "C" const char* NterpGetShorty(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  return method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty();
}
extern "C" const char* NterpGetShortyFromMethodId(ArtMethod* caller, uint32_t method_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  return caller->GetDexFile()->GetMethodShorty(method_index);
}
extern "C" const char* NterpGetShortyFromInvokePolymorphic(ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  dex::ProtoIndex proto_idx(inst->Opcode() == Instruction::INVOKE_POLYMORPHIC
      ? inst->VRegH_45cc()
      : inst->VRegH_4rcc());
  return caller->GetDexFile()->GetShorty(proto_idx);
}
extern "C" const char* NterpGetShortyFromInvokeCustom(ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  uint16_t call_site_index = (inst->Opcode() == Instruction::INVOKE_CUSTOM
      ? inst->VRegB_35c()
      : inst->VRegB_3rc());
  const DexFile* dex_file = caller->GetDexFile();
  dex::ProtoIndex proto_idx = dex_file->GetProtoIndexForCallSite(call_site_index);
  return dex_file->GetShorty(proto_idx);
}
static constexpr uint8_t kInvalidInvokeType = 255u;
static_assert(static_cast<uint8_t>(kMaxInvokeType) < kInvalidInvokeType);
static constexpr uint8_t GetOpcodeInvokeType(uint8_t opcode) {
  switch (opcode) {
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      return static_cast<uint8_t>(kDirect);
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      return static_cast<uint8_t>(kInterface);
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      return static_cast<uint8_t>(kStatic);
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
      return static_cast<uint8_t>(kSuper);
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      return static_cast<uint8_t>(kVirtual);
    default:
      return kInvalidInvokeType;
  }
}
static constexpr std::array<uint8_t, 256u> GenerateOpcodeInvokeTypes() {
  std::array<uint8_t, 256u> opcode_invoke_types{};
  for (size_t opcode = 0u; opcode != opcode_invoke_types.size(); ++opcode) {
    opcode_invoke_types[opcode] = GetOpcodeInvokeType(opcode);
  }
  return opcode_invoke_types;
}
static constexpr std::array<uint8_t, 256u> kOpcodeInvokeTypes = GenerateOpcodeInvokeTypes();
FLATTEN
extern "C" size_t NterpGetMethod(Thread* self, ArtMethod* caller, const uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UpdateHotness(caller);
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  Instruction::Code opcode = inst->Opcode();
  DCHECK(IsUint<8>(static_cast<std::underlying_type_t<Instruction::Code>>(opcode)));
  uint8_t raw_invoke_type = kOpcodeInvokeTypes[opcode];
  DCHECK_LE(raw_invoke_type, kMaxInvokeType);
  InvokeType invoke_type = static_cast<InvokeType>(raw_invoke_type);
  uint16_t method_index =
      (opcode >= Instruction::INVOKE_VIRTUAL_RANGE) ? inst->VRegB_3rc() : inst->VRegB_35c();
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  ArtMethod* resolved_method = caller->SkipAccessChecks()
      ? class_linker->ResolveMethod<ClassLinker::ResolveMode::kNoChecks>(
            self, method_index, caller, invoke_type)
      : class_linker->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
            self, method_index, caller, invoke_type);
  if (resolved_method == nullptr) {
    DCHECK(self->IsExceptionPending());
    return 0;
  }
  if (invoke_type == kSuper) {
    resolved_method = caller->SkipAccessChecks()
        ? FindSuperMethodToCall< false>(method_index, resolved_method, caller, self)
        : FindSuperMethodToCall< true>(method_index, resolved_method, caller, self);
    if (resolved_method == nullptr) {
      DCHECK(self->IsExceptionPending());
      return 0;
    }
  }
  if (invoke_type == kInterface) {
    size_t result = 0u;
    if (resolved_method->GetDeclaringClass()->IsObjectClass()) {
      DCHECK_LT(resolved_method->GetMethodIndex(), 0x10000);
      result = (resolved_method->GetMethodIndex() << 16) | 1U;
    } else {
      DCHECK(resolved_method->GetDeclaringClass()->IsInterface());
      DCHECK(!resolved_method->IsCopied());
      if (!resolved_method->IsAbstract()) {
        result = reinterpret_cast<size_t>(resolved_method) | 2U;
      } else {
        result = reinterpret_cast<size_t>(resolved_method);
      }
    }
    UpdateCache(self, dex_pc_ptr, result);
    return result;
  } else if (resolved_method->IsStringConstructor()) {
    CHECK_NE(invoke_type, kSuper);
    resolved_method = WellKnownClasses::StringInitToStringFactory(resolved_method);
    return reinterpret_cast<size_t>(resolved_method) | 1;
  } else if (invoke_type == kVirtual) {
    UpdateCache(self, dex_pc_ptr, resolved_method->GetMethodIndex());
    return resolved_method->GetMethodIndex();
  } else {
    UpdateCache(self, dex_pc_ptr, resolved_method);
    return reinterpret_cast<size_t>(resolved_method);
  }
}
extern "C" size_t NterpGetStaticField(Thread* self,
                                      ArtMethod* caller,
                                      const uint16_t* dex_pc_ptr,
                                      size_t resolve_field_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UpdateHotness(caller);
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  uint16_t field_index = inst->VRegB_21c();
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  Instruction::Code opcode = inst->Opcode();
  ArtField* resolved_field = ResolveFieldWithAccessChecks(
      self,
      class_linker,
      field_index,
      caller,
                     true,
                  IsInstructionSPut(opcode),
      resolve_field_type);
  if (resolved_field == nullptr) {
    DCHECK(self->IsExceptionPending());
    return 0;
  }
  if (UNLIKELY(!resolved_field->GetDeclaringClass()->IsVisiblyInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(resolved_field->GetDeclaringClass()));
    if (UNLIKELY(!class_linker->EnsureInitialized(
                      self, h_class, true, true))) {
      DCHECK(self->IsExceptionPending());
      return 0;
    }
    DCHECK(h_class->IsInitializing());
  }
  if (resolved_field->IsVolatile()) {
    return reinterpret_cast<size_t>(resolved_field) | 1;
  } else {
    if (opcode == Instruction::SPUT_OBJECT &&
        resolve_field_type == 0 &&
        resolved_field->ResolveType() == nullptr) {
      DCHECK(self->IsExceptionPending());
      self->ClearException();
    } else {
      UpdateCache(self, dex_pc_ptr, resolved_field);
    }
    return reinterpret_cast<size_t>(resolved_field);
  }
}
extern "C" uint32_t NterpGetInstanceFieldOffset(Thread* self,
                                                ArtMethod* caller,
                                                const uint16_t* dex_pc_ptr,
                                                size_t resolve_field_type)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UpdateHotness(caller);
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  uint16_t field_index = inst->VRegC_22c();
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  Instruction::Code opcode = inst->Opcode();
  ArtField* resolved_field = ResolveFieldWithAccessChecks(
      self,
      class_linker,
      field_index,
      caller,
                     false,
                  IsInstructionIPut(opcode),
      resolve_field_type);
  if (resolved_field == nullptr) {
    DCHECK(self->IsExceptionPending());
    return 0;
  }
  if (resolved_field->IsVolatile()) {
    return -resolved_field->GetOffset().Uint32Value();
  }
  if (opcode == Instruction::IPUT_OBJECT &&
      resolve_field_type == 0 &&
      resolved_field->ResolveType() == nullptr) {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
  } else {
    UpdateCache(self, dex_pc_ptr, resolved_field->GetOffset().Uint32Value());
  }
  return resolved_field->GetOffset().Uint32Value();
}
extern "C" mirror::Object* NterpGetClass(Thread* self, ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UpdateHotness(caller);
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  Instruction::Code opcode = inst->Opcode();
  DCHECK(opcode == Instruction::CHECK_CAST ||
         opcode == Instruction::INSTANCE_OF ||
         opcode == Instruction::CONST_CLASS ||
         opcode == Instruction::NEW_ARRAY);
  dex::TypeIndex index = dex::TypeIndex(
      (opcode == Instruction::CHECK_CAST || opcode == Instruction::CONST_CLASS)
          ? inst->VRegB_21c()
          : inst->VRegC_22c());
  ObjPtr<mirror::Class> c =
      ResolveVerifyAndClinit(index,
                             caller,
                             self,
                                                   false,
                                                  !caller->SkipAccessChecks());
  if (UNLIKELY(c == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }
  UpdateCache(self, dex_pc_ptr, c.Ptr());
  return c.Ptr();
}
extern "C" mirror::Object* NterpAllocateObject(Thread* self,
                                               ArtMethod* caller,
                                               uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UpdateHotness(caller);
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  DCHECK_EQ(inst->Opcode(), Instruction::NEW_INSTANCE);
  dex::TypeIndex index = dex::TypeIndex(inst->VRegB_21c());
  ObjPtr<mirror::Class> c =
      ResolveVerifyAndClinit(index,
                             caller,
                             self,
                                                   false,
                                                  !caller->SkipAccessChecks());
  if (UNLIKELY(c == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  if (UNLIKELY(c->IsStringClass())) {
    return mirror::String::AllocEmptyString(self, allocator_type).Ptr();
  } else {
    if (!c->IsFinalizable() && c->IsInstantiable()) {
      UpdateCache(self, dex_pc_ptr, c.Ptr());
    }
    return AllocObjectFromCode(c, self, allocator_type).Ptr();
  }
}
extern "C" mirror::Object* NterpLoadObject(Thread* self, ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  switch (inst->Opcode()) {
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO: {
      UpdateHotness(caller);
      dex::StringIndex string_index(
          (inst->Opcode() == Instruction::CONST_STRING)
              ? inst->VRegB_21c()
              : inst->VRegB_31c());
      ObjPtr<mirror::String> str = class_linker->ResolveString(string_index, caller);
      if (str == nullptr) {
        DCHECK(self->IsExceptionPending());
        return nullptr;
      }
      UpdateCache(self, dex_pc_ptr, str.Ptr());
      return str.Ptr();
    }
    case Instruction::CONST_METHOD_HANDLE: {
      return class_linker->ResolveMethodHandle(self, inst->VRegB_21c(), caller).Ptr();
    }
    case Instruction::CONST_METHOD_TYPE: {
      return class_linker->ResolveMethodType(
          self, dex::ProtoIndex(inst->VRegB_21c()), caller).Ptr();
    }
    default:
      LOG(FATAL) << "Unreachable";
  }
  return nullptr;
}
extern "C" void NterpUnimplemented() {
  LOG(FATAL) << "Unimplemented";
}
static mirror::Object* DoFilledNewArray(Thread* self,
                                        ArtMethod* caller,
                                        uint16_t* dex_pc_ptr,
                                        uint32_t* regs,
                                        bool is_range)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  if (kIsDebugBuild) {
    if (is_range) {
      DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY_RANGE);
    } else {
      DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY);
    }
  }
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  DCHECK_GE(length, 0);
  if (!is_range) {
    DCHECK_LE(length, 5);
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  ObjPtr<mirror::Class> array_class =
      ResolveVerifyAndClinit(dex::TypeIndex(type_idx),
                             caller,
                             self,
                                                   true,
                                                  !caller->SkipAccessChecks());
  if (UNLIKELY(array_class == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }
  DCHECK(array_class->IsArrayClass());
  ObjPtr<mirror::Class> component_class = array_class->GetComponentType();
  const bool is_primitive_int_component = component_class->IsPrimitiveInt();
  if (UNLIKELY(component_class->IsPrimitive() && !is_primitive_int_component)) {
    if (component_class->IsPrimitiveLong() || component_class->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            component_class->PrettyDescriptor().c_str());
    } else {
      self->ThrowNewExceptionF(
          "Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but 'int'",
          component_class->PrettyDescriptor().c_str());
    }
    return nullptr;
  }
  ObjPtr<mirror::Object> new_array = mirror::Array::Alloc(
      self,
      array_class,
      length,
      array_class->GetComponentSizeShift(),
      Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(new_array == nullptr)) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  uint32_t arg[Instruction::kMaxVarArgRegs];
  uint32_t vregC = 0;
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    inst->GetVarArgs(arg);
  }
  for (int32_t i = 0; i < length; ++i) {
    size_t src_reg = is_range ? vregC + i : arg[i];
    if (is_primitive_int_component) {
      new_array->AsIntArray()->SetWithoutChecks< false>(i, regs[src_reg]);
    } else {
      new_array->AsObjectArray<mirror::Object>()->SetWithoutChecks< false>(
          i, reinterpret_cast<mirror::Object*>(regs[src_reg]));
    }
  }
  return new_array.Ptr();
}
extern "C" mirror::Object* NterpFilledNewArray(Thread* self,
                                               ArtMethod* caller,
                                               uint32_t* registers,
                                               uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return DoFilledNewArray(self, caller, dex_pc_ptr, registers, false);
}
extern "C" mirror::Object* NterpFilledNewArrayRange(Thread* self,
                                                    ArtMethod* caller,
                                                    uint32_t* registers,
                                                    uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return DoFilledNewArray(self, caller, dex_pc_ptr, registers, true);
}
extern "C" jit::OsrData* NterpHotMethod(ArtMethod* method, uint16_t* dex_pc_ptr, uint32_t* vregs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  Runtime* runtime = Runtime::Current();
  if (method->IsMemorySharedMethod()) {
    DCHECK_EQ(Thread::Current()->GetSharedMethodHotness(), 0u);
    Thread::Current()->ResetSharedMethodHotness();
  } else {
    method->ResetCounter(runtime->GetJITOptions()->GetWarmupThreshold());
  }
  jit::Jit* jit = runtime->GetJit();
  if (jit != nullptr && jit->UseJitCompilation()) {
    if (dex_pc_ptr != nullptr) {
      CodeItemInstructionAccessor accessor(method->DexInstructions());
      uint32_t dex_pc = dex_pc_ptr - accessor.Insns();
      jit::OsrData* osr_data = jit->PrepareForOsr(
          method->GetInterfaceMethodIfProxy(kRuntimePointerSize), dex_pc, vregs);
      if (osr_data != nullptr) {
        return osr_data;
      }
    }
    jit->MaybeEnqueueCompilation(method, Thread::Current());
  }
  return nullptr;
}
extern "C" ssize_t NterpDoPackedSwitch(const uint16_t* switchData, int32_t testVal)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  const int kInstrLen = 3;
  uint16_t signature = *switchData++;
  DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kPackedSwitchSignature));
  uint16_t size = *switchData++;
  int32_t firstKey = *switchData++;
  firstKey |= (*switchData++) << 16;
  int index = testVal - firstKey;
  if (index < 0 || index >= size) {
    return kInstrLen;
  }
  const int32_t* entries = reinterpret_cast<const int32_t*>(switchData);
  return entries[index];
}
extern "C" ssize_t NterpDoSparseSwitch(const uint16_t* switchData, int32_t testVal)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension sants("In nterp");
  const int kInstrLen = 3;
  uint16_t size;
  const int32_t* keys;
  const int32_t* entries;
  uint16_t signature = *switchData++;
  DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
  size = *switchData++;
  keys = reinterpret_cast<const int32_t*>(switchData);
  entries = keys + size;
  int lo = 0;
  int hi = size - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    int32_t foundVal = keys[mid];
    if (testVal < foundVal) {
      hi = mid - 1;
    } else if (testVal > foundVal) {
      lo = mid + 1;
    } else {
      return entries[mid];
    }
  }
  return kInstrLen;
}
extern "C" void NterpFree(void* val) {
  free(val);
}
}
}
