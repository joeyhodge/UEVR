#pragma once

#include <array>
#include <sdk/BoundedDiscovery.hpp>

namespace uevr::pbz_native {

inline constexpr size_t stereo_pass_offset = 0xde0;
inline constexpr size_t stereo_index_offset = 0xde4;
inline constexpr size_t primary_index_offset = 0xde8;

// The real FSceneView constructor transfers InitOptions' pass/index and
// initializes PrimaryViewIndex. These are not the stock UE5.7 dd0/dd8 fields.
inline constexpr auto constructor_fields = std::to_array<uint8_t>({
    0x8b, 0x85, 0xc0, 0x01, 0, 0, 0x89, 0x83, 0xe0, 0x0d, 0, 0,
    0x8b, 0x85, 0xc4, 0x01, 0, 0, 0xc7, 0x83, 0xe8, 0x0d, 0, 0, 0xff, 0xff, 0xff, 0xff});
inline constexpr auto index_store = std::to_array<uint8_t>({0x89, 0x83, 0xe4, 0x0d, 0, 0});
inline constexpr auto primary_store = std::to_array<uint8_t>({0x89, 0x83, 0xe8, 0x0d, 0, 0});
inline constexpr auto secondary_test = std::to_array<uint8_t>({
    0x83, 0xb9, 0xe0, 0x0d, 0, 0, 0x02, 0x0f, 0x94, 0xc0, 0xc3});
inline constexpr auto primary_test = std::to_array<uint8_t>({
    0x8b, 0x81, 0xe0, 0x0d, 0, 0, 0x85, 0xc0, 0x74, 0x08,
    0x83, 0xf8, 0x01, 0x74, 0x03, 0x32, 0xc0, 0xc3, 0xb0, 0x01, 0xc3});

inline bool in_module(uintptr_t address, size_t count, uintptr_t base, size_t size) {
    return base >= 0x10000 && size <= UINTPTR_MAX - base && address >= base &&
        address - base <= size && count <= size - (address - base);
}

template <size_t N>
bool code_matches(const sdk::discovery::Memory& memory, uintptr_t address,
    const std::array<uint8_t, N>& expected, uintptr_t base, size_t size) {
    std::array<uint8_t, N> code{};
    return in_module(address, code.size(), base, size) && memory.executable &&
        memory.executable(memory.context, address, code.size()) && memory.load(address, code) && code == expected;
}

template <size_t N>
bool call_matches(const sdk::discovery::Memory& memory, uintptr_t address,
    const std::array<uint8_t, N>& expected, uintptr_t base, size_t size) {
    std::array<uint8_t, 5> call{};
    int32_t displacement{};
    if (!in_module(address, call.size(), base, size) || !memory.executable ||
        !memory.executable(memory.context, address, call.size()) ||
        !memory.load(address, call) || call[0] != 0xe8) { return false; }
    std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
    const auto target = sdk::discovery::relative_address(address + call.size(), displacement);
    return target && code_matches(memory, *target, expected, base, size);
}

inline bool layout_proven(const sdk::discovery::Memory& memory, uintptr_t constructor,
    uintptr_t base, size_t size) {
    // Check untouched constructor bytes and its two independent pass consumers.
    // The hook entry may be patched; no game functions are invoked for proof.
    return in_module(constructor, 0x1573, base, size) &&
        code_matches(memory, constructor + 0x540, constructor_fields, base, size) &&
        code_matches(memory, constructor + 0x5be, index_store, base, size) &&
        code_matches(memory, constructor + 0x153b, primary_store, base, size) &&
        call_matches(memory, constructor + 0x1541, secondary_test, base, size) &&
        call_matches(memory, constructor + 0x156e, primary_test, base, size);
}

} // namespace uevr::pbz_native
