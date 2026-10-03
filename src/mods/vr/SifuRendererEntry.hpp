#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <sdk/SceneDiscoveryState.hpp>
#include "mods/GameSpecific.hpp"

namespace uevr::sifu {

inline bool is_supported_runtime(
    std::wstring_view path, uint32_t version_ms, uint32_t version_ls, bool dx11) {
    if (version_ms != 0x0004001A || version_ls != 0x00020000 || !dx11) {
        return false;
    }
    const auto lowered = games::lowercase_path(path);
    const auto separator = lowered.find_last_of(L"/\\");
    const auto filename = std::wstring_view{lowered}.substr(
        separator == std::wstring::npos ? 0 : separator + 1);
    return filename == L"sifu-win64-shipping.exe";
}

inline bool should_use_native_fix_renderer(
    std::wstring_view path, uint32_t version_ms, uint32_t version_ls, bool dx11, bool native_fix) {
    return native_fix && is_supported_runtime(path, version_ms, version_ls, dx11);
}

inline bool matches_family_layout(const sdk::detail::FamilyLayout& layout) {
    // Corroborate the observed offsets; never install or overwrite SDK offsets here.
    return layout.has_vtable == false && layout.views == 0 && layout.render_target == 0x18 &&
        layout.scene_interface == 0x20 && layout.frame_count == 0xBC;
}

inline bool is_distinct_renderer_entry(uintptr_t entry, uintptr_t viewport_draw) {
    return entry != 0 && viewport_draw != 0 && entry != viewport_draw;
}

// Matching UE4.26.2 PDB/EXE: family R8 -> RDI, canvas RDX -> R13.
inline constexpr std::array<uint8_t, 34> renderer_entry_prefix{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00,
    0x00, 0x00, 0x49, 0x8B, 0x48, 0x20, 0x45, 0x33,
    0xE4, 0x49, 0x8B, 0xF8, 0x4C, 0x8B, 0xEA, 0x41,
    0x8B, 0xEC};
inline constexpr std::array<uint8_t, 24> renderer_root_unwind{
    0x01, 0x12, 0x09, 0x00, 0x12, 0x01, 0x18, 0x00,
    0x0B, 0xF0, 0x09, 0xD0, 0x07, 0xC0, 0x05, 0x70,
    0x04, 0x60, 0x03, 0x50, 0x02, 0x30, 0x00, 0x00};

// Sifu's extra show flags move FrameNumber to +0xbc (disp32), and the
// ViewExtensions array/count to +0xe0/+0xe8. Slot 5 receives the same family.
inline constexpr std::array<uint8_t, 76> renderer_family_loop{
    0x89, 0x87, 0xBC, 0x00, 0x00, 0x00, 0x39, 0x9F,
    0xE8, 0x00, 0x00, 0x00, 0x7E, 0x3E, 0x4C, 0x89,
    0xB4, 0x24, 0x00, 0x01, 0x00, 0x00, 0x4D, 0x8B,
    0xF4, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0x87, 0xE0, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xD7, 0x49, 0x8B, 0x0C, 0x06,
    0x48, 0x8B, 0x01, 0xFF, 0x50, 0x28, 0xFF, 0xC3,
    0x4D, 0x8D, 0x76, 0x10, 0x3B, 0x9F, 0xE8, 0x00,
    0x00, 0x00, 0x7C, 0xDE, 0x4C, 0x8B, 0xB4, 0x24,
    0x00, 0x01, 0x00, 0x00};
inline constexpr size_t renderer_loop_callback_return = 54;

inline bool has_callable_renderer_entry(
    std::span<const uint8_t> code, std::span<const uint8_t> unwind, size_t callback_return_offset) {
    if (code.size() < renderer_entry_prefix.size() || code.size() > 0x4000 ||
        unwind.size() < renderer_root_unwind.size() || callback_return_offset >= code.size() ||
        callback_return_offset < renderer_loop_callback_return ||
        !std::equal(renderer_entry_prefix.begin(), renderer_entry_prefix.end(), code.begin()) ||
        !std::equal(renderer_root_unwind.begin(), renderer_root_unwind.end(), unwind.begin())) {
        return false;
    }

    const auto loop_offset = callback_return_offset - renderer_loop_callback_return;
    return loop_offset >= renderer_entry_prefix.size() &&
        renderer_family_loop.size() <= code.size() - loop_offset &&
        std::equal(renderer_family_loop.begin(), renderer_family_loop.end(), code.begin() + loop_offset);
}

} // namespace uevr::sifu
