#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include <limits>

namespace utility::uobject {
inline std::optional<uintptr_t> browser_element(uintptr_t data, int32_t count, int32_t capacity,
    size_t stride, int32_t index) {
    if (!data || count < 0 || capacity < count || index < 0 || index >= count || !stride ||
        static_cast<size_t>(count) > (std::numeric_limits<uintptr_t>::max)() / stride) { return {}; }
    const auto bytes = static_cast<size_t>(count) * stride;
    if (data > (std::numeric_limits<uintptr_t>::max)() - bytes) { return {}; }
    return data + static_cast<size_t>(index) * stride;
}
}
