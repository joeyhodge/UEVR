#pragma once

#include <array>
#include <cstdint>

namespace uevr::breathedge {

inline constexpr uint32_t viewport_flags_offset = 0x6c;
inline constexpr uint8_t disable_world_rendering_mask = 0x02;
inline constexpr uintptr_t draw_viewport_subobject_offset = 0x28;

// Draw receives the FCommonViewportClient subobject, not its UObject owner.
// This candidate still requires the engine's GameViewport and full liveness checks.
constexpr uintptr_t viewport_candidate_from_draw(uintptr_t dispatch) noexcept {
    return dispatch > draw_viewport_subobject_offset && dispatch % alignof(uintptr_t) == 0
        ? dispatch - draw_viewport_subobject_offset : 0;
}

// Matching 5.7.4 shipping PDB: the unreflected viewport/world bits are not
// inferred from the engine minor version or applied to later game updates.
constexpr bool validated_binary(uint32_t timestamp, uint32_t image_size) noexcept {
    return timestamp == 0xd18f68c7 && image_size == 0x0a5b0000;
}

constexpr bool supported_mode(bool native, bool synchronized, bool extreme, bool screen_2d) noexcept {
    return (native || synchronized) && !extreme && !screen_2d;
}

constexpr bool visible_inventory_root(uint8_t visibility, float opacity) noexcept {
    return (visibility == 0 || visibility == 3 || visibility == 4) && opacity > 0.0f && opacity <= 1.0f;
}

constexpr bool playable_world(uint8_t flags, int32_t next_url_length) noexcept {
    // UWorld +0x18d: bBegunPlay=bit0, bIsTearingDown=bit5.
    return (flags & 0x21) == 0x01 && next_url_length == 0;
}

struct InventorySession {
    std::array<uintptr_t, 8> objects{};
    std::array<int32_t, 8> serials{};
    uintptr_t native_viewport{};

    bool operator==(const InventorySession&) const = default;
};

constexpr bool should_restore_inventory_world(
    const InventorySession& before, const InventorySession& after, bool still_inventory) noexcept {
    return still_inventory && before.native_viewport != 0 && before == after;
}

struct InventoryObservation {
    bool game_initialized{};
    bool inventory_root{};
    bool pause_root{};
    bool auto_pause{};
    bool cutscene{};
    bool death_screen{};
    uint8_t visibility{};
    float opacity{};
    uint8_t world_flags{};
    int32_t next_url_length{};
};

constexpr bool should_enable_inventory_world(const InventoryObservation& state) noexcept {
    return state.game_initialized && state.inventory_root && !state.pause_root && !state.auto_pause &&
        !state.cutscene && !state.death_screen &&
        visible_inventory_root(state.visibility, state.opacity) && playable_world(state.world_flags, state.next_url_length);
}

constexpr uint8_t enable_world_rendering(uint8_t flags) noexcept {
    return flags & static_cast<uint8_t>(~disable_world_rendering_mask);
}

constexpr uint8_t restore_world_rendering_bit(uint8_t current, uint8_t original) noexcept {
    return enable_world_rendering(current) | (original & disable_world_rendering_mask);
}

} // namespace uevr::breathedge
