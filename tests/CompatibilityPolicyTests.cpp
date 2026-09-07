#include <cstddef>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include <sdk/FSceneView.hpp>
#include <sdk/FSceneViewLayoutPolicy.hpp>

#include "mods/GameSpecific.hpp"
#include "mods/vr/BodycamTextureLayout.hpp"
#include "mods/vr/UE58OwnedUITexture.hpp"
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

void test_bodycam_owned_texture_layout() {
    namespace layout = uevr::bodycam_texture;
    constexpr uintptr_t owner = 0x10000;
    constexpr uintptr_t rhi = 0x20000;
    constexpr layout::ResourceIdentity valid{owner, rhi, rhi, 1920, 1080};

    expect(layout::private_resource_offset == 0x110 &&
               layout::texture_rhi_offset == 0x10 &&
               layout::render_target_offset == 0x50 &&
               layout::render_target_texture_offset == layout::render_target_offset + sizeof(void*),
        "Bodycam must use the binary-validated resource and render-target subobject layout");
    expect(layout::is_initialized_resource(valid, owner, 1920, 1080),
        "a complete owned UI target must be accepted independently of generic cached offsets");
    expect(!layout::is_initialized_resource(valid, owner + 8, 1920, 1080),
        "an unrelated UObject pointing to a valid texture must not be adopted");
    expect(!layout::is_initialized_resource(valid, owner, 2472, 2416),
        "the UI resource must not be published as a Native Fix eye target");
    expect(!layout::is_initialized_resource({}, 0, 0, 0),
        "empty ownership and pending resource state must fail closed");

    auto pending = valid;
    pending.texture_rhi = 0;
    expect(!layout::is_initialized_resource(pending, owner, 1920, 1080),
        "an uninitialized TextureRHI must remain retryable");
    pending = valid;
    pending.render_target_texture = 0;
    expect(!layout::is_initialized_resource(pending, owner, 1920, 1080),
        "an uninitialized FRenderTarget must not publish a partial chain");
    pending.render_target_texture = rhi + 8;
    expect(!layout::is_initialized_resource(pending, owner, 1920, 1080),
        "separated or mid-resize texture references must not be confused with the owned target");
    pending.render_target_texture = rhi;
    expect(layout::is_initialized_resource(pending, owner, 1920, 1080),
        "a previously pending resource must succeed after InitRHI without a latched failure");
    pending.width = 2472;
    pending.height = 2416;
    expect(layout::is_initialized_resource(pending, owner, 2472, 2416),
        "the same validated layout must accept the separate Native Fix eye extent");
    pending.width = 65537;
    expect(!layout::is_initialized_resource(pending, owner, 65537, 2416),
        "implausible render target dimensions must fail closed");

    expect(layout::is_render_target_accessor(layout::render_target_accessor),
        "the exact lea rax,[rcx+8]; ret accessor must validate without executing it");
    auto wrong_accessor = layout::render_target_accessor;
    wrong_accessor[1] = 0x8b;
    expect(!layout::is_render_target_accessor(wrong_accessor),
        "a pointer-loading accessor is not the reference-returning FRenderTarget accessor");
    wrong_accessor = layout::render_target_accessor;
    wrong_accessor[3] = 0x10;
    expect(!layout::is_render_target_accessor(wrong_accessor),
        "another member offset must not match the Bodycam FRenderTarget layout");
    expect(!layout::is_render_target_accessor(std::span<const uint8_t>{wrong_accessor}.first(4)),
        "a truncated function must never pass accessor validation");
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

void test_ue58_pooled_slate_fallback() {
    using namespace uevr::vr_compatibility;
    using enum UE58SlateArgumentSetup;

    constexpr std::array<UE58SlateArgumentSetup, 3> wrapper{NameR8, HiddenReturnRcx, RawTextureRdx};
    constexpr std::array<UE58SlateArgumentSetup, 3> registration{WrapperReturnRdx, ZeroFlagsR8, BuilderRcx};
    constexpr std::array<std::array<size_t, 3>, 6> orders{{
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    }};
    for (const auto& order : orders) {
        expect(is_ue58_strict_pooled_wrapper_setup({wrapper[order[0]], wrapper[order[1]], wrapper[order[2]]}),
            "independently scheduled wrapper argument writes must validate in every order");
        expect(is_ue58_strict_pooled_register_setup({registration[order[0]], registration[order[1]], registration[order[2]]}),
            "independently scheduled pooled registration writes must validate in every order");
    }
    for (size_t i = 0; i < 3; ++i) {
        auto invalid_wrapper = wrapper;
        invalid_wrapper[i] = Other;
        expect(!is_ue58_strict_pooled_wrapper_setup(invalid_wrapper),
            "missing or clobbered wrapper arguments must fail closed");
        auto invalid_registration = registration;
        invalid_registration[i] = Other;
        expect(!is_ue58_strict_pooled_register_setup(invalid_registration),
            "missing or clobbered pooled registration arguments must fail closed");
    }
    expect(!is_ue58_strict_pooled_wrapper_setup({NameR8, NameR8, RawTextureRdx}),
        "duplicate argument evidence must not substitute for a hidden return pointer");
    expect(!is_ue58_strict_pooled_register_setup(wrapper),
        "the raw wrapper and pooled registration ABIs must not be interchanged");

    UE58SlatePooledFallbackInputs input{
        .exact_ue58 = true,
        .dx12 = true,
        .cross_anchor_candidates = 4,
        .pooled_wrapper_transactions = 1,
        .shared_strict_pooled_transactions = 1,
    };
    for (uint32_t patch = 0; patch <= 3; ++patch) {
        input.exact_ue58 = is_validated_ue58_slate_source_version(0x00050008, patch << 16);
        expect(should_use_ue58_pooled_slate_fallback(input) == (patch <= 2),
            "pooled fallback must be restricted to validated UE5.8.0-5.8.2 source versions");
    }
    input.exact_ue58 = false;
    expect(!should_use_ue58_pooled_slate_fallback(input),
        "other engine versions must preserve their existing Slate route");
    input.exact_ue58 = true;
    input.dx12 = false;
    expect(!should_use_ue58_pooled_slate_fallback(input),
        "the new pooled rescue and owned resource layout must leave existing DX11 routes untouched");
    input.dx12 = true;
    for (uint32_t candidates = 0; candidates <= 3; ++candidates) {
        input.cross_anchor_candidates = candidates;
        expect(!should_use_ue58_pooled_slate_fallback(input),
            "accepted legacy call sets and empty scans must remain untouched");
    }
    for (uint32_t candidates = 4; candidates <= 8; ++candidates) {
        input.cross_anchor_candidates = candidates;
        expect(should_use_ue58_pooled_slate_fallback(input),
            "unrelated shared helpers must not hide one fully validated pooled transaction");
    }
    input.direct_raw_transactions = 1;
    expect(!should_use_ue58_pooled_slate_fallback(input),
        "mixed raw and pooled evidence must not activate the fallback");
    input.direct_raw_transactions = 0;
    for (const auto count : {0u, 2u}) {
        input.pooled_wrapper_transactions = count;
        expect(!should_use_ue58_pooled_slate_fallback(input),
            "missing or duplicate pooled transactions must fail closed");
        input.pooled_wrapper_transactions = 1;
        input.shared_strict_pooled_transactions = count;
        expect(!should_use_ue58_pooled_slate_fallback(input),
            "the same unique strict wrapper/register pair must exist at both anchors");
        input.shared_strict_pooled_transactions = 1;
    }
}

void test_ue58_owned_ui_resource() {
    namespace layout = uevr::ue58_owned_ui;
    constexpr uintptr_t base = 0x1000;
    constexpr uintptr_t owner = 0x1100;
    constexpr uintptr_t resource = 0x2000;
    constexpr uintptr_t wrong_reference = 0x2400;
    constexpr uintptr_t rhi = 0x3000;
    constexpr size_t owner_size = 0x178;
    std::vector<uint8_t> memory(0x4000);
    auto put = [&](uintptr_t address, const auto& value) {
        std::memcpy(memory.data() + address - base, &value, sizeof(value));
    };
    bool escaped_owner_bounds = false;
    auto read = [&](uintptr_t address, auto& out) {
        if (address >= owner && address < owner + 0x300 && address + sizeof(out) > owner + owner_size) {
            escaped_owner_bounds = true;
            return false;
        }
        if (address < base || address - base > memory.size() || sizeof(out) > memory.size() - (address - base)) {
            return false;
        }
        std::memcpy(&out, memory.data() + address - base, sizeof(out));
        return true;
    };
    bool accessor_valid = true;
    auto validate = [&](uintptr_t candidate, uintptr_t texture) {
        return accessor_valid && (candidate == resource || candidate == wrong_reference) && texture == rhi;
    };
    auto find = [&] { return layout::find_resource(owner, owner_size, 1920, 1080, read, validate); };
    auto initialize_resource = [&](uintptr_t candidate) {
        put(candidate + layout::owner_offset, owner);
        put(candidate + layout::texture_rhi_offset, rhi);
        put(candidate + layout::render_target_texture_offset, rhi);
        put(candidate + layout::width_offset, uint32_t{1920});
        put(candidate + layout::height_offset, uint32_t{1080});
    };

    expect(!find(), "uninitialized resources must not publish offsets");
    put(owner + 0x128, resource);
    initialize_resource(resource);
    expect(!find(), "game-thread resource alone must wait for the render-thread mirror");
    put(owner + 0x130, resource);
    auto valid = find();
    expect(valid == layout::Resource{0x128, resource, rhi}, "fully initialized GT/RT resource pair must resolve");

    // Reproduce the false-positive shape: TextureReference has a coincidental
    // RHI pointer at +0xc8 and a cleanup table at +0xe0, not an owned render resource.
    put(owner + 0x138, wrong_reference);
    put(wrong_reference + 0xc8, rhi);
    put(wrong_reference + 0xe0, uintptr_t{0x4000});
    expect(find() == valid, "TextureReference and out-of-object cleanup tables must be ignored");
    put(owner + 0x128, uintptr_t{});
    put(owner + 0x130, uintptr_t{});
    expect(!find(), "a matching RHI inside TextureReference is not a render resource");
    put(owner + 0x128, resource);
    put(owner + 0x130, resource);

    put(resource + layout::owner_offset, owner + 8);
    expect(!find(), "resource must point back to the exact rooted UI UObject");
    put(resource + layout::owner_offset, owner);
    put(resource + layout::render_target_texture_offset, rhi + 8);
    expect(!find(), "incomplete, multisample or separated RHI references must not be adopted");
    put(resource + layout::render_target_texture_offset, rhi);
    put(resource + layout::width_offset, uint32_t{4944});
    expect(!find(), "scene-sized resources must not be adopted as UI");
    put(resource + layout::width_offset, uint32_t{1920});
    accessor_valid = false;
    expect(!find(), "ownership alone must not bypass instruction validation");
    accessor_valid = true;

    initialize_resource(wrong_reference);
    put(owner + 0x140, wrong_reference);
    expect(!find(), "multiple structurally valid owner pairs are ambiguous and must fail closed");
    put(owner + 0x140, uintptr_t{});
    expect(find() == valid, "a failed observation must not poison later discovery");
    put(owner + 0x128, uintptr_t{});
    put(owner + 0x130, uintptr_t{});
    put(owner + 0x118, resource);
    put(owner + 0x120, resource);
    expect(find() == layout::Resource{0x118, resource, rhi},
        "owner discovery must validate the pair, not hardcode FarFarWest's +0x128 field");
    put(owner + 0x118, uintptr_t{});
    put(owner + 0x120, uintptr_t{});
    put(owner + 0x128, resource);
    put(owner + 0x130, resource);
    expect(!escaped_owner_bounds, "discovery must stay inside the reflected UObject size");
    expect(!layout::find_resource(owner, 0x301, 1920, 1080, read, validate), "unexpected object layouts must fail closed");
    expect(!layout::find_resource(UINTPTR_MAX - 8, owner_size, 1920, 1080, read, validate), "owner bounds overflow must fail closed");

    expect(layout::matches_accessor(layout::render_target_accessor, layout::render_target_accessor), "raw accessor must match");
    auto cleanup = layout::render_target_accessor;
    cleanup[0] = 0xe8;
    expect(!layout::matches_accessor(cleanup, layout::render_target_accessor), "cleanup calls must never be treated as accessors");
    auto native = layout::native_resource_accessor;
    expect(layout::matches_accessor(native, layout::native_resource_accessor), "validated direct native chain must match");
    for (size_t i = 0; i < native.size(); ++i) {
        native[i] ^= 1;
        expect(!layout::matches_accessor(native, layout::native_resource_accessor), "changed native accessor code must fail closed");
        native[i] ^= 1;
    }

    layout::StableResource stability{};
    layout::Observation observation{1, *valid, 0x5000};
    expect(!stability.observe(observation), "first complete chain must wait for a stable observation");
    expect(stability.observe(observation), "identical complete chain may publish");
    observation.generation = 2;
    expect(!stability.observe(observation), "a new generation must revalidate stability");
    observation.native += 8;
    expect(!stability.observe(observation), "a changed native resource must reset stability");
    expect(!stability.observe(std::nullopt), "a failed validation must clear prior evidence");
    expect(!stability.observe(observation), "recovery must not reuse evidence from before validation failed");
    expect(stability.observe(observation), "a later stable resource must remain retryable");
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

void test_farfarwest_view_extension_discovery() {
    using uevr::games::should_use_farfarwest_ue581_view_extension_layout;
    using uevr::vr_compatibility::is_valid_farfarwest_view_extension_mapping;

    constexpr auto exe = L"D:\\Games\\FarFarWest-Win64-Shipping.exe";
    expect(should_use_farfarwest_ue581_view_extension_layout(exe, 0x50008, 0x10000, true),
        "FarFarWest UE5.8.1 DX12 uses validated callback discovery in every rendering mode");
    expect(should_use_farfarwest_ue581_view_extension_layout(
        L"d:/games/FARFARWEST-WIN64-SHIPPING.EXE", 0x50008, 0x10000, true),
        "FarFarWest gate is case-insensitive and independent of install directory");
    for (const auto path : {L"Other.exe", L"FarFarWest-Win64-Shipping.exe.bak",
            L"D:\\FarFarWest-Win64-Shipping.exe\\Other.exe"}) {
        expect(!should_use_farfarwest_ue581_view_extension_layout(path, 0x50008, 0x10000, true),
            "other executables must not inherit FarFarWest discovery");
    }
    expect(!should_use_farfarwest_ue581_view_extension_layout(exe, 0x50008, 0x10000, false),
        "FarFarWest DX11 remains unchanged");
    for (const auto version : {0x50005u, 0x50006u, 0x50007u, 0x50009u}) {
        expect(!should_use_farfarwest_ue581_view_extension_layout(exe, version, 0x10000, true),
            "other engine minors remain unchanged");
    }
    for (const auto patch : {0u, 0x20000u, 0x30000u}) {
        expect(!should_use_farfarwest_ue581_view_extension_layout(exe, 0x50008, patch, true),
            "unvalidated FarFarWest engine patches fail closed");
    }
    expect(is_valid_farfarwest_view_extension_mapping(true, 20, 4, 6, 0xA0),
        "accept the source-validated completed-family callback mapping");
    expect(!is_valid_farfarwest_view_extension_mapping(true, 20, 0, 6, 0x70),
        "reject the observed SetupViewFamily/counter false positive from the live run");
    expect(!is_valid_farfarwest_view_extension_mapping(true, 20, 4, 6, 0x70),
        "correct slots must not validate the unrelated incrementing field");
    expect(!is_valid_farfarwest_view_extension_mapping(true, 20, 0, 6, 0xA0),
        "correct frame offset must not validate SetupViewFamily as Begin");
    expect(!is_valid_farfarwest_view_extension_mapping(true, 20, 4, 7, 0xA0),
        "per-view render callbacks must not be treated as per-family callbacks");
    expect(!is_valid_farfarwest_view_extension_mapping(false, 20, 4, 6, 0xA0),
        "unobserved source interface must not be accepted from cache");
    expect(!is_valid_farfarwest_view_extension_mapping(true, 19, 4, 6, 0xA0),
        "priority callback must not be treated as IsActiveThisFrame");
}

} // namespace

int main() {
    test_scene_view_layouts();
    test_rendering_mode_matrix();
    test_version_gates();
    test_ue58_render_pose_fallback();
    test_bodycam_owned_texture_layout();
    test_bodycam_native_fix_pre_exposure_pairing();
    test_ue58_pooled_slate_fallback();
    test_ue58_owned_ui_resource();
    test_ue58_slate_ui_capability();
    test_farfarwest_view_extension_discovery();

    if (failures != 0) {
        std::cerr << failures << " compatibility policy test(s) failed\n";
        return 1;
    }

    std::cout << "All compatibility policy tests passed\n";
    return 0;
}
