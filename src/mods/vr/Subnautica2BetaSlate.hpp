#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <bddisasm.h>

namespace uevr::subnautica2_beta::slate {
inline constexpr size_t resource_bytes = 0x30;
inline constexpr size_t texture_offset = 0x10;

inline bool pointer(uintptr_t value) noexcept {
    return value >= 0x10000 && value < 0x0000800000000000ull && value % alignof(void*) == 0;
}

struct Reference {
    uintptr_t vtable{}, cell{}, texture{};
    uint32_t width{}, height{};
};

// The shader-resource base has DestroyState at +8; ShaderResource is at +0x10.
// Only the viewport's current, already-proven scene texture authorizes this cell.
inline std::optional<Reference> reference(uintptr_t object, std::span<const uint8_t> bytes,
    uintptr_t scene) noexcept {
    if (!pointer(object) || object > 0x0000800000000000ull - resource_bytes ||
        bytes.size() < resource_bytes || !pointer(scene)) { return {}; }
    Reference result{};
    std::memcpy(&result.vtable, bytes.data(), sizeof(result.vtable));
    std::memcpy(&result.texture, bytes.data() + texture_offset, sizeof(result.texture));
    std::memcpy(&result.width, bytes.data() + 0x28, sizeof(result.width));
    std::memcpy(&result.height, bytes.data() + 0x2c, sizeof(result.height));
    if (!pointer(result.vtable) || result.texture != scene || !result.width || !result.height ||
        result.width > 32768 || result.height > 16384) { return {}; }
    result.cell = object + texture_offset;
    return result;
}

inline bool dimension_getter(std::span<const uint8_t> code, uint32_t member) {
    if (code.empty() || code.size() > 16 || (member != 0x28 && member != 0x2c)) { return false; }
    bool loaded{};
    for (size_t offset = 0; offset < code.size();) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset, ND_CODE_64, ND_DATA_64)) ||
            !ix.Length || ix.Length > code.size() - offset) { return false; }
        offset += ix.Length;
        if (ix.Instruction == ND_INS_NOP) { continue; }
        if (ix.Instruction == ND_INS_RETN) { return loaded && (!ix.HasImm1 || ix.Immediate1 == 0); }
        if (loaded || ix.Instruction != ND_INS_MOV || ix.OperandsCount < 2) { return false; }
        const auto& dst = ix.Operands[0];
        const auto& src = ix.Operands[1];
        if (dst.Type != ND_OP_REG || dst.Size != 4 || dst.Info.Register.Type != ND_REG_GPR ||
            dst.Info.Register.Reg != NDR_RAX || src.Type != ND_OP_MEM || src.Size != 4) { return false; }
        const auto& m = src.Info.Memory;
        if (!m.HasBase || m.Base != NDR_RCX || m.BaseSize != 8 || m.HasIndex || m.IsRipRel ||
            ix.HasSeg || !m.HasDisp || m.Disp != member) { return false; }
        loaded = true;
    }
    return false;
}
}
