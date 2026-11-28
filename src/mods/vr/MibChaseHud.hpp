#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uevr::mib_chase_hud {

constexpr bool executable_matches(std::wstring_view path) noexcept {
    path.remove_prefix(path.find_last_of(L"/\\") + 1);
    constexpr std::wstring_view name = L"mibchase-win64-shipping.exe";
    if (path.size() != name.size()) { return false; }
    for (size_t i = 0; i < name.size(); ++i) {
        const auto c = path[i];
        if ((c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c) != name[i]) { return false; }
    }
    return true;
}

inline constexpr uint32_t image_timestamp = 0x04bce40a;
inline constexpr uint32_t image_size = 0x0bac1000;
inline constexpr uintptr_t hud_draw_rva = 0x4d68490;
inline constexpr uintptr_t hud_draw_slot_rva = 0x936dba8 + 0x7c8;
inline constexpr uintptr_t hud_draw_return_rva = 0x2c235ce;
inline constexpr uintptr_t flush_canvas_rva = 0x31c82f0;
inline constexpr uintptr_t tile_draw_rva = 0x31bf1a0;
inline constexpr uintptr_t tile_draw_slot_rva = 0x8b53d50 + 0x18;
inline constexpr uintptr_t tile_draw_return_rva = 0x31c051c;
inline constexpr uintptr_t render_target_rdg_rva = 0x31ca4b0;
inline constexpr uintptr_t texture_resource_vtable_rva = 0x8d9e2f8;
inline constexpr uintptr_t render_target_vtable_rva = 0x8d9e3c0;
inline constexpr size_t render_target_rdg_slot = 1;
inline constexpr uintptr_t object_array_rva = 0xae269c0;
inline constexpr uintptr_t set_root_flags_rva = 0x1635a10;
inline constexpr uintptr_t clear_root_flags_rva = 0x161b1b0;
inline constexpr uint32_t root_set_flag = 0x40000000;

constexpr bool owned_canvas_target(uintptr_t image, uintptr_t resource_table, uintptr_t target_table,
    uintptr_t rdg_accessor) noexcept
{
    return image != 0 && resource_table == image + texture_resource_vtable_rva &&
        target_table == image + render_target_vtable_rva && rdg_accessor == image + render_target_rdg_rva;
}

struct Extent {
    int32_t width{}, height{};
    constexpr bool valid() const noexcept {
        return width >= 128 && height >= 128 && width <= 16384 && height <= 16384;
    }
    constexpr bool operator==(const Extent&) const = default;
};

struct Rect {
    int32_t left{}, top{}, right{}, bottom{};
    constexpr bool operator==(const Rect&) const = default;
};

using Matrix = std::array<double, 16>;

inline bool window_projection(const Matrix& base, const Matrix& top, Extent window) noexcept {
    if (!window.valid()) { return false; }
    for (size_t i = 0; i < base.size(); ++i) {
        if (!std::isfinite(base[i]) || !std::isfinite(top[i]) || std::abs(base[i] - top[i]) > 1e-8) {
            return false;
        }
    }
    // The renderer can remap depth; only the window-space XY and W basis matter here.
    const auto matches_element = [&](size_t i, double expected) {
        return std::abs(base[i] - expected) <= 1e-7;
    };
    return matches_element(0, 2.0 / window.width) && matches_element(5, -2.0 / window.height) &&
        matches_element(12, -1.0) && matches_element(13, 1.0) && matches_element(15, 1.0) &&
        matches_element(1, 0.0) && matches_element(3, 0.0) && matches_element(4, 0.0) && matches_element(7, 0.0) &&
        matches_element(8, 0.0) && matches_element(9, 0.0) && matches_element(11, 0.0);
}

constexpr bool can_redirect(
    Extent window, Extent canvas, Extent eye, Rect window_rect, Rect scissor, Rect view,
    int32_t queued_elements, bool hit_testing) noexcept
{
    const Rect target{0, 0, window.width, window.height};
    return window.valid() && eye.valid() && canvas == eye &&
        window_rect == target && (scissor == Rect{} || scissor == target) &&
        view == Rect{0, 0, eye.width, eye.height} && queued_elements == 0 && !hit_testing;
}

constexpr int32_t tile_blend(int32_t current, bool scoped) noexcept {
    constexpr int32_t translucent = 2;
    constexpr int32_t alpha_blend = 11;
    return scoped && current == translucent ? alpha_blend : current;
}

} // namespace uevr::mib_chase_hud
