#ifndef ART_COMPILER_DEBUG_ELF_DEBUG_FRAME_WRITER_H_
#define ART_COMPILER_DEBUG_ELF_DEBUG_FRAME_WRITER_H_ 
#include <vector>
#include "arch/instruction_set.h"
#include "debug/method_debug_info.h"
#include "dwarf/debug_frame_opcode_writer.h"
#include "dwarf/dwarf_constants.h"
#include "dwarf/headers.h"
#include "elf/elf_builder.h"
namespace art {
namespace debug {
<<<<<<< HEAD
static constexpr bool kWriteDebugFrameHdr = false;
static constexpr size_t kMinDebugFrameHdrEntries = 100;
||||||| 2afffd8909
=======
static constexpr bool kWriteDebugFrameHdr = false;
>>>>>>> 8cd84e42
static void WriteCIE(InstructionSet isa, std::vector<uint8_t>* buffer) {
  using Reg = dwarf::Reg;
  bool is64bit = Is64BitInstructionSet(isa);
  switch (isa) {
    case InstructionSet::kArm:
    case InstructionSet::kThumb2: {
      dwarf::DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::ArmCore(13), 0);
      for (int reg = 0; reg < 13; reg++) {
        if (reg < 4 || reg == 12) {
          opcodes.Undefined(Reg::ArmCore(reg));
        } else {
          opcodes.SameValue(Reg::ArmCore(reg));
        }
      }
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 16) {
          opcodes.Undefined(Reg::ArmFp(reg));
        } else {
          opcodes.SameValue(Reg::ArmFp(reg));
        }
      }
      auto return_reg = Reg::ArmCore(14);
      WriteCIE(is64bit, return_reg, opcodes, buffer);
      return;
    }
    case InstructionSet::kArm64: {
      dwarf::DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::Arm64Core(31), 0);
      for (int reg = 0; reg < 30; reg++) {
        if (reg < 8 || reg == 16 || reg == 17) {
          opcodes.Undefined(Reg::Arm64Core(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Core(reg));
        }
      }
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 8 || reg >= 16) {
          opcodes.Undefined(Reg::Arm64Fp(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Fp(reg));
        }
      }
      auto return_reg = Reg::Arm64Core(30);
      WriteCIE(is64bit, return_reg, opcodes, buffer);
      return;
    }
    case InstructionSet::kMips:
    case InstructionSet::kMips64: {
      dwarf::DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::MipsCore(29), 0);
      for (int reg = 1; reg < 26; reg++) {
        if (reg < 16 || reg == 24 || reg == 25) {
          opcodes.Undefined(Reg::MipsCore(reg));
        } else {
          opcodes.SameValue(Reg::MipsCore(reg));
        }
      }
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 24) {
          opcodes.Undefined(Reg::Mips64Fp(reg));
        } else {
          opcodes.SameValue(Reg::Mips64Fp(reg));
        }
      }
      auto return_reg = Reg::MipsCore(31);
      WriteCIE(is64bit, return_reg, opcodes, buffer);
      return;
    }
    case InstructionSet::kX86: {
      constexpr bool generate_opcodes_for_x86_fp = false;
      dwarf::DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86Core(4), 4);
      opcodes.Offset(Reg::X86Core(8), -4);
      for (int reg = 0; reg < 8; reg++) {
        if (reg <= 3) {
          opcodes.Undefined(Reg::X86Core(reg));
        } else if (reg == 4) {
        } else {
          opcodes.SameValue(Reg::X86Core(reg));
        }
      }
      if (generate_opcodes_for_x86_fp) {
        for (int reg = 0; reg < 8; reg++) {
          opcodes.Undefined(Reg::X86Fp(reg));
        }
      }
      auto return_reg = Reg::X86Core(8);
      WriteCIE(is64bit, return_reg, opcodes, buffer);
      return;
    }
    case InstructionSet::kX86_64: {
      dwarf::DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86_64Core(4), 8);
      opcodes.Offset(Reg::X86_64Core(16), -8);
      for (int reg = 0; reg < 16; reg++) {
        if (reg == 4) {
        } else if (reg < 12 && reg != 3 && reg != 5) {
          opcodes.Undefined(Reg::X86_64Core(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Core(reg));
        }
      }
      for (int reg = 0; reg < 16; reg++) {
        if (reg < 12) {
          opcodes.Undefined(Reg::X86_64Fp(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Fp(reg));
        }
      }
      auto return_reg = Reg::X86_64Core(16);
      WriteCIE(is64bit, return_reg, opcodes, buffer);
      return;
    }
    case InstructionSet::kNone:
      break;
  }
  LOG(FATAL) << "Cannot write CIE frame for ISA " << isa;
  UNREACHABLE();
}
template<typename ElfTypes>
void WriteCFISection(ElfBuilder<ElfTypes>* builder,
                     const ArrayRef<const MethodDebugInfo>& method_infos) {
  typedef typename ElfTypes::Addr Elf_Addr;
  std::vector<const MethodDebugInfo*> sorted_method_infos;
  sorted_method_infos.reserve(method_infos.size());
  for (size_t i = 0; i < method_infos.size(); i++) {
    if (!method_infos[i].cfi.empty() && !method_infos[i].deduped) {
      sorted_method_infos.push_back(&method_infos[i]);
    }
  }
  if (sorted_method_infos.empty()) {
    return;
  }
  std::stable_sort(
      sorted_method_infos.begin(),
      sorted_method_infos.end(),
      [](const MethodDebugInfo* lhs, const MethodDebugInfo* rhs) {
        ArrayRef<const uint8_t> l = lhs->cfi;
        ArrayRef<const uint8_t> r = rhs->cfi;
        return std::lexicographical_compare(l.begin(), l.end(), r.begin(), r.end());
      });
  std::vector<uint32_t> binary_search_table;
  if (kWriteDebugFrameHdr) {
    binary_search_table.reserve(2 * sorted_method_infos.size());
  }
  auto* cfi_section = builder->GetDebugFrame();
  {
    cfi_section->Start();
    const bool is64bit = Is64BitInstructionSet(builder->GetIsa());
    std::vector<uint8_t> buffer;
    WriteCIE(builder->GetIsa(), &buffer);
    cfi_section->WriteFully(buffer.data(), buffer.size());
    buffer.clear();
    for (const MethodDebugInfo* mi : sorted_method_infos) {
      DCHECK(!mi->deduped);
      DCHECK(!mi->cfi.empty());
      const Elf_Addr code_address = mi->code_address +
          (mi->is_code_address_text_relative ? builder->GetText()->GetAddress() : 0);
      if (kWriteDebugFrameHdr) {
        binary_search_table.push_back(dchecked_integral_cast<uint32_t>(code_address));
        binary_search_table.push_back(cfi_section->GetPosition());
      }
      dwarf::WriteFDE(is64bit,
                                         0,
                      code_address,
                      mi->code_size,
                      mi->cfi,
                      &buffer);
      cfi_section->WriteFully(buffer.data(), buffer.size());
      buffer.clear();
    }
    cfi_section->End();
  }
<<<<<<< HEAD
  if (kWriteDebugFrameHdr && method_infos.size() > kMinDebugFrameHdrEntries) {
||||||| 2afffd8909
  if (method_infos.size() > 1) {
=======
  if (kWriteDebugFrameHdr) {
>>>>>>> 8cd84e42
    std::sort(binary_search_table.begin(), binary_search_table.end());
    std::vector<uint8_t> header_buffer;
    dwarf::Writer<> header(&header_buffer);
    header.PushUint8(1);
    header.PushUint8(dwarf::DW_EH_PE_omit);
    header.PushUint8(dwarf::DW_EH_PE_udata4);
    header.PushUint8(dwarf::DW_EH_PE_udata4);
    header.PushUint32(dchecked_integral_cast<uint32_t>(binary_search_table.size()/2));
    auto* header_section = builder->GetDebugFrameHdr();
    header_section->Start();
    header_section->WriteFully(header_buffer.data(), header_buffer.size());
    header_section->WriteFully(binary_search_table.data(), binary_search_table.size());
    header_section->End();
  }
}
}
}
#endif
