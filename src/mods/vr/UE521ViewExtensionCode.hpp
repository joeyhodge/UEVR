#pragma once

#include "UE521ViewExtension.hpp"
#include <span>
#include <bddisasm.h>

namespace uevr::ue521::view_extension {
inline constexpr size_t max_caller_bytes = 0x1000;

inline bool member_at(const ND_OPERAND& operand, uint32_t displacement) {
    if (operand.Type != ND_OP_MEM) { return false; }
    const auto& memory = operand.Info.Memory;
    return memory.HasBase && memory.BaseSize == 8 && memory.Base != NDR_RSP &&
        !memory.HasIndex && !memory.IsRipRel && memory.HasDisp && memory.Disp == displacement;
}

inline bool slot_call(const INSTRUX& instruction, uint32_t slot) {
    return instruction.Category == ND_CAT_CALL && instruction.OperandsCount >= 1 &&
        instruction.Operands[0].Size == 8 && member_at(instruction.Operands[0], slot * sizeof(uintptr_t));
}

// Decode from the unwind entry, not a raw-byte match inside an immediate.
// Begin follows the scene frame write; RDG PreFamily precedes RDG PreView.
inline bool caller_contract(std::span<const uint8_t> code, size_t return_offset, Stage stage) {
    if (code.empty() || code.size() > max_caller_bytes || return_offset == 0 || return_offset > code.size()) {
        return false;
    }
    bool frame_write{}, family_call{};
    size_t offset{}, count{};
    while (offset < code.size() && count++ < 2048) {
        INSTRUX instruction{};
        if (!ND_SUCCESS(NdDecodeEx(&instruction, code.data() + offset, code.size() - offset,
                                  ND_CODE_64, ND_DATA_64)) || !instruction.Length) { return false; }
        offset += instruction.Length;
        if (!family_call && offset > return_offset) { return false; }
        if (offset == return_offset) {
            if (!slot_call(instruction, stage == Stage::begin ? begin_slot : pre_render_slot)) { return false; }
            if (stage == Stage::begin) { return frame_write; }
            family_call = true;
        } else if (!family_call && instruction.Instruction == ND_INS_MOV && instruction.OperandsCount >= 2 &&
            instruction.Operands[0].Size == 4 && instruction.Operands[0].Access.Write &&
            member_at(instruction.Operands[0], frame_offset) && instruction.Operands[1].Type == ND_OP_REG &&
            instruction.Operands[1].Info.Register.Type == ND_REG_GPR && instruction.Operands[1].Size == 4) {
            frame_write = true;
        } else if (family_call) {
            if (slot_call(instruction, 9)) { return true; }
            if (instruction.Category == ND_CAT_RET) { return false; }
        }
    }
    return false;
}
}
