#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace uevr::stellar_blade {

inline constexpr size_t slate_window_bytes = 0x2600;
inline constexpr size_t slate_argument_offset = 0x3B;
inline constexpr std::array<uint8_t, 17> slate_arguments{
    0x48,0x8B,0x85,0xA0,0x03,0x00,0x00,0x4D,0x8B,0xE8,0x48,0x89,0x45,0xF0,0x4C,0x8B,0xFA};
inline constexpr std::array<uint8_t, 72> viewport_texture_getter{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x89,0xC0,0x00,0x00,0x00,0x48,0x8B,0xDA,
    0x48,0x85,0xC9,0x74,0x23,0x48,0x8B,0x01,0xFF,0x10,0x48,0x85,0xC0,0x74,0x19,0x48,
    0x8B,0x40,0x08,0x48,0x89,0x03,0x48,0x85,0xC0,0x74,0x14,0xF0,0xFF,0x40,0x08,0x48,
    0x8B,0xC3,0x48,0x83,0xC4,0x20,0x5B,0xC3,0x48,0xC7,0x03,0x00,0x00,0x00,0x00,0x48,
    0x8B,0xC3,0x48,0x83,0xC4,0x20,0x5B,0xC3};

inline std::optional<uint32_t> provider_field(std::span<const uint8_t> code) {
    if (code.size() != viewport_texture_getter.size()) { return std::nullopt; }
    for (size_t i = 0; i < code.size(); ++i) {
        if ((i < 9 || i >= 13) && code[i] != viewport_texture_getter[i]) { return std::nullopt; }
    }
    uint32_t offset{};
    std::memcpy(&offset, code.data() + 9, sizeof(offset));
    // The getter proves the provider slot and the resource's +8 texture field,
    // without calling the getter (which would also acquire an RHI reference).
    if (offset < 0x40 || offset > 0x300 || offset % sizeof(uintptr_t)) { return std::nullopt; }
    return offset;
}

template <typename ReadExecutable>
std::optional<uint32_t> discover_provider_field(std::span<const uint8_t> draw, uintptr_t address,
    uintptr_t image_begin, uintptr_t image_end, ReadExecutable&& read_executable) {
    constexpr std::array<uint8_t, 15> entry{
        0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    if (draw.size() < slate_argument_offset + slate_arguments.size() || draw.size() > slate_window_bytes ||
        !std::equal(entry.begin(), entry.end(), draw.begin()) || image_end <= image_begin ||
        address < image_begin || address >= image_end || draw.size() > image_end - address ||
        !std::equal(slate_arguments.begin(), slate_arguments.end(), draw.begin() + slate_argument_offset)) {
        return std::nullopt;
    }
    uintptr_t selected{};
    std::optional<uint32_t> field;
    size_t matches{}, examined{};
    for (size_t i = entry.size(); i + 12 <= draw.size(); ++i) {
        // Both source callsites pass the same R8-derived viewport in R13, and
        // stack-local TRefCountPtr return storage in RDX. One also sets a flag.
        if (draw[i] != 0x48 || draw[i+1] != 0x8D || draw[i+2] != 0x55 ||
            draw[i+4] != 0x49 || draw[i+5] != 0x8B || draw[i+6] != 0xCD) { continue; }
        size_t call = i + 7;
        constexpr std::array<uint8_t, 5> flag{0x41,0x0F,0xBA,0xEC,0x07};
        if (call + flag.size() <= draw.size() &&
            std::equal(flag.begin(), flag.end(), draw.begin() + call)) { call += flag.size(); }
        if (call + 5 > draw.size() || draw[call] != 0xE8) { continue; }
        if (++examined > 64) { return std::nullopt; }
        int32_t displacement{};
        std::memcpy(&displacement, draw.data() + call + 1, sizeof(displacement));
        const auto next = address + call + 5;
        uintptr_t target{};
        if (displacement < 0) {
            const auto distance = static_cast<uintptr_t>(-static_cast<int64_t>(displacement));
            if (distance > next) { return std::nullopt; }
            target = next - distance;
        } else {
            if (static_cast<uintptr_t>(displacement) > std::numeric_limits<uintptr_t>::max() - next) {
                return std::nullopt;
            }
            target = next + displacement;
        }
        if (target < image_begin || target >= image_end || viewport_texture_getter.size() > image_end - target) {
            continue;
        }
        std::array<uint8_t, viewport_texture_getter.size()> code{};
        if (!read_executable(target, code.data(), code.size())) { continue; }
        const auto candidate = provider_field(code);
        if (!candidate) { continue; }
        if (selected && (selected != target || field != candidate)) { return std::nullopt; }
        selected = target;
        field = candidate;
        ++matches;
    }
    return matches >= 2 ? field : std::nullopt;
}

inline unsigned warming_view_count(bool guarded_stellar_blade, bool preserve_other_pair, bool bootstrap_failed) {
    // Never request a second eye after its source-backed state allocation failed.
    return (guarded_stellar_blade ? !bootstrap_failed : preserve_other_pair) ? 2u : 1u;
}

} // namespace uevr::stellar_blade
