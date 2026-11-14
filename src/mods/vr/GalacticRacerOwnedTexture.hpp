#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sdk/GalacticRacerRuntime.hpp>

namespace uevr::swgr_owned {
// UE5.7.4 source order, independently confirmed by SWGR's resource constructor
// and accessors. Never infer PrivateResource from an unrelated RHI-looking field.
inline constexpr uintptr_t render_target_offset = 0x50;
inline constexpr uintptr_t owner_offset = 0x90;
inline constexpr uintptr_t width_offset = 0xbc;
inline constexpr uintptr_t height_offset = 0xc0;
inline constexpr size_t max_owner_size = 0x300;
inline constexpr std::array<uint8_t, 7> size_x_code{0x8b,0x81,0xbc,0,0,0,0xc3};
inline constexpr std::array<uint8_t, 7> size_y_code{0x8b,0x81,0xc0,0,0,0,0xc3};
inline constexpr std::array<uint8_t, 5> texture_code{0x48,0x8d,0x41,0x08,0xc3};

struct Resource {
    uintptr_t private_resource_offset{}, resource{}, rhi_texture{};
    size_t owner_size{};
    bool operator==(const Resource&) const = default;
};

template<size_t N>
bool accessor(const sdk::discovery::Memory& m, uintptr_t object, uint32_t slot,
    const std::array<uint8_t, N>& code) {
    uintptr_t table{}, fn{};
    std::array<uint8_t, N> mask{};
    mask.fill(0xff);
    return m.load(object, table) && sdk::galactic_racer::pointer(table) &&
        m.load(table + slot * sizeof(uintptr_t), fn) &&
        sdk::galactic_racer::code_matches(m, fn, code, mask);
}

inline bool resource_matches(const sdk::discovery::Memory& m, uintptr_t owner,
    const Resource& expected, uint32_t width, uint32_t height) {
    using sdk::galactic_racer::pointer;
    if (!pointer(owner) || expected.owner_size < 2 * sizeof(uintptr_t) || expected.owner_size > max_owner_size ||
        expected.private_resource_offset < sizeof(uintptr_t) ||
        expected.private_resource_offset > expected.owner_size - 2 * sizeof(uintptr_t) ||
        (expected.private_resource_offset & 7) || !pointer(expected.resource) || !pointer(expected.rhi_texture) ||
        width == 0 || height == 0 || width > 65536 || height > 65536) { return false; }
    std::array<uintptr_t, 2> owner_refs{};
    uintptr_t actual_owner{}, texture_rhi{}, target_rhi{};
    std::array<uint32_t, 2> extent{};
    return m.load(owner + expected.private_resource_offset, owner_refs) &&
        owner_refs[0] == expected.resource && owner_refs[1] == expected.resource &&
        m.load(expected.resource + owner_offset, actual_owner) && actual_owner == owner &&
        m.load(expected.resource + 0x10, texture_rhi) && texture_rhi == expected.rhi_texture &&
        m.load(expected.resource + render_target_offset + 8, target_rhi) && target_rhi == texture_rhi &&
        m.load(expected.resource + width_offset, extent) && extent[0] == width && extent[1] == height;
}

inline std::optional<Resource> find_resource(const sdk::discovery::Memory& m,
    uintptr_t owner, size_t owner_size, uint32_t width, uint32_t height) {
    if (!sdk::galactic_racer::pointer(owner) || owner_size < 2 * sizeof(uintptr_t) || owner_size > max_owner_size ||
        width == 0 || height == 0 || width > 65536 || height > 65536) {
        return {};
    }
    std::optional<Resource> result{};
    for (uintptr_t offset = sizeof(uintptr_t); offset + 2 * sizeof(uintptr_t) <= owner_size; offset += sizeof(uintptr_t)) {
        std::array<uintptr_t, 2> refs{};
        if (!m.load(owner + offset, refs)) { return {}; }
        if (refs[0] == 0 || refs[0] != refs[1]) { continue; }
        uintptr_t rhi{};
        Resource candidate{offset, refs[0], 0, owner_size};
        if (!sdk::galactic_racer::pointer(candidate.resource) || !m.load(candidate.resource + 0x10, rhi)) { continue; }
        candidate.rhi_texture = rhi;
        if (!resource_matches(m, owner, candidate, width, height) ||
            !accessor(m, candidate.resource, 6, size_x_code) ||
            !accessor(m, candidate.resource, 7, size_y_code) ||
            !accessor(m, candidate.resource + render_target_offset, 2, texture_code)) { continue; }
        if (result) { return {}; }
        result = candidate;
    }
    return result;
}
}
