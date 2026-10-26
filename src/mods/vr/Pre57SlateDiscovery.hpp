#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <bddisasm.h>

namespace uevr::pre57_slate {
struct Extent {
    uint32_t width{}, height{};
    bool operator==(const Extent&) const = default;
};
static_assert(sizeof(Extent) == 2 * sizeof(uint32_t));

inline std::optional<Extent> checked_extent(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y) {
    const auto width = int64_t{max_x} - min_x;
    const auto height = int64_t{max_y} - min_y;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) { return {}; }
    return Extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

struct ImageRange {
    uintptr_t base{};
    size_t size{};
    bool contains(uintptr_t address, size_t count) const {
        return base >= 0x10000 && size != 0 && size <= 0xffffffffull &&
            base <= 0x0000800000000000ull - size && address >= base &&
            address - base <= size && count <= size - (address - base);
    }
};

struct Registration {
    uintptr_t name_reference{}, name{}, callsite{}, target{};
    bool operator==(const Registration&) const = default;
};

struct CodeContract {
    std::span<const uint8_t> code, mask;
};

struct Owner {
    uintptr_t begin{}, image_base{};
    bool operator==(const Owner&) const = default;
};

inline bool legacy_owner_matches(std::optional<Owner> expected, uintptr_t draw_window,
    uintptr_t image_base, std::optional<Owner> candidate) {
    // Missing unwind metadata must not disable an existing custom adapter.
    if (!expected || expected->begin != draw_window || expected->image_base != image_base) { return true; }
    return candidate && *candidate == *expected;
}

template<class Read, class Executable>
bool matches_callee(uintptr_t target, ImageRange image, CodeContract contract, Read&& read, Executable&& executable) {
    if (contract.code.empty() || contract.code.size() > 1024 || contract.code.size() != contract.mask.size() ||
        !image.contains(target, contract.code.size()) || !executable(target, contract.code.size())) { return false; }
    std::array<uint8_t, 1024> actual{};
    if (!read(target, actual.data(), contract.code.size())) { return false; }
    for (size_t i = 0; i < contract.code.size(); ++i) {
        if ((actual[i] & contract.mask[i]) != (contract.code[i] & contract.mask[i])) { return false; }
    }
    return true;
}

inline std::optional<size_t> legacy_scan_limit(ImageRange image, uintptr_t begin, size_t stolen) {
    if (stolen == 0 || stolen > 64 || !image.contains(begin, stolen + 1)) { return {}; }
    const auto limit = (std::min)(size_t{0x3000}, image.size - (begin - image.base));
    return limit > stolen ? std::optional<size_t>{limit} : std::nullopt;
}

enum class ScanState { Ready, InvalidRange, InvalidEntry, InvalidCode, NoCandidate, Ambiguous };
struct ScanResult {
    ScanState state{ScanState::NoCandidate};
    std::optional<Registration> match;
};

inline const char* name(ScanState state) {
    switch (state) {
    case ScanState::Ready: return "ready";
    case ScanState::InvalidRange: return "invalid function/module range";
    case ScanState::InvalidEntry: return "incomplete original hook entry";
    case ScanState::InvalidCode: return "truncated or invalid function code";
    case ScanState::Ambiguous: return "ambiguous output registration";
    default: return "raw output argument transport not proven";
    }
}

inline bool gpr(const ND_OPERAND& operand, uint32_t reg, size_t size = 8) {
    return operand.Type == ND_OP_REG && operand.Size == size &&
        operand.Info.Register.Type == ND_REG_GPR && operand.Info.Register.Reg == reg;
}

inline bool writes(const INSTRUX& instruction, uint32_t reg) {
    for (size_t i = 0; i < instruction.OperandsCount; ++i) {
        const auto& operand = instruction.Operands[i];
        if (operand.Type == ND_OP_REG && operand.Info.Register.Type == ND_REG_GPR &&
            operand.Info.Register.Reg == reg && (operand.Access.Write || operand.Access.CondWrite)) { return true; }
    }
    return false;
}

inline bool saved_register(const ND_OPERAND& operand) {
    if (operand.Type != ND_OP_REG || operand.Size != 8 || operand.Info.Register.Type != ND_REG_GPR) { return false; }
    const auto reg = operand.Info.Register.Reg;
    return reg == NDR_RBX || reg == NDR_RBP || reg == NDR_RSI || reg == NDR_RDI || (reg >= NDR_R12 && reg <= NDR_R15);
}

// This selects only the already-supported raw-texture ABI. It is not permission
// to enable dedicated UI for an unknown game or for a pooled/hidden-return ABI.
template<class Read, class Executable>
ScanResult find_raw_registration(std::span<const uint8_t> patched_code, uintptr_t begin,
    ImageRange image, std::span<const uint8_t> original_entry, CodeContract contract, Read&& read, Executable&& executable) {
    if (patched_code.empty() || patched_code.size() > 0x10000 ||
        !image.contains(begin, patched_code.size()) || !executable(begin, patched_code.size())) {
        return {ScanState::InvalidRange, {}};
    }
    if (original_entry.empty() || original_entry.size() > 64 || original_entry.size() > patched_code.size()) {
        return {ScanState::InvalidEntry, {}};
    }
    // Decode original instructions, never an inline hook's absolute address literal.
    std::vector<uint8_t> code(patched_code.begin(), patched_code.end());
    std::copy(original_entry.begin(), original_entry.end(), code.begin());
    std::optional<Registration> pending, result;
    size_t names{};
    bool builder_in_rcx{}, zero_flags{};
    for (size_t offset = 0; offset < code.size();) {
        INSTRUX ix{};
        if (!ND_SUCCESS(NdDecodeEx(&ix, code.data() + offset, code.size() - offset, ND_CODE_64, ND_DATA_64)) ||
            !ix.Length || ix.Length > code.size() - offset) { return {ScanState::InvalidCode, {}}; }
        const auto next = offset + ix.Length;
        if (offset < original_entry.size() && next > original_entry.size()) { return {ScanState::InvalidEntry, {}}; }
        const auto ip = begin + offset;
        const auto* bytes = code.data() + offset;
        if (ix.Category == ND_CAT_CALL) {
            if (pending && ip - pending->name_reference <= 96 && builder_in_rcx && zero_flags && ix.Length == 5 && bytes[0] == 0xe8) {
                int32_t displacement{};
                std::memcpy(&displacement, bytes + 1, sizeof(displacement));
                const auto target = static_cast<int64_t>(ip + 5) + displacement;
                if (target > 0 && matches_callee(static_cast<uintptr_t>(target), image, contract, read, executable)) {
                    if (result) { return {ScanState::Ambiguous, {}}; }
                    pending->callsite = ip;
                    pending->target = static_cast<uintptr_t>(target);
                    result = pending;
                }
            }
            pending.reset(); builder_in_rcx = zero_flags = false;
        } else if (ix.Category == ND_CAT_RET || ix.Category == ND_CAT_COND_BR || ix.Category == ND_CAT_UNCOND_BR ||
            ix.Category == ND_CAT_INTERRUPT || ix.Category == ND_CAT_SYSCALL || ix.Category == ND_CAT_SYSRET || ix.Category == ND_CAT_SYSTEM) {
            pending.reset(); builder_in_rcx = zero_flags = false;
        } else {
            if (writes(ix, NDR_RCX)) {
                builder_in_rcx = ix.Instruction == ND_INS_MOV && ix.OperandsCount >= 2 &&
                    gpr(ix.Operands[0], NDR_RCX) && saved_register(ix.Operands[1]);
            }
            if (writes(ix, NDR_R9)) {
                zero_flags = ix.OperandsCount >= 2 &&
                    ((ix.Instruction == ND_INS_XOR &&
                        (gpr(ix.Operands[0], NDR_R9, 4) || gpr(ix.Operands[0], NDR_R9)) &&
                        (gpr(ix.Operands[1], NDR_R9, 4) || gpr(ix.Operands[1], NDR_R9))) ||
                     (ix.Instruction == ND_INS_MOV &&
                        (gpr(ix.Operands[0], NDR_R9, 4) || gpr(ix.Operands[0], NDR_R9)) &&
                        ix.Operands[1].Type == ND_OP_IMM && ix.Operands[1].Info.Immediate.Imm == 0));
            }
            if (writes(ix, NDR_R8)) { pending.reset(); }
            if (pending && writes(ix, NDR_RDX) &&
                !(ix.Instruction == ND_INS_MOV && ix.OperandsCount >= 2 && gpr(ix.Operands[0], NDR_RDX) &&
                  ((ix.Operands[1].Type == ND_OP_REG && ix.Operands[1].Size == 8 && ix.Operands[1].Info.Register.Type == ND_REG_GPR) ||
                   (ix.Operands[1].Type == ND_OP_MEM && ix.Operands[1].Size == 8)))) {
                pending.reset();
            }
            if (ix.Length == 7 && bytes[0] == 0x4c && bytes[1] == 0x8d && bytes[2] == 0x05) {
                int32_t displacement{};
                std::memcpy(&displacement, bytes + 3, sizeof(displacement));
                const auto address = static_cast<int64_t>(ip + 7) + displacement;
                std::array<char16_t, 19> text{};
                if (address > 0 && image.contains(static_cast<uintptr_t>(address), sizeof(text)) &&
                    read(static_cast<uintptr_t>(address), text.data(), sizeof(text)) && text.back() == 0 &&
                    std::u16string_view{text.data(), text.size() - 1} == u"SlateOutputTexture") {
                    if (++names > 1) { return {ScanState::Ambiguous, {}}; }
                    pending = Registration{ip, static_cast<uintptr_t>(address), 0, 0};
                }
            }
        }
        offset = next;
    }
    return result ? ScanResult{ScanState::Ready, result} : ScanResult{};
}
}
