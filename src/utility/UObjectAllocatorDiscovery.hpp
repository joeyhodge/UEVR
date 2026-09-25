#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

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

} // namespace uevr::uobject::discovery
