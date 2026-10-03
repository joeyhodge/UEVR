#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <algorithm>

namespace uevr::uobject::discovery {

struct AllocatorEvidence {
    uintptr_t image_base{};
    size_t image_size{};
    uintptr_t function_start{};
    uintptr_t function_end{};
    std::array<uintptr_t, 3> diagnostic_references{};
    size_t object_array_references{};
    size_t direct_callers{};
    bool executable{};
    bool unwind_matches{};
};

inline bool validates_allocator_evidence(const AllocatorEvidence& evidence) {
    if (evidence.image_base == 0 || evidence.image_size < 0x100 ||
        evidence.image_size > std::numeric_limits<uintptr_t>::max() - evidence.image_base) {
        return false;
    }

    const auto image_end = evidence.image_base + evidence.image_size;
    if (!evidence.executable || !evidence.unwind_matches ||
        evidence.function_start < evidence.image_base || evidence.function_end > image_end ||
        evidence.function_end <= evidence.function_start ||
        evidence.function_end - evidence.function_start < 0x100 ||
        evidence.function_end - evidence.function_start > 0x2000 ||
        evidence.object_array_references < 3 || evidence.direct_callers == 0) {
        return false;
    }

    for (const auto reference : evidence.diagnostic_references) {
        if (reference < evidence.function_start || reference >= evidence.function_end) {
            return false;
        }
    }

    return true;
}

// Source + Townfall disassembly: clear the 24-byte FUObjectItem in RBX, then
// invalidate the object's InternalIndex at +0xC. Called only at a decoded
// instruction boundary inside an unwind owner of the FreeUObjectIndex diagnostic.
inline bool validates_townfall_free_stores(std::span<const uint8_t> bytes) {
    constexpr std::array<uint8_t, 23> clear_item{
        0x48,0xC7,0x43,0x08,0,0,0,0, 0x48,0xC7,0x43,0x10,0,0,0,0,
        0x48,0xC7,0x03,0,0,0,0};
    return bytes.size() >= 30 && std::equal(clear_item.begin(), clear_item.end(), bytes.begin()) &&
        bytes[23] == 0xC7 && (bytes[24] == 0x45 || bytes[24] == 0x46) && bytes[25] == 0x0C &&
        bytes[26] == 0xFF && bytes[27] == 0xFF && bytes[28] == 0xFF && bytes[29] == 0xFF;
}

} // namespace uevr::uobject::discovery
