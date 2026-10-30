#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <sdk/SceneDiscoveryState.hpp>
#include "mods/GameSpecific.hpp"

namespace uevr::b1 {

inline bool is_supported_runtime(
    std::wstring_view path, uint32_t version_ms, uint32_t version_ls, bool dx12) {
    if (!dx12 || version_ms != 0x00050000 || version_ls != 0) {
        return false;
    }
    const auto lowered = games::lowercase_path(path);
    const auto separator = lowered.find_last_of(L"/\\");
    const auto filename = std::wstring_view{lowered}.substr(
        separator == std::wstring::npos ? 0 : separator + 1);
    return filename == L"b1-win64-shipping.exe";
}

inline bool matches_family_layout(const sdk::detail::FamilyLayout& layout) {
    return layout.has_vtable == true && layout.vtable && *layout.vtable != 0 &&
        layout.views == 0x08 && layout.render_target == 0x20 &&
        layout.scene_interface == 0x28 && layout.frame_count == 0x7C;
}

inline bool is_distinct_renderer_entry(uintptr_t entry, uintptr_t viewport_draw) {
    return entry != 0 && viewport_draw != 0 && entry != viewport_draw;
}

// The callback's chained range and root total only 0x193 bytes in this build.
// The generic 0x200 size floor skips it and can select FViewport::Draw instead.
inline constexpr std::array<uint8_t, 16> renderer_entry_prefix{
    0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
inline constexpr std::array<uint8_t, 16> renderer_root_unwind{
    0x01, 0x10, 0x06, 0x00, 0x10, 0xF2, 0x09, 0xF0,
    0x07, 0xE0, 0x05, 0xC0, 0x03, 0x70, 0x02, 0x60};
inline constexpr std::array<uint8_t, 3> save_family{0x49, 0x8B, 0xF8}; // R8 -> RDI
inline constexpr std::array<uint8_t, 3> save_canvas{0x4C, 0x8B, 0xFA}; // RDX -> R15
inline constexpr std::array<uint8_t, 16> renderer_scene_lookup{
    0x48, 0x8B, 0x4F, 0x28, 0x45, 0x33, 0xF6, 0x48,
    0x8B, 0x01, 0xFF, 0x90, 0x28, 0x03, 0x00, 0x00};

// FrameNumber +0x7c, ViewExtensions +0xb0/+0xb8, and slot 5 called with
// the original family in RDX. Match the actual callback, not another stack frame.
inline constexpr std::array<uint8_t, 54> renderer_family_loop{
    0x89, 0x47, 0x7C, 0x39, 0xAF, 0xB8, 0x00, 0x00,
    0x00, 0x7E, 0x2B, 0x33, 0xDB, 0x0F, 0x1F, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x87, 0xB0,
    0x00, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x48, 0x8B,
    0x0C, 0x03, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x28,
    0xFF, 0xC5, 0x48, 0x8D, 0x5B, 0x10, 0x3B, 0xAF,
    0xB8, 0x00, 0x00, 0x00, 0x7C, 0xDE};
inline constexpr size_t renderer_loop_callback_return = 40;

template <size_t N>
inline bool matches_at(std::span<const uint8_t> code, size_t offset, const std::array<uint8_t, N>& bytes) {
    return offset <= code.size() && N <= code.size() - offset &&
        std::equal(bytes.begin(), bytes.end(), code.begin() + offset);
}

inline bool has_callable_renderer_entry(
    std::span<const uint8_t> code, std::span<const uint8_t> unwind, size_t callback_return_offset) {
    if (code.size() > 0x4000 || callback_return_offset >= code.size() ||
        callback_return_offset < renderer_loop_callback_return ||
        !matches_at(code, 0, renderer_entry_prefix) ||
        !matches_at(unwind, 0, renderer_root_unwind) ||
        !matches_at(code, 0x16, save_family) || !matches_at(code, 0x1F, save_canvas) ||
        !matches_at(code, 0x4B, renderer_scene_lookup)) {
        return false;
    }
    const auto loop_offset = callback_return_offset - renderer_loop_callback_return;
    return loop_offset >= 0x4B + renderer_scene_lookup.size() &&
        matches_at(code, loop_offset, renderer_family_loop);
}

} // namespace uevr::b1
