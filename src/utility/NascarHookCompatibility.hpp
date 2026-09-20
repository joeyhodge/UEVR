#pragma once

#include "Nascar26HookCompatibility.hpp"

namespace uevr::nascar {
// The state machines and object-table mechanism are layout-independent. The
// two verified executables select different, immutable ABI descriptions.
using nascar26::RedrawIdentity;
using nascar26::SyncedRedraw;
using nascar26::completed_synced_redraw;
using nascar26::GhostOwner;
using nascar26::GhostOwnerGate;
using nascar26::GhostCall;
using nascar26::GhostCallScope;
using nascar26::GhostStatus;
using nascar26::NativeOwnedObject;
using nascar26::native_owner_root_flag;
using nascar26::native_display_gamma_valid;
using nascar26::select_native_display_gamma;
using nascar26::NativeDisplayGamma;
using nascar26::NativeRect;
using nascar26::NativeView;
using nascar26::NativeCall;
using nascar26::NativeCallScope;
using nascar26::NativeFamilyTargets;
using nascar26::read_native_family_targets;
using nascar26::native_main_family_targets_valid;
using nascar26::native_projection_valid;
using nascar26::native_rect_valid;
using nascar26::native_pair_valid;
using nascar26::NativePairKey;
using nascar26::NativePairGate;
using nascar26::ghost_pair_valid;
using nascar26::ghost_consumer_matches;
using nascar26::valid_texture_desc;
using nascar26::scene_observation_fresh;
using nascar26::copy_compatible_format;
using nascar26::SceneIdentity;
using nascar26::SceneStability;
using nascar26::WindowSize;
using nascar26::read_memory;
using nascar26::find_virtual_slot_in_image;
using nascar26::intersects_image;
using nascar26::protect_external_memory;
using nascar26::SlotPatch;
using nascar26::ObjectVTable;
using nascar26::SlateResourceView;
using nascar26::supports_rendering_method;
using nascar26::needs_generic_renderer_hook;
using nascar26::viewport_client_subobject;
using nascar26::slate_subobject_from_viewport;
using nascar26::slate_slots;
using nascar26::native_capture_frt_slots;
using nascar26::native_display_gamma_slot;

struct Layout {
    uintptr_t engine_global_rva, engine_vtable_rva;
    size_t engine_slots, tick_slot;
    uintptr_t tick_rva, engine_viewport_offset;
    uintptr_t viewport_dispatch_vtable_rva;
    size_t viewport_dispatch_slots;
    uintptr_t draw_rva, viewport_draw_rva, viewport_vtable_rva;
    uintptr_t localplayer_vtable_rva;
    size_t localplayer_slots, init_options_slot, calc_scene_view_slot, projection_data_slot;
    uintptr_t init_options_rva, calc_scene_view_rva, projection_data_rva;
    uintptr_t init_options_return_rva, projection_data_return_rva, calc_scene_view_return_rva;
    uintptr_t view_state_vtable_rva, view_vtable_rva;
    uintptr_t renderer_global_rva, renderer_vtable_rva;
    size_t renderer_slots;
    uintptr_t renderer_single_rva, renderer_plural_rva, renderer_return_rva;
    uintptr_t family_copy_rva, family_delete_rva, family_vtable_rva, family_context_vtable_rva;
    size_t family_size, family_borrowed_offset, family_additional_offset;
    uintptr_t stereo_vtable_rva;
    size_t stereo_slots, stereo_manager_slot;
    uintptr_t stereo_enabled_rva, stereo_desired_rva, stereo_pass_rva, stereo_manager_rva;
    uintptr_t stereo_rect_rva, stereo_offset_rva, stereo_projection_rva, stereo_render_rva;
    uintptr_t slate_vtable_rva, slate_getter_rva, slate_getter_return_rva;
    uintptr_t texture_vtable_rva, texture_getter_rva;
    size_t texture_getter_slot, texture_resource_offset, texture_owner_resource_offset;
    uintptr_t texture_resource_vtable_rva, native_capture_frt_vtable_rva, native_capture_display_gamma_rva, texture_owner_vtable_rva;
    uintptr_t native_owner_set_flags_rva;
    size_t object_item_object_offset, object_item_flags_offset;
    bool packed_object_item;
    size_t world_instance_offset, world_scene_offset;
    size_t options_player_index_offset, options_pass_offset, options_index_offset;
    size_t view_player_index_offset, view_constrained_offset, view_rect_offset, view_pass_offset, view_index_offset, view_primary_offset;
};

inline constexpr Layout layout26{
    0xa2815c0, 0x8506718, 175, 94, 0x3b34970, 0xc88,
    0x8a08170, 61, 0x3b58020, 0x41ba530, 0x86d9958,
    0x85c7bd0, 116, 91, 92, 109, 0x3cc9130, 0x3cc88a0, 0x3cd1f20,
    0x3cc8b08, 0x3cc9310, 0x3b58946, 0x8027558, 0x8026d80,
    0xa250d10, 0x8033248, 76, 0x2b09dd0, 0x2b09680, 0x3b5969b,
    0x3ff5cc0, 0x3ff95b0, 0x86b3cd0, 0x86b3cd8, 0x198, 0x160, 0xb0,
    0x8769fc0, 21, 16, 0x120eef0, 0x429c770, 0x429dac0, 0x121d400,
    0x4293b00, 0x4294f20, 0x429d670, 0x42a9d00,
    0x86d9b78, 0x40c8690, 0x30578a5,
    0x80a1448, 0x2dd5c00, 5, 0xd0, 0x138, 0x87112a8, 0x8711370, 0x418e810, 0x8711bf0,
    0x150e680, 8, 0, true, 0x228, 0x250, 0x180, 0x1c0, 0x1c4,
    0x370, 0x380, 0x390, 0xdd0, 0xdd4, 0xdd8
};

// NASCAR25 5.5.4: independently checked against the runtime image and source.
inline constexpr Layout layout25{
    0x9cd47a0, 0x81bf128, 176, 97, 0x395d2e0, 0xbd0,
    0x8677808, 59, 0x399a8c0, 0x3fbd6e0, 0x8322820,
    0x8260e08, 118, 94, 95, 111, 0x3af61b0, 0x3af5990, 0x3afda40,
    0x3af5bd9, 0x3af6217, 0x399b199, 0x7cc48b8, 0x7cc4148,
    0x9ca7568, 0x7ccfca8, 71, 0x292ecb0, 0x292df20, 0x399be96,
    0x3e11d20, 0x3e16a40, 0x830af38, 0x7c6e930, 0x1b0, 0x178, 0xc8,
    0x8362640, 20, 15, 0x1145510, 0x3fc4b50, 0x3fc69a0, 0x11503f0,
    0x3fb5980, 0x3fb7450, 0x3fc65b0, 0x3fdd940,
    0x8322a60, 0x3ed8540, 0x2e52e53,
    0x7d2e6b8, 0x2bd7640, 4, 0xb8, 0x118, 0x8357000, 0x83570e8, 0x3f9e960, 0x8358dc8,
    0x14637c0, 0, 8, false, 0x1d8, 0x200, 0x170, 0x1b0, 0x1b4,
    0x338, 0x348, 0x358, 0x15e0, 0x15e4, 0x15e8
};

namespace title25 {
inline constexpr uintptr_t clear_root_flags_rva = 0x140ae50;
inline constexpr size_t rhi_command_root_offset = 0x28;

constexpr bool defer_native_capture_until_pose(
    bool exact_title, bool validated_build, bool dx12, bool requested, bool render_pose_ready) {
    return exact_title && validated_build && dx12 && requested && !render_pose_ready;
}

struct DedicatedUIReadiness {
    bool exact_title{};
    bool validated_build{};
    bool dx12{};
    bool code_preserving_mode{};
    bool game_data_initialized{};
    bool engine_valid{};
    bool slate_hook_valid{};
    bool stable_slate_draw{};
    bool render_callback_seen{};
    bool packed_scene_target_valid{};
};

constexpr bool can_initialize_dedicated_ui(const DedicatedUIReadiness& evidence) {
    return evidence.exact_title && evidence.validated_build && evidence.dx12 &&
        evidence.code_preserving_mode && evidence.game_data_initialized && evidence.engine_valid &&
        evidence.slate_hook_valid && evidence.stable_slate_draw && evidence.render_callback_seen &&
        evidence.packed_scene_target_valid;
}

struct NativeCopySourceStates {
    D3D12_RESOURCE_STATES left;
    D3D12_RESOURCE_STATES right;
};

constexpr std::optional<NativeCopySourceStates> native_copy_source_states(
    bool exact_title, bool validated_build, bool dx12, bool native_fix, bool owned_shader_read_copy) {
    if (!exact_title || !validated_build || !dx12 || !native_fix || !owned_shader_read_copy) { return {}; }
    return NativeCopySourceStates{
        static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    };
}

enum class NativeCopyLayout { double_wide, texture_array };

template<class Commands>
void copy_native_eye_pair(Commands& commands,
    ID3D12Resource* left, ID3D12Resource* right, ID3D12Resource* destination,
    D3D12_BOX left_box, D3D12_BOX right_box, UINT right_x,
    const NativeCopySourceStates& states, NativeCopyLayout layout) {
    // The owned left copy is shader-readable; the engine leaves the right capture in RTV.
    // Existing per-source helpers restore each state without changing the shared stereo helper.
    if (layout == NativeCopyLayout::texture_array) {
        commands.copy_region_to_subresource(left, destination, &left_box, 0,
            states.left, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands.copy_region_to_subresource(right, destination, &right_box, 1,
            states.right, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        commands.copy_region(left, destination, &left_box, 0, 0, 0,
            states.left, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands.copy_region(right, destination, &right_box, right_x, 0, 0,
            states.right, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

constexpr bool uses_validated_rhi_root(bool target, bool build, bool dx12, bool contract) {
    return target && build && dx12 && contract;
}

template<class Read>
std::optional<uintptr_t> read_rhi_command_root(uintptr_t command_list, bool validated, Read&& read) {
    if (!validated || !command_list || (command_list & (alignof(uintptr_t) - 1)) != 0 ||
        command_list > UINTPTR_MAX - rhi_command_root_offset - sizeof(uintptr_t)) { return {}; }
    uintptr_t root{};
    if (!read(command_list + rhi_command_root_offset, &root, sizeof(root)) ||
        (root & (alignof(uintptr_t) - 1)) != 0) { return {}; }
    // An empty list is valid; never search FMemStackBase for a substitute.
    return root;
}

constexpr bool matches_name(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    path = path.substr(slash == path.npos ? 0 : slash + 1);
    constexpr std::wstring_view name = L"nascar25_steam-win64-shipping.exe";
    if (path.size() != name.size()) { return false; }
    for (size_t i = 0; i < name.size(); ++i) {
        const auto c = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (c != name[i]) { return false; }
    }
    return true;
}
constexpr bool matches_build_metadata(uint32_t ms, uint32_t ls, uint32_t timestamp, size_t size) {
    return ms == 0x50005 && ls == 0x40000 && timestamp == 0x9fdd0f8a && size == 0xbb5e000;
}
void initialize();
bool is_target();
bool is_validated_build();
bool validate_rhi_command_layout();
bool validate_synced_redraw();
bool validate_ghost_view_setup();
bool validate_native_renderer();
bool validate_native_rooting();
bool native_owned_object_valid(const NativeOwnedObject& owner, uintptr_t item, bool require_root);
std::optional<float> read_native_display_gamma();
uintptr_t image_base();
}

inline bool is_title25() { return title25::is_target(); }
inline const Layout& layout() { return is_title25() ? layout25 : layout26; }
inline void initialize() { title25::initialize(); if (!is_title25()) { nascar26::initialize(); } }
inline bool is_target() { return is_title25() || nascar26::is_target(); }
inline bool is_validated_build() { return is_title25() ? title25::is_validated_build() : nascar26::is_validated_build(); }
constexpr bool uses_legacy_dedicated_ui(bool title25, bool validated_build, bool dx12) {
    return title25 && validated_build && dx12;
}
inline bool uses_legacy_dedicated_ui(bool dx12) {
    return uses_legacy_dedicated_ui(is_title25(), is_validated_build(), dx12);
}
inline uintptr_t image_base() { return is_title25() ? title25::image_base() : nascar26::image_base(); }
inline bool validate_synced_redraw() { return is_title25() ? title25::validate_synced_redraw() : nascar26::validate_synced_redraw(); }
inline bool validate_ghost_view_setup() { return is_title25() ? title25::validate_ghost_view_setup() : nascar26::validate_ghost_view_setup(); }
inline bool validate_native_renderer() { return is_title25() ? title25::validate_native_renderer() : nascar26::validate_native_renderer(); }
inline bool validate_native_rooting() { return is_title25() ? title25::validate_native_rooting() : nascar26::validate_native_rooting(); }
inline bool native_owned_object_valid(const NativeOwnedObject& owner, uintptr_t item, bool root = true) {
    return is_title25() ? title25::native_owned_object_valid(owner, item, root) : nascar26::native_owned_object_valid(owner, item, root);
}
inline std::optional<float> read_native_display_gamma() {
    return is_title25() ? title25::read_native_display_gamma() : nascar26::read_native_display_gamma();
}

inline void restore_ghost_bootstrap_labels(void* options, const Layout& abi = layout()) {
    const int32_t primary = 1, index = 0;
    std::memcpy(static_cast<uint8_t*>(options) + abi.options_pass_offset, &primary, sizeof(primary));
    std::memcpy(static_cast<uint8_t*>(options) + abi.options_index_offset, &index, sizeof(index));
}

class NativeSingletonScope {
public:
    bool apply(void* view, uintptr_t family, uintptr_t replacement, const Layout& abi = layout()) {
        if (m_view || !view || !family || !replacement) { return false; }
        auto* bytes = static_cast<uint8_t*>(view);
        m_pass_offset = abi.view_pass_offset;
        std::memcpy(&m_family, bytes + 8, sizeof(m_family));
        std::memcpy(&m_pass, bytes + m_pass_offset, sizeof(m_pass));
        int32_t primary{};
        std::memcpy(&primary, bytes + abi.view_primary_offset, sizeof(primary));
        if (m_family != family || m_pass != 2 || primary != 0) { return false; }
        m_view = bytes;
        const int32_t primary_pass = 1;
        std::memcpy(bytes + 8, &replacement, sizeof(replacement));
        std::memcpy(bytes + m_pass_offset, &primary_pass, sizeof(primary_pass));
        return true;
    }
    void restore() {
        if (!m_view) { return; }
        std::memcpy(m_view + 8, &m_family, sizeof(m_family));
        std::memcpy(m_view + m_pass_offset, &m_pass, sizeof(m_pass));
        m_view = nullptr;
    }
    ~NativeSingletonScope() { restore(); }
    NativeSingletonScope() = default;
    NativeSingletonScope(const NativeSingletonScope&) = delete;
    NativeSingletonScope& operator=(const NativeSingletonScope&) = delete;
private:
    uint8_t* m_view{};
    uintptr_t m_family{};
    size_t m_pass_offset{};
    int32_t m_pass{};
};

inline void release_borrowed_native_interfaces(void* family, const std::array<uintptr_t, 4>& borrowed, const Layout& abi = layout()) {
    for (size_t i = 0; i < borrowed.size(); ++i) {
        uintptr_t current{};
        auto* slot = static_cast<uint8_t*>(family) + abi.family_borrowed_offset + i * sizeof(uintptr_t);
        std::memcpy(&current, slot, sizeof(current));
        if (current && current == borrowed[i]) { std::memset(slot, 0, sizeof(current)); }
    }
}

struct WindowSize25 { double x{}, y{}; };
static_assert(sizeof(WindowSize25) == 16);
}
