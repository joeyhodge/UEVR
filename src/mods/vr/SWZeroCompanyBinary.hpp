#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uevr::sw_zero_company {

inline constexpr std::array<uint8_t, 16> copy_prologue{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00};
inline constexpr std::array<uint8_t, 39> copy_call_layout{
    0x48, 0x89, 0x74, 0x24, 0x28, 0x48, 0x8B, 0xD7,
    0x48, 0x8B, 0x48, 0x28, 0xFF, 0x80, 0x60, 0x01,
    0x00, 0x00, 0x8B, 0x84, 0x24, 0x20, 0x01, 0x00,
    0x00, 0x89, 0x44, 0x24, 0x20, 0x4C, 0x8B, 0x11,
    0x41, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00};
inline constexpr uint32_t copy_call_offset = 0x7F;
inline constexpr std::array<uint8_t, 24> separate_target_code{
    0x80, 0xB9, 0xB7, 0x01, 0x00, 0x00, 0x00, 0x75, 0x0C,
    0x80, 0xB9, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x75, 0x03,
    0x32, 0xC0, 0xC3, 0xB0, 0x01, 0xC3};

inline constexpr std::array<uint8_t, 21> original_slate_arguments{
    0x4D, 0x8B, 0x61, 0x18, 0x4D, 0x8B, 0xF9, 0x49, 0x8B, 0x41, 0x08,
    0x4D, 0x8B, 0xE8, 0x48, 0x89, 0x95, 0x70, 0x01, 0x00, 0x00};
inline constexpr std::array<uint8_t, 4> original_slate_renderer{0x48, 0x89, 0x4D, 0x88};
inline constexpr std::array<uint8_t, 20> original_register_prologue{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30};
inline constexpr std::array<uint8_t, 18> original_nanite_prologue{
    0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0xC8, 0xFA, 0xFF, 0xFF};
inline constexpr std::array<uint8_t, 35> original_hologram_call{
    0x48, 0x89, 0x74, 0x24, 0x38, 0x49, 0x8B, 0xCE, 0x4C, 0x89, 0x54, 0x24, 0x30,
    0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8D, 0x85, 0x70, 0x10, 0x00, 0x00,
    0x48, 0x89, 0x44, 0x24, 0x20, 0xE8, 0x8B, 0x62, 0x5D, 0x00};

inline constexpr std::array<uint8_t, 21> update_slate_arguments{
    0x4D, 0x8B, 0x79, 0x18, 0x4D, 0x8B, 0xE9, 0x49, 0x8B, 0x41, 0x08,
    0x4D, 0x8B, 0xF0, 0x48, 0x89, 0x95, 0x80, 0x00, 0x00, 0x00};
inline constexpr std::array<uint8_t, 4> update_slate_renderer{0x48, 0x89, 0x4D, 0x18};
inline constexpr std::array<uint8_t, 22> update_register_prologue{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x38,
    0x41, 0x0F, 0xB6, 0xF1, 0x49, 0x8B, 0xE8, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
inline constexpr std::array<uint8_t, 20> update_nanite_prologue{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x80, 0xFC, 0xFF, 0xFF};
inline constexpr std::array<uint8_t, 35> update_hologram_call{
    0x4C, 0x89, 0x64, 0x24, 0x38, 0x49, 0x8B, 0xCE, 0x4C, 0x89, 0x5C, 0x24, 0x30,
    0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8D, 0x85, 0x30, 0x06, 0x00, 0x00,
    0x48, 0x89, 0x44, 0x24, 0x20, 0xE8, 0x6C, 0xF2, 0x4D, 0x00};

// In these updates the constructor is inlined into InitializeHMDDevice. The first
// StereoEmulationHeight reference instead belongs to SetFinalViewRect.
inline constexpr std::array<uint8_t, 28> initialize_hmd_prologue{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x08, 0xFD, 0xFF, 0xFF,
    0x48, 0x81, 0xEC, 0xF8, 0x03, 0x00, 0x00};
inline constexpr std::array<uint8_t, 21> update_stereo_assignment{
    0x48, 0x8D, 0x05, 0xDF, 0xD7, 0xF2, 0x05, 0x48, 0x89, 0x07,
    0x48, 0x8D, 0x05, 0x55, 0xD7, 0xF2, 0x05, 0x48, 0x89, 0x47, 0x08};
inline constexpr std::array<uint8_t, 3> stereo_enabled_code{0xB0, 0x01, 0xC3};
inline constexpr std::array<uint8_t, 14> stereo_rtm_accessor{
    0x33, 0xD2, 0x48, 0x8D, 0x41, 0x08, 0x48, 0x85, 0xC9, 0x48, 0x0F, 0x44, 0xC2, 0xC3};

struct VtableEntry {
    uint32_t slot;
    uint32_t rva;
};
inline constexpr std::array<VtableEntry, 10> update_stereo_entries{{
    {0, 0x7974970}, {1, 0x4183B60}, {4, 0x7973990}, {5, 0x79739A0},
    {8, 0x79739C0}, {9, 0x7973A10}, {11, 0x7973C10}, {12, 0x7974060},
    {14, 0x7974380}, {16, 0x417CC50},
}};
inline constexpr std::array<VtableEntry, 3> update_stereo_rtm_entries{{
    {0, 0x7974760}, {2, 0x7974960}, {8, 0x7974790},
}};

inline constexpr std::array<uint8_t, 21> latest_stereo_assignment{
    0x48, 0x8D, 0x05, 0x9F, 0xE6, 0xF2, 0x05, 0x48, 0x89, 0x07,
    0x48, 0x8D, 0x05, 0x3D, 0xE7, 0xF2, 0x05, 0x48, 0x89, 0x47, 0x08};
inline constexpr std::array<VtableEntry, 10> latest_stereo_entries{{
    {0, 0x79752A0}, {1, 0x4183C00}, {4, 0x79742C0}, {5, 0x79742D0},
    {8, 0x79742F0}, {9, 0x7974340}, {11, 0x7974540}, {12, 0x7974990},
    {14, 0x7974CB0}, {16, 0x417CCF0},
}};
inline constexpr std::array<VtableEntry, 3> latest_stereo_rtm_entries{{
    {0, 0x7975090}, {2, 0x7975290}, {8, 0x79750C0},
}};

struct InlinedStereoLayout {
    uint32_t initialize_hmd_rva;
    uint32_t assignment_rva;
    uint32_t primary_rva;
    uint32_t secondary_rva;
    std::span<const uint8_t> assignment;
    std::span<const VtableEntry> entries;
    std::span<const VtableEntry> rtm_entries;
};
inline constexpr InlinedStereoLayout update_inlined_stereo{
    0x79749C0, 0x7975432, 0xD8A2C18, 0xD8A2B98,
    update_stereo_assignment, update_stereo_entries, update_stereo_rtm_entries};
inline constexpr InlinedStereoLayout latest_inlined_stereo{
    0x79752F0, 0x7975D62, 0xD8A4408, 0xD8A44B0,
    latest_stereo_assignment, latest_stereo_entries, latest_stereo_rtm_entries};

inline constexpr std::array<uint8_t, 21> revision_199049_stereo_assignment{
    0x48, 0x8D, 0x05, 0xF7, 0x58, 0xF5, 0x05, 0x48, 0x89, 0x07,
    0x48, 0x8D, 0x05, 0x6D, 0x58, 0xF5, 0x05, 0x48, 0x89, 0x47, 0x08};
inline constexpr std::array<VtableEntry, 10> revision_199049_stereo_entries{{
    {0, 0x797D020}, {1, 0x4189F20}, {4, 0x797C040}, {5, 0x797C050},
    {8, 0x797C070}, {9, 0x797C0C0}, {11, 0x797C2C0}, {12, 0x797C710},
    {14, 0x797CA30}, {16, 0x4183010},
}};
inline constexpr std::array<VtableEntry, 3> revision_199049_stereo_rtm_entries{{
    {0, 0x797CE10}, {2, 0x797D010}, {8, 0x797CE40},
}};
inline constexpr InlinedStereoLayout revision_199049_inlined_stereo{
    0x797D070, 0x797DAE2, 0xD8D33E0, 0xD8D3360,
    revision_199049_stereo_assignment, revision_199049_stereo_entries, revision_199049_stereo_rtm_entries};
inline constexpr std::array<uint8_t, 35> revision_199049_hologram_call{
    0x4C, 0x89, 0x64, 0x24, 0x38, 0x49, 0x8B, 0xCE, 0x4C, 0x89, 0x5C, 0x24, 0x30,
    0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8D, 0x85, 0x30, 0x06, 0x00, 0x00,
    0x48, 0x89, 0x44, 0x24, 0x20, 0xE8, 0x4C, 0xF3, 0x4D, 0x00};
inline constexpr size_t revision_199049_visible_clusters_load_offset = 0x14E;
inline constexpr std::array<uint8_t, 4> revision_199049_visible_clusters_load{0x49, 0x8B, 0x47, 0x38};
inline constexpr size_t revision_199049_shading_mask_load_offset = 0x7E4;
inline constexpr std::array<uint8_t, 4> revision_199049_shading_mask_load{0x48, 0x8B, 0x41, 0x60};
inline constexpr size_t revision_199049_raster_code_size =
    revision_199049_shading_mask_load_offset + revision_199049_shading_mask_load.size();

// Slot 4 returns FRHIDescriptorHandle through hidden output storage. It must
// never be called with GetNativeResource's pointer-return ABI.
inline constexpr size_t native_resource_slot = 5;
inline constexpr std::array<uint8_t, 71> latest_native_resource_code{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0xB8, 0x00, 0x00,
    0x00, 0x33, 0xDB, 0x48, 0x85, 0xC0, 0x74, 0x09, 0x48, 0x8B, 0x58, 0x20,
    0x48, 0x85, 0xDB, 0x75, 0x21, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x38, 0x48,
    0x85, 0xC0, 0x74, 0x16, 0x48, 0x8B, 0x80, 0xB8, 0x00, 0x00, 0x00, 0x48,
    0x85, 0xC0, 0x74, 0x0A, 0x48, 0x8B, 0x40, 0x20, 0x48, 0x83, 0xC4, 0x20,
    0x5B, 0xC3, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
struct NativeResourceLayout {
    uint32_t texture_vtable_rva;
    uint32_t backbuffer_vtable_rva;
    uint32_t getter_rva;
    std::span<const uint8_t> getter_code;
};
inline constexpr NativeResourceLayout latest_native_resource{
    0xD1F0A58, 0xD1F7950, 0x5F97B80, latest_native_resource_code};
inline constexpr NativeResourceLayout revision_199049_native_resource{
    0xD21F9B8, 0xD2267F8, 0x5F9ECA0, latest_native_resource_code};

struct BinaryLayout {
    uint32_t revision;
    uint32_t slate_rva;
    uint32_t separate_target_rva;
    uint32_t register_texture_rva;
    uint32_t copy_rva;
    uint32_t nanite_rva;
    uint32_t hologram_return_rva;
    uint32_t slate_renderer_offset;
    std::span<const uint8_t> slate_arguments;
    std::span<const uint8_t> slate_renderer;
    std::span<const uint8_t> register_prologue;
    std::span<const uint8_t> nanite_prologue;
    std::span<const uint8_t> hologram_call;
    const InlinedStereoLayout* inlined_stereo{};
    const NativeResourceLayout* native_resource{};
    bool native_fix_hologram_guard{};
};

// These UE5.6.1 builds have the same call ABIs and resource members, but different
// code addresses/register allocation. Never mix independently selected revisions.
inline constexpr std::array<BinaryLayout, 4> binary_layouts{{
    {196320, 0x3A3D140, 0x4EB90C0, 0x2D4D9E0, 0x3711F60, 0x307CB10, 0x2AA6885, 0x44,
        original_slate_arguments, original_slate_renderer, original_register_prologue,
        original_nanite_prologue, original_hologram_call},
    {196985, 0x6275360, 0x770FFF0, 0x577CE70, 0x5F96B70, 0x5A1B4A0, 0x553C234, 0x45,
        update_slate_arguments, update_slate_renderer, update_register_prologue,
        update_nanite_prologue, update_hologram_call, &update_inlined_stereo},
    {197649, 0x6275900, 0x7710500, 0x577CDD0, 0x5F97110, 0x5A1B400, 0x553C194, 0x45,
        update_slate_arguments, update_slate_renderer, update_register_prologue,
        update_nanite_prologue, update_hologram_call, &latest_inlined_stereo, &latest_native_resource},
    {199049, 0x627CBE0, 0x7718180, 0x5783A10, 0x5F9E230, 0x5A220E0, 0x5542D94, 0x45,
        update_slate_arguments, update_slate_renderer, update_register_prologue,
        update_nanite_prologue, revision_199049_hologram_call,
        &revision_199049_inlined_stereo, &revision_199049_native_resource, true},
}};

inline bool should_guard_hologram_dispatch(bool dx12, bool hmd_active, bool native_stereo,
    bool native_fix, bool native_fix_validated, uintptr_t expected_caller, uintptr_t actual_caller)
{
    return dx12 && hmd_active && native_stereo && (!native_fix || native_fix_validated) &&
        expected_caller != 0 && expected_caller == actual_caller;
}

inline bool matches_bytes(std::span<const uint8_t> code, std::span<const uint8_t> expected) {
    return !expected.empty() && code.size() >= expected.size() &&
        std::equal(expected.begin(), expected.end(), code.begin());
}

inline bool matches_revision_199049_hologram_raster(std::span<const uint8_t> code) {
    return code.size() >= revision_199049_raster_code_size &&
        matches_bytes(code.subspan(revision_199049_visible_clusters_load_offset), revision_199049_visible_clusters_load) &&
        matches_bytes(code.subspan(revision_199049_shading_mask_load_offset), revision_199049_shading_mask_load);
}

inline bool matches_native_resource_accessor(const BinaryLayout& layout, uintptr_t image_base,
    uintptr_t vtable, uintptr_t getter, std::span<const uint8_t> code)
{
    const auto* const native = layout.native_resource;
    if (native == nullptr || image_base == 0 || vtable < image_base || getter < image_base) {
        return false;
    }
    return (vtable - image_base == native->texture_vtable_rva ||
            vtable - image_base == native->backbuffer_vtable_rva) &&
        getter - image_base == native->getter_rva && matches_bytes(code, native->getter_code);
}

inline bool matches_pinned_ui_target(uintptr_t expected, uintptr_t active, uintptr_t retained, uintptr_t native) {
    return expected != 0 && expected == active && expected == retained && native != 0;
}

template <typename Validated, typename Legacy>
auto resolve_native_resource(const BinaryLayout* layout, Validated validated, Legacy legacy) {
    // A failed validated lookup must not restart the unsafe legacy slot probe.
    if (layout != nullptr && layout->native_resource != nullptr) {
        return validated();
    }
    return legacy();
}

inline bool matches_slate_arguments(const BinaryLayout& layout, std::span<const uint8_t> code) {
    // These moves survive SafetyHook's entry patch. RCX=this, RDX=24-byte sret,
    // R8=FRDGBuilder, R9=inputs in both builds; do not inspect a patched prologue.
    return code.size() >= 0x2D && code.size() >= layout.slate_renderer_offset &&
        matches_bytes(code.subspan(0x2D), layout.slate_arguments) &&
        matches_bytes(code.subspan(layout.slate_renderer_offset), layout.slate_renderer);
}

template <typename Matcher>
const BinaryLayout* select_unique_layout(Matcher&& matches) {
    const BinaryLayout* selected{};
    for (const auto& layout : binary_layouts) {
        if (matches(layout)) {
            if (selected != nullptr) {
                return nullptr;
            }
            selected = &layout;
        }
    }
    return selected;
}

} // namespace uevr::sw_zero_company
