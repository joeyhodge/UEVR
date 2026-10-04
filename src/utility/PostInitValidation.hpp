#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <bddisasm.h>

namespace uevr::post_init {
inline std::optional<uintptr_t> relative_target(uintptr_t next, int64_t displacement) {
    if (displacement >= 0) {
        const auto distance = static_cast<uintptr_t>(displacement);
        if (distance > (std::numeric_limits<uintptr_t>::max)() - next) {
            return std::nullopt;
        }
        return next + distance;
    }
    const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
    return distance <= next ? std::optional{next - distance} : std::nullopt;
}

// This checks code readability/plausibility, NOT virtual-function identity.
// Use it only at a source-verified slot, never to select a neighboring virtual.
// read_code must provide at most 15 readable, executable, module-owned bytes.
struct NoPointerReader {
    std::optional<uintptr_t> operator()(uintptr_t) const { return std::nullopt; }
};
template<typename ReadCode, typename ReadPointer = NoPointerReader>
bool validates_source_slot_body(uintptr_t entry, ReadCode&& read_code, ReadPointer read_pointer = {}) {
    constexpr size_t byte_budget = 256;
    constexpr size_t max_thunks = 4;
    std::array<uintptr_t, max_thunks + 1> entries{};

    for (size_t thunk = 0; thunk <= max_thunks; ++thunk) {
        if (entry == 0) { return false; }
        for (size_t i = 0; i < thunk; ++i) {
            if (entries[i] == entry) { return false; }
        }
        entries[thunk] = entry;
        std::array<uintptr_t, byte_budget / 2> forward_branches{};
        size_t branch_count{};
        size_t decoded_bytes{};
        bool saw_call{};
        bool follow_thunk{};
        auto ip = entry;

        while (decoded_bytes < byte_budget) {
            const auto code = read_code(ip);
            if (code.empty() || code.size() > 15) { return false; }
            INSTRUX ix{};
            if (!ND_SUCCESS(NdDecodeEx(&ix, code.data(), code.size(), ND_CODE_64, ND_DATA_64)) ||
                ix.Length == 0 || ix.Length > code.size() ||
                ip > (std::numeric_limits<uintptr_t>::max)() - ix.Length) {
                return false;
            }
            const auto next = ip + ix.Length;
            decoded_bytes += ix.Length;
            std::optional<uintptr_t> target;
            bool relative_branch{};
            bool indirect_rip_branch{};
            for (size_t i = 0; i < ix.OperandsCount; ++i) {
                if (ix.Operands[i].Type == ND_OP_OFFS) {
                    relative_branch = true;
                    target = relative_target(next, ix.Operands[i].Info.RelativeOffset.Rel);
                    break;
                }
                if (ix.Category == ND_CAT_UNCOND_BR && ix.Operands[i].Type == ND_OP_MEM) {
                    const auto& memory = ix.Operands[i].Info.Memory;
                    if (memory.IsRipRel && memory.HasDisp) {
                        indirect_rip_branch = true;
                        if (const auto slot = relative_target(next, static_cast<int64_t>(memory.Disp))) {
                            target = read_pointer(*slot);
                        }
                        break;
                    }
                }
            }
            if (ix.Category == ND_CAT_CALL) {
                saw_call = true; // Never follow or invoke a candidate's calls.
            }
            if (ix.Category == ND_CAT_COND_BR || ix.Category == ND_CAT_UNCOND_BR) {
                if ((relative_branch || indirect_rip_branch) &&
                    (!target || read_code(*target).empty())) { return false; }
                if (ix.Category == ND_CAT_COND_BR && target && *target > next) {
                    if (branch_count == forward_branches.size()) { return false; }
                    forward_branches[branch_count++] = *target;
                }
            }
            if (ix.Instruction == ND_INS_INT3) {
                // Development checks may end in an assertion-only INT3. Accept
                // it only when an earlier conditional branch skips exactly it.
                bool bypassed{};
                for (size_t i = 0; i < branch_count; ++i) {
                    bypassed |= forward_branches[i] == next;
                }
                if (!saw_call || !bypassed) { return false; }
            } else if (ix.Instruction == ND_INS_RETN) {
                for (size_t i = 0; i < ix.OperandsCount; ++i) {
                    if (ix.Operands[i].Type == ND_OP_IMM) { return false; }
                }
                return true; // Includes a folded no-op at the verified slot.
            } else if (ix.Category == ND_CAT_UNCOND_BR) {
                if (decoded_bytes <= 16 && target) {
                    entry = *target;
                    follow_thunk = true;
                    break;
                }
                // Preserve the historical non-trivial tail-call acceptance,
                // but a direct target must also be in readable executable code.
                return (decoded_bytes > 8 || saw_call) &&
                    (!target || !read_code(*target).empty());
            } else if (ix.Instruction == ND_INS_UD2 || ix.Category == ND_CAT_INTERRUPT) {
                return false;
            }
            ip = next;
        }
        if (!follow_thunk) {
            return saw_call; // Same bounded-prefix acceptance as the existing scanner.
        }
    }
    return false;
}

constexpr bool may_scan_legacy_virtuals(bool source_slot_required, bool slot_validated) {
    return !source_slot_required && !slot_validated;
}
}
