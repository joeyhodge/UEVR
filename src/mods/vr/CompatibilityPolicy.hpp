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

} // namespace uevr::vr_compatibility
