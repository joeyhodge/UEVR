#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace utility::uobject {
constexpr size_t MAX_CACHED_CLASS_CHAIN_DEPTH = 128;

template <typename Range, typename NameLookup>
[[nodiscard]] bool cached_class_chain_matches(
    std::wstring_view direct_name,
    const Range& cached_chain,
    std::wstring_view filter,
    NameLookup&& lookup_name,
    size_t max_depth = MAX_CACHED_CLASS_CHAIN_DEPTH) {
    if (filter.empty()) {
        return true;
    }

    if (direct_name.find(filter) != std::wstring_view::npos) {
        return true;
    }

    size_t depth{};

    for (const auto class_pointer : cached_chain) {
        if (depth++ >= max_depth) {
            break;
        }

        // Class pointers are opaque keys here. Never dereference game memory
        // while filtering a cached UObjectHook class list.
        const auto* cached_name = lookup_name(class_pointer);

        if (cached_name != nullptr && cached_name->find(filter) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}
}
