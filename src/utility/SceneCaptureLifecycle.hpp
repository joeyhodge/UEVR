#pragma once

namespace uevr::scene_capture {
struct ComponentPlan {
    bool deferred_add{};
    bool finish_after_configuration{};
};

constexpr ComponentPlan component_plan(bool legacy_factory, bool passive_holder,
                                       bool supports_deferred_configuration) {
    if (legacy_factory) {
        return {}; // The custom factory already registers its component.
    }
    if (passive_holder) {
        return {true, false}; // An actor-owned reference, never a capture request.
    }
    if (supports_deferred_configuration) {
        return {true, true};
    }
    // AddComponentByClass (or the SDK fallback) already finishes an immediate add.
    return {};
}
}
