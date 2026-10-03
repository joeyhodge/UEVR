#pragma once

#include <cstdint>
#include <limits>

namespace utility::gpu {
// A completion observation only: never reset a list, discard recorded work,
// release a swapchain image, or wait on an event as a side effect.
template<class CompletedValue>
bool references_retired(bool recorded, bool submitted, bool poisoned, bool has_fence,
    uint64_t submitted_value, CompletedValue&& completed_value) {
    if (recorded || poisoned) { return false; }
    if (!submitted) { return true; }
    if (!has_fence || submitted_value == 0) { return false; }
    const auto completed = completed_value();
    return completed != (std::numeric_limits<uint64_t>::max)() && completed >= submitted_value;
}
}
