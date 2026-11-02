#pragma once

#include "ProSpiCameraTrace.hpp"

namespace uevr::prospi::shot {
struct Object {
    uintptr_t address{}, vtable{};
    int32_t index{-1}, serial{-1};
    bool valid() const noexcept { return address && vtable && index >= 0; }
};
inline bool same_object(const Object& a, const Object& b) noexcept {
    return a.address == b.address && a.vtable == b.vtable && a.index == b.index &&
        (a.serial <= 0 || a.serial == b.serial);
}
struct Identity {
    Object world{}, manager{}, view_target{};
    uintptr_t native_camera{}, camera_actor{};
    bool valid{}, cut_known{}, cut{};
};
enum class Change : uint8_t { unavailable, unchanged, owner, target, native_camera, cut };
inline Change compare(const Identity& expected, const Identity& current) noexcept {
    if (!expected.valid || !current.valid || !expected.world.valid() || !current.world.valid() ||
        !expected.manager.valid() || !current.manager.valid()) { return Change::unavailable; }
    if (!same_object(expected.world, current.world) || !same_object(expected.manager, current.manager)) { return Change::owner; }
    if (!same_object(expected.view_target, current.view_target)) { return Change::target; }
    if (expected.native_camera && current.native_camera &&
        (expected.native_camera != current.native_camera || expected.camera_actor != current.camera_actor)) { return Change::native_camera; }
    if (expected.cut_known && current.cut_known && current.cut && !expected.cut) { return Change::cut; }
    return Change::unchanged;
}
inline bool changed(Change change) noexcept { return change != Change::unavailable && change != Change::unchanged; }
}
