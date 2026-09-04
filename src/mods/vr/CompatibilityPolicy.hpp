#pragma once

#include <cstdint>

namespace uevr::vr_compatibility {

enum class RenderingMethod : int32_t {
    NativeStereo = 0,
    Synchronized = 1,
    Alternating = 2,
    SyntheticDibr = 3,
    SyntheticDibrSingleView = 4,
};

// UE5.8 keeps the same high-level Slate source contract across 5.8.0-5.8.2,
// but optimized games expose either the raw-texture helper or its pooled
// RegisterExternalTexture transaction.
enum class UE58SlateRouteABI : uint8_t {
    Unknown,
    DirectRawTexture,
    PooledWrapper,
    Ambiguous,
};

enum class UE58SlateScannerState : uint8_t {
    NotRun,
    Scanning,
    DrawFunctionUnproven,
    CallsiteUnproven,
    HookFailed,
    Proven,
};

enum class UE58DedicatedUICapability : uint8_t {
    Unproven,
    Observing,
    EngineOwned,
    SyntheticRequired,
    Quarantined,
};

struct UE58SlateRuntimeObservation {
    bool exact_ue58{};
    bool scanner_proven{};
    UE58SlateRouteABI route_abi{UE58SlateRouteABI::Unknown};
    bool runtime_name_validated{};
    bool target_desc_valid{};
    bool scene_relation_valid{};
    bool target_is_scene{};
    bool target_is_distinct_from_scene{};
    bool trusted_extent_valid{};
    bool target_matches_trusted_extent{};
    bool scene_extent_differs_from_trusted_extent{};
    uint32_t stable_observations{};
};

struct UE58SlateCallABIObservation {
    bool rcx_builder{};
    bool rcx_hidden_return{};
    bool rdx_raw_texture{};
    bool r8_anchor_name{};
    bool r9_zero_flags{};
};

struct UE58SyntheticUICreationInputs {
    bool exact_ue58{};
    bool synthetic_required{};
    bool game_data_initialized{};
    bool engine_valid{};
    bool slate_hook_valid{};
    bool stable_slate_draw{};
    bool packed_scene_target_valid{};
};

constexpr bool is_validated_ue58_slate_source_version(
    uint32_t file_version_ms,
    uint32_t file_version_ls) noexcept {
    const auto major = static_cast<uint16_t>(file_version_ms >> 16);
    const auto minor = static_cast<uint16_t>(file_version_ms & 0xffffu);
    const auto patch = static_cast<uint16_t>(file_version_ls >> 16);

    return major == 5 && minor == 8 && patch <= 2;
}

constexpr bool should_enable_ue58_automatic_ui_route(
    UE58DedicatedUICapability capability) noexcept {
    return capability == UE58DedicatedUICapability::EngineOwned ||
        capability == UE58DedicatedUICapability::SyntheticRequired;
}

constexpr bool should_create_ue58_synthetic_ui_target(
    UE58DedicatedUICapability capability) noexcept {
    return capability == UE58DedicatedUICapability::SyntheticRequired;
}

constexpr bool should_attempt_ue58_synthetic_ui_creation(
    const UE58SyntheticUICreationInputs& input) noexcept {
    return input.exact_ue58 &&
        input.synthetic_required &&
        input.game_data_initialized &&
        input.engine_valid &&
        input.slate_hook_valid &&
        input.stable_slate_draw &&
        input.packed_scene_target_valid;
}

constexpr bool should_use_ue58_slate_ui_resource_worker(
    bool exact_ue58,
    bool dx12,
    bool synthetic_required,
    bool prerender_viewfamily_seen) noexcept {
    return exact_ue58 && dx12 && synthetic_required && !prerender_viewfamily_seen;
}

constexpr bool is_ue58_direct_raw_texture_transaction(
    const UE58SlateCallABIObservation& input) noexcept {
    return input.rcx_builder &&
        !input.rcx_hidden_return &&
        input.rdx_raw_texture &&
        input.r8_anchor_name &&
        input.r9_zero_flags;
}

constexpr bool is_ue58_pooled_wrapper_input_transaction(
    const UE58SlateCallABIObservation& input) noexcept {
    return input.rcx_hidden_return &&
        input.rdx_raw_texture &&
        input.r8_anchor_name;
}

constexpr UE58SlateRouteABI classify_ue58_slate_route_abi(
    bool draw_function_proven,
    uint32_t direct_raw_transactions,
    uint32_t pooled_wrapper_transactions) noexcept {
    if (!draw_function_proven) {
        return UE58SlateRouteABI::Unknown;
    }

    if (direct_raw_transactions == 1 && pooled_wrapper_transactions == 0) {
        return UE58SlateRouteABI::DirectRawTexture;
    }

    if (direct_raw_transactions == 0 && pooled_wrapper_transactions == 1) {
        return UE58SlateRouteABI::PooledWrapper;
    }

    return UE58SlateRouteABI::Ambiguous;
}

constexpr UE58DedicatedUICapability evaluate_ue58_dedicated_ui_capability(
    const UE58SlateRuntimeObservation& input) noexcept {
    if (!input.exact_ue58 || !input.scanner_proven) {
        return UE58DedicatedUICapability::Unproven;
    }

    if (input.route_abi == UE58SlateRouteABI::Unknown ||
        input.route_abi == UE58SlateRouteABI::Ambiguous)
    {
        return UE58DedicatedUICapability::Quarantined;
    }

    if (!input.runtime_name_validated || !input.target_desc_valid ||
        !input.scene_relation_valid || !input.trusted_extent_valid)
    {
        return UE58DedicatedUICapability::Observing;
    }

    if (input.target_is_scene && input.target_is_distinct_from_scene) {
        return UE58DedicatedUICapability::Quarantined;
    }

    if (input.target_is_scene && input.scene_extent_differs_from_trusted_extent &&
        input.stable_observations >= 3)
    {
        return UE58DedicatedUICapability::SyntheticRequired;
    }

    if (input.target_is_distinct_from_scene && input.target_matches_trusted_extent &&
        input.stable_observations >= 3)
    {
        return UE58DedicatedUICapability::EngineOwned;
    }

    return UE58DedicatedUICapability::Observing;
}

constexpr const char* to_string(UE58SlateRouteABI value) noexcept {
    switch (value) {
    case UE58SlateRouteABI::DirectRawTexture:
        return "direct raw texture";
    case UE58SlateRouteABI::PooledWrapper:
        return "pooled wrapper";
    case UE58SlateRouteABI::Ambiguous:
        return "ambiguous";
    default:
        return "unknown";
    }
}

constexpr const char* to_string(UE58SlateScannerState value) noexcept {
    switch (value) {
    case UE58SlateScannerState::Scanning:
        return "scanning";
    case UE58SlateScannerState::DrawFunctionUnproven:
        return "draw function unproven";
    case UE58SlateScannerState::CallsiteUnproven:
        return "callsite unproven";
    case UE58SlateScannerState::HookFailed:
        return "hook failed";
    case UE58SlateScannerState::Proven:
        return "proven";
    default:
        return "not run";
    }
}

constexpr const char* to_string(UE58DedicatedUICapability value) noexcept {
    switch (value) {
    case UE58DedicatedUICapability::Observing:
        return "observing";
    case UE58DedicatedUICapability::EngineOwned:
        return "engine owned";
    case UE58DedicatedUICapability::SyntheticRequired:
        return "synthetic required";
    case UE58DedicatedUICapability::Quarantined:
        return "quarantined";
    default:
        return "unproven";
    }
}

struct ModeMatrixInputs {
    RenderingMethod rendering_method{RenderingMethod::NativeStereo};
    bool extreme_compatibility{};
    bool native_stereo_fix_requested{};
    bool ghosting_fix_requested{};
    bool sceneview_compatibility{};
    bool splitscreen_compatibility{};
    bool dibr_engine_supported{};
    bool dx12{};
    bool openxr{};
    bool using_2d_screen{};
    bool stereo_emulation{};
};

struct ModeMatrix {
    bool using_afr{};
    bool using_native_stereo{};
    bool native_stereo_fix_active{};
    bool ghosting_remap_active{};
    bool dibr_selected{};
    bool dibr_preview_active{};
    bool dibr_single_view_requested{};
    bool dibr_single_view_eligible{};
};

constexpr bool is_using_afr(RenderingMethod method, bool extreme_compatibility) noexcept {
    return method == RenderingMethod::Alternating ||
        method == RenderingMethod::Synchronized ||
        extreme_compatibility;
}

constexpr bool is_using_native_stereo(RenderingMethod method, bool using_afr) noexcept {
    return method == RenderingMethod::NativeStereo && !using_afr;
}

constexpr bool is_native_stereo_fix_active(
    bool requested,
    RenderingMethod method,
    bool using_afr) noexcept {
    return requested && method == RenderingMethod::NativeStereo && !using_afr;
}

constexpr bool should_enable_ghosting_remap(
    bool requested,
    bool using_afr,
    bool native_stereo_fix_active,
    bool splitscreen_compatibility,
    bool sceneview_compatibility) noexcept {
    return requested &&
        using_afr &&
        !native_stereo_fix_active &&
        !splitscreen_compatibility &&
        !sceneview_compatibility;
}

constexpr bool is_dibr_selected(RenderingMethod method) noexcept {
    return method == RenderingMethod::SyntheticDibr ||
        method == RenderingMethod::SyntheticDibrSingleView;
}

constexpr bool is_dibr_preview_active(
    bool dibr_selected,
    bool dibr_engine_supported,
    bool dx12,
    bool openxr,
    bool native_stereo_fix_active,
    bool extreme_compatibility,
    bool sceneview_compatibility,
    bool splitscreen_compatibility,
    bool using_2d_screen,
    bool stereo_emulation) noexcept {
    return dibr_selected &&
        dibr_engine_supported &&
        dx12 &&
        openxr &&
        !native_stereo_fix_active &&
        !extreme_compatibility &&
        !sceneview_compatibility &&
        !splitscreen_compatibility &&
        !using_2d_screen &&
        !stereo_emulation;
}

constexpr bool is_dibr_single_view_eligible(bool requested, bool preview_active) noexcept {
    return requested && preview_active;
}

constexpr ModeMatrix evaluate_mode_matrix(const ModeMatrixInputs& input) noexcept {
    ModeMatrix result{};
    result.using_afr = is_using_afr(input.rendering_method, input.extreme_compatibility);
    result.using_native_stereo = is_using_native_stereo(input.rendering_method, result.using_afr);
    result.native_stereo_fix_active = is_native_stereo_fix_active(
        input.native_stereo_fix_requested,
        input.rendering_method,
        result.using_afr);
    result.ghosting_remap_active = should_enable_ghosting_remap(
        input.ghosting_fix_requested,
        result.using_afr,
        result.native_stereo_fix_active,
        input.splitscreen_compatibility,
        input.sceneview_compatibility);
    result.dibr_selected = is_dibr_selected(input.rendering_method);
    result.dibr_preview_active = is_dibr_preview_active(
        result.dibr_selected,
        input.dibr_engine_supported,
        input.dx12,
        input.openxr,
        result.native_stereo_fix_active,
        input.extreme_compatibility,
        input.sceneview_compatibility,
        input.splitscreen_compatibility,
        input.using_2d_screen,
        input.stereo_emulation);
    result.dibr_single_view_requested =
        input.rendering_method == RenderingMethod::SyntheticDibrSingleView;
    result.dibr_single_view_eligible = is_dibr_single_view_eligible(
        result.dibr_single_view_requested,
        result.dibr_preview_active);
    return result;
}

constexpr bool should_use_ue58_render_target_manager_abi(bool exact_ue58) noexcept {
    return exact_ue58;
}

constexpr bool should_use_ue56_post_init_slot(
    bool exact_ue56,
    bool dx11,
    bool dx12) noexcept {
    return exact_ue56 && (dx11 || dx12);
}

struct BodycamPreExposureSample {
    uint32_t stereo_pass{};
    uintptr_t state{};
    uintptr_t adaptation_state{};
    uintptr_t render_state{};
    uintptr_t state_vtable{};
    uint64_t observation{};
};

constexpr bool is_bodycam_primary_pre_exposure_sample(
    const BodycamPreExposureSample& sample) noexcept {
    return sample.stereo_pass == 1 &&
        sample.state != 0 &&
        sample.state == sample.adaptation_state &&
        sample.state == sample.render_state &&
        sample.state_vtable != 0;
}

constexpr bool should_reuse_bodycam_primary_pre_exposure(
    const BodycamPreExposureSample& primary,
    const BodycamPreExposureSample& secondary) noexcept {
    return is_bodycam_primary_pre_exposure_sample(primary) &&
        secondary.stereo_pass == 2 &&
        secondary.state != 0 &&
        secondary.state == secondary.render_state &&
        secondary.state != secondary.adaptation_state &&
        secondary.adaptation_state == primary.state &&
        secondary.state_vtable == primary.state_vtable &&
        secondary.observation != 0 &&
        primary.observation == secondary.observation - 1;
}

struct UE58RenderPoseFallbackInputs {
    bool exact_ue58{};
    bool d3d12{};
    bool openxr{};
    bool native_stereo{};
    bool hmd_active{};
    bool runtime_ready{};
    bool draw_hook_resolved{};
};

constexpr bool should_use_ue58_render_pose_fallback(
    const UE58RenderPoseFallbackInputs& input) noexcept {
    return input.exact_ue58 && input.d3d12 && input.openxr &&
        input.native_stereo && input.hmd_active && input.runtime_ready &&
        !input.draw_hook_resolved;
}

} // namespace uevr::vr_compatibility
