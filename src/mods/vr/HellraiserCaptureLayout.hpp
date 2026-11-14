#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace uevr::hellraiser_capture {

// Confirmed in this game's UE5.6.1 accessors and resource constructor. A
// matching version alone never authorizes these offsets or a native call.
inline constexpr uintptr_t private_resource_offset = 0x120;
inline constexpr uintptr_t render_thread_resource_offset = 0x128;
inline constexpr uintptr_t texture_rhi_offset = 0x10;
inline constexpr uintptr_t render_target_offset = 0x50;
inline constexpr uintptr_t render_target_texture_offset = 0x58;
inline constexpr uintptr_t owner_offset = 0x90;
inline constexpr uintptr_t width_offset = 0xbc;
inline constexpr uintptr_t height_offset = 0xc0;
inline constexpr uintptr_t resource_size = 0xd0;
inline constexpr uintptr_t d3d_resource_offset = 0xb8;
inline constexpr uintptr_t native_resource_offset = 0x20;

inline constexpr std::array<uint8_t, 5> target_accessor{0x48, 0x8d, 0x41, 0x08, 0xc3};
inline constexpr std::array<uint8_t, 5> shader_accessor{0x48, 0x8d, 0x41, 0xc0, 0xc3};
inline constexpr std::array<uint8_t, 14> gamma_accessor{
    0x48, 0x8b, 0x49, 0x40, 0x48, 0x8b, 0x01, 0x48, 0xff, 0xa0, 0xe0, 0x03, 0x00, 0x00};
inline constexpr std::array<uint8_t, 71> native_accessor{
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xb8, 0x00, 0x00, 0x00,
    0x33, 0xdb, 0x48, 0x85, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x58, 0x20, 0x48, 0x85,
    0xdb, 0x75, 0x21, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x38, 0x48, 0x85, 0xc0, 0x74,
    0x16, 0x48, 0x8b, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0a,
    0x48, 0x8b, 0x40, 0x20, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3, 0x48, 0x8b, 0xc3,
    0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};

constexpr bool object_range(uintptr_t address, size_t size) noexcept {
    return address != 0 && address % alignof(uintptr_t) == 0 &&
        size != 0 && address <= (std::numeric_limits<uintptr_t>::max)() - size;
}

template <size_t N, typename Read, typename ModuleOwned, typename Executable>
bool validate_accessor(uintptr_t table, size_t slot, const std::array<uint8_t, N>& expected,
    const Read& read, const ModuleOwned& module_owned, const Executable& executable)
{
    if (slot > 8 || !object_range(table, (slot + 1) * sizeof(uintptr_t)) || !module_owned(table)) {
        return false;
    }
    uintptr_t function{};
    std::array<uint8_t, N> code{};
    return read(table + slot * sizeof(uintptr_t), function) && function != 0 &&
        function <= (std::numeric_limits<uintptr_t>::max)() - N && module_owned(function) &&
        executable(function, N) && read(function, code) && code == expected;
}

struct Resource {
    uintptr_t resource{};
    uintptr_t resource_vtable{};
    uintptr_t render_target_vtable{};
    uintptr_t rhi_texture{};
    uintptr_t rhi_vtable{};
    uintptr_t d3d_resource{};
    uintptr_t native_resource{};
    bool operator==(const Resource&) const = default;
};

template <typename Read, typename ModuleOwned, typename Executable>
std::optional<Resource> find_resource(uintptr_t owner, uint32_t width, uint32_t height,
    uintptr_t expected_rhi_vtable, const Read& read, const ModuleOwned& module_owned,
    const Executable& executable, const char*& reason)
{
    reason = "invalid owner, eye extent or RHI vtable";
    if (!object_range(owner, render_thread_resource_offset + sizeof(uintptr_t)) ||
        width == 0 || height == 0 || width > 65536 || height > 65536 || expected_rhi_vtable == 0)
    {
        return std::nullopt;
    }
    Resource result{};
    uintptr_t rt_resource{}, actual_owner{}, target_rhi{};
    uint32_t actual_width{}, actual_height{};
    reason = "game-thread/render-thread resource pair is pending";
    if (!read(owner + private_resource_offset, result.resource) ||
        !read(owner + render_thread_resource_offset, rt_resource) ||
        !object_range(result.resource, resource_size) || rt_resource != result.resource)
    {
        return std::nullopt;
    }
    reason = "owned render resource, eye extent or RHI pair is pending";
    if (!read(result.resource + owner_offset, actual_owner) || actual_owner != owner ||
        !read(result.resource + width_offset, actual_width) || actual_width != width ||
        !read(result.resource + height_offset, actual_height) || actual_height != height ||
        !read(result.resource + texture_rhi_offset, result.rhi_texture) ||
        !read(result.resource + render_target_texture_offset, target_rhi) || target_rhi != result.rhi_texture ||
        !object_range(result.rhi_texture, d3d_resource_offset + sizeof(uintptr_t)) ||
        !read(result.rhi_texture, result.rhi_vtable) || result.rhi_vtable != expected_rhi_vtable ||
        !read(result.resource, result.resource_vtable) || !module_owned(result.resource_vtable) ||
        !read(result.resource + render_target_offset, result.render_target_vtable))
    {
        return std::nullopt;
    }
    reason = "render-target or native-resource accessor proof failed";
    if (!validate_accessor(result.render_target_vtable, 2, target_accessor, read, module_owned, executable) ||
        !validate_accessor(result.render_target_vtable, 4, shader_accessor, read, module_owned, executable) ||
        !validate_accessor(result.render_target_vtable, 6, gamma_accessor, read, module_owned, executable) ||
        !validate_accessor(result.rhi_vtable, 5, native_accessor, read, module_owned, executable))
    {
        return std::nullopt;
    }
    // Do not execute GetNativeResource's linked-texture fallback during InitRHI.
    reason = "direct D3D12 resource is pending";
    if (!read(result.rhi_texture + d3d_resource_offset, result.d3d_resource) ||
        !object_range(result.d3d_resource, native_resource_offset + sizeof(uintptr_t)) ||
        !read(result.d3d_resource + native_resource_offset, result.native_resource) ||
        !object_range(result.native_resource, sizeof(uintptr_t)))
    {
        return std::nullopt;
    }
    reason = "ready";
    return result;
}

struct NativeDescriptor {
    uintptr_t device{};
    uint64_t width{};
    uint32_t height{};
    uint32_t mips{};
    uint32_t array_size{};
    uint32_t samples{};
    bool texture2d{};
    bool bgra{};
    bool render_target{};
    bool depth_stencil{};
    bool deny_shader_resource{};
};

constexpr bool valid_descriptor(const NativeDescriptor& desc, uintptr_t device,
    uint32_t width, uint32_t height) noexcept
{
    return device != 0 && desc.device == device && width != 0 && height != 0 &&
        width <= 65536 && height <= 65536 && desc.width == width && desc.height == height &&
        desc.mips == 1 && desc.array_size == 1 && desc.samples == 1 &&
        desc.texture2d && desc.bgra && desc.render_target && !desc.depth_stencil && !desc.deny_shader_resource;
}

struct Observation {
    uint64_t generation{};
    Resource resource{};
    bool operator==(const Observation&) const = default;
};

class StableResource {
public:
    bool observe(std::optional<Observation> observation) {
        if (!observation || observation->generation == 0 || observation->resource.native_resource == 0) {
            m_previous.reset();
            return false;
        }
        const bool stable = m_previous == observation;
        m_previous = observation;
        return stable;
    }

private:
    std::optional<Observation> m_previous{};
};

} // namespace uevr::hellraiser_capture
