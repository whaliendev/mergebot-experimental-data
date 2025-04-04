/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Mterp entry point and support functions.
 */

#include "nterp.h"
#include "base/quasi_atomic.h"
#include "class_linker-inl.h"
#include "dex/dex_instruction_utils.h"
#include "debugger.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "interpreter/interpreter_cache-inl.h"
#include "interpreter/interpreter_common.h"
#include "interpreter/interpreter_intrinsics.h"
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

static mirror::Object* DoFilledNewArray(Thread* self,
                                        ArtMethod* caller,
                                        uint16_t* dex_pc_ptr,
                                        uint32_t* regs,
                                        bool is_range)
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

    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" mirror::Object* NterpAllocateObject(Thread* self,
                                               ArtMethod* caller,
                                               uint16_t* dex_pc_ptr)
extern "C" mirror::Object* NterpGetClass(Thread* self, ArtMethod* caller, uint16_t* dex_pc_ptr)
extern "C" uint32_t NterpGetInstanceFieldOffset(Thread* self,
                                                ArtMethod* caller,
                                                const uint16_t* dex_pc_ptr,
                                                size_t resolve_field_type)  // Resolve if not zero
extern "C" size_t NterpGetStaticField(Thread* self,
                                      ArtMethod* caller,
                                      const uint16_t* dex_pc_ptr,
                                      size_t resolve_field_type)  // Resolve if not zero
// The entrypoint for nterp, which ArtMethods can directly point to.
extern "C" void ExecuteNterpImpl()// Another entrypoint, which does a clinit check at entry.
extern "C" void ExecuteNterpWithClinitImpl()const void* GetNterpEntryPoint() {
  return reinterpret_cast<const void*>(interpreter::ExecuteNterpImpl);
}

const void* GetNterpWithClinitEntryPoint() {
  return reinterpret_cast<const void*>(interpreter::ExecuteNterpWithClinitImpl);
}

/*
 * Verify some constants used by the nterp interpreter.
 */
void CheckNterpAsmConstants() {
  /*
   * If we're using computed goto instruction transitions, make sure
   * none of the handlers overflows the byte limit.  This won't tell
   * which one did, but if any one is too big the total size will
   * overflow.
   */
  const int width = kNterpHandlerSize;
  ptrdiff_t interp_size = reinterpret_cast<uintptr_t>(artNterpAsmInstructionEnd) -
                          reinterpret_cast<uintptr_t>(artNterpAsmInstructionStart);
  if ((interp_size == 0) || (interp_size != (art::kNumPackedOpcodes * width))) {
      LOG(FATAL) << "ERROR: unexpected asm interp size " << interp_size
                 << "(did an instruction handler exceed " << width << " bytes?)";
  }
}

static mirror::Object* DoFilledNewArray(Thread* self,
                                        ArtMethod* caller,
                                        uint16_t* dex_pc_ptr,
                                        uint32_t* regs,
                                        bool is_range)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
template<typename T>
inline void UpdateCache(Thread* self, uint16_t* dex_pc_ptr, T* value) {
  UpdateCache(self, dex_pc_ptr, reinterpret_cast<size_t>(value)){
  UpdateCache(self, dex_pc_ptr, reinterpret_cast<size_t>(value));
}

template<typename T>
inline void UpdateCache(Thread* self, uint16_t* dex_pc_ptr, T* value) {
  UpdateCache(self, dex_pc_ptr, reinterpret_cast<size_t>(value)){
  UpdateCache(self, dex_pc_ptr, reinterpret_cast<size_t>(value));
}

#ifdef __arm__

extern "C" void NterpStoreArm32Fprs(const char* shorty,
                                    uint32_t* registers,
                                    uint32_t* stack_args,
                                    const uint32_t* fprs) {
  // Note `shorty` has already the returned type removed.
  ScopedAssertNoThreadSuspension sants("In nterp");
  uint32_t arg_index = 0;
  uint32_t fpr_double_index = 0;
  uint32_t fpr_index = 0;
  for (uint32_t shorty_index = 0; shorty[shorty_index] != '\0'; ++shorty_index) {
    char arg_type = shorty[shorty_index];
    switch (arg_type) {
      case 'D': {
        // Double should not overlap with float.
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
  // Note `shorty` has already the returned type removed.
  ScopedAssertNoThreadSuspension sants("In nterp");
  uint32_t fpr_double_index = 0;
  uint32_t fpr_index = 0;
  for (uint32_t shorty_index = 0; shorty[shorty_index] != '\0'; ++shorty_index) {
    char arg_type = shorty[shorty_index];
    switch (arg_type) {
      case 'D': {
        // Double should not overlap with float.
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
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" const char* NterpGetShorty(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" const char* NterpGetShortyFromMethodId(ArtMethod* caller, uint32_t method_index)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" const char* NterpGetShortyFromInvokePolymorphic(ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" const char* NterpGetShortyFromInvokeCustom(ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
static mirror::Object* DoFilledNewArray(Thread* self,
                                        ArtMethod* caller,
                                        uint16_t* dex_pc_ptr,
                                        uint32_t* regs,
                                        bool is_range)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" mirror::Object* NterpLoadObject(Thread* self, ArtMethod* caller, uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" void NterpUnimplemented() {
  LOG(FATAL) << "Unimplemented";
}

static mirror::Object* DoFilledNewArray(Thread* self,
                                        ArtMethod* caller,
                                        uint16_t* dex_pc_ptr,
                                        uint32_t* regs,
                                        bool is_range)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" mirror::Object* NterpFilledNewArray(Thread* self,
                                               ArtMethod* caller,
                                               uint32_t* registers,
                                               uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" mirror::Object* NterpFilledNewArrayRange(Thread* self,
                                                    ArtMethod* caller,
                                                    uint32_t* registers,
                                                    uint16_t* dex_pc_ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" jit::OsrData* NterpHotMethod(ArtMethod* method, uint16_t* dex_pc_ptr, uint32_t* vregs)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
extern "C" ssize_t NterpDoPackedSwitch(const uint16_t* switchData, int32_t testVal)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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
    
/*
 * Find the matching case.  Returns the offset to the handler instructions.
 *
 * Returns 3 if we don't find a match (it's the size of the sparse-switch
 * instruction).
 */
extern "C" ssize_t NterpDoSparseSwitch(const uint16_t* switchData, int32_t testVal)
    REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension sants("In nterp");
    const int kInstrLen = 3;
    uint16_t size;
    const int32_t* keys;
    const int32_t* entries;
    
    /*
    * Sparse switch data format:
    *  ushort ident = 0x0200   magic value
    *  ushort size             number of entries in the table; > 0
    *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
    *  int targets[size]       branch targets, relative to switch opcode
    *
    * Total size is (2+size*4) 16-bit code units.
    */
    
    uint16_t signature = *switchData++;
    DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));
    
    size = *switchData++;
    
    /* The keys are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    keys = reinterpret_cast<const int32_t*>(switchData);
    
    /* The entries are guaranteed to be aligned on a 32-bit boundary;
    * we can treat them as a native int array.
    */
    entries = keys + size;
    
    /*
    * Binary-search through the array of keys, which are guaranteed to
    * be sorted low-to-high.
    */
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

} // namespace interpreter

} // namespace art
