#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace uevr::stellar_blade {

inline constexpr std::array<uint8_t, 20> renderer_entry_prefix{
    0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x60, 0x49, 0x8B, 0x48, 0x20, 0x45, 0x33, 0xE4};
inline constexpr std::array<uint8_t, 16> renderer_root_unwind{
    0x01, 0x0D, 0x06, 0x00, 0x0D, 0xB2, 0x09, 0xF0,
    0x07, 0xE0, 0x05, 0xC0, 0x03, 0x70, 0x02, 0x60};

inline bool has_callable_renderer_entry(
    std::span<const uint8_t> code,
    std::span<const uint8_t> unwind) {
    // The matching UE4.26 binary saves five registers and reserves 0x60 bytes
    // before its CHAININFO continuation. Only the original ABI entry is callable.
    return code.size() >= renderer_entry_prefix.size() &&
        unwind.size() >= renderer_root_unwind.size() &&
        std::equal(renderer_entry_prefix.begin(), renderer_entry_prefix.end(), code.begin()) &&
        std::equal(renderer_root_unwind.begin(), renderer_root_unwind.end(), unwind.begin());
}

} // namespace uevr::stellar_blade
