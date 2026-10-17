#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sdk/GearsEDayDiscovery.hpp>
#include "GearsEDayNativeContracts.hpp"

namespace uevr::gears_native {
inline constexpr const char* family_copy_signature =
    "53 55 56 57 41 56 48 83 EC 40 48 8D 05 ? ? ? ? 48 8B D9 48 89 01 48 8B FA 48 83 C1 08";
inline constexpr size_t family_size = 0x198;
inline constexpr size_t interfaces_offset = 0x160;

struct FamilyFunctions {
    uintptr_t copy{};
    uintptr_t deleting_destructor{};
    uintptr_t complete_destructor{};
    uintptr_t vtable{};
};

template<class FunctionSize>
std::optional<FamilyFunctions> family_functions(const sdk::discovery::Memory& memory,
    uintptr_t copy, uintptr_t base, size_t image_size, FunctionSize&& function_size)
{
    const auto contains = [&](uintptr_t address, size_t size) {
        return address >= base && address - base <= image_size && size <= image_size - (address - base);
    };
    const auto call = [&](uintptr_t address) -> std::optional<uintptr_t> {
        std::array<uint8_t, 5> bytes{};
        int32_t displacement{};
        if (!memory.load(address, bytes) || bytes[0] != 0xe8) { return {}; }
        std::memcpy(&displacement, bytes.data() + 1, 4);
        const auto target = sdk::discovery::relative_address(address + 5, displacement);
        return target && contains(*target, 1) ? target : std::nullopt;
    };
    using sdk::gears_eday::matches;
    if (!contains(copy, family_copy_code.size()) || function_size(copy) != family_copy_code.size() ||
        !matches(memory, copy, family_copy_code, family_copy_mask)) { return {}; }
    int32_t displacement{};
    if (!memory.load(copy + 0x0e, displacement)) { return {}; }
    const auto table = sdk::discovery::relative_address(copy + 0x12, displacement);
    uintptr_t deleting{};
    if (!table || !contains(*table, sizeof(uintptr_t)) || !memory.load(*table, deleting) ||
        !contains(deleting, family_delete_code.size()) || function_size(deleting) != family_delete_code.size() ||
        !matches(memory, deleting, family_delete_code, family_delete_mask)) { return {}; }
    const auto destroy = call(deleting + 0x0f);
    const auto views_copy = call(copy + 0x31);
    const auto all_views_copy = call(copy + 0x4c);
    if (!destroy || !views_copy || all_views_copy != views_copy ||
        !contains(*destroy, family_destroy_code.size()) || function_size(*destroy) != family_destroy_code.size() ||
        !matches(memory, *destroy, family_destroy_code, family_destroy_mask) ||
        !contains(*views_copy, view_array_copy_code.size()) || function_size(*views_copy) != view_array_copy_code.size() ||
        !matches(memory, *views_copy, view_array_copy_code, view_array_copy_mask)) { return {}; }
    int32_t destroy_displacement{};
    if (!memory.load(*destroy + 9, destroy_displacement) ||
        sdk::discovery::relative_address(*destroy + 0x0d, destroy_displacement) != table) { return {}; }
    return FamilyFunctions{copy, deleting, *destroy, *table};
}

inline bool singleton_layout(const sdk::discovery::Memory& memory, uintptr_t constructor) {
    return sdk::gears_eday::view_constructor(memory, constructor) &&
        sdk::gears_eday::matches(memory, constructor + 0x214, view_singleton_code, view_singleton_mask);
}

inline bool renderer_layout(const sdk::discovery::Memory& memory, uintptr_t renderer, size_t stolen_size) {
    return stolen_size > 0 && stolen_size <= 0x2a &&
        renderer <= (std::numeric_limits<uintptr_t>::max)() - 0x60 &&
        sdk::gears_eday::matches(memory, renderer + 0x2a, renderer_array_code, renderer_array_mask);
}

inline bool fresh_family(const sdk::discovery::Memory& memory, uintptr_t family, uintptr_t vtable) {
    std::array<uint8_t, family_size> bytes{};
    if (!memory.load(family, bytes)) { return false; }
    const auto pointer_at = [&](size_t offset) {
        uintptr_t value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    // Owned upscalers and SceneRenderer must not be recycled from a prior render.
    return pointer_at(0) == vtable && bytes[0xb0] <= 1 && pointer_at(0x158) == 0 &&
        pointer_at(0x168) == 0 && pointer_at(0x170) == 0 && pointer_at(0x178) == 0;
}
}
