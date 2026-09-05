#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uevr::bodycam_texture {

inline constexpr uint32_t private_resource_offset = 0x110;
inline constexpr uint32_t texture_rhi_offset = 0x10;
inline constexpr uint32_t render_target_offset = 0x50;
inline constexpr uint32_t render_target_texture_offset = 0x58;
inline constexpr uint32_t owner_offset = 0x90;
inline constexpr uint32_t width_offset = 0xbc;
inline constexpr uint32_t height_offset = 0xc0;

struct ResourceIdentity {
    uintptr_t owner{};
    uintptr_t texture_rhi{};
    uintptr_t render_target_texture{};
    uint32_t width{};
    uint32_t height{};
};

constexpr bool is_initialized_resource(
    const ResourceIdentity& resource,
    uintptr_t expected_owner,
    uint32_t expected_width,
    uint32_t expected_height) noexcept {
    // UEVR creates single-sample, non-separated BGRA targets. InitRHI must
    // finish publishing both references before either target can be used.
    return expected_owner != 0 && resource.owner == expected_owner &&
        resource.texture_rhi != 0 &&
        resource.texture_rhi == resource.render_target_texture &&
        expected_width != 0 && expected_width <= 65536 &&
        expected_height != 0 && expected_height <= 65536 &&
        resource.width == expected_width && resource.height == expected_height;
}

inline constexpr std::array<uint8_t, 5> render_target_accessor{
    0x48, 0x8d, 0x41, 0x08, 0xc3}; // lea rax,[rcx+8]; ret

constexpr bool is_render_target_accessor(std::span<const uint8_t> code) noexcept {
    if (code.size() != render_target_accessor.size()) {
        return false;
    }
    for (std::size_t i = 0; i < render_target_accessor.size(); ++i) {
        if (code[i] != render_target_accessor[i]) {
            return false;
        }
    }
    return true;
}

} // namespace uevr::bodycam_texture
