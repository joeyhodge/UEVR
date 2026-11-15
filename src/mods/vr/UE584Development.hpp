#pragma once

#include <limits>
#include "UE58NativeFix.hpp"
#include "UE59NativeContracts.hpp"
#include "UE584DevelopmentContracts.hpp"

namespace uevr::ue584_development {

inline bool view_layout(const sdk::discovery::Memory& memory, uintptr_t constructor) {
    namespace s = ue58_native::contract;
    return sdk::discovery::relative_address(constructor, 0x5d2) &&
        ue58_native::matches(memory, constructor + 0x39, s::view_identity_code, s::view_identity_mask) &&
        ue58_native::matches(memory, constructor + 0x3af, s::view_rects_code, s::view_rects_mask) &&
        ue58_native::matches(memory, constructor + 0x495, s::view_projection_code, s::view_projection_mask) &&
        ue58_native::matches(memory, constructor + 0x53d, contract::view_stereo_code, contract::view_stereo_mask);
}

inline bool renderer_layout(const sdk::discovery::Memory& memory, uintptr_t renderer) {
    return sdk::discovery::relative_address(renderer, 0x488) &&
        ue58_native::matches(memory, renderer + 0x30, contract::renderer_code, contract::renderer_mask);
}

inline constexpr const char* family_copy_signature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 48 8D 71 08 45 33 FF 4C 89 3E";

template<class FunctionSize>
std::optional<ue58_native::FamilyFunctions> family_functions(const sdk::discovery::Memory& memory,
    uintptr_t copy, uintptr_t base, size_t image_size, FunctionSize&& function_size) {
    // Development retains the same deep-copy ABI as the reviewed 5.9 build,
    // but its base destructor and renderer must be proven independently.
    namespace s = ue59_native::contract;
    const auto contains = [&](uintptr_t address, size_t bytes) {
        return base >= 0x10000 && image_size != 0 && base <= (std::numeric_limits<uintptr_t>::max)() - image_size &&
            address >= base && address - base < image_size && bytes != 0 && bytes <= image_size - (address - base);
    };
    if (!contains(copy, s::development_family_copy_code.size()) ||
        function_size(copy) != s::development_family_copy_code.size() ||
        !ue58_native::matches(memory, copy, s::development_family_copy_code, s::development_family_copy_mask)) { return {}; }
    int32_t displacement{};
    if (!memory.load(copy + 0x25, displacement)) { return {}; }
    const auto table = sdk::discovery::relative_address(copy + 0x29, displacement);
    uintptr_t deleting{};
    if (!table || !contains(*table, sizeof(deleting)) || !memory.load(*table, deleting) ||
        !contains(deleting, s::development_family_delete_code.size()) ||
        function_size(deleting) != s::development_family_delete_code.size() ||
        !ue58_native::matches(memory, deleting, s::development_family_delete_code, s::development_family_delete_mask) ||
        !memory.load(deleting + 0x10, displacement)) { return {}; }
    const auto destroy = sdk::discovery::relative_address(deleting + 0x14, displacement);
    if (!destroy || !contains(*destroy, contract::family_destroy_code.size()) ||
        function_size(*destroy) != contract::family_destroy_code.size() ||
        !ue58_native::matches(memory, *destroy, contract::family_destroy_code, contract::family_destroy_mask) ||
        !memory.load(*destroy + 9, displacement) ||
        sdk::discovery::relative_address(*destroy + 0xd, displacement) != table) { return {}; }
    return ue58_native::FamilyFunctions{copy, *destroy, *table, true};
}

} // namespace uevr::ue584_development
