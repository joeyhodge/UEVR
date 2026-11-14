#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <sdk/UE6Contracts.hpp>

namespace uevr::ue60 {
template<class Read, class Polymorphic>
bool valid_family(uintptr_t address, uint32_t frame_offset, Read&& read, Polymorphic&& polymorphic) {
    const auto pointer = [](uintptr_t p) { return p >= 0x10000 && p % 8 == 0; };
    if (!pointer(address) || address > UINTPTR_MAX - 0x100 ||
        frame_offset < 0x80 || frame_offset >= 0x100 || frame_offset % 4) { return false; }
    std::array<uint8_t, 0x100> before{}, after{};
    if (!read(address, before.data(), before.size()) || !polymorphic(address)) { return false; }
    uintptr_t views{}, target{};
    int32_t count{}, capacity{};
    uint32_t frame{};
    std::memcpy(&views, before.data() + sdk::ue6::family_views, 8);
    std::memcpy(&count, before.data() + 16, 4);
    std::memcpy(&capacity, before.data() + 20, 4);
    std::memcpy(&target, before.data() + sdk::ue6::family_render_target, 8);
    std::memcpy(&frame, before.data() + frame_offset, 4);
    if (!pointer(views) || count < 1 || count > 8 || capacity < count || capacity > 16 ||
        !pointer(target) || !polymorphic(target) || frame < 10 || frame == UINT32_MAX) { return false; }
    unsigned scenes{};
    for (const auto offset : sdk::ue6::family_scene_candidates) {
        uintptr_t scene{};
        std::memcpy(&scene, before.data() + offset, 8);
        if (pointer(scene) && polymorphic(scene)) { ++scenes; }
    }
    if (scenes != 1) { return false; }
    std::array<uintptr_t, 8> entries{};
    const size_t bytes = static_cast<size_t>(count) * sizeof(uintptr_t);
    if (views > UINTPTR_MAX - bytes || !read(views, entries.data(), bytes)) { return false; }
    for (int32_t i{}; i < count; ++i) {
        uintptr_t family{};
        if (!pointer(entries[i]) || entries[i] > UINTPTR_MAX - 16 || !polymorphic(entries[i]) ||
            !read(entries[i] + 8, &family, sizeof(family)) || family != address) { return false; }
    }
    // Discovery observes identities, not retained ownership. Discard racing
    // snapshots; do not turn a callback counter or RDG builder into a family.
    return read(address, after.data(), after.size()) &&
        std::memcmp(before.data(), after.data(), before.size()) == 0;
}
} // namespace uevr::ue60
