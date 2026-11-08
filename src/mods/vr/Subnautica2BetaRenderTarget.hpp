#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <bddisasm.h>

namespace uevr::subnautica2_beta {
inline bool target(std::wstring_view path, uint32_t version_ms) noexcept {
    if (version_ms != 0x50007) { return false; }
    const auto slash = path.find_last_of(L"\\/");
    path = path.substr(slash == path.npos ? 0 : slash + 1);
    constexpr std::wstring_view expected = L"subnautica2-win64-test.exe";
    if (path.size() != expected.size()) { return false; }
    for (size_t i = 0; i != path.size(); ++i) {
        const auto ch = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (ch != expected[i]) { return false; }
    }
    return true;
}

struct StackCell {
    uint32_t base{};
    int64_t displacement{};
    bool operator==(const StackCell&) const = default;
};
inline std::optional<uintptr_t> add_displacement(uintptr_t base, int64_t displacement) noexcept {
    if (displacement >= 0) {
        const auto distance = static_cast<uintptr_t>(displacement);
        if (distance > (std::numeric_limits<uintptr_t>::max)() - base) { return {}; }
        return base + distance;
    }
    const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
    if (distance > base) { return {}; }
    return base - distance;
}
inline bool reg(const ND_OPERAND& operand, uint32_t number) noexcept {
    return operand.Type == ND_OP_REG && operand.Size == 8 &&
        operand.Info.Register.Type == ND_REG_GPR && operand.Info.Register.Reg == number;
}
inline std::optional<StackCell> stack_cell(const ND_OPERAND& operand) noexcept {
    if (operand.Type != ND_OP_MEM) { return {}; }
    const auto& memory = operand.Info.Memory;
    if (!memory.HasBase || memory.BaseSize != 8 || memory.HasIndex || memory.IsRipRel ||
        (memory.Base != NDR_RSP && memory.Base != NDR_RBP)) { return {}; }
    const auto displacement = memory.HasDisp ? static_cast<int64_t>(memory.Disp) : 0;
    if (displacement < -0x2000 || displacement > 0x2000) { return {}; }
    return StackCell{memory.Base, displacement};
}

struct FinalizeOutput {
    StackCell initializer, output;
    std::optional<uintptr_t> address(uintptr_t rsp, uintptr_t rbp) const noexcept {
        const auto resolve = [&](StackCell cell) -> std::optional<uintptr_t> {
            if (cell.base != NDR_RSP && cell.base != NDR_RBP) { return {}; }
            if (cell.displacement < -0x2000 || cell.displacement > 0x2000) { return {}; }
            const auto base = cell.base == NDR_RSP ? rsp : rbp;
            const auto value = add_displacement(base, cell.displacement);
            if (!base || !value || *value < 0x10000 || *value > 0x00007ffffffff000ull || *value % alignof(void*)) { return {}; }
            return value;
        };
        const auto init = resolve(initializer), out = resolve(output);
        if (!init || !out || (*out >= *init && *out < *init + 0x90)) { return {}; }
        return out;
    }
};

// Finalize's hidden result is distinct from the earlier initializer result.
// Accept only the bounded two-LEA stack setup; never infer ownership from RAX.
inline std::optional<FinalizeOutput> finalize_output(std::span<const uint8_t> code) {
    if (code.empty() || code.size() > 0x30) { return {}; }
    std::optional<StackCell> initializer, output;
    size_t offset{};
    while (offset < code.size()) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset, ND_CODE_64, ND_DATA_64)) ||
            !ix.Length || ix.Length > code.size() - offset) { return {}; }
        offset += ix.Length;
        if (ix.Instruction == ND_INS_NOP) { continue; }
        if (ix.Instruction != ND_INS_LEA || ix.OperandsCount < 2) { return {}; }
        const auto cell = stack_cell(ix.Operands[1]);
        if (!cell) { return {}; }
        if (reg(ix.Operands[0], NDR_RCX) && !initializer) { initializer = cell; }
        else if (reg(ix.Operands[0], NDR_RDX) && !output) { output = cell; }
        else { return {}; }
    }
    if (!initializer || !output || initializer == output) { return {}; }
    return FinalizeOutput{*initializer, *output};
}

inline bool scene_extent(uint64_t width, uint32_t height, uint32_t eye_width, uint32_t eye_height) noexcept {
    return eye_width && eye_width <= 16384 && eye_height && eye_height <= 16384 && height == eye_height &&
        (width == eye_width || width == uint64_t(eye_width) * 2);
}

inline bool rhi_shape(std::span<const uint8_t> bytes, uint32_t eye_width, uint32_t eye_height) noexcept {
    // PDB/source-matched UE5.7 FRHITexture::TextureDesc, before any virtual call.
    if (bytes.size() < 0x54) { return false; }
    uint64_t flags{};
    uint32_t width{}, height{};
    std::memcpy(&flags, bytes.data() + 0x20, sizeof(flags));
    std::memcpy(&width, bytes.data() + 0x44, sizeof(width));
    std::memcpy(&height, bytes.data() + 0x48, sizeof(height));
    constexpr uint64_t render_target = 1ull << 0;
    constexpr uint64_t depth_stencil = (1ull << 2) | (1ull << 27);
    return (flags & render_target) != 0 && (flags & depth_stencil) == 0 && bytes[0x50] == 1 && bytes[0x51] == 1 &&
        scene_extent(width, height, eye_width, eye_height);
}
}
