#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include <sdk/FSceneView.hpp>
#include <sdk/FSceneViewFamily.hpp>
#include <sdk/FSceneViewLayoutPolicy.hpp>
#include <sdk/MafiaDiscovery.hpp>

#include "mods/GameSpecific.hpp"
#include "mods/vr/BodycamTextureLayout.hpp"
#include "mods/vr/BreathedgeInventoryPolicy.hpp"
#include "mods/vr/Borderlands4Slate.hpp"
#include "mods/vr/UE58OwnedUITexture.hpp"
#include "mods/vr/CompatibilityPolicy.hpp"
#include "mods/vr/StellarBladeRendererEntry.hpp"
#include "mods/vr/HiFiRushRendererEntry.hpp"
#include "mods/vr/SifuRendererEntry.hpp"
#include "mods/vr/SifuMeshCommands.hpp"
#include "mods/vr/KtjLFogResources.hpp"
#include "mods/vr/KtjLHookContracts.hpp"
#include "mods/vr/SWZeroCompanyBinary.hpp"

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

void test_family_snapshot_accessors() {
    static_assert(std::is_empty_v<sdk::FSceneViewFamily> && std::is_empty_v<sdk::FSceneViewInitOptionsBase>,
        "publication storage must not add bytes to engine overlays");
    for (const auto offsets : {std::array<uint32_t, 3>{0, 0x18, 0x20}, {8, 0x20, 0x28},
                              {8, 0x30, 0x38}, {8, 0x30, 0x40}}) {
        alignas(16) std::array<uint8_t, 0x100> storage{};
        auto* family = new (storage.data()) sdk::FSceneViewFamily;
        auto* target = reinterpret_cast<sdk::FRenderTarget*>(uintptr_t{0x12340000});
        auto* scene = reinterpret_cast<sdk::FSceneInterface*>(uintptr_t{0x56780000});
        const uint32_t frame{45};
        std::memcpy(storage.data() + offsets[1], &target, sizeof(target));
        std::memcpy(storage.data() + offsets[2], &scene, sizeof(scene));
        std::memcpy(storage.data() + 0x80, &frame, sizeof(frame));
        sdk::FSceneViewFamily::set_offsets(offsets[0], offsets[1], offsets[2]);
        sdk::FSceneViewFamily::set_frame_count_offset(0x80);
        const auto snapshot = sdk::FSceneViewFamily::get_layout_snapshot();
        expect(family->get_render_target() == target && family->get_scene_interface() == scene &&
               family->get_frame_count() == frame &&
               reinterpret_cast<uint8_t*>(family->get_views()) == storage.data() + offsets[0],
            "production getters preserve legacy, Medium, UE5.4 and UE5.5+ offsets including zero");
        auto expected = storage;
        auto* replacement = reinterpret_cast<sdk::FRenderTarget*>(uintptr_t{0x98760000});
        std::memcpy(expected.data() + offsets[1], &replacement, sizeof(replacement));
        family->set_render_target(replacement);
        expect(storage == expected, "target write changes exactly the previously validated slot");
        sdk::FSceneViewFamily::set_frame_count_offset(0x84);
        expect(snapshot->frame_count == 0x80u && sdk::FSceneViewFamily::get_layout_snapshot()->frame_count == 0x84u,
            "separate frame metadata publication leaves old readers intact");
    }
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

void test_borderlands4_dedicated_ui_gate() {
    using uevr::games::should_use_borderlands4_ue554_dedicated_ui_target;
    for (const auto path : {L"Borderlands4.exe", L"C:\\Games\\Borderlands4.exe",
                            L"D:/Games/Borderlands4/Borderlands4.exe", L"C:\\Games\\BORDERLANDS4.EXE"}) {
        expect(should_use_borderlands4_ue554_dedicated_ui_target(path, L"5.5.4", 0, 0, true),
            "exact Borderlands4 UE5.5.4 DX12 executable must enter dedicated UI routing");
        expect(!should_use_borderlands4_ue554_dedicated_ui_target(path, L"5.5.4", 0, 0, false),
            "Borderlands4 DX11 must retain its existing UI routing");
    }

    for (const auto version : {L"", L"unknown", L"0.00", L"5.5"}) {
        for (const auto revision : {0u, 1u, 0xffffu}) {
            expect(should_use_borderlands4_ue554_dedicated_ui_target(
                       L"Borderlands4.exe", version, 0x00050005, 0x00040000 | revision, true),
                "Borderlands4 UE5.5.4 file evidence includes the observed 5.5.4.1 build revision");
        }
        expect(!should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0x00050005, 0x00030000, true) &&
               !should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0, 0, true),
            "another or unknown file patch must not enable Borderlands4 dedicated UI");
        expect(!should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0x00050006, 0x00040000, true) &&
               !should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0x00050005, 0x00050001, true) &&
               !should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0x00050005, 0x00040000, false),
            "Borderlands4 file fallback must retain exact version and DX12 boundaries");
    }

    for (const auto version : {L"4.25", L"4.27.2", L"5.2.1", L"5.4.4", L"5.5.0", L"5.5.3",
                               L"5.5.5", L"5.5.40", L"5.6.1", L"5.7.4", L"5.8.1", L"6.0"}) {
        expect(!should_use_borderlands4_ue554_dedicated_ui_target(
                   L"Borderlands4.exe", version, 0x00050005, 0x00040000, true),
            "conflicting embedded UE versions must not inherit Borderlands4 UE5.5.4 UI routing");
    }

    for (const auto path : {L"", L"Borderlands3.exe", L"NotBorderlands4.exe", L"Borderlands4.exe.bak",
                            L"C:\\Borderlands4.exe\\Other.exe", L"C:/Borderlands4/Other.exe",
                            L"MechWarrior-Win64-Shipping.exe", L"Stalker2-Win64-Shipping.exe",
                            L"Bodycam-Win64-Shipping.exe", L"SWZeroCompany.exe", L"ES2-Win64-Shipping.exe",
                            L"DaysGone.exe", L"SHCO.exe", L"ObserverSystemRedux.exe",
                            L"AVENAGame-Win64-Shipping.exe", L"Voyage-Win64-Shipping.exe"}) {
        expect(!should_use_borderlands4_ue554_dedicated_ui_target(path, L"5.5.4", 0, 0, true) &&
               !should_use_borderlands4_ue554_dedicated_ui_target(
                   path, L"5.5", 0x00050005, 0x00040000, true),
            "other games, directory names and partial filenames must not enter the Borderlands4 UI gate");
    }
}

void test_borderlands4_slate_inputs() {
    using namespace uevr::borderlands4_slate;
    constexpr uintptr_t base = 0x10000;
    constexpr uintptr_t renderer = base + 0x100, outputs = base + 0x200;
    constexpr uintptr_t element_list = base + 0x300, window = base + 0x400;
    constexpr uintptr_t viewport = base + 0x500, input_address = base + 0x600, array_address = base + 0x700;
    std::array<uint8_t, 0x800> memory{};
    size_t readable_size = memory.size(), reads{}, object_checks{};
    uintptr_t invalid_object{};
    const auto write = [&](uintptr_t address, const auto& value) {
        std::memcpy(memory.data() + address - base, &value, sizeof(value));
    };
    const auto read = [&](uintptr_t address, void* output, size_t size) {
        ++reads;
        if (address < base || address - base > readable_size || size > readable_size - (address - base)) {
            return false;
        }
        std::memcpy(output, memory.data() + address - base, size);
        return true;
    };
    const auto is_object = [&](uintptr_t address) {
        ++object_checks;
        return address != invalid_object && (address == renderer || address == window || address == viewport);
    };
    Inputs inputs{};
    inputs.renderer = renderer;
    inputs.window_element_list = element_list;
    inputs.window = window;
    inputs.viewport_info = viewport;
    inputs.scene_view_rect = {{0, 0}, {2560, 1440}};
    inputs.viewport_scale_ui = -1.0f;
    const Inputs valid = inputs;
    write(input_address, inputs);
    write(array_address, ArrayView{input_address, 1, 0xffffffffu});
    const auto before = memory;
    auto match = probe(renderer, outputs, 0, input_address, read, is_object);
    expect(match && match->abi == Abi::RendererFirst && match->outputs == outputs &&
           match->width == 2560 && match->height == 1440,
        "Borderlands4 accepts complete renderer-first Slate inputs with source-valid negative scale sentinel");
    expect(memory == before && reads <= 4 && object_checks == 3,
        "Borderlands4 probe only makes bounded observations and never modifies input memory");
    match = probe(outputs, renderer, 0, input_address, read, is_object);
    expect(match && match->abi == Abi::OutputsFirst && match->outputs == outputs,
        "Borderlands4 recognizes hidden output storage without treating it as a renderer or command list");
    match = probe(renderer, outputs, array_address, 0, read, is_object);
    expect(match && match->abi == Abi::SingleWindowArray && match->outputs == 0,
        "Borderlands4 single-window array has no output-storage promotion and ignores count padding");
    expect(!probe(renderer, outputs, array_address, input_address, read, is_object),
        "ambiguous direct and array matches must pass through without selecting an ABI");

    for (const auto extent : {Point{1280, 720}, {1920, 1080}, {3840, 2160}, {10240, 4320}}) {
        inputs.scene_view_rect = {{-20, 30}, {extent.x - 20, extent.y + 30}};
        inputs.viewport_scale_ui = 0.0f;
        write(input_address, inputs);
        match = probe(renderer, outputs, 0, input_address, read, is_object);
        expect(match && match->width == extent.x && match->height == extent.y,
            "Borderlands4 uses validated Slate dimensions, never a hardcoded monitor resolution");
    }
    for (const auto rect : {Rect{{0, 0}, {0, 1080}}, {{0, 0}, {1920, -1}},
                           {{0, 0}, {16385, 1080}},
                           {{std::numeric_limits<int32_t>::min(), 0}, {std::numeric_limits<int32_t>::max(), 1080}}}) {
        inputs = valid;
        inputs.scene_view_rect = rect;
        write(input_address, inputs);
        expect(!probe(renderer, outputs, 0, input_address, read, is_object),
            "empty, oversized and overflowing Slate extents must not initiate UI routing");
    }
    for (const auto scale : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        inputs = valid;
        inputs.viewport_scale_ui = scale;
        write(input_address, inputs);
        expect(!probe(renderer, outputs, 0, input_address, read, is_object),
            "non-finite Slate scale must fail validation");
    }
    inputs = valid;
    inputs.renderer = window;
    write(input_address, inputs);
    expect(!probe(renderer, outputs, 0, input_address, read, is_object),
        "Borderlands4 rejects an input owned by a different renderer");
    write(input_address, valid);
    for (const auto object : {renderer, window, viewport}) {
        invalid_object = object;
        expect(!probe(renderer, outputs, 0, input_address, read, is_object),
            "each live renderer, window and viewport must pass object validation");
    }
    invalid_object = 0;
    for (const auto count : {-1, 0, 2, std::numeric_limits<int32_t>::max()}) {
        write(array_address, ArrayView{input_address, count, 0});
        reads = 0;
        expect(!probe(renderer, outputs, array_address, 0, read, is_object) && reads == 1,
            "invalid and multi-window arrays are bounded passthroughs without walking guessed strides");
    }
    write(array_address, ArrayView{std::numeric_limits<uintptr_t>::max(), 1, 0});
    expect(!probe(renderer, outputs, array_address, 0, read, is_object),
        "unreadable array data must not be dereferenced");
    expect(!probe(renderer, input_address, 0, input_address, read, is_object) &&
           !probe(renderer, base - 1, 0, input_address, read, is_object),
        "aliased or unreadable output storage cannot be promoted as a texture result");
    readable_size = input_address - base + sizeof(Inputs) - 1;
    expect(!probe(renderer, outputs, 0, input_address, read, is_object),
        "a truncated input prefix must not use partially validated state");
    readable_size = memory.size();
    expect(probe(renderer, outputs, 0, input_address, read, is_object).has_value(),
        "a failed or incomplete early draw must not poison a later complete input");
}

void test_stalker2_lazy_ghost_bootstrap() {
    using namespace uevr::vr_compatibility;
    const auto supported = [](std::wstring_view path, std::wstring_view version,
                              uint32_t ms = 0x00050005, uint32_t ls = 0x00040000) {
        return uevr::games::is_stalker2_ue554_lazy_viewstate_runtime(path, version, ms, ls);
    };
    constexpr auto stalker = L"D:\\Games\\Stalker2-Win64-Shipping.exe";
    expect(supported(stalker, L"5.5.4") &&
           supported(L"Stalker2-Win64-Shipping.exe", L"5.5.4.0") &&
           supported(L"D:/Games/STALKER2-WIN64-SHIPPING.EXE", L"5.5.4"),
        "lazy ViewStates capability must accept the validated title independent of install path or case");
    for (const auto version : {L"", L"unknown", L"0.00", L"5.5"}) {
        expect(supported(stalker, version), "partial engine evidence needs the exact 5.5.4 file version");
        expect(!supported(stalker, version, 0x00050005, 0x00030000) &&
               !supported(stalker, version, 0x00050006, 0x00040000) &&
               !supported(stalker, version, 0, 0),
            "unknown or mismatched patch evidence must retain the existing bootstrap path");
    }
    for (const auto version : {L"5.1.1", L"5.4.4", L"5.5.3", L"5.5.40", L"5.6.1", L"5.7.4", L"5.8.1"}) {
        expect(!supported(stalker, version),
            "a known different engine version must override a coincidental 5.5.4 file version");
    }
    for (const auto path : {L"Other.exe", L"Medium-Win64-Shipping.exe", L"SHCO.exe",
                           L"DuneSandbox-Win64-Shipping.exe", L"DaysGone.exe",
                           L"Stalker2-Win64-Shipping.exe.backup", L"NotStalker2-Win64-Shipping.exe",
                           L"C:/Stalker2-Win64-Shipping.exe/Other.exe"}) {
        expect(!supported(path, L"5.5.4"), "the new bootstrap must require the exact executable basename");
    }

    for (unsigned mask = 0; mask < 32; ++mask) {
        const bool runtime = mask & 1;
        const bool sync = mask & 2;
        const bool native_fix = mask & 4;
        const bool split = mask & 8;
        const bool scene_compat = mask & 16;
        expect(should_avoid_stalker2_synced_post_init(runtime, sync, native_fix, split, scene_compat) == (mask == 3),
            "PostInit bypass must exclude DX11, other versions/titles, Native, Native Fix, alternate modes and compatibility paths");
    }

    for (const auto method : {RenderingMethod::NativeStereo, RenderingMethod::Synchronized,
                             RenderingMethod::Alternating, RenderingMethod::SyntheticDibr,
                             RenderingMethod::SyntheticDibrSingleView}) {
        expect(should_avoid_stalker2_synced_post_init(true, method == RenderingMethod::Synchronized, false, false, false) ==
               (method == RenderingMethod::Synchronized),
            "only explicit Synced may select lazy bootstrap, never Native under Extreme Compatibility");
    }

    for (unsigned mask = 0; mask < 16; ++mask) {
        const bool initialized = mask & 1;
        const bool lazy = mask & 2;
        const bool main_draw = mask & 4;
        const bool hook_ready = mask & 8;
        const bool expected = lazy ? main_draw && hook_ready : initialized;
        expect(is_ghosting_bootstrap_allocator_ready(initialized, lazy, main_draw, hook_ready) == expected,
            "lazy allocation must require the real Draw/hook path; every legacy PostInit prerequisite stays unchanged");
    }
    expect(is_ghosting_bootstrap_allocator_ready(false, true, true, true),
        "Stalker2 may use its natural allocation without falsely publishing PostInit completion");
    expect(!is_ghosting_bootstrap_allocator_ready(true, true, false, true),
        "a prior Native bootstrap must not allow a Synced pulse on an auxiliary view");
    expect(!is_ghosting_bootstrap_allocator_ready(false, false, true, true),
        "leaving Synced must restore the legacy prerequisite, not leak the lazy capability into Native");
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

    expect(should_harden_ue58_synthetic_ui_initialization(true, true, true, false),
        "only validated automatic UE5.8 DX12 synthetic UI requests use the new lifecycle");
    expect(!should_harden_ue58_synthetic_ui_initialization(true, false, true, false),
        "AVENA-style DX11 initialization remains unchanged");
    expect(!should_harden_ue58_synthetic_ui_initialization(false, true, true, false),
        "earlier and unvalidated future engines remain outside the new lifecycle");
    expect(!should_harden_ue58_synthetic_ui_initialization(true, true, false, false),
        "engine-owned, observing and quarantined UI routes do not start synthetic requests");
    expect(!should_harden_ue58_synthetic_ui_initialization(true, true, true, true),
        "legacy allowlisted title-specific UI ownership remains unchanged");

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

void test_mafia_discovery() {
    using namespace sdk::mafia;
    expect(is_ue544_runtime(L"D:/anywhere/MAFIATHEOLDCOUNTRY.EXE", 0x50004, 0x40000),
        "Mafia scope uses the exact basename and validated UE5.4.4 version");
    for (const auto path : {L"Other.exe", L"MafiaTheOldCountry.exe.bak", L"D:/MafiaTheOldCountry.exe/Other.exe"}) {
        expect(!is_ue544_runtime(path, 0x50004, 0x40000), "other titles never inherit Mafia discovery");
    }
    for (const auto version : {0x40019u, 0x50003u, 0x50005u, 0x50006u, 0x50008u}) {
        expect(!is_ue544_runtime(L"MafiaTheOldCountry.exe", version, 0x40000), "other engine layouts remain unchanged");
    }
    expect(!is_ue544_runtime(L"MafiaTheOldCountry.exe", 0x50004, 0x30000), "unvalidated patch versions fail closed");
    expect(supports_interface_fallback(L"r.AllowOcclusionQueries"), "missing occlusion data can use the validated variable interface");
    expect(!supports_interface_fallback(L"r.OneFrameThreadLag"), "already working raw controls retain their existing path");

    // Actual registration bytes from the matching Mafia executable; all runtime
    // addresses are decoded from relocations, never compiled into the scanner.
    const std::array<uint8_t, 59> registration{
        0x48,0x83,0xEC,0x28,0x48,0x8B,0x0D,0x85,0x75,0xC3,0x06,0x48,0x85,0xC9,0x74,0x32,
        0x48,0x8B,0x01,0xC7,0x44,0x24,0x20,0x01,0x00,0x00,0x00,0x48,0x8D,0x15,0x3A,0xD9,0xAC,0x04,
        0x4C,0x8D,0x05,0xDF,0x0C,0xC6,0x06,0x4C,0x8D,0x0D,0xE4,0xD7,0xAC,0x04,0xFF,0x50,0x38,
        0x48,0x83,0x3D,0x55,0x75,0xC3,0x06,0x00};
    for (const uintptr_t slide : {uintptr_t{0}, uintptr_t{0x10000000}, uintptr_t{0x7ff000000000}}) {
        expect(console_registration_storage(registration, 0x1442dd0c0 + slide, 0x148daaa1c + slide) == 0x14af14650 + slide,
            "tail-call registration resolves the same storage under ASLR");
    }
    for (size_t length = 0; length < registration.size(); ++length) {
        expect(!console_registration_storage(std::span{registration}.first(length), 0x1442dd0c0, 0x148daaa1c),
            "truncated registration never publishes a global");
    }
    expect(!console_registration_storage(registration, 0x1442dd0c0, 0x148daaa1d), "unrelated string reference is rejected");
    for (const size_t offset : {4u, 6u, 16u, 18u, 23u, 27u, 29u, 48u, 50u, 53u, 54u}) {
        auto bad = registration;
        bad[offset] ^= 1;
        expect(!console_registration_storage(bad, 0x1442dd0c0, 0x148daaa1c), "incorrect register, call, or storage agreement fails closed");
    }
    auto negative = registration;
    const int32_t load_displacement = -75; // address - 64, relative to address + 11
    const int32_t check_displacement = -123; // same storage, relative to address + 59
    std::memcpy(negative.data() + 7, &load_displacement, sizeof(load_displacement));
    std::memcpy(negative.data() + 54, &check_displacement, sizeof(check_displacement));
    expect(console_registration_storage(negative, 0x1442dd0c0, 0x148daaa1c) == 0x1442dd080,
        "negative RIP-relative manager displacement is sign-extended");

    std::array<uint8_t, 0x180> draw{};
    draw.fill(0x90);
    const std::array<uint8_t, 8> spill{0x48,0x89,0x94,0x24,0x88,0,0,0};
    const std::array<uint8_t, 23> debug{0x48,0x8B,0xB4,0x24,0x88,0,0,0,0x48,0x8B,0x06,
        0x48,0x89,0xF1,0xFF,0x90,0x60,0x01,0,0,0x48,0x89,0xC7};
    const std::array<uint8_t, 17> size{0x48,0x8B,0x06,0x48,0x8D,0x94,0x24,0x48,0x02,0,0,0x48,0x89,0xF1,0xFF,0x50,0x28};
    std::copy(spill.begin(), spill.end(), draw.begin() + 0x20);
    const std::array<uint8_t, 6> earlier_stereo_call{0xFF,0x90,0xD8,0x01,0,0};
    std::copy(earlier_stereo_call.begin(), earlier_stereo_call.end(), draw.begin() + 0x40);
    std::copy(debug.begin(), debug.end(), draw.begin() + 0x60);
    std::copy(size.begin(), size.end(), draw.begin() + 0xC0);
    const auto indices = viewport_helper_indices(draw);
    expect(indices && indices->debug_canvas == 44 && indices->size_xy == 5,
        "argument-proven helpers ignore the preceding IsStereoRenderingAllowed call");
    for (const size_t offset : {0x22u,0x24u,0x64u,0x70u,0xC2u,0xD0u}) {
        auto bad = draw;
        bad[offset] ^= 1;
        expect(!viewport_helper_indices(bad), "wrong viewport identity or helper slot is rejected");
    }
    auto ambiguous = draw;
    std::copy(size.begin(), size.end(), ambiguous.begin() + 0xE0);
    expect(!viewport_helper_indices(ambiguous), "ambiguous helper transactions must not publish indices");
    expect(!viewport_helper_indices(std::span{draw}.first(0xD0)), "both complete calls are required");

    for (const bool mafia : {false, true}) {
        for (const bool native : {false, true}) {
            for (const bool fix : {false, true}) {
                expect(uevr::vr_compatibility::should_preserve_mafia_pending_rhi_identity(mafia, native, fix) == (mafia && native && fix),
                    "pending-command ownership change is restricted to Mafia Native Fix, not ordinary Native, AFR or other games");
            }
        }
    }
}

void test_sifu_callable_renderer_entry() {
    using namespace uevr::sifu;
    for (const auto path : {L"Sifu-Win64-Shipping.exe", L"C:\\Games\\sifu-WIN64-shipping.EXE",
                           L"D:/Games/Sifu-Win64-Shipping.exe"}) {
        for (const uint32_t ms : {0u, 0x00040019u, 0x0004001Au, 0x0004001Bu, 0x00050008u}) {
            for (const uint32_t ls : {0u, 0x00010000u, 0x00020000u, 0x00020001u, 0x00030000u}) {
                for (const bool dx11 : {false, true}) {
                    for (const bool native_fix : {false, true}) {
                        expect(should_use_native_fix_renderer(path, ms, ls, dx11, native_fix) ==
                               (ms == 0x0004001A && ls == 0x00020000 && dx11 && native_fix),
                            "Sifu renderer is restricted to exact UE4.26.2, DX11 and effective Native Fix");
                    }
                }
            }
        }
    }
    for (const auto path : {L"", L"Sifu.exe", L"Sifu-Win64-Shipping.exe.bak", L"OtherSifu-Win64-Shipping.exe",
                           L"D:/Sifu-Win64-Shipping.exe/Other.exe", L"SHCO.exe", L"TheMedium-Win64-Shipping.exe",
                           L"Hi-Fi-RUSH.exe", L"SB-Win64-Shipping.exe", L"ObserverSystemRedux.exe",
                           L"HellbladeGame-Win64-Shipping.exe", L"prospi-Win64-Shipping.exe"}) {
        expect(!should_use_native_fix_renderer(path, 0x0004001A, 0x00020000, true, true),
            "other games and filename substrings retain their original renderer resolver");
    }
    expect(!is_distinct_renderer_entry(0, 0x2000), "Sifu missing renderer fails closed");
    expect(!is_distinct_renderer_entry(0x1000, 0), "Sifu missing Draw identity fails closed");
    expect(!is_distinct_renderer_entry(0x2000, 0x2000), "Sifu cannot hook Draw as the renderer");
    expect(is_distinct_renderer_entry(0x1000, 0x2000), "Sifu distinct renderer can be validated");

    sdk::detail::FamilyLayout layout{};
    expect(!matches_family_layout(layout), "Sifu cannot substitute undiscovered family offsets");
    layout.has_vtable = false;
    layout.views = 0;
    layout.render_target = 0x18;
    layout.scene_interface = 0x20;
    layout.frame_count = 0xBC;
    const auto original = layout;
    expect(matches_family_layout(layout) && layout == original, "Sifu layout corroboration is read-only");
    for (const auto field : {&sdk::detail::FamilyLayout::views, &sdk::detail::FamilyLayout::render_target,
                            &sdk::detail::FamilyLayout::scene_interface, &sdk::detail::FamilyLayout::frame_count}) {
        auto bad = layout;
        (bad.*field).reset();
        expect(!matches_family_layout(bad), "incomplete Sifu discovery remains retryable without publishing offsets");
        bad.*field = *(layout.*field) + 8;
        expect(!matches_family_layout(bad), "Sifu rejects a different learned family layout");
    }
    auto bad_layout = layout;
    bad_layout.has_vtable.reset();
    expect(!matches_family_layout(bad_layout), "unproven Sifu family polymorphism fails closed");
    bad_layout.has_vtable = true;
    expect(!matches_family_layout(bad_layout), "polymorphic families cannot inherit Sifu's layout");
    bad_layout = layout;
    bad_layout.frame_count = 0x5C;
    expect(!matches_family_layout(bad_layout), "stock UE4 frame offset cannot be reused for Sifu");

    // Independent EXE fixture, matching dev PDB: callable root [0,0x1ac),
    // CHAININFO callback child [0x1ac,0x1ea); its parent remains the ABI entry.
    const std::array<uint8_t, 34> prefix{
        0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x81,0xEC,0xC0,0x00,
        0x00,0x00,0x49,0x8B,0x48,0x20,0x45,0x33,0xE4,0x49,0x8B,0xF8,0x4C,0x8B,0xEA,0x41,0x8B,0xEC};
    const std::array<uint8_t, 24> unwind{
        0x01,0x12,0x09,0x00,0x12,0x01,0x18,0x00,0x0B,0xF0,0x09,0xD0,
        0x07,0xC0,0x05,0x70,0x04,0x60,0x03,0x50,0x02,0x30,0x00,0x00};
    const std::array<uint8_t, 76> loop{
        0x89,0x87,0xBC,0x00,0x00,0x00,0x39,0x9F,0xE8,0x00,0x00,0x00,0x7E,0x3E,0x4C,0x89,
        0xB4,0x24,0x00,0x01,0x00,0x00,0x4D,0x8B,0xF4,0x66,0x0F,0x1F,0x84,0x00,0x00,0x00,
        0x00,0x00,0x48,0x8B,0x87,0xE0,0x00,0x00,0x00,0x48,0x8B,0xD7,0x49,0x8B,0x0C,0x06,
        0x48,0x8B,0x01,0xFF,0x50,0x28,0xFF,0xC3,0x4D,0x8D,0x76,0x10,0x3B,0x9F,0xE8,0x00,
        0x00,0x00,0x7C,0xDE,0x4C,0x8B,0xB4,0x24,0x00,0x01,0x00,0x00};
    std::array<uint8_t, 0x1EA> combined{};
    std::copy(prefix.begin(), prefix.end(), combined.begin());
    std::copy(loop.begin(), loop.end(), combined.begin() + 0x19E);
    constexpr size_t callback = 0x1D4;
    expect(has_callable_renderer_entry(combined, unwind, callback),
        "Sifu disp32 frame store and short chained callback validate without relaxing stock thresholds");
    expect(!has_callable_renderer_entry(std::span{combined}.subspan(0x1AC), unwind, callback - 0x1AC),
        "Sifu callback continuation must never become a callable hook entry");
    for (size_t i = 0; i < combined.size(); ++i) {
        expect(!has_callable_renderer_entry(std::span{combined}.first(i), unwind, callback),
            "truncated Sifu entry or callback loop fails closed");
        if (i >= prefix.size() && i < 0x19E) { continue; }
        auto bad = combined;
        bad[i] ^= 1;
        expect(!has_callable_renderer_entry(bad, unwind, callback),
            "changed Sifu ABI, disp32 offsets, callback argument, loop or virtual slot fails closed");
    }
    for (size_t i = 0; i < unwind.size(); ++i) {
        expect(!has_callable_renderer_entry(combined, std::span{unwind}.first(i), callback),
            "truncated Sifu root unwind fails closed");
        auto bad = unwind;
        bad[i] ^= 1;
        expect(!has_callable_renderer_entry(combined, bad, callback), "changed Sifu root unwind fails closed");
    }
    auto child_unwind = unwind;
    child_unwind[0] = 0x21;
    expect(!has_callable_renderer_entry(combined, child_unwind, callback), "Sifu CHAININFO cannot be treated as a root");
    for (const auto offset : {size_t{0}, size_t{53}, callback - 1, callback + 1, combined.size(), SIZE_MAX}) {
        expect(!has_callable_renderer_entry(combined, unwind, offset), "Sifu unrelated callback or invalid bounds fail closed");
    }
    std::vector<uint8_t> oversized(0x4001);
    std::copy(combined.begin(), combined.end(), oversized.begin());
    expect(!has_callable_renderer_entry(oversized, unwind, callback), "Sifu validation work stays bounded");
}

void test_hifi_rush_callable_renderer_entry() {
    using namespace uevr::hifi;
    for (const auto path : {L"C:\\Games\\Hi-Fi-RUSH.exe", L"hi-fi-rush.exe", L"Hi-Fi-RUSH.exe.bak",
                           L"SHCO.exe", L"TheMedium-Win64-Shipping.exe", L"prospi-Win64-Shipping.exe",
                           L"HellbladeGame-Win64-Shipping.exe", L"SB-Win64-Shipping.exe", L"Other.exe"}) {
        for (const bool version : {false, true}) {
            for (const bool dx12 : {false, true}) {
                for (const bool native_fix : {false, true}) {
                    const bool exact_game = std::wstring_view{path} == L"C:\\Games\\Hi-Fi-RUSH.exe" ||
                        std::wstring_view{path} == L"hi-fi-rush.exe";
                    expect(should_use_native_fix_renderer(path, version, dx12, native_fix) ==
                        (exact_game && version && dx12 && native_fix), "Hi-Fi renderer stays inside its mode/version/game gate");
                }
            }
        }
    }
    expect(!is_distinct_renderer_entry(0, 0x2000), "missing renderer fails closed");
    expect(!is_distinct_renderer_entry(0x1000, 0), "missing Draw identity fails closed");
    expect(!is_distinct_renderer_entry(0x2000, 0x2000), "Draw cannot be installed as the renderer");
    expect(is_distinct_renderer_entry(0x1000, 0x2000), "distinct renderer remains eligible for validation");

    // Captured HBK .pdata: root [0,+0x19b), callback child [+0x19b,+0x1da).
    std::array<uint8_t, 0x1DA> combined{};
    std::copy(renderer_entry_prefix.begin(), renderer_entry_prefix.end(), combined.begin());
    std::copy(renderer_family_loop.begin(), renderer_family_loop.end(), combined.begin() + 0x190);
    constexpr size_t callback = 0x1C4;
    expect(has_callable_renderer_entry(combined, renderer_root_unwind, callback),
        "validated short root plus callback child is accepted without lowering generic thresholds");
    expect(!has_callable_renderer_entry(std::span{combined}.subspan(0x19B), renderer_root_unwind, callback - 0x19B),
        "callback child is not a callable entry");
    for (size_t i = 0; i < combined.size(); ++i) {
        expect(!has_callable_renderer_entry(std::span{combined}.first(i), renderer_root_unwind, callback),
            "truncated root or callback loop is rejected");
    }
    for (size_t i = 0; i < renderer_root_unwind.size(); ++i) {
        auto bad = renderer_root_unwind;
        bad[i] ^= 1;
        expect(!has_callable_renderer_entry(combined, bad, callback), "unvalidated unwind is rejected");
        expect(!has_callable_renderer_entry(combined, std::span{renderer_root_unwind}.first(i), callback),
            "truncated unwind is rejected");
    }
    for (size_t i = 0; i < combined.size(); ++i) {
        if (i >= renderer_entry_prefix.size() && i < 0x190) { continue; }
        auto bad = combined;
        bad[i] ^= 1;
        expect(!has_callable_renderer_entry(bad, renderer_root_unwind, callback),
            "changed entry ABI, family offset, callback argument or slot fails closed");
    }
    for (const auto offset : {size_t{0}, size_t{51}, callback - 1, callback + 1, combined.size(), SIZE_MAX}) {
        expect(!has_callable_renderer_entry(combined, renderer_root_unwind, offset),
            "unrelated or out-of-bounds return address cannot validate a renderer");
    }
    auto child_unwind = renderer_root_unwind;
    child_unwind[0] = 0x21;
    expect(!has_callable_renderer_entry(combined, child_unwind, callback), "CHAININFO is never accepted as the root");
}

void test_stellar_blade_callable_renderer_entry() {
    using uevr::games::should_use_stellar_blade_callable_renderer_entry;
    using uevr::stellar_blade::has_callable_renderer_entry;

    for (const auto path : {L"SB-Win64-Shipping.exe", L"D:/Games/SB-Win64-Shipping.exe",
                           L"C:\\Games\\sb-WIN64-shipping.EXE"}) {
        for (const bool ue426 : {false, true}) {
            for (const bool dx12 : {false, true}) {
                for (const bool native_fix : {false, true}) {
                    expect(should_use_stellar_blade_callable_renderer_entry(path, ue426, dx12, native_fix) ==
                           (ue426 && dx12 && native_fix),
                        "Stellar Blade entry repair is limited to UE4.26 DX12 Native Fix");
                }
            }
        }
    }
    for (const auto path : {L"", L"SB-Win64-Shipping.exe.bak", L"Other-SB-Win64-Shipping.exe",
                           L"D:/SB-Win64-Shipping.exe/Other.exe", L"Medium-Win64-Shipping.exe",
                           L"SHCO.exe", L"ObserverSystemRedux.exe", L"HellbladeGame-Win64-Shipping.exe",
                           L"Sycamore-Win64-Shipping.exe", L"Stalker2-Win64-Shipping.exe"}) {
        expect(!should_use_stellar_blade_callable_renderer_entry(path, true, true, true),
            "other titles and filename substrings must retain their existing renderer resolver");
    }

    // Exact EXE bytes: the 0x14-byte root is smaller than the old 0x200 body
    // threshold; its 0x20f-byte child alone contains both old body signatures.
    const std::array<uint8_t, 20> root{
        0x40,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,
        0x83,0xEC,0x60,0x49,0x8B,0x48,0x20,0x45,0x33,0xE4};
    const std::array<uint8_t, 16> unwind{
        0x01,0x0D,0x06,0x00,0x0D,0xB2,0x09,0xF0,
        0x07,0xE0,0x05,0xC0,0x03,0x70,0x02,0x60};
    expect(has_callable_renderer_entry(root, unwind), "validated short callable root is accepted");
    std::array<uint8_t, 0x223> combined{};
    std::copy(root.begin(), root.end(), combined.begin());
    const std::array<uint8_t, 8> child_prefix{0x48,0x89,0xAC,0x24,0x98,0x00,0x00,0x00};
    std::copy(child_prefix.begin(), child_prefix.end(), combined.begin() + root.size());
    combined[0x1D5] = 0x89; combined[0x1D6] = 0x47; combined[0x1D7] = 0x5C;
    combined[0x201] = 0xFF; combined[0x202] = 0x50; combined[0x203] = 0x28;
    auto child_unwind = unwind;
    child_unwind[0] = 0x21; // Version 1, CHAININFO; not a callable prologue.
    expect(has_callable_renderer_entry(combined, unwind), "root plus callback-containing child is accepted");
    expect(!has_callable_renderer_entry(std::span{combined}.subspan(root.size()), child_unwind),
        "large matching continuation must never become a callable hook entry");
    expect(!has_callable_renderer_entry(root, child_unwind), "CHAININFO root is rejected even with matching code");
    for (size_t length = 0; length < root.size(); ++length) {
        expect(!has_callable_renderer_entry(std::span{root}.first(length), unwind), "unreadable or truncated code fails closed");
    }
    for (size_t length = 0; length < unwind.size(); ++length) {
        expect(!has_callable_renderer_entry(root, std::span{unwind}.first(length)), "missing or truncated unwind fails closed");
    }
    for (size_t offset = 0; offset < root.size(); ++offset) {
        auto bad = root;
        bad[offset] ^= 1;
        expect(!has_callable_renderer_entry(bad, unwind), "unvalidated ABI prologue changes fail closed");
    }
    for (size_t offset = 0; offset < unwind.size(); ++offset) {
        auto bad = unwind;
        bad[offset] ^= 1;
        expect(!has_callable_renderer_entry(root, bad), "unvalidated unwind versions, flags and operations fail closed");
    }
}

void test_sw_zero_company_binary_revisions() {
    using namespace uevr::sw_zero_company;
    expect(binary_layouts[0].revision == 196320 && binary_layouts[1].revision == 196985,
        "SWZC retains the original revision alongside the validated update");
    expect(select_unique_layout([](const auto&) { return false; }) == nullptr,
        "SWZC unknown binary fails closed");
    expect(select_unique_layout([](const auto&) { return true; }) == nullptr,
        "SWZC ambiguous binary cannot mix compatibility revisions");

    for (const auto& layout : binary_layouts) {
        expect(select_unique_layout([&](const auto& candidate) { return candidate.revision == layout.revision; }) == &layout,
            "SWZC selection preserves one immutable revision");
        std::array<uint8_t, 0x50> slate{};
        std::copy(layout.slate_arguments.begin(), layout.slate_arguments.end(), slate.begin() + 0x2D);
        std::copy(layout.slate_renderer.begin(), layout.slate_renderer.end(), slate.begin() + layout.slate_renderer_offset);
        expect(matches_slate_arguments(layout, slate), "SWZC validated hidden-sret Slate arguments are accepted");
        // Discovery can run after the DrawWindow entry has already been hooked.
        std::fill(slate.begin(), slate.begin() + 0x20, 0xCC);
        expect(matches_slate_arguments(layout, slate), "SWZC Slate identity does not depend on a patched entry");
        for (size_t size = 0; size < layout.slate_renderer_offset + layout.slate_renderer.size(); ++size) {
            expect(!matches_slate_arguments(layout, std::span{slate}.first(size)),
                "SWZC truncated Slate evidence fails closed");
        }
        for (const auto& other : binary_layouts) {
            if (other.revision != layout.revision) {
                expect(!matches_slate_arguments(other, slate), "SWZC Slate revisions cannot cross-match");
            }
        }
        for (const auto offset : {0x2Du, 0x35u, 0x41u, layout.slate_renderer_offset, layout.slate_renderer_offset + 3}) {
            auto bad = slate;
            bad[offset] ^= 1;
            expect(!matches_slate_arguments(layout, bad), "SWZC changed Slate ABI evidence is rejected");
        }
        int32_t displacement{};
        std::memcpy(&displacement, layout.hologram_call.data() + layout.hologram_call.size() - 4, sizeof(displacement));
        expect(layout.hologram_call[layout.hologram_call.size() - 5] == 0xE8 &&
               static_cast<int64_t>(layout.hologram_return_rva) + displacement == layout.nanite_rva,
            "SWZC hologram caller must reach the dispatch in the same revision");
    }

    const auto check_exact_code = [](std::span<const uint8_t> code) {
        expect(matches_bytes(code, code), "SWZC complete validated code is accepted");
        expect(!matches_bytes(code, {}), "SWZC empty signature cannot validate a target");
        for (size_t size = 0; size < code.size(); ++size) {
            expect(!matches_bytes(code.first(size), code), "SWZC truncated hook evidence is rejected");
        }
        for (size_t offset = 0; offset < code.size(); ++offset) {
            std::vector<uint8_t> bad{code.begin(), code.end()};
            bad[offset] ^= 1;
            expect(!matches_bytes(bad, code), "SWZC changed hook evidence is rejected");
        }
    };
    for (const auto& layout : binary_layouts) {
        check_exact_code(layout.register_prologue);
        check_exact_code(layout.nanite_prologue);
        check_exact_code(layout.hologram_call);
    }
    check_exact_code(copy_prologue);
    check_exact_code(copy_call_layout);
    check_exact_code(separate_target_code);
    check_exact_code(initialize_hmd_prologue);
    check_exact_code(stereo_assignment);
    check_exact_code(stereo_enabled_code);
    check_exact_code(stereo_rtm_accessor);

    int32_t primary_disp{}, secondary_disp{};
    std::memcpy(&primary_disp, stereo_assignment.data() + 3, sizeof(primary_disp));
    std::memcpy(&secondary_disp, stereo_assignment.data() + 13, sizeof(secondary_disp));
    expect(static_cast<int64_t>(stereo_assignment_rva) + 7 + primary_disp == stereo_primary_rva &&
           static_cast<int64_t>(stereo_assignment_rva) + 17 + secondary_disp == stereo_secondary_rva,
        "SWZC inlined constructor assignments must address the exact two interfaces");
    expect(inlined_stereo_revision == binary_layouts[1].revision &&
           stereo_entries[1].slot == 1 && stereo_entries.back().slot == 16,
        "SWZC inlined constructor applies only to the new revision and retains the original stereo slot ABI");
}

} // namespace

void test_sifu_native_mesh_commands() {
    using namespace uevr::sifu;
    expect(is_supported_runtime(L"D:\\Games\\Sifu-Win64-Shipping.exe", 0x4001A, 0x20000, true),
        "Sifu DX11 4.26.2 cache guard accepts the validated runtime");
    for (const auto path : {L"Medium-Win64-Shipping.exe", L"HellbladeGame-Win64-Shipping.exe",
                           L"Other-Win64-Shipping.exe", L"Sifu-Win64-Shipping.exe.bak"}) {
        expect(!is_supported_runtime(path, 0x4001A, 0x20000, true),
            "Sifu cache guard excludes other executables");
    }
    expect(!is_supported_runtime(L"Sifu-Win64-Shipping.exe", 0x4001A, 0x20000, false),
        "Sifu cache guard does not change DX12");
    expect(!is_supported_runtime(L"Sifu-Win64-Shipping.exe", 0x4001A, 0x10000, true) &&
           !is_supported_runtime(L"Sifu-Win64-Shipping.exe", 0x4001B, 0x20000, true),
        "Sifu cache guard does not infer other engine versions");

    const auto validate = [](auto render, auto any, auto call) {
        return validate_mesh_command_code(mesh_command_timestamp, mesh_command_image_size, render, any, call);
    };
    expect(validate(cached_render_thread_code, cached_any_thread_code, cached_relevance_call_code),
        "Sifu complete getter and relevance caller evidence validates");
    expect(!validate_mesh_command_code(mesh_command_timestamp + 1, mesh_command_image_size,
               cached_render_thread_code, cached_any_thread_code, cached_relevance_call_code) &&
           !validate_mesh_command_code(mesh_command_timestamp, mesh_command_image_size + 1,
               cached_render_thread_code, cached_any_thread_code, cached_relevance_call_code),
        "Sifu cache guard rejects unknown binary revisions");
    const std::array<std::span<const uint8_t>, 3> signatures{
        cached_render_thread_code, cached_any_thread_code, cached_relevance_call_code};
    for (size_t index = 0; index < signatures.size(); ++index) {
        for (size_t length = 0; length < signatures[index].size(); ++length) {
            auto evidence = signatures;
            evidence[index] = evidence[index].first(length);
            expect(!validate(evidence[0], evidence[1], evidence[2]),
                "Sifu cache guard rejects every truncated signature");
        }
        for (size_t offset = 0; offset < signatures[index].size(); ++offset) {
            auto evidence = signatures;
            std::vector<uint8_t> changed{evidence[index].begin(), evidence[index].end()};
            changed[offset] ^= 1;
            evidence[index] = changed;
            expect(!validate(evidence[0], evidence[1], evidence[2]),
                "Sifu cache guard rejects changed getter and caller instructions");
        }
    }
    const auto relative = [](uint32_t rva, std::span<const uint8_t> bytes, size_t disp_offset, size_t length) {
        int32_t displacement{};
        std::memcpy(&displacement, bytes.data() + disp_offset, sizeof(displacement));
        return static_cast<int64_t>(rva) + length + displacement;
    };
    expect(relative(cached_relevance_call_rva, cached_relevance_call_code, 1, 5) == cached_render_thread_rva,
        "Sifu relevance constructor calls the validated render-thread decision");
    expect(relative(cached_render_thread_rva, cached_render_thread_code, 3, 7) ==
           relative(cached_any_thread_rva, cached_any_thread_code, 16, 20),
        "Sifu render-thread and any-thread decisions read the same console variable");

    for (const bool ready : {false, true}) {
        for (const bool stereo : {false, true}) {
            for (const bool native : {false, true}) {
                const bool rebuild = rebuild_native_mesh_commands(ready, stereo, native);
                expect(rebuild == (ready && stereo && native),
                    "Sifu uncached commands require complete hooks and active Native stereo");
                for (const bool original_value : {false, true}) {
                    int calls = 0;
                    const bool result = select_cached_mesh_commands(rebuild, [&]() {
                        ++calls;
                        return original_value;
                    });
                    expect(result == (!rebuild && original_value) && calls == (rebuild ? 0 : 1),
                        "Sifu Native rebuilds; other modes preserve either original cache decision exactly");
                }
            }
        }
    }
}

void test_breathedge_inventory_world_guard() {
    using namespace uevr::breathedge;
    using uevr::games::is_breathedge2_inventory_runtime;
    for (const auto path : {L"Breathedge2-Win64-Shipping.exe", L"D:\\Games\\Breathedge2-Win64-Shipping.exe",
                           L"D:/Steam/BREATHEDGE2-WIN64-SHIPPING.EXE"}) {
        expect(is_breathedge2_inventory_runtime(path, 0x00050007, 0x00040000, true),
            "Breathedge exact executable leaf accepts case and either path separator");
        expect(!is_breathedge2_inventory_runtime(path, 0x00050007, 0x00040000, false),
            "Breathedge inventory guard does not assume the DX11 path");
        for (const auto version : {std::pair{0x00050007u, 0x00030000u}, {0x00050007u, 0x00050000u},
                                   {0x00050008u, 0x00040000u}, {0x00050007u, 0x00040001u}}) {
            expect(!is_breathedge2_inventory_runtime(path, version.first, version.second, true),
                "Breathedge inventory guard rejects other engine patches and revisions");
        }
    }
    for (const auto path : {L"Other-Win64-Shipping.exe", L"Breathedge2-Win64-Shipping.exe.bak",
                           L"NotBreathedge2-Win64-Shipping.exe", L"D:/Breathedge2-Win64-Shipping.exe/Other.exe"}) {
        expect(!is_breathedge2_inventory_runtime(path, 0x00050007, 0x00040000, true),
            "Breathedge inventory guard cannot match substrings or parent directories");
    }
    expect(validated_binary(0xd18f68c7, 0x0a5b0000) &&
        !validated_binary(0xd18f68c6, 0x0a5b0000) && !validated_binary(0xd18f68c7, 0x0a5b1000),
        "unreflected viewport/world bits require the matching PDB binary fingerprint");
    // Captured Draw uses the secondary FCommonViewportClient base, while the
    // engine's reflected GameViewport points to the complete UObject.
    constexpr uintptr_t main_viewport = 0x23490fb89e0;
    constexpr uintptr_t draw_dispatch = 0x23490fb8a08;
    expect(viewport_candidate_from_draw(draw_dispatch) == main_viewport,
        "live Draw subobject must resolve to the actual viewport UObject before inspecting flags");
    expect(viewport_candidate_from_draw(draw_dispatch) + viewport_flags_offset == draw_dispatch + 0x44,
        "viewport flag access matches the game's Draw instructions and PDB");
    for (const auto wrong : {main_viewport, main_viewport + 0x38, draw_dispatch + 0x28, draw_dispatch - 0x28}) {
        expect(viewport_candidate_from_draw(wrong) != main_viewport,
            "primary, FExec and already-adjusted inputs cannot pass the main viewport identity check");
    }
    for (uintptr_t bad = 0; bad <= draw_viewport_subobject_offset; ++bad) {
        expect(viewport_candidate_from_draw(bad) == 0, "null and underflowing Draw arguments fail closed");
    }
    expect(viewport_candidate_from_draw(draw_dispatch + 1) == 0,
        "misaligned Draw arguments cannot become viewport UObject candidates");
    for (unsigned bits = 0; bits < 16; ++bits) {
        const bool native = (bits & 1) != 0, sync = (bits & 2) != 0;
        const bool extreme = (bits & 4) != 0, screen_2d = (bits & 8) != 0;
        expect(supported_mode(native, sync, extreme, screen_2d) == ((native || sync) && !extreme && !screen_2d),
            "Native including Native Fix and Synced are supported; 2D, Extreme and other modes are unchanged");
    }

    const InventoryObservation inventory{true, true, false, false, false, false, 4, 1.0f, 1, 0};
    expect(should_enable_inventory_world(inventory), "live inventory parent is eligible independently of its selected tab");
    for (const auto member : {&InventoryObservation::game_initialized, &InventoryObservation::inventory_root}) {
        auto invalid = inventory;
        invalid.*member = false;
        expect(!should_enable_inventory_world(invalid), "missing initialization or inventory root fails closed");
    }
    for (const auto member : {&InventoryObservation::pause_root, &InventoryObservation::auto_pause,
                             &InventoryObservation::cutscene, &InventoryObservation::death_screen}) {
        auto invalid = inventory;
        invalid.*member = true;
        expect(!should_enable_inventory_world(invalid), "pause, auto-pause, cinematics and death screens never enable the guard");
    }
    for (unsigned value = 0; value <= 255; ++value) {
        auto observed = inventory;
        observed.visibility = static_cast<uint8_t>(value);
        expect(should_enable_inventory_world(observed) == (value == 0 || value == 3 || value == 4),
            "hidden, collapsed and unknown visibility values are excluded");
        observed = inventory;
        observed.world_flags = static_cast<uint8_t>(value);
        expect(should_enable_inventory_world(observed) == ((value & 0x21) == 1),
            "world must have begun play and must not be tearing down");
    }
    for (float opacity : {0.0f, -1.0f, 1.1f, std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()}) {
        auto invalid = inventory;
        invalid.opacity = opacity;
        expect(!should_enable_inventory_world(invalid), "invisible and invalid inventory opacity fails closed");
    }
    for (int32_t length : {-1, 1, 20, std::numeric_limits<int32_t>::max()}) {
        auto invalid = inventory;
        invalid.next_url_length = length;
        expect(!should_enable_inventory_world(invalid), "pending travel or malformed travel state leaves world rendering untouched");
    }

    InventorySession before{{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80}, {1, 2, 3, 4, 5, 6, 7, 8}, 0x100};
    expect(should_restore_inventory_world(before, before, true) && !should_restore_inventory_world(before, before, false),
        "restoration requires a still-active validated inventory session");
    for (size_t i = 0; i < before.objects.size(); ++i) {
        auto changed = before;
        ++changed.objects[i];
        expect(!should_restore_inventory_world(before, changed, true), "owner replacement or world travel must not restore a stale bit");
        changed = before;
        ++changed.serials[i];
        expect(!should_restore_inventory_world(before, changed, true), "object address reuse must not restore a stale bit");
    }
    auto changed = before;
    ++changed.native_viewport;
    expect(!should_restore_inventory_world(before, changed, true), "native viewport replacement invalidates restoration");
    expect(!should_restore_inventory_world({}, {}, true), "empty sessions never restore flags");
    for (unsigned original = 0; original <= 255; ++original) {
        for (unsigned current = 0; current <= 255; ++current) {
            const auto enabled = enable_world_rendering(static_cast<uint8_t>(original));
            const auto restored = restore_world_rendering_bit(static_cast<uint8_t>(current), static_cast<uint8_t>(original));
            expect(enabled == (original & ~2u) && restored == ((current & ~2u) | (original & 2u)),
                "only bDisableWorldRendering is changed and unrelated engine writes survive restoration");
        }
    }
    for (unsigned opening = 0; opening < 10; ++opening) {
        auto session = before;
        session.objects[6] += opening * 0x100;
        session.serials[6] += opening;
        uint8_t flags = 0x82;
        flags = enable_world_rendering(flags);
        expect(flags == 0x80, "each inventory opening enables the world only for the draw");
        expect(should_restore_inventory_world(session, session, true), "new inventory roots are accepted without a one-shot latch");
        flags = restore_world_rendering_bit(flags, 0x82);
        expect(flags == 0x82, "each draw restores the game's inventory rendering intent");
        auto closed = inventory;
        closed.inventory_root = false;
        expect(!should_enable_inventory_world(closed), "after inventory closes the guard is inactive");
    }
}

void test_ktjl_fog_resources() {
    namespace f = uevr::ktjl::fog;
    namespace k = sdk::ktjl;
    constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
    constexpr uintptr_t left = 0x62000000, right = 0x62001000, states = 0x63000000;
    const auto slot = views + f::view_stride + f::fog_reference_offset;
    struct Fixture {
        struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool executable{}; };
        std::vector<Block> blocks;
        size_t reads{};
        void add(uintptr_t address, const void* p, size_t n, bool code = false) {
            const auto first = static_cast<const uint8_t*>(p);
            blocks.push_back({address, {first, first + n}, code});
        }
        void put(uintptr_t address, const void* p, size_t n) {
            for (auto& b : blocks) {
                if (address >= b.address && address - b.address <= b.bytes.size() && n <= b.bytes.size() - (address - b.address)) {
                    std::memcpy(b.bytes.data() + address - b.address, p, n); return;
                }
            }
            expect(false, "KTJL fog fixture writes stay in mapped storage");
        }
        sdk::discovery::Memory memory() {
            return {this,
                [](void* c, uintptr_t a, void* p, size_t n) {
                    auto& self = *static_cast<Fixture*>(c); ++self.reads;
                    for (const auto& b : self.blocks) {
                        if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                            std::memcpy(p, b.bytes.data() + a - b.address, n); return true;
                        }
                    }
                    return false;
                },
                [](void* c, uintptr_t a, size_t n) {
                    for (const auto& b : static_cast<Fixture*>(c)->blocks) {
                        if (b.executable && a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) { return true; }
                    }
                    return false;
                }};
        }
    };
    const auto resource = [&](uintptr_t texture) {
        std::array<uint8_t, 0xF0> data{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(data.data() + offset, &value, sizeof(value)); };
        put(0, base + f::pool_vtable_rva); put(8, texture); put(0x10, texture); put(0x18, texture + 0x100);
        put(0x88, int32_t{2}); put(0xE8, base + f::pool_rva);
        put(0x90 + 0x14, int32_t{80}); put(0x90 + 0x18, int32_t{45}); put(0x90 + 0x1C, int32_t{64});
        put(0x90 + 0x20, int32_t{1}); put(0x90 + 0x26, uint16_t{1}); put(0x90 + 0x28, uint16_t{1});
        put(0x90 + 0x2C, uint32_t{10}); put(0x90 + 0x30, uint32_t{8}); put(0x90 + 0x34, uint32_t{0x40010009});
        return data;
    };
    const auto fixture = [&] {
        Fixture x;
        std::array<uint8_t, 512> pe{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(pe.data() + offset, &value, sizeof(value)); };
        put(0, uint16_t{0x5A4D}); put(0x3C, uint32_t{0x80}); put(0x80, uint32_t{0x4550});
        put(0x84, uint16_t{0x8664}); put(0x88, k::image_timestamp); put(0x98, uint16_t{0x20B}); put(0xD0, k::image_size);
        x.add(base, pe.data(), pe.size());
        const auto code = [&](uintptr_t rva, const auto& bytes) { x.add(base + rva, bytes.data(), bytes.size(), true); };
        code(0xAFFF76, k::class_name_access); code(0xA1DC2F, k::object_iteration); code(0x58DC4E, k::name_entry_access);
        for (const auto& e : f::code_evidence) { code(e.rva, e.bytes); }
        const auto accessor = base + f::get_desc_rva, release = base + 0x55AAE0, destructor = base + 0x57836D0;
        x.add(base + f::pool_vtable_rva + 0x10, &accessor, 8); x.add(base + f::pool_vtable_rva + 0x38, &release, 8);
        x.add(base + f::view_vtable_rva, &destructor, 8);
        const f::Header h{views, 2, 2}; x.add(renderer + f::views_offset, &h, sizeof(h));
        for (size_t i = 0; i < 2; ++i) {
            const f::ViewPrefix v{base + f::view_vtable_rva, 0, renderer + 0x10, states + 0x100 * i};
            const auto vt = base + k::stereo::state_vtable_rva;
            const f::Rect rect{static_cast<int32_t>(640 * i), 0, static_cast<int32_t>(640 * (i + 1)), 360};
            const auto ref = i == 0 ? left : uintptr_t{};
            x.add(views + f::view_stride * i, &v, sizeof(v)); x.add(v.state, &vt, 8);
            x.add(views + f::view_stride * i + f::rect_offset, &rect, sizeof(rect));
            x.add(views + f::view_stride * i + f::fog_reference_offset, &ref, 8);
        }
        const auto a = resource(0x64000000), b = resource(0x65000000);
        x.add(left, a.data(), a.size()); x.add(right, b.data(), b.size());
        return x;
    };
    const auto mutate = [](Fixture& x, uintptr_t address, auto value) { x.put(address, &value, sizeof(value)); };
    expect(f::supports(L"D:\\Games\\SuicideSquad_KTJL.exe", true), "KTJL fog gate accepts exact DX12 executable");
    for (const auto path : {L"Other.exe", L"SuicideSquad_KTJL.exe.bak", L"D:\\SuicideSquad_KTJL.exe\\Other.exe"}) {
        expect(!f::supports(path, true), "KTJL fog repair does not select other titles");
    }
    expect(!f::supports(L"SuicideSquad_KTJL.exe", false), "KTJL fog repair does not change DX11");
    auto x = fixture();
    expect(f::validate_code(x.memory(), base), "KTJL complete fog producer/consumer/allocator/cleanup evidence validates");
    for (const auto& e : f::code_evidence) {
        for (size_t i = 0; i < e.bytes.size(); ++i) {
            x = fixture(); mutate(x, base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!f::validate_code(x.memory(), base), "any changed KTJL fog instruction rejects installation");
        }
        x = fixture();
        for (auto& b : x.blocks) { if (b.address == base + e.rva) { b.bytes.pop_back(); } }
        expect(!f::validate_code(x.memory(), base), "truncated KTJL fog instructions reject installation");
    }
    for (const auto address : {base + 0x88, base + f::pool_vtable_rva + 0x10, base + f::pool_vtable_rva + 0x38, base + f::view_vtable_rva}) {
        x = fixture(); mutate(x, address, uint32_t{});
        expect(!f::validate_code(x.memory(), base), "changed KTJL image/accessor/cleanup vtable rejects installation");
    }
    x = fixture();
    auto p = f::prepare(x.memory(), base, renderer, true);
    expect(p.decision == f::Decision::allocate && p.right_slot == slot && p.left.address == left && x.reads <= 16,
        "null right fog volume resolves only its owned ref with bounded render-thread reads");
    const auto before = x.blocks;
    int calls{};
    auto allocate = [&](const f::Plan& plan) { ++calls; mutate(x, plan.right_slot, right); return true; };
    expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::allocated && calls == 1,
        "right volume allocation succeeds only after its engine-owned postconditions validate");
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i].address != slot) { expect(before[i].bytes == x.blocks[i].bytes, "fog allocation preserves every primary/family/scene byte"); }
    }
    expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::existing && calls == 1,
        "existing distinct right fog volume is never overwritten or reallocated");
    for (int frame = 0; frame < 20; ++frame) {
        mutate(x, slot, uintptr_t{});
        expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::allocated,
            "new per-frame FViewInfo fog refs are repaired without stale private texture caching");
    }
    x = fixture(); calls = 0;
    expect(f::ensure(x.memory(), base, renderer, false, allocate) == f::Outcome::passthrough && calls == 0 && x.reads == 0,
        "disabled fog stays an exact no-allocation passthrough");
    for (int count : {0, 1, 3, 4}) {
        x = fixture(); mutate(x, renderer + f::views_offset + 8, int32_t{count});
        expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::passthrough && calls == 0,
            "mono/AFR/capture/non-pair families never enter the two-view repair");
    }
    const std::array<uintptr_t, 17> invalid{
        views, views + f::view_stride, views + 0x10, views + f::view_stride + 0x10,
        views + 0x18, states, states + 0x100, views + f::rect_offset + 8,
        views + f::view_stride + f::rect_offset + 12, views + f::fog_reference_offset,
        left, left + 8, left + 0x10, left + 0x18, left + 0x88, left + 0xE8, left + 0x90 + 0x2C};
    for (auto address : invalid) {
        x = fixture(); mutate(x, address, uint32_t{});
        expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::rejected && calls == 0,
            "invalid view/resource/descriptor never authorizes an engine allocator call");
    }
    x = fixture(); mutate(x, views + f::view_stride + 0x18, states);
    expect(f::prepare(x.memory(), base, renderer, true).decision == f::Decision::reject, "aliased eye states are not a validated pair");
    x = fixture(); mutate(x, slot, left);
    expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::rejected, "never share the primary fog texture with the secondary eye");
    x = fixture();
    expect(f::ensure(x.memory(), base, renderer, true, [](const f::Plan&) { return false; }) == f::Outcome::rejected,
        "allocator refusal prevents dispatch with a null resource");
    expect(f::ensure(x.memory(), base, renderer, true, [](const f::Plan&) { return true; }) == f::Outcome::rejected,
        "allocator return alone cannot authorize dispatch");
    x = fixture(); mutate(x, right + 8, uintptr_t{0x64000000});
    expect(f::ensure(x.memory(), base, renderer, true, allocate) == f::Outcome::rejected,
        "distinct pooled objects cannot alias the same GPU texture");
    for (const auto offset : {0x14, 0x18, 0x1C, 0x20, 0x26, 0x28, 0x2C, 0x34}) {
        x = fixture(); mutate(x, left + 0x90 + offset, uint16_t{});
        expect(f::prepare(x.memory(), base, renderer, true).decision == f::Decision::reject,
            "invalid volume extent/array/mip/sample/format/flags are rejected");
    }
    x = fixture(); p = f::prepare(x.memory(), base, renderer, true); mutate(x, slot, right);
    mutate(x, renderer + f::views_offset, views + 0x20000);
    expect(!f::allocation_completed(x.memory(), base, p), "changed renderer generation cannot publish a stale fog ref");
}

void test_ktjl_hook_contracts() {
    namespace h = uevr::ktjl::hooks;
    namespace k = sdk::ktjl;
    constexpr uintptr_t base = 0x140000000;
    struct Fixture {
        struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool code; };
        std::vector<Block> blocks;
        void add(uintptr_t a, std::span<const uint8_t> bytes, bool code) {
            blocks.push_back({a, {bytes.begin(), bytes.end()}, code});
        }
        sdk::discovery::Memory memory() {
            return {this, [](void* c, uintptr_t a, void* p, size_t n) {
                for (const auto& b : static_cast<Fixture*>(c)->blocks) {
                    if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                        std::memcpy(p, b.bytes.data() + a - b.address, n); return true;
                    }
                }
                return false;
            }, [](void* c, uintptr_t a, size_t n) {
                for (const auto& b : static_cast<Fixture*>(c)->blocks) {
                    if (b.code && a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                        return true;
                    }
                }
                return false;
            }};
        }
    };
    Fixture original;
    std::array<uint8_t, 512> pe{};
    const auto put = [&](size_t offset, auto value) { std::memcpy(pe.data() + offset, &value, sizeof(value)); };
    put(0, uint16_t{0x5A4D}); put(0x3C, uint32_t{0x80}); put(0x80, uint32_t{0x4550});
    put(0x84, uint16_t{0x8664}); put(0x88, k::image_timestamp); put(0x98, uint16_t{0x20B}); put(0xD0, k::image_size);
    original.add(base, pe, false);
    original.add(base + 0xAFFF76, k::class_name_access, true);
    original.add(base + 0xA1DC2F, k::object_iteration, true);
    original.add(base + 0x58DC4E, k::name_entry_access, true);
    original.add(base + h::add_object_rva, h::add_object_entry, true);
    original.add(base + 0x6368D9, h::add_object_store, true);
    original.add(base + h::texture_create_rva, h::texture_entry, true);
    original.add(base + h::allocate_return_rva, h::texture_callsite, true);
    const auto add_ok = [&](Fixture& f) { return h::validates_add_object(f.memory(), base, base + h::add_object_rva); };
    const auto texture_ok = [&](Fixture& f) { return h::validates_texture(f.memory(), base); };
    expect(add_ok(original) && texture_ok(original), "KTJL exact allocator and texture call contracts validate");
    expect(!h::validates_add_object(original.memory(), base, base + h::add_object_rva + 1),
        "KTJL never assigns the RDX ABI to a different discovered function");
    for (size_t block = 1; block < original.blocks.size(); ++block) {
        const auto check = [&](Fixture& f) { return block < 4 ? add_ok(f) || texture_ok(f) :
            block < 6 ? add_ok(f) : texture_ok(f); };
        for (size_t i = 0; i < original.blocks[block].bytes.size(); ++i) {
            auto changed = original; changed.blocks[block].bytes[i] ^= 1;
            expect(!check(changed), "KTJL changed ABI instructions reject hook installation");
        }
        auto changed = original; changed.blocks[block].bytes.pop_back();
        expect(!check(changed), "KTJL truncated code rejects hook installation");
        changed = original; changed.blocks[block].code = false;
        expect(!check(changed), "KTJL non-executable candidate rejects hook installation");
    }
    for (const auto offset : {0, 0x80, 0x84, 0x88, 0x98, 0xD0}) {
        auto changed = original; changed.blocks[0].bytes[offset] ^= 1;
        expect(!add_ok(changed) && !texture_ok(changed), "KTJL wrong executable image rejects both repairs");
    }
    const auto caller = base + h::texture_return_rva;
    expect(h::owns_texture_call(base, caller, true, 6008, 2936, 1, 0, 1, false), "KTJL exact viewport transaction is owned");
    for (const auto size : {0U, 16385U}) {
        expect(!h::owns_texture_call(base, caller, true, size, 2936, 1, 0, 1, false), "KTJL bad width rejected");
        expect(!h::owns_texture_call(base, caller, true, 6008, size, 1, 0, 1, false), "KTJL bad height rejected");
    }
    expect(!h::owns_texture_call(base, caller + 1, true, 6008, 2936, 1, 0, 1, false), "unrelated texture caller passes through");
    expect(!h::owns_texture_call(base, caller, false, 6008, 2936, 1, 0, 1, false), "unarmed/other-thread texture call passes through");
    expect(!h::owns_texture_call(base, caller, true, 6008, 2936, 2, 0, 1, false), "changed mip contract passes through");
    expect(!h::owns_texture_call(base, caller, true, 6008, 2936, 1, 8, 1, false), "changed creation flags pass through");
    expect(!h::owns_texture_call(base, caller, true, 6008, 2936, 1, 0, 2, false), "changed target flags pass through");
    expect(!h::owns_texture_call(base, caller, true, 6008, 2936, 1, 0, 1, true), "separate resolve textures pass through");
    auto next = [](uintptr_t p, uintptr_t& out) { out = p == 0x20000 ? 0x30000 : 0; return true; };
    const auto chain = h::collect_class_chain(0x20000, next);
    expect(chain && chain->count == 2 && chain->classes[0] == 0x20000 && chain->classes[1] == 0x30000,
        "complete class chain is snapshotted in order");
    expect(!h::collect_class_chain(0, next) && !h::collect_class_chain(0xFFFFFF01, next), "null/unaligned class is rejected");
    expect(!h::collect_class_chain(0x20000, [](uintptr_t, uintptr_t&) { return false; }), "unreadable class cannot publish a partial chain");
    expect(!h::collect_class_chain(0x20000, [](uintptr_t p, uintptr_t& out) { out = p; return true; }), "self-cycle rejected");
    expect(!h::collect_class_chain(0x20000, [](uintptr_t p, uintptr_t& out) { out = p == 0x20000 ? 0x30000 : 0x20000; return true; }),
        "multi-node cycle rejected");
    expect(!h::collect_class_chain(0x20000, [](uintptr_t p, uintptr_t& out) { out = p + 8; return true; }), "class traversal is bounded");
}

#include "KtjLRendererEntryTests.hpp"
#include "KtjLCloudResourcesTests.hpp"
#include "DuneFrameHandoffTests.hpp"

int main(int argc, char** argv) {
    test_dune_frame_handoff();
    if (argc == 3 && std::string_view{argv[1]} == "--ktjl-memory-image") { test_ktjl_cloud_memory_image(argv[2]); }
    test_ktjl_renderer_entry();
    test_ktjl_cloud_resources();
    test_ktjl_hook_contracts();
    test_ktjl_fog_resources();
    test_breathedge_inventory_world_guard();
    test_scene_view_layouts();
    test_rendering_mode_matrix();
    test_version_gates();
    test_borderlands4_dedicated_ui_gate();
    test_borderlands4_slate_inputs();
    test_stalker2_lazy_ghost_bootstrap();
    test_ue58_render_pose_fallback();
    test_bodycam_owned_texture_layout();
    test_bodycam_native_fix_pre_exposure_pairing();
    test_ue58_pooled_slate_fallback();
    test_ue58_owned_ui_resource();
    test_ue58_slate_ui_capability();
    test_farfarwest_view_extension_discovery();
    test_mafia_discovery();
    test_family_snapshot_accessors();
    test_stellar_blade_callable_renderer_entry();
    test_hifi_rush_callable_renderer_entry();
    test_sifu_callable_renderer_entry();
    test_sifu_native_mesh_commands();
    test_sw_zero_company_binary_revisions();

    if (failures != 0) {
        std::cerr << failures << " compatibility policy test(s) failed\n";
        return 1;
    }

    std::cout << "All compatibility policy tests passed\n";
    return 0;
}
