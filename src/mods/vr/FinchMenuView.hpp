#pragma once

#include <cstddef>
#include <cstdint>

namespace uevr::finch_menu {
inline constexpr uint32_t menu_camera_offset = 0x2df8;
inline constexpr uint32_t outer_offset = 0x20;

struct Binding {
    uintptr_t menu{}, camera{}, level{};
    uint32_t camera_field{}, outer_field{};
    bool validated{};
};

inline bool range(uintptr_t address, size_t size) {
    return address >= 0x10000 && !(address & 7) && address < 0x0000800000000000ull &&
        size <= 0x0000800000000000ull - address;
}

constexpr bool journal_target(uintptr_t target, uintptr_t pending, uintptr_t pawn, uintptr_t menu_camera) {
    if (!menu_camera) { return false; }
    if (target == menu_camera) { return !pending || pending == menu_camera || (pawn && pending == pawn); }
    return pawn && target == pawn && pending == menu_camera;
}

// The host supplies a source-verified build/layout and current object serials.
// A camera name, -90 degree pitch, or an arbitrary scripted target is not proof.
template<class Memory, class Current>
bool journal_view(const Memory& memory, const Binding& binding, uintptr_t manager, uintptr_t pawn,
    bool current_local_context, Current&& current) {
    if (!binding.validated || !current_local_context || binding.camera_field != menu_camera_offset ||
        binding.outer_field != outer_offset ||
        !range(binding.menu, binding.camera_field + 8) || !range(binding.camera, binding.outer_field + 8) ||
        !range(binding.level, 0x28) || !range(manager, 0xf68)) { return false; }
    uintptr_t target{}, pending{}, camera{}, menu_level{}, camera_level{};
    if (!memory.load(manager + 0xb78, target) || !memory.load(manager + 0xf60, pending) ||
        !journal_target(target, pending, pawn, binding.camera) || !current() ||
        !memory.load(binding.menu + binding.camera_field, camera) || camera != binding.camera ||
        !memory.load(binding.menu + binding.outer_field, menu_level) || menu_level != binding.level ||
        !memory.load(binding.camera + binding.outer_field, camera_level) || camera_level != binding.level) { return false; }
    return true;
}

constexpr bool flatten_pitch(bool enabled, bool validated_journal) { return enabled && !validated_journal; }
} // namespace uevr::finch_menu
