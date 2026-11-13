#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace uevr::dune_menu {

inline constexpr uintptr_t draw_subobject_offset = 0x28;
inline constexpr uint32_t viewport_flags_offset = 0x6c;
inline constexpr uint32_t disable_world_mask = 0x04;
inline constexpr int64_t maximum_observation_age_ns = 1'000'000'000;

// Independently checked in Dune's Draw and its reflected world-rendering
// getter/setter. These are read-only contracts, not permission to change flags.
inline constexpr auto read_code = std::to_array<uint8_t>({
    0x8b,0x40,0x6c,0xc1,0xe8,0x02,0xf6,0xd0,0x24,0x01,0x88,0x06});
inline constexpr auto write_code = std::to_array<uint8_t>({
    0x8b,0x48,0x6c,0x40,0x0f,0xb6,0xd6,0x83,0xe1,0xfb,
    0x83,0xf2,0x01,0xc1,0xe2,0x02,0x0b,0xd1,0x89,0x50,0x6c});
inline constexpr auto draw_test = std::to_array<uint8_t>({0xf6,0x47,0x44,0x04,0x0f,0x85});
inline constexpr auto draw_this = std::to_array<uint8_t>({0x48,0x8b,0xf9});
inline constexpr auto draw_render = std::to_array<uint8_t>({0x4c,0x8b,0x08,0x41,0xff,0x51,0x40});
inline constexpr auto draw_cleanup = std::to_array<uint8_t>({0x48,0x8b,0x10,0xff,0x92,0x10,0x01,0,0});

template <size_t N>
std::optional<size_t> unique(std::span<const uint8_t> code, const std::array<uint8_t, N>& pattern) {
    const auto found = std::search(code.begin(), code.end(), pattern.begin(), pattern.end());
    if (found == code.end() || std::search(found + 1, code.end(), pattern.begin(), pattern.end()) != code.end()) { return {}; }
    return static_cast<size_t>(found - code.begin());
}

inline bool valid_draw(std::span<const uint8_t> code) {
    if (code.size() > 0x4000) { return false; }
    const auto self = unique(code.first(std::min<size_t>(code.size(), 0x100)), draw_this);
    const auto gate = unique(code, draw_test), render = unique(code, draw_render), cleanup = unique(code, draw_cleanup);
    if (!self || !gate || !render || !cleanup || *self >= *gate || *gate + 10 > code.size() ||
        *gate + 10 >= *render || *render >= *cleanup || *cleanup - *gate > 0x400) { return false; }
    int32_t skip{};
    std::memcpy(&skip, code.data() + *gate + 6, sizeof(skip));
    return skip > 0 && static_cast<uint64_t>(*gate + 10) + static_cast<uint32_t>(skip) < *render;
}

constexpr uintptr_t canonical_client(uintptr_t dispatch) {
    return dispatch > draw_subobject_offset && !(dispatch % alignof(uintptr_t))
        ? dispatch - draw_subobject_offset : 0;
}

struct Observation {
    uint64_t generation{}, request{};
    uintptr_t viewport{}, client{};
    int32_t client_serial{};
    int64_t observed_ns{};
    bool world_disabled{};
};

constexpr bool current(const Observation& value, uint64_t generation, uint64_t request,
    uintptr_t viewport, int64_t now_ns) {
    return generation && request && viewport && value.client && value.generation == generation &&
        value.request == request && value.viewport == viewport && value.observed_ns > 0 &&
        now_ns >= value.observed_ns && now_ns - value.observed_ns <= maximum_observation_age_ns;
}

constexpr bool same_owner(const Observation& before, const Observation& after) {
    return before.generation == after.generation && before.request == after.request &&
        before.viewport == after.viewport && before.client == after.client && before.client_serial == after.client_serial;
}

struct Frame {
    Observation owner{};
    uint64_t session_generation{};
    int64_t display_time{};
    uint64_t serial{};
    uint64_t wait_sequence{};
    bool should_render{};
};

constexpr bool same_frame(const Frame& frame, uint64_t session_generation, uint64_t wait_sequence) {
    return frame.serial && frame.session_generation && frame.session_generation == session_generation &&
        frame.display_time > 0 && frame.wait_sequence && frame.wait_sequence == wait_sequence;
}

constexpr bool same_lease(const Frame& expected, const Frame& value) {
    return expected.serial && expected.serial == value.serial &&
        same_frame(value, expected.session_generation, expected.wait_sequence) &&
        expected.display_time == value.display_time && expected.should_render == value.should_render &&
        same_owner(expected.owner, value.owner);
}

// UI-only waits cannot authorize a retained 3D scene. Keep this floor after
// closing the menu; the next real family must have consumed a newer XR pose.
struct SceneCutoff {
    int64_t display_time{};
    void observe(int64_t value) { if (value > display_time) { display_time = value; } }
    bool allows(int64_t value) const { return display_time == 0 || value > display_time; }
    void reset() { display_time = 0; }
};

constexpr bool ui_extent(uint64_t width, uint32_t height, uint16_t array_size, uint16_t mips,
    uint32_t samples, uint64_t destination_width, uint32_t destination_height) {
    return width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
        width == destination_width && height == destination_height && array_size == 1 && mips == 1 && samples == 1;
}

} // namespace uevr::dune_menu
