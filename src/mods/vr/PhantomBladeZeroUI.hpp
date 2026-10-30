#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include "UE58OwnedUITexture.hpp"

namespace uevr::pbz_ui {

// Verified in PBZ's resource destructor, dimension getters and secondary vtable.
inline constexpr uintptr_t render_target_offset = 0x50;
inline constexpr uintptr_t render_target_texture_offset = 0x58;
inline constexpr uintptr_t owner_offset = 0x90;
inline constexpr uintptr_t width_offset = 0xbc;
inline constexpr uintptr_t height_offset = 0xc0;
inline constexpr uintptr_t resource_size = 0xd0;
inline constexpr auto size_x_accessor = std::to_array<uint8_t>({0x8b, 0x81, 0xbc, 0, 0, 0, 0xc3});
inline constexpr auto size_y_accessor = std::to_array<uint8_t>({0x8b, 0x81, 0xc0, 0, 0, 0, 0xc3});

// Read only the non-linked resource path. Never execute the fallback vcall.
inline constexpr auto native_resource_prefix = std::to_array<uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xd0, 0, 0, 0,
    0x33, 0xdb, 0x48, 0x85, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x58, 0x20,
    0x48, 0x85, 0xdb});

// The hook's first instructions are patched; validate the untouched input ABI.
inline constexpr uintptr_t slate_inputs_offset = 0x36;
inline constexpr auto slate_inputs_contract = std::to_array<uint8_t>({
    0x49, 0x8b, 0x59, 0x18, 0x4c, 0x8b, 0xf1, 0x49, 0x8b, 0x41, 0x08,
    0x49, 0x8b, 0xf9, 0x48, 0x89, 0x55, 0x28, 0x4d, 0x8b, 0xe8});

constexpr bool valid_extent(uint32_t width, uint32_t height) {
    return width >= 320 && height >= 240 && width <= 16384 && height <= 16384 &&
        uint64_t{width} * height <= 64ull * 1024 * 1024;
}

struct Creation {
    bool exact_game{}, dx12{}, inputs_proven{}, game_data{}, engine{}, stable_slate{}, main_target{};
    uint32_t width{}, height{};
};

constexpr bool can_request(const Creation& state) {
    // Menus may not construct any scene family. Do not wait for that callback.
    return state.exact_game && state.dx12 && state.inputs_proven && state.game_data &&
        state.engine && state.stable_slate && state.main_target && valid_extent(state.width, state.height);
}

template <typename Read, typename Validate>
std::optional<ue58_owned_ui::Resource> find_owned_resource(
    uintptr_t owner, size_t owner_size, uint32_t width, uint32_t height,
    const Read& read, const Validate& validate) {
    constexpr auto limit = (std::numeric_limits<uintptr_t>::max)();
    if (owner < 0x10000 || owner_size < 2 * sizeof(uintptr_t) ||
        owner_size > ue58_owned_ui::max_owner_size || owner > limit - owner_size ||
        !valid_extent(width, height))
    {
        return std::nullopt;
    }

    std::optional<ue58_owned_ui::Resource> result;
    for (uintptr_t offset = sizeof(uintptr_t); offset + 2 * sizeof(uintptr_t) <= owner_size; offset += sizeof(uintptr_t)) {
        uintptr_t resource{}, render_resource{}, actual_owner{}, rhi{}, target_rhi{};
        uint32_t actual_width{}, actual_height{};
        if (!read(owner + offset, resource) || !read(owner + offset + sizeof(uintptr_t), render_resource)) {
            return std::nullopt;
        }
        if (resource < 0x10000 || resource != render_resource || (resource & 7) != 0 ||
            resource > limit - resource_size)
        {
            continue;
        }
        if (!read(resource + owner_offset, actual_owner) || actual_owner != owner ||
            !read(resource + ue58_owned_ui::texture_rhi_offset, rhi) || rhi < 0x10000 || (rhi & 7) != 0 ||
            !read(resource + render_target_texture_offset, target_rhi) || target_rhi != rhi ||
            !read(resource + width_offset, actual_width) || actual_width != width ||
            !read(resource + height_offset, actual_height) || actual_height != height ||
            !validate(resource, rhi))
        {
            continue;
        }
        if (result) {
            return std::nullopt;
        }
        result = ue58_owned_ui::Resource{offset, resource, rhi, owner_size};
    }
    return result;
}

template <typename Read>
bool resource_matches(uintptr_t owner, const ue58_owned_ui::Resource& expected,
    uint32_t width, uint32_t height, const Read& read) {
    constexpr auto limit = (std::numeric_limits<uintptr_t>::max)();
    if (owner < 0x10000 || expected.owner_size < 2 * sizeof(uintptr_t) ||
        expected.owner_size > ue58_owned_ui::max_owner_size || owner > limit - expected.owner_size ||
        expected.private_resource_offset < sizeof(uintptr_t) || (expected.private_resource_offset & 7) != 0 ||
        expected.private_resource_offset > expected.owner_size - 2 * sizeof(uintptr_t) ||
        expected.resource < 0x10000 || expected.resource > limit - resource_size ||
        expected.rhi_texture < 0x10000 || !valid_extent(width, height))
    {
        return false;
    }
    uintptr_t resource{}, rt_resource{}, actual_owner{}, rhi{}, target_rhi{};
    uint32_t actual_width{}, actual_height{};
    return read(owner + expected.private_resource_offset, resource) && resource == expected.resource &&
        read(owner + expected.private_resource_offset + sizeof(uintptr_t), rt_resource) && rt_resource == resource &&
        read(resource + owner_offset, actual_owner) && actual_owner == owner &&
        read(resource + ue58_owned_ui::texture_rhi_offset, rhi) && rhi == expected.rhi_texture &&
        read(resource + render_target_texture_offset, target_rhi) && target_rhi == rhi &&
        read(resource + width_offset, actual_width) && actual_width == width &&
        read(resource + height_offset, actual_height) && actual_height == height;
}

} // namespace uevr::pbz_ui
