#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace uevr::dune_renderer {

inline constexpr size_t maximum_abi_bytes = 0x400;

template <size_t N>
std::optional<size_t> find(std::span<const uint8_t> code, const std::array<uint8_t, N>& bytes, size_t start = 0) {
    if (start > code.size()) { return {}; }
    const auto found = std::search(code.begin() + start, code.end(), bytes.begin(), bytes.end());
    return found == code.end() ? std::nullopt : std::optional<size_t>{found - code.begin()};
}

inline bool valid_r14_family_abi(std::span<const uint8_t> code) {
    code = code.first(std::min(code.size(), maximum_abi_bytes));
    const auto early = code.first(std::min<size_t>(code.size(), 0x100));
    // October's compiler preserves the array view in R14. Prove the alias,
    // data/count reads, end-pointer calculation, and real family frame store.
    const auto r14 = find(early, std::array<uint8_t, 3>{0x4D, 0x8B, 0xF0});
    const auto data = r14 ? find(early, std::array<uint8_t, 3>{0x4D, 0x8B, 0x26}, *r14 + 3) : std::nullopt;
    const auto count = data ? find(code, std::array<uint8_t, 12>{
        0x49,0x63,0x46,0x08, 0x49,0x83,0xC6,0x08, 0x4D,0x8D,0x3C,0xC4}, *data + 3) : std::nullopt;
    return count && find(code, std::array<uint8_t, 6>{0x89,0x83,0x84,0,0,0}, *count + 12).has_value();
}

inline bool valid_array_view_abi(std::span<const uint8_t> code) {
    const auto early = code.first(std::min<size_t>(code.size(), 0x100));
    const auto direct = find(early, std::array<uint8_t, 3>{0x4D, 0x8B, 0x20});
    if (direct && find(early, std::array<uint8_t, 4>{0x49, 0x63, 0x40, 0x08}, *direct + 3)) { return true; }
    const auto rdi = find(early, std::array<uint8_t, 3>{0x49, 0x8B, 0xF8});
    const auto rdi_data = rdi ? find(early, std::array<uint8_t, 3>{0x4C, 0x8B, 0x27}, *rdi + 3) : std::nullopt;
    return (rdi_data && find(early, std::array<uint8_t, 4>{0x48, 0x63, 0x47, 0x08}, *rdi_data + 3)) ||
        valid_r14_family_abi(code);
}

} // namespace uevr::dune_renderer
