#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

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
[[nodiscard]] bool cached_object_identity_is_current(
    Object* object,
    int32_t internal_index,
    int32_t serial_number,
    bool identity_valid,
    bool tracked,
    bool require_identity,
    IdentityValidator&& validate_identity) {
    if (!tracked || object == nullptr) {
        return false;
    }

    // Preserve legacy layouts that cannot expose an FUObjectArray identity,
    // unless the caller explicitly requires fail-closed validation.
    if (!identity_valid) {
        return !require_identity;
    }

    if (internal_index < 0) {
        return false;
    }

    // The pointer remains an opaque token; only the array slot is inspected.
    return validate_identity(object, internal_index, serial_number);
}

template <typename Object, typename IdentityValidator>
[[nodiscard]] bool cached_recent_object_is_current(
    const CachedRecentObject<Object>& entry,
    bool tracked,
    IdentityValidator&& validate_identity) {
    return cached_object_identity_is_current(
        entry.object,
        entry.internal_index,
        entry.serial_number,
        entry.identity_valid,
        tracked,
        true,
        std::forward<IdentityValidator>(validate_identity));
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
