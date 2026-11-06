#pragma once

#include <cstddef>

namespace utility::validation {
// Stop only when the result is decided. Every new invocation probes afresh.
template<size_t Count, size_t Required, class Predicate>
bool at_least(Predicate&& predicate) {
    static_assert(Required > 0 && Required <= Count);
    size_t accepted{};
    for (size_t i{}; i < Count; ++i) {
        if (predicate(i) && ++accepted == Required) { return true; }
        if (accepted + (Count - i - 1) < Required) { return false; }
    }
    return false;
}
}
