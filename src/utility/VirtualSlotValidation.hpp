#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace utility::validation {
// Only the immediate caller may use the result. No proof survives an engine call.
template<class Read, class TableProof, class Executable>
std::optional<uintptr_t> virtual_slot(uintptr_t object, size_t slot,
    Read read, TableProof table_proof, Executable executable) {
    uintptr_t table{}, function{};
    if (!object || !read(object, table) || !table || !table_proof(table) ||
        slot > ((std::numeric_limits<uintptr_t>::max)() - table) / sizeof(uintptr_t)) {
        return std::nullopt;
    }
    // read() already checks the complete slot range; do not query it twice.
    return read(table + slot * sizeof(uintptr_t), function) && function && executable(function)
        ? std::optional{function} : std::nullopt;
}
}
