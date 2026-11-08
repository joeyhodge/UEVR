#pragma once

#include <cstdint>

namespace uevr::prospi::slate_validation {
struct Inputs {
    bool targeted_native_fix{}, stable_draw{}, scene_ready{}, ui_ready{};
    uintptr_t current_table{}, selected_table{};
};

// This does not validate/adopt the engine texture. Its type proof is unused
// while both established destinations exist and the selected RHI table is unchanged.
inline bool may_defer_unused_proof(const Inputs& in) {
    return in.targeted_native_fix && in.stable_draw && in.scene_ready && in.ui_ready &&
        in.current_table && in.current_table == in.selected_table;
}

inline bool needs_late_proof(bool deferred, bool ui_ready, uintptr_t engine, uintptr_t scene) {
    return deferred && !ui_ready && engine && engine != scene;
}
}
