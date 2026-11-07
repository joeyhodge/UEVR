#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace uevr::ue521::view_extension {
// MSVC groups the deprecated RHI overload before the RDG overload. These
// slots and family prefix agree with 5.2.0/1 source and Adventure's PDB/code.
inline constexpr uint32_t begin_slot = 5;
inline constexpr uint32_t pre_render_slot = 7;
inline constexpr uint32_t active_slot = 20;
inline constexpr uint32_t frame_offset = 0x84;

constexpr bool eligible(uint32_t major, uint32_t minor, uint32_t patch,
    uint32_t active, bool destructor_called) {
    return major == 5 && minor == 2 && patch <= 1 && active == active_slot && !destructor_called;
}

struct Family {
    uintptr_t target{}, scene{};
    uint32_t frame{};
};

template<class Read, class Polymorphic>
std::optional<Family> read_family(uintptr_t address, Read&& read, Polymorphic&& polymorphic) {
    const auto pointer = [](uintptr_t value) { return value >= 0x10000 && value % alignof(uintptr_t) == 0; };
    if (!pointer(address) || address > UINTPTR_MAX - (frame_offset + 4)) { return {}; }
    std::array<uint8_t, frame_offset + 4> before{}, after{};
    if (!read(address, before.data(), before.size())) { return {}; }
    uintptr_t views{}, target{}, scene{};
    int32_t count{}, capacity{};
    uint32_t frame{};
    std::memcpy(&views, before.data() + 8, 8);
    std::memcpy(&count, before.data() + 16, 4);
    std::memcpy(&capacity, before.data() + 20, 4);
    std::memcpy(&target, before.data() + 0x20, 8);
    std::memcpy(&scene, before.data() + 0x28, 8);
    std::memcpy(&frame, before.data() + frame_offset, 4);
    if (!pointer(views) || count < 1 || count > 8 || capacity < count || capacity > 16 ||
        frame < 10 || frame == UINT32_MAX || !pointer(target) || !pointer(scene) ||
        !polymorphic(address) || !polymorphic(target) || !polymorphic(scene)) { return {}; }
    std::array<uintptr_t, 8> pointers{};
    const auto bytes = static_cast<size_t>(count) * sizeof(uintptr_t);
    if (views > UINTPTR_MAX - bytes || !read(views, pointers.data(), bytes)) { return {}; }
    for (int32_t i{}; i < count; ++i) {
        uintptr_t first{};
        if (!pointer(pointers[i]) || !read(pointers[i], &first, sizeof(first))) { return {}; }
    }
    if (!read(address, after.data(), after.size()) ||
        std::memcmp(before.data(), after.data(), 0x30) != 0 ||
        std::memcmp(before.data() + frame_offset, after.data() + frame_offset, 4) != 0) { return {}; }
    return Family{target, scene, frame};
}

enum class Stage { begin, pre_render };
struct Witness {
    uint32_t valid{}, advances{}, frame{};
};
struct Proof {
    uintptr_t target{}, scene{};
    Witness begin{}, pre_render{};
};

// Startup discovery only. Keys correlate fresh callback arguments; they are
// never retained ownership, scene-state validation, or capture authorization.
class Evidence {
public:
    void observe(Stage stage, const Family& family) {
        if (!family.target || !family.scene || family.frame < 10 || family.frame == UINT32_MAX) { return; }
        auto* slot = &m_slots[0];
        bool found{};
        for (auto& item : m_slots) {
            if (item.proof.target == family.target && item.proof.scene == family.scene) {
                slot = &item; found = true; break;
            }
            if (item.touched < slot->touched) { slot = &item; }
        }
        if (!found) { *slot = {}; slot->proof.target = family.target; slot->proof.scene = family.scene; }
        slot->touched = ++m_sequence;
        auto& witness = stage == Stage::begin ? slot->proof.begin : slot->proof.pre_render;
        const auto jump = witness.frame > family.frame ? witness.frame - family.frame : family.frame - witness.frame;
        if (witness.frame && jump > 64) {
            slot->proof.begin = {}; slot->proof.pre_render = {};
        }
        witness.valid = (std::min)(witness.valid + 1, uint32_t{1024});
        if (witness.frame && family.frame > witness.frame && family.frame - witness.frame <= 64) {
            witness.advances = (std::min)(witness.advances + 1, uint32_t{1024});
        }
        witness.frame = (std::max)(witness.frame, family.frame);
    }

    std::optional<Proof> ready() const {
        for (const auto& slot : m_slots) {
            const auto& p = slot.proof;
            const auto gap = p.begin.frame > p.pre_render.frame
                ? p.begin.frame - p.pre_render.frame : p.pre_render.frame - p.begin.frame;
            if (p.begin.valid >= 8 && p.pre_render.valid >= 8 &&
                p.begin.advances >= 2 && p.pre_render.advances >= 2 && gap <= 64) { return p; }
        }
        return {};
    }

private:
    struct Slot { Proof proof{}; uint64_t touched{}; };
    std::array<Slot, 4> m_slots{};
    uint64_t m_sequence{};
};
}
