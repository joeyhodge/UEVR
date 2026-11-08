#pragma once

#include <sdk/ObjectIdentity.hpp>

namespace uevr::prospi::observation {
inline bool published(const sdk::object_liveness::Identity& id,
    const sdk::object_array::Layout* layout, const void* header) {
    return layout != nullptr && header != nullptr && id.layout == layout && id.header_layout == header;
}

// Consume only the just-returned, double-witnessed SDK observation. Retained
// identities and checks after native calls still use sdk::is_current_object.
inline std::optional<sdk::object_liveness::Identity> fresh(
    const std::optional<sdk::object_liveness::Observation>& value, uintptr_t requested,
    const sdk::object_array::Layout* layout, const void* header,
    sdk::object_liveness::Policy policy) {
    if (!value || !requested || value->identity.object != requested ||
        !value->identity.vtable || value->identity.index < 0 ||
        !published(value->identity, layout, header) ||
        sdk::object_liveness::rejected(policy, value->flags)) { return std::nullopt; }
    return value->identity;
}
}
