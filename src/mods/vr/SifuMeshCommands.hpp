#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace uevr::sifu {

// Exact shipping EXE/PDB pair captured on 2026-09-13; unknown revisions stay unchanged.
inline constexpr uint32_t mesh_command_timestamp = 0x68409AB0;
inline constexpr uint32_t mesh_command_image_size = 0x06295000;
inline constexpr uint32_t cached_render_thread_rva = 0x025980C0;
inline constexpr uint32_t cached_any_thread_rva = 0x025980D0;
inline constexpr uint32_t cached_relevance_call_rva = 0x025A5235;

inline constexpr std::array<uint8_t, 15> cached_render_thread_code{
    0x48, 0x8B, 0x05, 0xA1, 0xAC, 0x66, 0x03, 0x83,
    0x78, 0x04, 0x00, 0x0F, 0x9F, 0xC0, 0xC3};
inline constexpr std::array<uint8_t, 66> cached_any_thread_code{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x3D,
    0xA3, 0x68, 0x59, 0x03, 0x00, 0x48, 0x8B, 0x1D,
    0x84, 0xAC, 0x66, 0x03, 0x74, 0x1E, 0xFF, 0x15,
    0xD4, 0x46, 0xCB, 0x01, 0x33, 0xC9, 0x3B, 0x05,
    0x7C, 0x68, 0x59, 0x03, 0x0F, 0x95, 0xC1, 0x83,
    0x3C, 0x8B, 0x00, 0x0F, 0x9F, 0xC0, 0x48, 0x83,
    0xC4, 0x20, 0x5B, 0xC3, 0x33, 0xC9, 0x39, 0x0C,
    0x8B, 0x0F, 0x9F, 0xC0, 0x48, 0x83, 0xC4, 0x20,
    0x5B, 0xC3};
inline constexpr std::array<uint8_t, 11> cached_relevance_call_code{
    0xE8, 0x86, 0x2E, 0xFF, 0xFF, 0x88, 0x83, 0xF4,
    0xF2, 0x03, 0x00};

inline bool validate_mesh_command_code(uint32_t timestamp, uint32_t image_size,
    std::span<const uint8_t> render_thread, std::span<const uint8_t> any_thread,
    std::span<const uint8_t> relevance_call) {
    const auto matches = [](auto code, auto expected) {
        return code.size() == expected.size() &&
            std::equal(expected.begin(), expected.end(), code.begin());
    };
    return timestamp == mesh_command_timestamp && image_size == mesh_command_image_size &&
        matches(render_thread, cached_render_thread_code) &&
        matches(any_thread, cached_any_thread_code) &&
        matches(relevance_call, cached_relevance_call_code);
}

inline constexpr bool rebuild_native_mesh_commands(
    bool validated_hooks, bool stereo_enabled, bool native_stereo) {
    return validated_hooks && stereo_enabled && native_stereo;
}

template <typename Original>
inline bool select_cached_mesh_commands(bool rebuild, Original&& original) {
    return !rebuild && original();
}

} // namespace uevr::sifu
