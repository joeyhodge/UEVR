#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "utility/ProtectedHookTrampoline.hpp"

namespace uevr::hifi {

inline bool should_use_native_fix_renderer(
    std::wstring_view path, bool hbk_ue427, bool dx12, bool native_fix) {
    return matches_game(path, hbk_ue427) && dx12 && native_fix;
}

inline bool is_distinct_renderer_entry(uintptr_t entry, uintptr_t viewport_draw) {
    return entry != 0 && viewport_draw != 0 && entry != viewport_draw;
}

// The HBK renderer keeps the family from R8 in RDI and the canvas from RDX in R15.
inline constexpr std::array<uint8_t, 32> renderer_entry_prefix{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xD0,
    0x00, 0x00, 0x00, 0x49, 0x8B, 0x48, 0x20, 0x45,
    0x33, 0xE4, 0x49, 0x8B, 0xF8, 0x4C, 0x8B, 0xFA};
inline constexpr std::array<uint8_t, 24> renderer_root_unwind{
    0x01, 0x13, 0x09, 0x00, 0x13, 0x34, 0x21, 0x00,
    0x13, 0x01, 0x1A, 0x00, 0x0C, 0xF0, 0x0A, 0xC0,
    0x08, 0x70, 0x07, 0x60, 0x06, 0x50, 0x00, 0x00};

// FrameNumber (+0x5c), ViewExtensions (+0x88/+0x90), and slot 5 with that same
// family in RDX. The callback lives in a short CHAININFO child, not an ABI entry.
inline constexpr std::array<uint8_t, 74> renderer_family_loop{
    0x89, 0x47, 0x5C, 0x39, 0x9F, 0x90, 0x00, 0x00,
    0x00, 0x7E, 0x3F, 0x4C, 0x89, 0xB4, 0x24, 0x00,
    0x01, 0x00, 0x00, 0x4D, 0x8B, 0xF4, 0x66, 0x66,
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x87, 0x88, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xD7, 0x49, 0x8B, 0x0C, 0x06, 0x48, 0x8B,
    0x01, 0xFF, 0x50, 0x28, 0xFF, 0xC3, 0x4D, 0x8D,
    0x76, 0x10, 0x3B, 0x9F, 0x90, 0x00, 0x00, 0x00,
    0x7C, 0xDE, 0x4C, 0x8B, 0xB4, 0x24, 0x00, 0x01,
    0x00, 0x00};
inline constexpr size_t renderer_loop_callback_return = 52;

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

} // namespace uevr::hifi
