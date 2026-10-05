#pragma once

#include <bddisasm.h>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace uevr::prospi::roof {
// Deliberately conservative: accept only the source/BN-proven byte-flag setter
// reached by the reflected SceneComponent.SetVisibility exec, not a signature
// elsewhere in the protected executable.
inline bool visibility_setter(std::span<const uint8_t> code, uint32_t offset, uint8_t mask) {
    if (!std::has_single_bit(mask) || offset < 0x28 || offset > 0x1000) { return false; }
    const auto shift = std::countr_zero(mask);
    const auto reg = [](const ND_OPERAND& op, uint32_t n, size_t bytes) {
        return op.Type == ND_OP_REG && op.Info.Register.Type == ND_REG_GPR &&
            op.Info.Register.Reg == n && !op.Info.Register.IsHigh8 && op.Size == bytes;
    };
    const auto mem = [](const ND_OPERAND& op, uint32_t n, uint64_t displacement, size_t bytes) {
        return op.Type == ND_OP_MEM && op.Info.Memory.HasBase && !op.Info.Memory.HasIndex &&
            !op.Info.Memory.IsRipRel && op.Info.Memory.Base == n && op.Info.Memory.Disp == displacement &&
            op.Size == bytes;
    };
    const auto imm = [](const ND_OPERAND& op, uint64_t value) {
        return op.Type == ND_OP_IMM && op.Info.Immediate.Imm == value;
    };
    size_t at{}, instructions{}, stage{};
    bool vtable{};
    while (at < code.size() && instructions++ < 64) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + at, code.size() - at, ND_CODE_64, ND_DATA_64)) || !ix.Length) {
            return false;
        }
        const auto mnemonic = std::string_view{ix.Mnemonic};
        const auto& a = ix.Operands[0];
        const auto& b = ix.Operands[1];
        if (mnemonic == "RET" || mnemonic == "JMP") { return false; }
        if (mnemonic == "MOV" && reg(a, NDR_RAX, 8) && mem(b, NDR_RDI, 0, 8)) { vtable = true; }
        switch (stage) {
        case 0: if (mnemonic == "MOV" && reg(a, NDR_RDI, 8) && reg(b, NDR_RCX, 8)) { ++stage; } break;
        case 1: if (mnemonic == "MOVZX" && reg(a, NDR_RCX, 4) && mem(b, NDR_RCX, offset, 1)) { ++stage; } break;
        case 2: if (mnemonic == "SHR" && reg(a, NDR_RAX, 1) && imm(b, shift)) { ++stage; } break;
        case 3: if (mnemonic == "AND" && reg(a, NDR_RAX, 1) && imm(b, 1)) { ++stage; } break;
        case 4: if (mnemonic == "CMP" && reg(a, NDR_RDX, 1) && reg(b, NDR_RAX, 1)) { ++stage; } break;
        case 5: if (mnemonic == "AND" && reg(a, NDR_RCX, 1) && imm(b, uint8_t(~mask))) { ++stage; } break;
        case 6: if (mnemonic == "SHL" && reg(a, NDR_RAX, 1) && imm(b, shift)) { ++stage; } break;
        case 7: if (mnemonic == "OR" && reg(a, NDR_RCX, 1) && reg(b, NDR_RAX, 1)) { ++stage; } break;
        case 8: if (mnemonic == "MOV" && mem(a, NDR_RDI, offset, 1) && reg(b, NDR_RCX, 1)) { ++stage; } break;
        case 9: if (mnemonic == "MOV" && reg(a, NDR_RCX, 8) && reg(b, NDR_RDI, 8)) { ++stage; } break;
        case 10: return vtable && ix.Instruction == ND_INS_CALLNI && mem(a, NDR_RAX, 0x470, 8);
        }
        // A direct or indirect call before the verified visibility mutation is
        // not the expected entry (including an already-detoured candidate).
        if (stage < 10 && (mnemonic.starts_with("CALL") || mnemonic == "INT3")) { return false; }
        at += ix.Length;
    }
    return false;
}
}
