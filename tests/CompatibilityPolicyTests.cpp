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

    expect(uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, true, true),
        "Stalker2 UE5.5 DX12 Native Fix must use its validated capture layout");
    expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, true, false),
        "plain Native Stalker2 must not enter the Native Fix capture layout");
    expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, false, true),
        "Stalker2 DX11 must not inherit the validated DX12 capture layout");
    expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.1.1", 0x00050001, true, true),
        "legacy Stalker2 must not inherit the UE5.5 capture layout");
    expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.5.4", 0x00050005, true, true),
        "other UE5.5 Native Fix games must retain generic capture discovery");

    expect(uevr::games::should_use_stalker2_ue55_synced_scene_target(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, true, true, true),
        "Stalker2 UE5.5 Synced DX12 must use its completed-Draw scene target");
    expect(!uevr::games::should_use_stalker2_ue55_synced_scene_target(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, true, false, true),
        "Stalker2 Native must not enter the Synced scene-target path");
    expect(!uevr::games::should_use_stalker2_ue55_synced_scene_target(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, false, true, true),
        "Stalker2 DX11 must not inherit the DX12 scene-target path");
    expect(!uevr::games::should_use_stalker2_ue55_synced_scene_target(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0, true, true, false),
        "Stalker2 scene-target publication must wait for completed Draw");
    expect(!uevr::games::should_use_stalker2_ue55_synced_scene_target(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.5.4", 0x00050005, true, true, true),
        "other UE5.5 Synced games must retain generic scene-target handling");

    expect(uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\StormEscape-Win64-Shipping.exe", L"5.6.1", 0, 0, true, true),
        "StormEscape UE5.6.1 DX12 Native Fix must use its validated stock capture layout");
    expect(uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\StormEscape-Win64-Shipping.exe", L"unknown", 0x00050006, 0x00010000, true, true),
        "StormEscape must retain the exact file-version fallback");
    expect(!uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\StormEscape-Win64-Shipping.exe", L"5.6.0", 0x00050006, 0, true, true),
        "other StormEscape UE5.6 patch layouts must fail closed");
    expect(!uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\StormEscape-Win64-Shipping.exe", L"5.6.1", 0, 0, false, true),
        "StormEscape DX11 must not inherit the validated DX12 capture layout");
    expect(!uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\StormEscape-Win64-Shipping.exe", L"5.6.1", 0, 0, true, false),
        "plain Native StormEscape must not enter the Native Fix capture layout");
    expect(!uevr::games::should_use_storm_escape_ue561_native_fix_capture_layout(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.6.1", 0x00050006, 0x00010000, true, true),
        "other UE5.6.1 games must retain their existing capture discovery");

    expect(uevr::games::stalker2_native_fix_requires_same_pass(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.1.1", 0),
        "legacy Stalker2 Native Fix must retain the stable same-pass handoff");
    expect(uevr::games::stalker2_native_fix_requires_same_pass(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.5.4", 0),
        "updated Stalker2 Native Fix must use the stable same-pass handoff");
    expect(!uevr::games::stalker2_native_fix_requires_same_pass(
               L"C:\\Games\\Stalker2-Win64-Shipping.exe", L"5.6.0", 0x00050006),
        "unvalidated future Stalker2 layouts must fail closed");
    expect(!uevr::games::stalker2_native_fix_requires_same_pass(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.5.4", 0x00050005),
        "other UE5.5 games must not inherit the Stalker2 Native Fix handoff");

    expect(uevr::games::is_sw_zero_company_ue56_runtime(
               L"C:\\Games\\SWZeroCompany.exe", L"5.6.1", 0),
        "SWZeroCompany UE5.6 must enter only its validated scene-target compatibility path");
    expect(uevr::games::is_sw_zero_company_ue56_runtime(
               L"C:/Games/SWZeroCompany.exe", L"unknown", 0x00050006),
        "SWZeroCompany must retain the exact UE5.6 file-version fallback");
    expect(!uevr::games::is_sw_zero_company_ue56_runtime(
               L"C:\\Games\\SWZeroCompany.exe", L"5.7.0", 0x00050006),
        "SWZeroCompany on another engine minor must fail the UE5.6 path closed");
    expect(!uevr::games::is_sw_zero_company_ue56_runtime(
               L"C:\\Games\\SWZeroCompany.exe.backup", L"5.6.1", 0x00050006),
        "partial SWZeroCompany executable names must not enter the compatibility path");
    expect(!uevr::games::is_sw_zero_company_ue56_runtime(
               L"C:\\Games\\Other.exe", L"5.6.1", 0x00050006),
        "other UE5.6 games must not inherit the SWZeroCompany texture ABI");

    expect(uevr::games::should_use_bodycam_ue554_dx12_texture_layout(
               L"C:\\Games\\Bodycam-Win64-Shipping.exe", L"5.5.4", 0, 0, true),
        "Bodycam UE5.5.4 DX12 must enter its validated scene-viewport texture layout");
    expect(uevr::games::should_use_bodycam_ue554_dx12_texture_layout(
               L"C:/Games/Bodycam-Win64-Shipping.exe", L"unknown", 0x00050005, 0x00040000, true),
        "Bodycam must retain the exact 5.5.4 file-version fallback");
    expect(!uevr::games::should_use_bodycam_ue554_dx12_texture_layout(
               L"C:\\Games\\Bodycam-Win64-Shipping.exe", L"5.5.3", 0x00050005, 0x00040000, true),
        "another Bodycam patch must fail the validated 5.5.4 layout closed");
    expect(!uevr::games::should_use_bodycam_ue554_dx12_texture_layout(
               L"C:\\Games\\Bodycam-Win64-Shipping.exe", L"5.5.4", 0, 0, false),
        "Bodycam DX11 must retain the existing texture path");
    expect(!uevr::games::should_use_bodycam_ue554_dx12_texture_layout(
               L"C:\\Games\\Other-Win64-Shipping.exe", L"5.5.4", 0x00050005, 0x00040000, true),
        "other UE5.5.4 games must not inherit the Bodycam viewport ABI");
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

void test_bodycam_native_fix_pre_exposure_pairing() {
    using uevr::vr_compatibility::BodycamPreExposureSample;
    using uevr::vr_compatibility::is_bodycam_primary_pre_exposure_sample;
    using uevr::vr_compatibility::should_reuse_bodycam_primary_pre_exposure;

    constexpr BodycamPreExposureSample primary{
        .stereo_pass = 1,
        .state = 0x1000,
        .adaptation_state = 0x1000,
        .render_state = 0x1000,
        .state_vtable = 0x5000,
        .observation = 20,
    };
    BodycamPreExposureSample secondary{
        .stereo_pass = 2,
        .state = 0x2000,
        .adaptation_state = 0x1000,
        .render_state = 0x2000,
        .state_vtable = 0x5000,
        .observation = 21,
    };

    expect(is_bodycam_primary_pre_exposure_sample(primary),
        "Bodycam primary exposure samples must use one self-owned state");
    expect(should_reuse_bodycam_primary_pre_exposure(primary, secondary),
        "Bodycam's adjacent secondary view may reuse its validated primary exposure");

    secondary.observation = 22;
    expect(!should_reuse_bodycam_primary_pre_exposure(primary, secondary),
        "non-adjacent Bodycam views must not share a latched exposure");

    secondary.observation = 21;
    secondary.adaptation_state = secondary.state;
    expect(!should_reuse_bodycam_primary_pre_exposure(primary, secondary),
        "a self-owned secondary adaptation state must preserve the engine result");

    secondary.adaptation_state = primary.state;
    secondary.state_vtable = 0x6000;
    expect(!should_reuse_bodycam_primary_pre_exposure(primary, secondary),
        "mismatched Bodycam view-state types must fail exposure pairing closed");
}

void test_ue58_slate_ui_capability() {
    using namespace uevr::vr_compatibility;

    expect(is_validated_ue58_slate_source_version(0x00050008, 0x00000000),
        "UE5.8.0 must be eligible for validated Slate capability routing");
    expect(is_validated_ue58_slate_source_version(0x00050008, 0x00010000),
        "UE5.8.1 must be eligible for validated Slate capability routing");
    expect(is_validated_ue58_slate_source_version(0x00050008, 0x00020000),
        "UE5.8.2 must be eligible for validated Slate capability routing");
    expect(!is_validated_ue58_slate_source_version(0x00050008, 0x00030000),
        "an unvalidated future UE5.8 patch must fail automatic Slate routing closed");
    expect(!is_validated_ue58_slate_source_version(0x00050007, 0x00020000),
        "UE5.7 must remain outside the UE5.8 Slate capability route");

    UE58SlateCallABIObservation direct_call{
        .rcx_builder = true,
        .rdx_raw_texture = true,
        .r8_anchor_name = true,
        .r9_zero_flags = true,
    };
    expect(is_ue58_direct_raw_texture_transaction(direct_call),
        "the validated builder/raw/name/zero-flags call shape must classify as direct raw texture");

    direct_call.rcx_hidden_return = true;
    expect(!is_ue58_direct_raw_texture_transaction(direct_call),
        "a hidden-return wrapper must not classify as a direct raw-texture transaction");

    UE58SlateCallABIObservation pooled_wrapper{
        .rcx_hidden_return = true,
        .rdx_raw_texture = true,
        .r8_anchor_name = true,
    };
    expect(is_ue58_pooled_wrapper_input_transaction(pooled_wrapper),
        "the validated hidden-return/raw/name call shape must classify as a pooled-wrapper input");
    pooled_wrapper.r8_anchor_name = false;
    expect(!is_ue58_pooled_wrapper_input_transaction(pooled_wrapper),
        "a pooled-wrapper candidate without the proven Slate name register must fail closed");

    expect(classify_ue58_slate_route_abi(false, 1, 0) == UE58SlateRouteABI::Unknown,
        "UE5.8 Slate ABI classification must wait for a proven DrawWindow function");
    expect(classify_ue58_slate_route_abi(true, 1, 0) == UE58SlateRouteABI::DirectRawTexture,
        "one validated raw transaction must classify as the direct ABI");
    expect(classify_ue58_slate_route_abi(true, 0, 1) == UE58SlateRouteABI::PooledWrapper,
        "one validated pooled transaction must classify as the wrapper ABI");
    expect(classify_ue58_slate_route_abi(true, 1, 1) == UE58SlateRouteABI::Ambiguous,
        "mixed UE5.8 Slate transaction ABIs must fail closed");
    expect(classify_ue58_slate_route_abi(true, 2, 0) == UE58SlateRouteABI::Ambiguous,
        "multiple raw transaction candidates must remain ambiguous");

    UE58SlateRuntimeObservation observation{
        .exact_ue58 = true,
        .scanner_proven = true,
        .route_abi = UE58SlateRouteABI::DirectRawTexture,
    };

    expect(evaluate_ue58_dedicated_ui_capability(observation) == UE58DedicatedUICapability::Observing,
        "a proven scanner must still wait for runtime target evidence");

    observation.runtime_name_validated = true;
    observation.target_desc_valid = true;
    observation.scene_relation_valid = true;
    observation.target_is_distinct_from_scene = true;
    observation.trusted_extent_valid = true;
    observation.target_matches_trusted_extent = true;
    observation.stable_observations = 2;
    expect(evaluate_ue58_dedicated_ui_capability(observation) == UE58DedicatedUICapability::Observing,
        "two matching engine-owned observations must not establish capability");

    observation.stable_observations = 3;
    auto capability = evaluate_ue58_dedicated_ui_capability(observation);
    expect(capability == UE58DedicatedUICapability::EngineOwned,
        "three stable distinct window-sized targets must diagnose an engine-owned UI target");
    expect(should_enable_ue58_automatic_ui_route(capability),
        "a proven engine-owned target must enable the automatic route");
    expect(!should_create_ue58_synthetic_ui_target(capability),
        "an engine-owned target must not allocate a synthetic texture");

    observation.target_is_distinct_from_scene = false;
    observation.target_matches_trusted_extent = false;
    observation.target_is_scene = true;
    observation.scene_extent_differs_from_trusted_extent = true;
    capability = evaluate_ue58_dedicated_ui_capability(observation);
    expect(capability == UE58DedicatedUICapability::SyntheticRequired,
        "three stable packed-scene observations must diagnose a synthetic UI requirement");
    expect(should_enable_ue58_automatic_ui_route(capability),
        "a proven packed-scene target must enable the automatic route");
    expect(should_create_ue58_synthetic_ui_target(capability),
        "a proven packed-scene target must request one synthetic UI texture");

    UE58SyntheticUICreationInputs creation{
        .exact_ue58 = true,
        .synthetic_required = true,
        .game_data_initialized = true,
        .engine_valid = true,
        .slate_hook_valid = true,
        .stable_slate_draw = true,
        .packed_scene_target_valid = true,
    };
    expect(should_attempt_ue58_synthetic_ui_creation(creation),
        "a proven UE5.8 synthetic route may allocate after validating the packed scene target");

    creation.packed_scene_target_valid = false;
    expect(!should_attempt_ue58_synthetic_ui_creation(creation),
        "an invalid packed scene target must fail synthetic UI creation closed");

    creation.packed_scene_target_valid = true;
    creation.synthetic_required = false;
    expect(!should_attempt_ue58_synthetic_ui_creation(creation),
        "engine-owned and unproven routes must not allocate a synthetic UI target");

    creation.synthetic_required = true;
    creation.exact_ue58 = false;
    expect(!should_attempt_ue58_synthetic_ui_creation(creation),
        "other engine versions must retain their existing dedicated UI prerequisites");

    expect(should_use_ue58_slate_ui_resource_worker(true, true, true, false),
        "a proven UE5.8 DX12 synthetic route without PreRender must use the Slate render-thread worker");
    expect(!should_use_ue58_slate_ui_resource_worker(true, false, true, false),
        "UE5.8 DX11 must retain its existing render-resource worker path");
    expect(!should_use_ue58_slate_ui_resource_worker(true, true, true, true),
        "a working PreRender callback must retain the standard render-resource worker path");
    expect(!should_use_ue58_slate_ui_resource_worker(false, true, true, false),
        "other engine versions must not use the UE5.8 Slate render-resource worker");

    observation.target_is_distinct_from_scene = true;
    expect(evaluate_ue58_dedicated_ui_capability(observation) == UE58DedicatedUICapability::Quarantined,
        "contradictory scene ownership evidence must be quarantined");
    expect(!should_enable_ue58_automatic_ui_route(UE58DedicatedUICapability::Quarantined),
        "quarantined evidence must preserve the original Slate path");
    expect(!should_enable_ue58_automatic_ui_route(UE58DedicatedUICapability::Observing),
        "an observing classifier must preserve the original Slate path");

    observation = {
        .exact_ue58 = true,
        .scanner_proven = true,
        .route_abi = UE58SlateRouteABI::Ambiguous,
    };
    expect(evaluate_ue58_dedicated_ui_capability(observation) == UE58DedicatedUICapability::Quarantined,
        "an ambiguous static ABI must not become an automatic route candidate");

    observation.exact_ue58 = false;
    expect(evaluate_ue58_dedicated_ui_capability(observation) == UE58DedicatedUICapability::Unproven,
        "UE5.7 and earlier must remain outside the UE5.8 diagnostic policy");
}

} // namespace

int main() {
    test_scene_view_layouts();
    test_rendering_mode_matrix();
    test_version_gates();
    test_ue58_render_pose_fallback();
    test_bodycam_native_fix_pre_exposure_pairing();
    test_ue58_slate_ui_capability();

    if (failures != 0) {
        std::cerr << failures << " compatibility policy test(s) failed\n";
        return 1;
    }

    std::cout << "All compatibility policy tests passed\n";
    return 0;
}
