#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace uevr::ue58_owned_ui {

// Shipping UE5.8 FTextureRenderTarget2DResource. These are only usable after
// the owner round-trip, both RHI references, and accessor code all agree.
inline constexpr uintptr_t texture_rhi_offset = 0x10;
inline constexpr uintptr_t render_target_offset = 0x40;
inline constexpr uintptr_t render_target_texture_offset = 0x48;
inline constexpr uintptr_t owner_offset = 0x80;
inline constexpr uintptr_t width_offset = 0xac;
inline constexpr uintptr_t height_offset = 0xb0;
inline constexpr uintptr_t resource_size = 0xc0;
inline constexpr uintptr_t max_owner_size = 0x300;
inline constexpr uintptr_t d3d_resource_offset = 0xd0;
inline constexpr uintptr_t native_resource_offset = 0x20;

inline constexpr std::array<uint8_t, 5> render_target_accessor{
    0x48, 0x8d, 0x41, 0x08, 0xc3}; // lea rax,[rcx+8]; ret
inline constexpr std::array<uint8_t, 7> size_x_accessor{
    0x8b, 0x81, 0xac, 0x00, 0x00, 0x00, 0xc3};
// Slot 5 reads +0xd0 -> +0x20, with a linked-texture fallback. Never execute
// that fallback while InitRHI is pending: validate the code and read directly.
inline constexpr std::array<uint8_t, 71> native_resource_accessor{
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xd0, 0x00, 0x00, 0x00,
    0x33, 0xdb, 0x48, 0x85, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x58, 0x20, 0x48, 0x85,
    0xdb, 0x75, 0x21, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x38, 0x48, 0x85, 0xc0, 0x74,
    0x16, 0x48, 0x8b, 0x80, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0a,
    0x48, 0x8b, 0x40, 0x20, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3, 0x48, 0x8b, 0xc3,
    0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};

template <size_t N>
constexpr bool matches_accessor(std::span<const uint8_t> code, const std::array<uint8_t, N>& expected) noexcept {
    if (code.size() != N) {
        return false;
    }
    for (size_t i = 0; i < N; ++i) {
        if (code[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

struct Resource {
    uintptr_t private_resource_offset{};
    uintptr_t resource{};
    uintptr_t rhi_texture{};
    bool operator==(const Resource&) const = default;
};

template <typename Read, typename Validate>
std::optional<Resource> find_resource(
    uintptr_t owner, size_t owner_size, uint32_t width, uint32_t height,
    const Read& read, const Validate& validate) {
    if (owner == 0 || owner_size < 2 * sizeof(uintptr_t) || owner_size > max_owner_size ||
        owner > std::numeric_limits<uintptr_t>::max() - owner_size ||
        width == 0 || height == 0 || width > 65536 || height > 65536)
    {
        return std::nullopt;
    }

    std::optional<Resource> result{};
    for (uintptr_t offset = sizeof(uintptr_t); offset + 2 * sizeof(uintptr_t) <= owner_size; offset += sizeof(uintptr_t)) {
        uintptr_t resource{}, render_thread_resource{};
        if (!read(owner + offset, resource) || !read(owner + offset + sizeof(uintptr_t), render_thread_resource)) {
            return std::nullopt;
        }
        if (resource == 0 || resource != render_thread_resource || (resource % alignof(uintptr_t)) != 0 ||
            resource > std::numeric_limits<uintptr_t>::max() - resource_size)
        {
            continue;
        }

        uintptr_t actual_owner{}, rhi{}, target_rhi{};
        uint32_t actual_width{}, actual_height{};
        if (!read(resource + owner_offset, actual_owner) || actual_owner != owner ||
            !read(resource + texture_rhi_offset, rhi) || rhi == 0 ||
            !read(resource + render_target_texture_offset, target_rhi) || target_rhi != rhi ||
            !read(resource + width_offset, actual_width) || actual_width != width ||
            !read(resource + height_offset, actual_height) || actual_height != height ||
            !validate(resource, rhi))
        {
            continue;
        }
        if (result) {
            return std::nullopt; // Ambiguous owner fields must not publish an offset.
        }
        result = Resource{offset, resource, rhi};
    }
    return result;
}

struct Observation {
    uint64_t generation{};
    Resource resource{};
    uintptr_t native{};
    bool operator==(const Observation&) const = default;
};

class StableResource {
public:
    bool observe(const std::optional<Observation>& current) noexcept {
        const bool stable = current && previous == current;
        previous = current;
        return stable;
    }
private:
    std::optional<Observation> previous{};
};

} // namespace uevr::ue58_owned_ui
