#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <optional>

namespace uevr::dispatch_movie {
inline constexpr uint32_t image_timestamp = 1769654770;
inline constexpr uint32_t image_size = 0x489d000;
inline constexpr uintptr_t setter_rva = 0xd17b10, draw_rva = 0xd15220;
inline constexpr uintptr_t extension_vtable_rva = 0x328a7e0;
inline constexpr uintptr_t dx11_texture_vtable_rva = 0x3387148;
inline constexpr uintptr_t dx12_texture_vtable_rva = 0x3394850, dx12_resource_vtable_rva = 0x3391d90;
inline constexpr uintptr_t present_rva = 0x14650c0;
inline constexpr std::array<uint8_t,24> present_code{
    0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x33,0xdb,0x8b,0xf2,0x48,0x8b,0xf9,0x85,0xd2};
inline constexpr std::array<uint8_t,24> setter_code{
    0x0f,0x10,0x02,0x48,0x8b,0x81,0xa0,0,0,0,0x0f,0x11,0x41,0x40,0x0f,0x10,0x4a,0x10,0x0f,0x11,0x49,0x50,0x0f,0x10};
inline constexpr std::array<uint8_t,24> draw_code{
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x4c,0x89,0x70,0x20,0x55,0x48,0x8d,0x68,0xa9};

inline bool contract(uint32_t timestamp, uint32_t size,
    std::span<const uint8_t> setter, std::span<const uint8_t> draw) noexcept {
    return timestamp == image_timestamp && size == image_size &&
        setter.size() == setter_code.size() && draw.size() == draw_code.size() &&
        std::equal(setter.begin(),setter.end(),setter_code.begin()) &&
        std::equal(draw.begin(),draw.end(),draw_code.begin());
}
struct Scope {
    bool dispatch{}, requested{}, openxr{}, native{}, manual_2d{}, mono{}, dx11{}, dx12{};
    bool enabled() const noexcept { return dispatch && requested && openxr && native && !manual_2d && !mono && (dx11 != dx12); }
};
struct Owner {
    uintptr_t subsystem{}, extension{}, texture{}, world{};
    uint64_t weak_texture{};
    uintptr_t source{}, player{};
    bool valid() const noexcept { return subsystem && extension && texture && world && source && player &&
        int32_t(weak_texture) >= 0 && uint32_t(weak_texture >> 32) != 0; }
    bool operator==(const Owner&) const = default;
};
inline bool extent(uint32_t width, uint32_t height) noexcept {
    return width >= 16 && width <= 8192 && height >= 16 && height <= 8192 &&
        double(width)/height >= 0.5 && double(width)/height <= 3.0;
}
inline bool current(uint64_t epoch, uint64_t expected_epoch, uint64_t sampled, uint64_t now) noexcept {
    return epoch && epoch == expected_epoch && sampled && now >= sampled && now-sampled <= 500;
}
inline bool shader_read_state(uint32_t state) noexcept {
    return state && !(state & ~uint32_t{0xc0});
}
// Exact Dispatch FD3D12Resource/CResourceState layout, independently checked
// against the shipping constructor and GetSubresourceState implementation.
template<class Read> std::optional<uint32_t> shader_state(uintptr_t resource, uint16_t mips, Read&& read) {
    if (!resource || !mips || mips > 14) { return {}; }
    uint8_t flags{};
    uint32_t value{};
    if (!read(resource+0xfc,&flags,sizeof(flags))) { return {}; }
    if (flags & 1) {
        uint32_t all{};
        if (!read(resource+0xc8,&all,sizeof(all))) { return {}; }
        if (all & 0x80000000u) { value=all & 0x7fffffffu; }
        else {
            int32_t count{};
            uintptr_t allocation{};
            if (!read(resource+0xe8,&count,sizeof(count)) || count != mips ||
                !read(resource+0xe0,&allocation,sizeof(allocation)) || (!allocation && mips > 4) ||
                !read(allocation ? allocation : resource+0xd0,&value,sizeof(value))) { return {}; }
        }
    } else if (!read(resource+0xf0,&value,sizeof(value))) { return {}; }
    return shader_read_state(value) ? std::optional{value} : std::nullopt;
}
struct Geometry { float width{}, height{}; };
inline Geometry geometry(uint32_t width, uint32_t height, float meters) noexcept {
    if (!extent(width,height) || !std::isfinite(meters) || meters <= 0 || meters > 10) { return {}; }
    return {float(width)/float(height)*meters, meters};
}
}
