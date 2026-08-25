#include <cstddef>
#include <iostream>
#include <string_view>

#include <sdk/FSceneView.hpp>
#include <sdk/FSceneViewLayoutPolicy.hpp>

#include "mods/GameSpecific.hpp"
#include "mods/vr/CompatibilityPolicy.hpp"

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) {
        return;
    }

    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

constexpr size_t offset_delta(size_t member, size_t base) {
    return member - base;
}

void test_scene_view_layouts() {
    using sdk::scene_view_layout::FSceneViewInitLayout;
    using sdk::scene_view_layout::classify_engine_version;
    using sdk::scene_view_layout::classify_engine_version_string;
    using sdk::scene_view_layout::world_to_meters_delta;

    expect(classify_engine_version_string(L"4.11") == FSceneViewInitLayout::UE4_8_UE4_15,
        "UE4.11 string detection must select the oldest validated layout");
    expect(classify_engine_version_string(L"4.19") == FSceneViewInitLayout::UE4_16_UE4_19,
        "UE4.19 string detection must select the intermediate legacy layout");
    expect(classify_engine_version_string(L"5.6.1") == FSceneViewInitLayout::UE56_UE57,
        "UE5.6 patch versions must select the UE5.6/5.7 layout");
    expect(classify_engine_version_string(L"5.8.2") == FSceneViewInitLayout::UE58,
        "UE5.8 patch versions must select the validated UE5.8 layout");
    expect(classify_engine_version_string(L"5.9") == FSceneViewInitLayout::UnsupportedNewer,
        "UE5.9 string detection must fail closed");
    expect(classify_engine_version_string(L"6.0") == FSceneViewInitLayout::UnsupportedNewer,
        "UE6 string detection must fail closed");
    expect(!classify_engine_version_string(L"5.5.4").has_value(),
        "pre-UE5.6 string detection must retain the file-version fallback");

    expect(classify_engine_version(4, 11) == FSceneViewInitLayout::UE4_8_UE4_15,
        "UE4.11 must use the validated pre-player-index layout");
    expect(classify_engine_version(4, 15) == FSceneViewInitLayout::UE4_8_UE4_15,
        "UE4.15 must remain at the upper boundary of the oldest layout");
    expect(classify_engine_version(4, 16) == FSceneViewInitLayout::UE4_16_UE4_19,
        "UE4.16 must begin the intermediate legacy layout");
    expect(classify_engine_version(4, 19) == FSceneViewInitLayout::UE4_16_UE4_19,
        "UE4.19 must use the validated pre-StereoIPD layout");
    expect(classify_engine_version(4, 20) == FSceneViewInitLayout::UE4_20_UE55,
        "UE4.20 must use the modern UE4 layout");
    expect(classify_engine_version(4, 24) == FSceneViewInitLayout::UE4_20_UE55,
        "UE4.24 must retain the modern UE4 layout");
    expect(classify_engine_version(4, 27) == FSceneViewInitLayout::UE4_20_UE55,
        "UE4.27 must retain the modern UE4 layout");
    expect(classify_engine_version(5, 0) == FSceneViewInitLayout::UE4_20_UE55,
        "UE5.0 must use the pre-FSceneViewOwner layout");
    expect(classify_engine_version(5, 4) == FSceneViewInitLayout::UE4_20_UE55,
        "UE5.4 must use the CameraToViewTarget layout without FSceneViewOwner");
    expect(classify_engine_version(5, 5) == FSceneViewInitLayout::UE4_20_UE55,
        "UE5.5 must retain the pre-FSceneViewOwner layout");
    expect(classify_engine_version(5, 6) == FSceneViewInitLayout::UE56_UE57,
        "UE5.6 must use the FSceneViewOwner layout");
    expect(classify_engine_version(5, 7) == FSceneViewInitLayout::UE56_UE57,
        "UE5.7 must use the FSceneViewOwner layout");
    expect(classify_engine_version(5, 8) == FSceneViewInitLayout::UE58,
        "UE5.8 must use the skylight-scale layout");
    expect(classify_engine_version(5, 9) == FSceneViewInitLayout::UnsupportedNewer,
        "unknown future UE5 layouts must fail closed");
    expect(classify_engine_version(6, 0) == FSceneViewInitLayout::UnsupportedNewer,
        "UE6 layouts must fail closed until validated");

    expect(world_to_meters_delta(FSceneViewInitLayout::UE4_8_UE4_15) == 4,
        "UE4.8-4.15 WorldToMeters must immediately follow StereoPass");
    expect(world_to_meters_delta(FSceneViewInitLayout::UE4_16_UE4_19) == 4,
        "UE4.16-4.19 WorldToMeters must immediately follow StereoPass");
    expect(world_to_meters_delta(FSceneViewInitLayout::UE4_20_UE55) == 8,
        "UE4.20+ WorldToMeters must account for StereoIPD");

    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE4, player_index),
        offsetof(sdk::FSceneViewInitOptionsUE4, family)) == 0x18,
        "UE4/UE5.0-5.5 PlayerIndex delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE4, stereo_pass),
        offsetof(sdk::FSceneViewInitOptionsUE4, family)) == 0x58,
        "UE4/UE5.0-5.5 StereoPass delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE50To53, player_index),
        offsetof(sdk::FSceneViewInitOptionsUE50To53, family)) == 0x18,
        "UE5.0-5.3 PlayerIndex delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE50To53, stereo_pass),
        offsetof(sdk::FSceneViewInitOptionsUE50To53, family)) == 0x58,
        "UE5.0-5.3 StereoPass delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE5, player_index),
        offsetof(sdk::FSceneViewInitOptionsUE5, family)) == 0x18,
        "UE5.4/5.5 PlayerIndex delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE5, stereo_pass),
        offsetof(sdk::FSceneViewInitOptionsUE5, family)) == 0x58,
        "UE5.4/5.5 StereoPass delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE56, player_index),
        offsetof(sdk::FSceneViewInitOptionsUE56, family)) == 0x28,
        "UE5.6/5.7 PlayerIndex delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE56, stereo_pass),
        offsetof(sdk::FSceneViewInitOptionsUE56, family)) == 0x68,
        "UE5.6/5.7 StereoPass delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE58, player_index),
        offsetof(sdk::FSceneViewInitOptionsUE58, family)) == 0x28,
        "UE5.8 PlayerIndex delta changed");
    expect(offset_delta(
        offsetof(sdk::FSceneViewInitOptionsUE58, stereo_pass),
        offsetof(sdk::FSceneViewInitOptionsUE58, family)) == 0x78,
        "UE5.8 StereoPass delta changed");
}

void test_rendering_mode_matrix() {
    using namespace uevr::vr_compatibility;

    constexpr RenderingMethod methods[]{
        RenderingMethod::NativeStereo,
        RenderingMethod::Synchronized,
        RenderingMethod::Alternating,
        RenderingMethod::SyntheticDibr,
        RenderingMethod::SyntheticDibrSingleView,
    };

    for (const auto method : methods) {
        for (const bool extreme_compatibility : {false, true}) {
            const bool expected_afr =
                method == RenderingMethod::Synchronized ||
                method == RenderingMethod::Alternating ||
                extreme_compatibility;
            expect(is_using_afr(method, extreme_compatibility) == expected_afr,
                "AFR policy diverged from the runtime rendering-method matrix");
            expect(is_using_native_stereo(method, expected_afr) ==
                    (method == RenderingMethod::NativeStereo && !expected_afr),
                "Native Stereo policy diverged from the runtime rendering-method matrix");
            expect(is_native_stereo_fix_active(true, method, expected_afr) ==
                    (method == RenderingMethod::NativeStereo && !expected_afr),
                "Native Fix policy diverged from the runtime rendering-method matrix");
            expect(is_dibr_selected(method) ==
                    (method == RenderingMethod::SyntheticDibr ||
                        method == RenderingMethod::SyntheticDibrSingleView),
                "DIBR selection policy diverged from the runtime rendering-method matrix");
        }
    }

    auto input = ModeMatrixInputs{};
    auto mode = evaluate_mode_matrix(input);
    expect(mode.using_native_stereo && !mode.using_afr && !mode.native_stereo_fix_active,
        "plain Native Stereo must remain native without enabling Native Fix");

    input.native_stereo_fix_requested = true;
    mode = evaluate_mode_matrix(input);
    expect(mode.native_stereo_fix_active && !mode.ghosting_remap_active,
        "Native Fix must activate only in Native Stereo and exclude Ghost remapping");

    input.extreme_compatibility = true;
    mode = evaluate_mode_matrix(input);
    expect(mode.using_afr && !mode.using_native_stereo && !mode.native_stereo_fix_active,
        "Extreme Compatibility must keep Native rendering on its synchronized AFR path");

    input = {};
    input.rendering_method = RenderingMethod::Synchronized;
    input.native_stereo_fix_requested = true;
    input.ghosting_fix_requested = true;
    mode = evaluate_mode_matrix(input);
    expect(mode.using_afr && !mode.using_native_stereo && !mode.native_stereo_fix_active,
        "Synchronized rendering must not inherit Native Fix");
    expect(mode.ghosting_remap_active,
        "Ghost remapping must remain available in synchronized AFR");

    input.rendering_method = RenderingMethod::Alternating;
    mode = evaluate_mode_matrix(input);
    expect(mode.using_afr && mode.ghosting_remap_active,
        "Alternating AFR must retain Ghost remapping eligibility");

    input.splitscreen_compatibility = true;
    mode = evaluate_mode_matrix(input);
    expect(!mode.ghosting_remap_active,
        "split-screen compatibility must exclude Ghost remapping");

    input = {};
    input.rendering_method = RenderingMethod::SyntheticDibrSingleView;
    input.dibr_engine_supported = true;
    input.dx12 = true;
    input.openxr = true;
    mode = evaluate_mode_matrix(input);
    expect(mode.dibr_selected && mode.dibr_preview_active && mode.dibr_single_view_eligible,
        "DIBR single-view must activate only after all preview prerequisites pass");

    input.native_stereo_fix_requested = true;
    mode = evaluate_mode_matrix(input);
    expect(!mode.native_stereo_fix_active && mode.dibr_single_view_eligible,
        "a stale Native Fix request must not affect a DIBR rendering method");

    input.native_stereo_fix_requested = false;
    input.sceneview_compatibility = true;
    mode = evaluate_mode_matrix(input);
    expect(mode.dibr_selected && !mode.dibr_preview_active && !mode.dibr_single_view_eligible,
        "SceneView compatibility must fail DIBR single-view closed");

    input.sceneview_compatibility = false;
    input.dx12 = false;
    mode = evaluate_mode_matrix(input);
    expect(!mode.dibr_preview_active && !mode.dibr_single_view_eligible,
        "DIBR must remain unavailable without D3D12");

    input.dx12 = true;
    input.using_2d_screen = true;
    mode = evaluate_mode_matrix(input);
    expect(!mode.dibr_preview_active && !mode.dibr_single_view_eligible,
        "2D-screen presentation must exclude DIBR");

    input.using_2d_screen = false;
    input.stereo_emulation = true;
    mode = evaluate_mode_matrix(input);
    expect(!mode.dibr_preview_active && !mode.dibr_single_view_eligible,
        "stereo emulation must exclude DIBR");
}

void test_version_gates() {
    using namespace uevr::vr_compatibility;

    expect(should_use_ue58_render_target_manager_abi(true),
        "exact UE5.8 must use the validated RTM ABI");
    expect(!should_use_ue58_render_target_manager_abi(false),
        "unknown future engines must not inherit the UE5.8 RTM ABI");

    expect(should_use_ue56_post_init_slot(true, true, false),
        "UE5.6 DX11 must use validated PostInit slot 10");
    expect(should_use_ue56_post_init_slot(true, false, true),
        "UE5.6 DX12 must retain validated PostInit slot 10");
    expect(!should_use_ue56_post_init_slot(true, false, false),
        "UE5.6 non-DX backends must fail closed");
    expect(!should_use_ue56_post_init_slot(false, true, false),
        "other engine versions must not enter the UE5.6-specific gate");

    expect(uevr::games::is_stalker2_legacy_ue51_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.1.1", 0),
        "legacy Stalker2 UE5.1 must retain its frame-loop guards");
    expect(!uevr::games::is_stalker2_legacy_ue51_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0x00050005),
        "updated Stalker2 UE5.5 must not inherit UE5.1 frame-loop guards");
    expect(!uevr::games::is_stalker2_legacy_ue51_runtime(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.1.1", 0x00050001),
        "other UE5.1 games must not inherit Stalker2 frame-loop guards");
    expect(uevr::games::is_stalker2_legacy_ue51_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"unknown", 0x00050001),
        "legacy Stalker2 must retain the file-version fallback");

    expect(uevr::games::is_stalker2_ue55_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0),
        "updated Stalker2 UE5.5 must use its validated Slate DrawWindows array and dedicated UI path");
    expect(!uevr::games::is_stalker2_ue55_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.1.1", 0x00050001),
        "legacy Stalker2 UE5.1 must not inherit the UE5.5 Slate ABI");
    expect(!uevr::games::is_stalker2_ue55_runtime(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.5.4", 0x00050005),
        "other UE5.5 games must not inherit the Stalker2 Slate ABI");
    expect(uevr::games::is_stalker2_ue55_runtime(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"unknown", 0x00050005),
        "updated Stalker2 must retain the UE5.5 file-version fallback");
}

void test_ue58_render_pose_fallback() {
    using namespace uevr::vr_compatibility;

    UE58RenderPoseFallbackInputs input{
        .exact_ue58 = true,
        .d3d12 = true,
        .openxr = true,
        .native_stereo = true,
        .hmd_active = true,
        .runtime_ready = true,
        .draw_hook_resolved = false,
    };

    expect(should_use_ue58_render_pose_fallback(input),
        "UE5.8 D3D12 Native may publish a first-eye pose when Draw is unavailable");

    input.exact_ue58 = false;
    expect(!should_use_ue58_render_pose_fallback(input),
        "other engine versions must retain their existing pose path");

    input.exact_ue58 = true;
    input.native_stereo = false;
    expect(!should_use_ue58_render_pose_fallback(input),
        "synchronized and alternating rendering must retain their existing pose path");

    input.native_stereo = true;
    input.d3d12 = false;
    expect(!should_use_ue58_render_pose_fallback(input),
        "UE5.8 DX11 must retain its existing BeginRenderViewFamily fallback");

    input.d3d12 = true;
    input.draw_hook_resolved = true;
    expect(!should_use_ue58_render_pose_fallback(input),
        "a resolved Draw hook must retain ownership of pre-view pose publication");

    input.draw_hook_resolved = false;
    input.runtime_ready = false;
    expect(!should_use_ue58_render_pose_fallback(input),
        "an unready OpenXR runtime must fail the pre-view pose fallback closed");
}

} // namespace

int main() {
    test_scene_view_layouts();
    test_rendering_mode_matrix();
    test_version_gates();
    test_ue58_render_pose_fallback();

    if (failures != 0) {
        std::cerr << failures << " compatibility policy test(s) failed\n";
        return 1;
    }

    std::cout << "All compatibility policy tests passed\n";
    return 0;
}
