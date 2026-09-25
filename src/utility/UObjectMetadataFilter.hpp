#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace utility::uobject {
constexpr size_t MAX_CACHED_CLASS_CHAIN_DEPTH = 128;

template <typename Object>
struct CachedRecentObject {
    Object* object{};
    std::wstring full_name{};
    int32_t internal_index{-1};
    int32_t serial_number{-1};
    bool identity_valid{false};
};

template <typename Object, typename IdentityValidator>
[[nodiscard]] bool cached_recent_object_is_current(
    const CachedRecentObject<Object>& entry,
    bool tracked,
    IdentityValidator&& validate_identity) {
    if (!tracked || entry.object == nullptr || !entry.identity_valid || entry.internal_index < 0) {
        return false;
    }

    // The pointer is only an opaque identity token. The validator must resolve
    // it through FUObjectArray rather than dereferencing stale game memory.
    return validate_identity(entry.object, entry.internal_index, entry.serial_number);
}

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
