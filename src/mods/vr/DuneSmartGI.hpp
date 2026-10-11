#pragma once

#include "DuneFrameHandoffDiscovery.hpp"
#include "DuneNativeTransaction.hpp"
#include "DuneSmartGIContract.hpp"

#include <cmath>
#include <cstring>
#include <type_traits>

namespace uevr::dune_smartgi {

inline constexpr size_t exposure_state_offset = 0x2478;
inline constexpr size_t view_info_state_offset = 0x2498;
inline constexpr size_t clipmap_count_offset = 0x1E50;
inline constexpr size_t clipmaps_offset = 0x1E68;
inline constexpr size_t dimensions_offset = 0x2268;
inline constexpr size_t clipmap_stride = 0x80;
inline constexpr size_t maximum_clipmaps = 8;

struct Vector3 { double x{}, y{}, z{}; };
struct Vector4 { double x{}, y{}, z{}, w{1.0}; };

// Contains numbers only. UE creates and owns the actual uniform buffer after
// consuming this value; no view state, exposure or GPU resource is replaced.
struct ClipmapInfo {
    std::array<int32_t, 3> dimensions{};
    int32_t count{};
    std::array<Vector3, maximum_clipmaps> world_to_uv_scale{}, world_to_uv_bias{}, origin{}, extent{};
    std::array<Vector4, maximum_clipmaps> voxel_size_and_radius{};
};
static_assert(std::is_trivially_copyable_v<ClipmapInfo> && sizeof(ClipmapInfo) == 0x410);
static_assert(offsetof(ClipmapInfo, count) == 0xC && offsetof(ClipmapInfo, world_to_uv_bias) == 0xD0 &&
    offsetof(ClipmapInfo, origin) == 0x190 && offsetof(ClipmapInfo, extent) == 0x250 &&
    offsetof(ClipmapInfo, voxel_size_and_radius) == 0x310);

inline constexpr auto exposure_code = std::to_array<uint8_t>({
    0x48,0x8B,0x43,0x10,0x80,0xB8,0x8B,0,0,0,0,0x74,0x4C,0xF6,0x40,0x30,0x01,0x74,0x46,
    0x48,0x8B,0x43,0x18,0x48,0x8B,0xCB,0x48,0x89,0x83,0x78,0x24,0,0,
    0xE8,0,0,0,0,0x84,0xC0,0x74,0x2F,0x48,0x8B,0x4B,0x10,0x4C,0x63,0x83,0xA8,0x13,0,0,
    0x48,0x8B,0x51,0x08,0x4A,0x8B,0x3C,0xC2,0x48,0x8B,0xCF,0xE8,0,0,0,0,
    0x84,0xC0,0x48,0x8B,0xC3,0x74,0x10,0x48,0x8B,0x4F,0x18,0x48,0x89,0x8B,0x78,0x24,0,0,0xEB,0x03});
inline constexpr std::array<size_t, 2> exposure_displacements{34, 65};
inline constexpr auto secondary_test = std::to_array<uint8_t>({0x83,0xB9,0xA0,0x13,0,0,0x02,0x0F,0x94,0xC0,0xC3});
inline constexpr auto primary_test = std::to_array<uint8_t>({0x83,0xB9,0xA0,0x13,0,0,0x01,0x0F,0x96,0xC0,0xC3});

inline bool valid_getter(std::span<const uint8_t> code) {
    return code.size() == getter_code.size() &&
        dune_frame::matches_masked(code, getter_code, getter_displacements);
}

struct UnwindFunction {
    uint32_t begin{}, end{}, unwind{};
    bool operator==(const UnwindFunction&) const = default;
};
static_assert(sizeof(UnwindFunction) == 12);

// The getter's loop and epilogue both chain directly to its entry, not to each
// other. Permit that ONLY for the complete validated getter; do not relax the
// shared renderer/callback unwind resolver for unrelated functions.
template <typename Read, typename Lookup>
bool getter_segment_owner(dune_frame::CodeImage image, uintptr_t entry, uintptr_t at,
    Read&& read, Lookup&& lookup) {
    if (!image.contains(entry, getter_code.size()) || at < entry || at - entry >= getter_code.size()) { return false; }
    const auto root = lookup(entry), segment = lookup(at);
    if (!root || !segment || root->begin != entry - image.base || segment->begin != at - image.base) { return false; }
    auto current = *segment;
    for (unsigned depth = 0; depth < 4; ++depth) {
        std::array<uint8_t, 4> info{};
        if (current.end <= current.begin || !image.contains(image.base + current.begin, current.end - current.begin) ||
            !image.contains(image.base + current.unwind, info.size()) ||
            !read(image.base + current.unwind, info.data(), info.size()) || (info[0] & 7) != 1) { return false; }
        const auto flags = info[0] >> 3;
        if (flags == 0) { return current == *root; }
        if (flags != 4) { return false; }
        const auto chain = image.base + current.unwind + 4 + ((static_cast<size_t>(info[2]) + 1) & ~size_t{1}) * 2;
        UnwindFunction parent{};
        if (!image.contains(chain, sizeof(parent)) || !read(chain, &parent, sizeof(parent)) ||
            parent.begin >= current.begin || parent.end > current.begin || parent.end <= parent.begin) { return false; }
        const auto registered = lookup(image.base + parent.begin);
        if (!registered || *registered != parent) { return false; }
        current = parent;
    }
    return false;
}

template <typename SegmentLength, typename OwnsCode>
bool valid_getter_unwind(uintptr_t entry, SegmentLength&& length, OwnsCode&& owns) {
    if (entry > std::numeric_limits<uintptr_t>::max() - getter_code.size()) { return false; }
    size_t offset{};
    for (unsigned segments = 0; offset < getter_code.size() && segments < 8; ++segments) {
        const auto size = length(entry + offset);
        if (!size || !*size || *size > getter_code.size() - offset || !owns(entry, entry + offset, *size)) { return false; }
        offset += *size;
    }
    return offset == getter_code.size();
}

template <typename ReadCode>
bool valid_exposure_constructor(dune_frame::CodeImage image, uintptr_t entry,
    std::span<const uint8_t> code, ReadCode&& read) {
    if (!image.contains(entry, code.size()) || !dune_native::valid_constructor(code)) { return false; }
    std::optional<size_t> offset{};
    for (size_t i = 0; i + exposure_code.size() <= code.size(); ++i) {
        if (!dune_frame::matches_masked(code.subspan(i), exposure_code, exposure_displacements)) { continue; }
        if (offset) { return false; }
        offset = i;
    }
    if (!offset) { return false; }
    const auto secondary = dune_frame::relative_call(image, entry + *offset + 33, code.subspan(*offset + 33));
    const auto primary = dune_frame::relative_call(image, entry + *offset + 64, code.subspan(*offset + 64));
    auto s = secondary_test, p = primary_test;
    return secondary && primary && read(*secondary, s.data(), s.size()) && s == secondary_test &&
        read(*primary, p.data(), p.size()) && p == primary_test;
}

inline bool pointer(uintptr_t address) {
    return address >= 0x10000 && (address & 7) == 0 &&
        address <= std::numeric_limits<uintptr_t>::max() - 0x3000;
}

constexpr bool enabled(bool validated, bool dx12, bool openxr, bool hmd_active, bool native) {
    return validated && dx12 && openxr && hmd_active && native;
}

// The game's constructor establishes the exposure link to the primary eye.
// Paired families also prove that link against their current first view. The
// right singleton is accepted only under the validated Native Fix adapter.
template <typename Read, typename ValidObject>
std::optional<uintptr_t> missing_secondary_source(uintptr_t view, bool native_fix,
    Read&& read, ValidObject&& valid) {
    if (!pointer(view)) { return {}; }
    const auto eye = dune_native::read_eye(view, read);
    uintptr_t own{}, primary{}, data{}, scene{};
    std::array<int32_t, 2> shape{};
    int32_t count{};
    if (!eye || eye->stereo_index != 1 || eye->primary_index != 0 ||
        !pointer(eye->family) || !pointer(eye->state) ||
        !read(view + view_info_state_offset, &own, sizeof(own)) || own != eye->state ||
        !read(view + exposure_state_offset, &primary, sizeof(primary)) || !pointer(primary) || primary == own ||
        !valid(own) || !valid(primary) || !valid(eye->family) ||
        !read(own + clipmap_count_offset, &count, sizeof(count)) || count != 0 ||
        !read(eye->family + 8, &data, sizeof(data)) || !pointer(data) ||
        !read(eye->family + 0x10, shape.data(), sizeof(shape)) ||
        (shape[0] != 1 && shape[0] != 2) || shape[1] < shape[0] || shape[1] > 1024 ||
        !read(eye->family + 0x28, &scene, sizeof(scene)) || !pointer(scene) || !valid(scene)) { return {}; }
    uintptr_t own_table{}, primary_table{};
    std::array<uintptr_t, 2> views{};
    if (!read(own, &own_table, sizeof(own_table)) || !read(primary, &primary_table, sizeof(primary_table)) ||
        own_table != primary_table || !read(data, views.data(), shape[0] * sizeof(uintptr_t))) { return {}; }
    if (shape[0] == 1) {
        return native_fix && eye->pass == 1 && views[0] == view ? std::optional{primary} : std::nullopt;
    }
    if (eye->pass != 2 || views[1] != view || views[0] == view) { return {}; }
    const auto left = dune_native::read_eye(views[0], read);
    uintptr_t left_state{};
    return left && left->family == eye->family && left->pass == 1 && left->stereo_index == 0 &&
        left->primary_index == 0 && left->state == primary &&
        read(views[0] + view_info_state_offset, &left_state, sizeof(left_state)) && left_state == primary
        ? std::optional{primary} : std::nullopt;
}

inline bool finite(Vector3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool positive(Vector3 v) { return finite(v) && v.x > 0 && v.y > 0 && v.z > 0; }

template <typename Read>
std::optional<ClipmapInfo> read_primary_clipmaps(uintptr_t state, Read&& read) {
    if (!pointer(state)) { return {}; }
    ClipmapInfo result{};
    if (!read(state + clipmap_count_offset, &result.count, sizeof(result.count)) ||
        result.count < 1 || result.count > maximum_clipmaps ||
        !read(state + dimensions_offset, result.dimensions.data(), sizeof(result.dimensions)) ||
        std::any_of(result.dimensions.begin(), result.dimensions.end(), [](int32_t n) { return n <= 0 || n > 65536; })) { return {}; }
    for (int32_t i = 0; i < result.count; ++i) {
        const auto entry = state + clipmaps_offset + static_cast<size_t>(i) * clipmap_stride;
        // Read the contiguous numeric prefix once, not each scalar separately.
        struct Sample { Vector3 origin, extent, voxel; float radius; } sample{};
        static_assert(offsetof(Sample, extent) == 0x18 && offsetof(Sample, voxel) == 0x30 &&
            offsetof(Sample, radius) == 0x48 && sizeof(Sample) == 0x50);
        if (!read(entry, &sample, sizeof(sample)) || !finite(sample.origin) || !positive(sample.extent) ||
            !positive(sample.voxel) || !std::isfinite(sample.radius) || sample.radius < 0) { return {}; }
        const auto e = sample.extent, o = sample.origin, voxel = sample.voxel;
        const Vector3 scale{0.5 / e.x, 0.5 / e.y, 0.5 / e.z};
        const Vector3 bias{(e.x - o.x) * scale.x, (e.y - o.y) * scale.y, (e.z - o.z) * scale.z};
        if (!positive(scale) || !finite(bias)) { return {}; }
        result.origin[i] = o; result.extent[i] = e;
        result.world_to_uv_scale[i] = scale; result.world_to_uv_bias[i] = bias;
        result.voxel_size_and_radius[i] = {voxel.x, voxel.y, voxel.z, static_cast<double>(sample.radius)};
    }
    int32_t count{};
    return read(state + clipmap_count_offset, &count, sizeof(count)) && count == result.count
        ? std::optional{result} : std::nullopt;
}

} // namespace uevr::dune_smartgi
