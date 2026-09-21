#pragma once

#include <algorithm>
#include <optional>
#include "KtjLCloudOutputContracts.hpp"

namespace uevr::ktjl::cloud_output {
inline constexpr uintptr_t producer_return_rva = 0x767936;
inline constexpr uintptr_t consumer_ret_rva = 0xA1ABF9;
inline constexpr std::array<uintptr_t, 2> consumer_returns{0xA1A927, 0xA1AA2C};
inline constexpr uintptr_t targets_rva = 0x8F6FC88, quality_rva = 0x9233E70, flags_rva = 0x8F7B530;
inline constexpr std::array<size_t, 2> slots{0x1F38, 0x1F40};
inline constexpr std::array<uint32_t, 2> formats{10, 15}, target_flags{0x10009, 0x10001};

struct Pair {
    uintptr_t renderer{};
    fog::Header views{};
    std::array<uintptr_t, 2> states{};
    bool operator==(const Pair&) const = default;
};

inline std::optional<Pair> read_pair(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t view) {
    fog::ViewPrefix first{};
    if (!sdk::ktjl::pointer(view) || !m.load(view, first) || first.vtable != base + fog::view_vtable_rva ||
        !sdk::ktjl::pointer(first.family) || first.family < 0x10) { return {}; }
    Pair p{};
    p.renderer = first.family - 0x10;
    if (!m.load(p.renderer + fog::views_offset, p.views) || p.views.count != 2 ||
        p.views.capacity < 2 || p.views.capacity > 16 || !sdk::ktjl::pointer(p.views.data) ||
        (view != p.views.data && view != p.views.data + fog::view_stride)) { return {}; }
    for (size_t eye = 0; eye != 2; ++eye) {
        fog::ViewPrefix v{};
        uintptr_t vt{};
        const auto address = p.views.data + eye * fog::view_stride;
        if (!m.load(address, v) || v.vtable != first.vtable || v.family != first.family ||
            !m.load(address + cloud::state_offset, p.states[eye]) || p.states[eye] != v.state ||
            !sdk::ktjl::pointer(v.state) || !m.load(v.state, vt) || vt != base + sdk::ktjl::stereo::state_vtable_rva) { return {}; }
    }
    fog::Header again{};
    if (p.states[0] == p.states[1] || !m.load(p.renderer + fog::views_offset, again) || again != p.views) { return {}; }
    return p;
}

struct Extent {
    int32_t width{}, height{};
    uint32_t flags{};
    bool operator==(const Extent&) const = default;
};

inline std::optional<Extent> read_extent(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t rhi) {
    uintptr_t targets{};
    uint32_t quality{};
    std::array<int32_t, 2> size{};
    Extent extent{};
    if (!sdk::ktjl::pointer(rhi) || !m.load(rhi + 0xD0, targets) ||
        !m.load(base + quality_rva, quality) || (quality & 31) > 2 ||
        !m.load(base + flags_rva, extent.flags)) { return {}; }
    if (targets == 0) { targets = base + targets_rva; }
    if (!sdk::ktjl::pointer(targets) || !m.load(targets + 0x514, size) ||
        size[0] <= 0 || size[0] > 32768 || size[1] <= 0 || size[1] > 32768) { return {}; }
    const auto divisor = 4 >> (quality & 31);
    extent.width = std::max(8, size[0] / divisor);
    extent.height = std::max(8, size[1] / divisor);
    return extent;
}

inline bool read_resource(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t address,
                          size_t slot, fog::Resource& out) {
    std::array<uint8_t, 0xF0> bytes{};
    if (slot >= slots.size() || !sdk::ktjl::pointer(address) || !m.load(address, bytes) ||
        fog::field<uintptr_t>(bytes, 0) != base + fog::pool_vtable_rva ||
        fog::field<int32_t>(bytes, 0x88) < 2 || fog::field<uint8_t>(bytes, 0xE0) != 0 ||
        fog::field<uintptr_t>(bytes, 0xE8) != base + fog::pool_rva) { return false; }
    out.address = address;
    out.target = fog::field<uintptr_t>(bytes, 8);
    out.shader = fog::field<uintptr_t>(bytes, 0x10);
    out.uav = fog::field<uintptr_t>(bytes, 0x18);
    std::memcpy(out.descriptor.bytes.data(), bytes.data() + 0x90, out.descriptor.bytes.size());
    const auto& d = out.descriptor;
    return sdk::ktjl::pointer(out.target) && sdk::ktjl::pointer(out.shader) && sdk::ktjl::pointer(out.uav) &&
        d.at<int32_t>(0x14) >= 8 && d.at<int32_t>(0x14) <= 32768 &&
        d.at<int32_t>(0x18) >= 8 && d.at<int32_t>(0x18) <= 32768 && d.at<int32_t>(0x1C) == 0 &&
        d.at<int32_t>(0x20) == 1 && d.at<uint16_t>(0x24) == 0 && d.at<uint16_t>(0x26) == 1 &&
        d.at<uint16_t>(0x28) == 1 && d.at<uint32_t>(0x2C) == formats[slot] &&
        d.at<uint32_t>(0x34) == target_flags[slot] && d.at<uint16_t>(0x38) == 0;
}

inline bool read_eye(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t state,
                     std::array<fog::Resource, 2>& resources, bool allow_empty = false) {
    for (size_t i = 0; i != slots.size(); ++i) {
        uintptr_t ref{};
        if (!m.load(state + slots[i], ref)) { return false; }
        if (ref == 0 && allow_empty) { continue; }
        if (!read_resource(m, base, ref, i, resources[i])) { return false; }
    }
    return !resources[0].address || !resources[1].address || cloud::distinct(resources[0], resources[1]);
}

inline bool matches_extent(const std::array<fog::Resource, 2>& resources, const Extent& size) {
    for (size_t i = 0; i != resources.size(); ++i) {
        const auto& d = resources[i].descriptor;
        const auto flags = i == 0 ? size.flags : 0u;
        if (!resources[i].address || d.at<int32_t>(0x14) != size.width || d.at<int32_t>(0x18) != size.height ||
            (d.at<uint32_t>(0x30) & ~0x80000000u) != (flags & ~0x80000000u)) { return false; }
    }
    return true;
}

enum class Outcome { passthrough, existing, prepared, rejected };

template<class Prepare>
Outcome prepare_secondary(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t rhi, uintptr_t view,
                          uintptr_t renderer, uintptr_t caller_rhi, uintptr_t array, uintptr_t stack,
                          Prepare&& prepare) {
    const auto p = read_pair(m, base, view);
    // The recursive engine call below is for Views[1], so it never recurses again.
    if (!p || view != p->views.data) { return Outcome::passthrough; }
    uintptr_t caller{};
    if (renderer != p->renderer || caller_rhi != rhi || array != p->renderer + fog::views_offset ||
        !m.load(stack, caller) || caller != base + producer_return_rva) { return Outcome::passthrough; }
    const auto size = read_extent(m, base, rhi);
    std::array<fog::Resource, 2> right{};
    if (!size || !read_eye(m, base, p->states[1], right, true)) { return Outcome::rejected; }
    if (matches_extent(right, *size)) { return Outcome::existing; }
    // Run only the game's resource producer, never a renderer or PostInit replay.
    // It owns resizing, refcounts, and the two slots released by FSceneViewState.
    if (!prepare(rhi, p->views.data + fog::view_stride) || read_pair(m, base, view) != p ||
        read_extent(m, base, rhi) != size || !read_eye(m, base, p->states[1], right) ||
        !matches_extent(right, *size)) { return Outcome::rejected; }
    return Outcome::prepared;
}

inline bool reject_consumer(const sdk::discovery::Memory& m, uintptr_t base, uintptr_t view, uintptr_t stack) {
    const auto p = read_pair(m, base, view);
    if (!p || view != p->views.data + fog::view_stride) { return false; }
    uintptr_t caller{};
    if (!m.load(stack, caller) || std::none_of(consumer_returns.begin(), consumer_returns.end(),
            [&](uintptr_t rva) { return caller == base + rva; })) { return false; }
    std::array<fog::Resource, 2> left{}, right{};
    if (!read_eye(m, base, p->states[0], left) || !read_eye(m, base, p->states[1], right)) { return true; }
    for (size_t i = 0; i != right.size(); ++i) {
        if (!cloud::distinct(left[i], right[i]) ||
            left[i].descriptor.at<int32_t>(0x14) != right[i].descriptor.at<int32_t>(0x14) ||
            left[i].descriptor.at<int32_t>(0x18) != right[i].descriptor.at<int32_t>(0x18)) { return true; }
    }
    return read_pair(m, base, view) != p;
}
}
