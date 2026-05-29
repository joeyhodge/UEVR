#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <winternl.h>

#include <asmjit/asmjit.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility/Memory.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/String.hpp>
#include <utility/Thread.hpp>
#include <utility/Emulation.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/EngineModule.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameEngine.hpp>
#include <sdk/CVar.hpp>
#include <sdk/Slate.hpp>
#include <sdk/DynamicRHI.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/Utility.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UGameViewportClient.hpp>
#include <sdk/Globals.hpp>
#include <sdk/FName.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FViewport.hpp>
#include <sdk/UKismetRenderingLibrary.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/FSceneViewFamily.hpp>

#include <sdk/UGameplayStatics.hpp>
#include <sdk/APawn.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/FTextureRenderTargetResource.hpp>

#include "Framework.hpp"
#include "Mods.hpp"
#include "mods/UObjectHook.hpp"
#include "mods/GameSpecific.hpp"

#include <bdshemu.h>
#include <bddisasm.h>
#include <disasmtypes.h>

#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/threading/RenderThreadWorker.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>
#include "../VR.hpp"
#include "../../utility/Logging.hpp"

#include "FFakeStereoRenderingHook.hpp"

#include <tracy/Tracy.hpp>

//#define FFAKE_STEREO_RENDERING_LOG_ALL_CALLS

FFakeStereoRenderingHook* g_hook = nullptr;
uint32_t g_frame_count{};

namespace {
bool is_readable_process_range(uintptr_t address, size_t size);
bool is_executable_process_range(uintptr_t address, size_t size);

std::mutex g_shf_texture_probe_mutex{};
std::unordered_set<uintptr_t> g_shf_logged_texture_probe_keys{};
std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> g_shf_last_texture_probe_by_base{};
std::unordered_set<uintptr_t> g_shf_logged_rtm_candidate_natives{};
uint64_t g_shf_rtm_candidate_count{};
uint64_t g_shf_rtm_candidate_suppressed{};

constexpr uint32_t AVOWED_NATIVE_FIX_STABLE_FRAMES = 180;
constexpr uint32_t AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES = 45;
constexpr auto AVOWED_NATIVE_FIX_RENDER_GAP = std::chrono::milliseconds(250);
constexpr auto AVOWED_NATIVE_FIX_TRANSITION_HOLD = std::chrono::seconds(10);
constexpr auto AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD = std::chrono::milliseconds(1500);
constexpr auto AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING = std::chrono::seconds(60);

bool is_deadzone_ue56_executable() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path || exe_path->find(L"DeadzoneSteam-Win64-Shipping") == std::wstring::npos) {
            return false;
        }

        const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
        const auto file_version = sdk::get_file_version_info();

        return str_version.starts_with("5.6") || file_version.dwFileVersionMS == 0x00050006;
    }();

    return result;
}

struct AvowedNativeFixGateState {
    uintptr_t scene{};
    uintptr_t render_target{};
    uintptr_t scene_capture_render_target{};
    uintptr_t scene_capture_native{};
    uintptr_t last_ready_scene{};
    uintptr_t last_ready_render_target{};
    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point hold_until{};
    std::chrono::steady_clock::time_point missing_since{};
    uint32_t stable_frames{};
    uint32_t required_stable_frames{AVOWED_NATIVE_FIX_STABLE_FRAMES};
    bool ready{};
    bool has_baseline{};
    bool had_ready_baseline{};
    bool targets_missing{};
    bool fast_reacquire{};
};

std::mutex g_avowed_native_fix_gate_mutex{};
AvowedNativeFixGateState g_avowed_native_fix_gate{};
std::mutex g_ue56_rt_probe_mutex{};
std::unordered_map<uintptr_t, bool> g_ue56_native_resource_probe_cache{};

struct UE51RenderTargetChurnStats {
    uint64_t allocate_seen{};
    uint64_t ui_created{};
    uint64_t ui_reused{};
    uintptr_t last_allocate_return_address{};
    uintptr_t last_ui_create_return_address{};
    uintptr_t last_ui_texture{};
    uint32_t last_ui_width{};
    uint32_t last_ui_height{};
    std::chrono::steady_clock::time_point last_log{};
};

std::mutex g_ue51_rt_churn_mutex{};
UE51RenderTargetChurnStats g_ue51_rt_churn{};
constexpr auto ENGINE_RENDER_TIMING_LOG_INTERVAL = std::chrono::seconds(5);

struct EngineRenderTimingStats {
    uint64_t count{};
    double total_ms{};
    double max_ms{};

    void add(std::chrono::steady_clock::duration duration) {
        const auto ms = std::chrono::duration<double, std::milli>{duration}.count();
        ++count;
        total_ms += ms;
        if (ms > max_ms) {
            max_ms = ms;
        }
    }

    double avg() const {
        return count == 0 ? 0.0 : total_ms / (double)count;
    }

    void reset() {
        count = 0;
        total_ms = 0.0;
        max_ms = 0.0;
    }
};

EngineRenderTimingStats g_begin_render_viewfamily_real_timing{};
EngineRenderTimingStats g_begin_render_viewfamily_timing{};
EngineRenderTimingStats g_prerender_viewfamily_rt_timing{};
std::chrono::steady_clock::time_point g_engine_render_last_log{};

bool shf_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"SHf-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool aphelion_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"PIO-WinGDK-Shipping") != std::wstring::npos ||
             exe_path->find(L"PIO-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"Aphelion") != std::wstring::npos);
    }();

    return result;
}

bool ark_ascended_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"ArkAscended.exe") != std::wstring::npos ||
             exe_path->find(L"ArkAscended-Win64-Shipping") != std::wstring::npos);
    }();

    return result;
}

bool mechwarrior_clans_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"MechWarrior-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"MW5Clans") != std::wstring::npos);
    }();

    return result;
}

bool directive8020_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Directive8020") != std::wstring::npos;
    }();

    return result;
}

bool everwind_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Everwind-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool stalker2_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool avowed_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_avowed_executable_path(*exe_path);
    }();

    return result;
}

bool subnautica2_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"Subnautica2-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"Subnautica2-WinGDK-Shipping") != std::wstring::npos);
    }();

    return result;
}

bool daysgone_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"DaysGone.exe") != std::wstring::npos ||
             exe_path->find(L"BendGame") != std::wstring::npos);
    }();

    return result;
}

bool pitpanic_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        // Cover both the demo and future/full Pit Panic executable names.
        return exe_path && exe_path->find(L"PitPanic") != std::wstring::npos;
    }();

    return result;
}

bool windrose_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Windrose-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

void avowed_native_fix_gate_reset(const char* reason) {
    if (!avowed_is_current_game()) {
        return;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (g_avowed_native_fix_gate.has_baseline || g_avowed_native_fix_gate.ready || g_avowed_native_fix_gate.stable_frames != 0) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Resetting render transition gate: {}",
            reason != nullptr ? reason : "<unknown>");
    }

    g_avowed_native_fix_gate = {};
}

uintptr_t avowed_try_get_native_resource(FRHITexture2D* texture) {
    if (!avowed_is_current_game() || texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return 0;
    }

    try {
        const auto native = texture->get_native_resource();

        if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
            return 0;
        }

        return (uintptr_t)native;
    } catch (...) {
        return 0;
    }
}

bool avowed_native_fix_gate_update(
    uintptr_t scene,
    uintptr_t render_target,
    uintptr_t scene_capture_render_target,
    uintptr_t scene_capture_native,
    bool prerequisites_ready,
    uint32_t* out_stable_frames = nullptr,
    uint32_t* out_required_stable_frames = nullptr)
{
    if (!avowed_is_current_game()) {
        return true;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (out_stable_frames != nullptr) {
        *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
    }

    if (out_required_stable_frames != nullptr) {
        *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;
    }

    const auto now = std::chrono::steady_clock::now();

    if (g_avowed_native_fix_gate.last_update.time_since_epoch().count() != 0) {
        const auto render_gap = now - g_avowed_native_fix_gate.last_update;

        if (render_gap > AVOWED_NATIVE_FIX_RENDER_GAP) {
            const auto can_fast_reacquire = g_avowed_native_fix_gate.had_ready_baseline || g_avowed_native_fix_gate.ready;
            g_avowed_native_fix_gate.ready = false;
            g_avowed_native_fix_gate.stable_frames = 0;
            g_avowed_native_fix_gate.required_stable_frames =
                can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
            g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;
            const auto requested_hold_duration = can_fast_reacquire
                ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
                : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
            g_avowed_native_fix_gate.hold_until = std::max(
                g_avowed_native_fix_gate.hold_until,
                now + requested_hold_duration);

            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Render gap {:.0f}ms detected; holding one view for transition safety fast_reacquire={}",
                std::chrono::duration<double, std::milli>(render_gap).count(),
                can_fast_reacquire);
        }
    }

    g_avowed_native_fix_gate.last_update = now;

    if (!prerequisites_ready || scene == 0 || render_target == 0 || scene_capture_render_target == 0) {
        if (g_avowed_native_fix_gate.has_baseline || g_avowed_native_fix_gate.ready || g_avowed_native_fix_gate.stable_frames != 0) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Holding one view while render targets are unavailable scene={:x} target={:x} capture_rt={:x} capture_native={:x} prereqs={}",
                scene,
                render_target,
                scene_capture_render_target,
                scene_capture_native,
                prerequisites_ready);
        }

        if (g_avowed_native_fix_gate.had_ready_baseline || g_avowed_native_fix_gate.ready) {
            if (!g_avowed_native_fix_gate.targets_missing) {
                g_avowed_native_fix_gate.missing_since = now;
            }

            // Inventory/menu transitions briefly remove Avowed's capture target. Keep the last
            // known-good gameplay baseline so reacquiring the same path can use a short gate.
            g_avowed_native_fix_gate.targets_missing = true;
            g_avowed_native_fix_gate.ready = false;
            g_avowed_native_fix_gate.stable_frames = 0;
            g_avowed_native_fix_gate.fast_reacquire = false;
            g_avowed_native_fix_gate.required_stable_frames = AVOWED_NATIVE_FIX_STABLE_FRAMES;
        } else {
            g_avowed_native_fix_gate = {};
        }

        if (out_stable_frames != nullptr) {
            *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
                ? g_avowed_native_fix_gate.required_stable_frames
                : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        }

        return false;
    }

    const auto baseline_changed =
        !g_avowed_native_fix_gate.has_baseline ||
        g_avowed_native_fix_gate.scene != scene ||
        g_avowed_native_fix_gate.render_target != render_target ||
        g_avowed_native_fix_gate.scene_capture_render_target != scene_capture_render_target ||
        g_avowed_native_fix_gate.scene_capture_native != scene_capture_native;

    if (baseline_changed) {
        const auto missing_duration = g_avowed_native_fix_gate.missing_since.time_since_epoch().count() != 0
            ? now - g_avowed_native_fix_gate.missing_since
            : std::chrono::steady_clock::duration{};
        const auto matches_last_ready_path =
            g_avowed_native_fix_gate.had_ready_baseline &&
            g_avowed_native_fix_gate.last_ready_scene == scene &&
            g_avowed_native_fix_gate.last_ready_render_target == render_target;
        const auto can_fast_reacquire =
            g_avowed_native_fix_gate.targets_missing &&
            matches_last_ready_path &&
            missing_duration <= AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING;

        g_avowed_native_fix_gate.scene = scene;
        g_avowed_native_fix_gate.render_target = render_target;
        g_avowed_native_fix_gate.scene_capture_render_target = scene_capture_render_target;
        g_avowed_native_fix_gate.scene_capture_native = scene_capture_native;
        const auto requested_hold_duration = can_fast_reacquire
            ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
            : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
        const auto requested_hold_until = now + requested_hold_duration;
        g_avowed_native_fix_gate.hold_until = can_fast_reacquire
            ? requested_hold_until
            : std::max(g_avowed_native_fix_gate.hold_until, requested_hold_until);
        g_avowed_native_fix_gate.stable_frames = 0;
        g_avowed_native_fix_gate.required_stable_frames =
            can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.has_baseline = true;
        g_avowed_native_fix_gate.targets_missing = false;
        g_avowed_native_fix_gate.missing_since = {};
        g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Render transition detected; holding one view scene={:x} target={:x} capture_rt={:x} capture_native={:x} fast_reacquire={} stable_required={}",
            scene,
            render_target,
            scene_capture_render_target,
            scene_capture_native,
            can_fast_reacquire,
            g_avowed_native_fix_gate.required_stable_frames);

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames;
        }

        return false;
    }

    if (g_avowed_native_fix_gate.targets_missing) {
        const auto missing_duration = g_avowed_native_fix_gate.missing_since.time_since_epoch().count() != 0
            ? now - g_avowed_native_fix_gate.missing_since
            : std::chrono::steady_clock::duration{};
        const auto matches_last_ready_path =
            g_avowed_native_fix_gate.had_ready_baseline &&
            g_avowed_native_fix_gate.last_ready_scene == scene &&
            g_avowed_native_fix_gate.last_ready_render_target == render_target;
        const auto can_fast_reacquire =
            matches_last_ready_path &&
            missing_duration <= AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING;

        const auto requested_hold_duration = can_fast_reacquire
            ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
            : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
        const auto requested_hold_until = now + requested_hold_duration;
        g_avowed_native_fix_gate.hold_until = can_fast_reacquire
            ? requested_hold_until
            : std::max(g_avowed_native_fix_gate.hold_until, requested_hold_until);
        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.stable_frames = 0;
        g_avowed_native_fix_gate.required_stable_frames =
            can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        g_avowed_native_fix_gate.targets_missing = false;
        g_avowed_native_fix_gate.missing_since = {};
        g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Render targets reacquired on existing baseline fast_reacquire={} stable_required={}",
            can_fast_reacquire,
            g_avowed_native_fix_gate.required_stable_frames);
    }

    if (g_avowed_native_fix_gate.hold_until > now) {
        if (out_stable_frames != nullptr) {
            *out_stable_frames = 0;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames;
        }

        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.stable_frames = 0;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Holding one view during transition grace window remaining={:.1f}s",
            std::chrono::duration<double>(g_avowed_native_fix_gate.hold_until - now).count());

        return false;
    }

    if (!g_avowed_native_fix_gate.ready) {
        const auto required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;

        if (g_avowed_native_fix_gate.stable_frames < required_stable_frames) {
            ++g_avowed_native_fix_gate.stable_frames;
        }

        if (out_stable_frames != nullptr) {
            *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = required_stable_frames;
        }

        if (g_avowed_native_fix_gate.stable_frames >= required_stable_frames) {
            g_avowed_native_fix_gate.ready = true;
            g_avowed_native_fix_gate.had_ready_baseline = true;
            g_avowed_native_fix_gate.last_ready_scene = scene;
            g_avowed_native_fix_gate.last_ready_render_target = render_target;
            SPDLOG_INFO(
                "[Avowed][NativeStereoFix] Render transition stabilized after {} frames; enabling two-view native fix fast_reacquire={}",
                g_avowed_native_fix_gate.stable_frames,
                g_avowed_native_fix_gate.fast_reacquire);
        }
    }

    return g_avowed_native_fix_gate.ready;
}

bool avowed_native_fix_gate_ready(uint32_t* out_stable_frames = nullptr, uint32_t* out_required_stable_frames = nullptr) {
    if (!avowed_is_current_game()) {
        return true;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (out_stable_frames != nullptr) {
        *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
    }

    if (out_required_stable_frames != nullptr) {
        *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;
    }

    return g_avowed_native_fix_gate.ready;
}

bool is_ue_5_7_or_newer() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        if (str_version.starts_with("5.7") || str_version.starts_with("5.8") || str_version.starts_with("5.9")) {
            return true;
        }
    }

    return disk_version.dwFileVersionMS >= 0x50007;
}

bool is_ue_5_6_or_newer() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        if (str_version.starts_with("5.6") || str_version.starts_with("5.7") || str_version.starts_with("5.8") || str_version.starts_with("5.9")) {
            return true;
        }
    }

    return disk_version.dwFileVersionMS >= 0x50006;
}

bool is_ue_5_1_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("5.1");
    }

    return disk_version.dwFileVersionMS >= 0x50001 && disk_version.dwFileVersionMS < 0x50002;
}

bool is_ue_5_1_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("5.1");
    }

    return disk_version.dwFileVersionMS >= 0x50001 && disk_version.dwFileVersionMS < 0x50002;
}

bool is_ue_5_4_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("5.4");
    }

    return disk_version.dwFileVersionMS >= 0x50004 && disk_version.dwFileVersionMS < 0x50005;
}

bool is_ue_5_4_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    return is_ue_5_4_runtime();
}

bool is_ue_5_5_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("5.5");
    }

    return disk_version.dwFileVersionMS >= 0x50005 && disk_version.dwFileVersionMS < 0x50006;
}

bool is_ue_5_5_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    return is_ue_5_5_runtime();
}

bool is_ue_5_5_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    return is_ue_5_5_runtime();
}

bool is_ue_5_6_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("5.6");
    }

    return disk_version.dwFileVersionMS >= 0x50006 && disk_version.dwFileVersionMS < 0x50007;
}

bool ue56_dx12_try_get_native_resource(FRHITexture2D* texture, const char* source, ID3D12Resource** out_native = nullptr, D3D12_RESOURCE_DESC* out_desc = nullptr) {
    if (out_native != nullptr) {
        *out_native = nullptr;
    }

    if (out_desc != nullptr) {
        *out_desc = {};
    }

    if (!is_ue_5_6_dx12_backend()) {
        return true;
    }

    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.6][RT] {} candidate is not readable yet: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    void* vtable = nullptr;

    try {
        vtable = *(void**)texture;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.6][RT] {} candidate has no readable vtable: tex={:x} vtable={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
        return false;
    }

    {
        std::scoped_lock _{g_ue56_rt_probe_mutex};
        if (const auto it = g_ue56_native_resource_probe_cache.find((uintptr_t)vtable);
            it != g_ue56_native_resource_probe_cache.end() && !it->second)
        {
            return false;
        }
    }

    // UE 5.6 can expose Slate/viewport FRHITexture candidates whose native-resource
    // vtable discovery executes unsafe render-thread thunks. Do not probe them from
    // the fallback path; let the D3D12 backbuffer/texture hooks discover the scene.
    SPDLOG_WARNING_EVERY_N_SEC(2, "[UE5.6][RT] Refusing unsafe FRHITexture::GetNativeResource probing for {} candidate: tex={:x} vtable={:x}",
        source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
    {
        std::scoped_lock _{g_ue56_rt_probe_mutex};
        g_ue56_native_resource_probe_cache[(uintptr_t)vtable] = false;
    }
    return false;
}

bool is_ue57_dx11_backend() {
    return is_ue_5_7_or_newer() && g_framework != nullptr && g_framework->is_dx11();
}

bool supports_ue57_dedicated_ui_target() {
    if (!is_ue_5_7_or_newer() || g_framework == nullptr) {
        return false;
    }

    return g_framework->is_dx12() || g_framework->is_dx11();
}

bool supports_ue55_dedicated_ui_target_for_current_game() {
    // These UE5.5 titles expose a valid Slate UI texture but route Slate to the
    // wrong target, leaving the HUD clipped in the upper-left/left-eye path.
    // Keep this allowlisted and DX12-only until more UE5.5 games validate it.
    return (aphelion_is_current_game() ||
            ark_ascended_is_current_game() ||
            mechwarrior_clans_is_current_game() ||
            directive8020_is_current_game() ||
            everwind_is_current_game() ||
            is_deadzone_ue56_executable()) &&
        g_framework != nullptr &&
        g_framework->is_dx12() &&
        !is_ue_5_7_or_newer();
}

bool supports_dedicated_ui_target_for_current_game() {
    return supports_ue57_dedicated_ui_target() || supports_ue55_dedicated_ui_target_for_current_game();
}

bool should_preserve_promoted_ue55_slate_target() {
    return mechwarrior_clans_is_current_game() || everwind_is_current_game() || is_deadzone_ue56_executable();
}

bool is_probable_ue57_dx11_texture_desc_prepare_function(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 0x80)) {
        return false;
    }

    try {
        // D3D11 UE 5.7 can put a descriptor prepare/copy helper before the real
        // create wrapper. Calling that helper with the create signature is unsafe.
        return utility::scan(fn, 0x100, "0F B6 42 32").has_value()
            && utility::scan(fn, 0x100, "48 83 C2 38").has_value()
            && (utility::scan(fn, 0x100, "0F 10 42 08").has_value() ||
                utility::scan(fn, 0x100, "48 8B 02").has_value() ||
                utility::scan(fn, 0x100, "48 8B 42 24").has_value());
    } catch (...) {
        return false;
    }
}

void log_engine_render_timing_if_needed() {
    if (!is_ue_5_7_or_newer()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (g_engine_render_last_log.time_since_epoch().count() == 0) {
        g_engine_render_last_log = now;
        return;
    }

    if (now - g_engine_render_last_log < ENGINE_RENDER_TIMING_LOG_INTERVAL) {
        return;
    }

    if (g_begin_render_viewfamily_real_timing.count == 0 &&
        g_begin_render_viewfamily_timing.count == 0 &&
        g_prerender_viewfamily_rt_timing.count == 0)
    {
        g_engine_render_last_log = now;
        return;
    }

    auto vr = VR::get();
    const auto hmd_active = vr != nullptr && vr->is_hmd_active();
    const auto native_stereo = vr != nullptr && vr->is_native_stereo_fix_enabled();

    spdlog::info(
        "[UE57][engine-render-profiler] begin_render_viewfamily_real avg={:.2f}ms max={:.2f}ms n={} begin_render_viewfamily avg={:.2f}ms max={:.2f}ms n={} prerender_viewfamily_rt avg={:.2f}ms max={:.2f}ms n={} hmd={} native_stereo={}",
        g_begin_render_viewfamily_real_timing.avg(),
        g_begin_render_viewfamily_real_timing.max_ms,
        g_begin_render_viewfamily_real_timing.count,
        g_begin_render_viewfamily_timing.avg(),
        g_begin_render_viewfamily_timing.max_ms,
        g_begin_render_viewfamily_timing.count,
        g_prerender_viewfamily_rt_timing.avg(),
        g_prerender_viewfamily_rt_timing.max_ms,
        g_prerender_viewfamily_rt_timing.count,
        hmd_active,
        native_stereo
    );

    g_engine_render_last_log = now;
    g_begin_render_viewfamily_real_timing.reset();
    g_begin_render_viewfamily_timing.reset();
    g_prerender_viewfamily_rt_timing.reset();
}

bool shf_is_valid_texture_with_vtable(FRHITexture2D* texture, void* required_vtable) {
    if (texture == nullptr || required_vtable == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return false;
    }

    void* vtable{};

    try {
        vtable = *(void**)texture;
    } catch (...) {
        return false;
    }

    if (vtable != required_vtable) {
        return false;
    }

    FRHITexture2D::set_vtable(vtable);
    return true;
}

std::optional<D3D12_RESOURCE_DESC> shf_try_get_d3d12_desc(FRHITexture2D* texture) {
    if (texture == nullptr) {
        return std::nullopt;
    }

    try {
        const auto native = (ID3D12Resource*)texture->get_native_resource();

        if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
            return std::nullopt;
        }

        return native->GetDesc();
    } catch (...) {
        return std::nullopt;
    }
}

bool is_probable_d3d_native_resource(void* native) {
    if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
        return false;
    }

    void* vtable{};

    try {
        vtable = *(void**)native;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        return false;
    }

    const auto module = utility::get_module_within(vtable);
    if (!module) {
        return false;
    }

    const auto module_path = utility::get_module_path(*module);
    if (!module_path) {
        return false;
    }

    auto module_path_lower = std::string(*module_path);
    std::transform(module_path_lower.begin(), module_path_lower.end(), module_path_lower.begin(), ::tolower);
    return module_path_lower.ends_with("d3d12.dll") ||
        module_path_lower.ends_with("d3d12core.dll") ||
        module_path_lower.ends_with("dxgi.dll") ||
        module_path_lower.ends_with("d3d12sdklayers.dll");
}

std::optional<uintptr_t> ue55_find_texture_desc_offset(FRHITexture2D* texture) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return std::nullopt;
    }

    const auto texture_address = (uintptr_t)texture;
    constexpr std::array<uintptr_t, 2> desc_offsets{0x20, 0xf0};

    for (const auto desc_offset : desc_offsets) {
        if (texture_address + desc_offset < texture_address ||
            IsBadReadPtr((void*)(texture_address + desc_offset), 0x38)) {
            continue;
        }

        const auto extent_x = *(const int32_t*)(texture_address + desc_offset + 0x24);
        const auto extent_y = *(const int32_t*)(texture_address + desc_offset + 0x28);
        const auto num_mips = *(const uint8_t*)(texture_address + desc_offset + 0x30);
        const auto num_samples = *(const uint8_t*)(texture_address + desc_offset + 0x31);
        const auto dimension = *(const uint8_t*)(texture_address + desc_offset + 0x32);
        const auto format = *(const uint8_t*)(texture_address + desc_offset + 0x33);

        if (extent_x <= 0 || extent_y <= 0 || extent_x > 65536 || extent_y > 65536) {
            continue;
        }

        if (num_mips == 0 || num_mips > 32) {
            continue;
        }

        if (!(num_samples == 1 || num_samples == 2 || num_samples == 4 || num_samples == 8 || num_samples == 16)) {
            continue;
        }

        if (dimension > 8 || format == 0 || format > 128) {
            continue;
        }

        return desc_offset;
    }

    return std::nullopt;
}

bool ue55_dx12_try_get_native_resource_direct(
    FRHITexture2D* texture,
    const char* source,
    ID3D12Resource** out_native = nullptr,
    D3D12_RESOURCE_DESC* out_desc = nullptr)
{
    if (out_native != nullptr) {
        *out_native = nullptr;
    }

    if (out_desc != nullptr) {
        *out_desc = {};
    }

    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] {} candidate is not readable: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    void** vtable{};

    try {
        vtable = *(void***)texture;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*) * 6)) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] {} candidate has no readable vtable: tex={:x} vtable={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
        return false;
    }

    const auto desc_offset = ue55_find_texture_desc_offset(texture);
    if (!desc_offset) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] refusing {} candidate without a UE5.5/5.6 FRHITextureDesc: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    const std::array<size_t, 2> direct_slots = *desc_offset == 0xf0
        ? std::array<size_t, 2>{5ull, 4ull}
        : std::array<size_t, 2>{4ull, 5ull};

    for (const auto slot : direct_slots) {
        using GetNativeResourceFn = void* (*)(const FRHITexture2D*);
        const auto fn = (GetNativeResourceFn)vtable[slot];

        if (fn == nullptr || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
            continue;
        }

        void* native_raw = nullptr;

        try {
            native_raw = fn(texture);
        } catch (...) {
            continue;
        }

        if (!is_probable_d3d_native_resource(native_raw)) {
            continue;
        }

        auto* native = (ID3D12Resource*)native_raw;
        D3D12_RESOURCE_DESC desc{};

        try {
            desc = native->GetDesc();
        } catch (...) {
            continue;
        }

        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width == 0 ||
            desc.Height == 0 ||
            desc.Width > 65536 ||
            desc.Height > 65536)
        {
            continue;
        }

        FRHITexture2D::set_vtable(vtable);

        if (out_native != nullptr) {
            *out_native = native;
        }

        if (out_desc != nullptr) {
            *out_desc = desc;
        }

        return true;
    }

    SPDLOG_INFO_EVERY_N_SEC(2,
        "[UE5.5][SlateUI] direct GetNativeResource slots {} and {} did not produce a valid D3D12 texture for {} tex={:x}",
        direct_slots[0],
        direct_slots[1],
        source != nullptr ? source : "<unknown>",
        (uintptr_t)texture);

    return false;
}

std::optional<D3D12_RESOURCE_DESC> ue55_try_get_d3d12_desc(FRHITexture2D* texture, const char* source) {
    D3D12_RESOURCE_DESC desc{};

    if (!ue55_dx12_try_get_native_resource_direct(texture, source, nullptr, &desc)) {
        return std::nullopt;
    }

    return desc;
}

void shf_log_rtm_candidate(VRRenderTargetManager_Base* rtm, FRHITexture2D* texture, const char* source) {
    if (!shf_is_current_game() || !g_framework->is_dx12() || rtm == nullptr || texture == nullptr) {
        return;
    }

    ID3D12Resource* native = nullptr;
    std::optional<D3D12_RESOURCE_DESC> desc{};

    try {
        native = (ID3D12Resource*)texture->get_native_resource();

        if (native != nullptr && !IsBadReadPtr(native, sizeof(void*))) {
            desc = native->GetDesc();
        }
    } catch (...) {
    }

    bool log_unique = false;
    uint64_t seen = 0;
    uint64_t unique = 0;
    uint64_t suppressed = 0;

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};
        ++g_shf_rtm_candidate_count;
        seen = g_shf_rtm_candidate_count;

        const auto key = (uintptr_t)(native != nullptr ? native : (ID3D12Resource*)texture);

        if (!g_shf_logged_rtm_candidate_natives.contains(key)) {
            g_shf_logged_rtm_candidate_natives.insert(key);
            log_unique = g_shf_logged_rtm_candidate_natives.size() <= 64;
        } else {
            ++g_shf_rtm_candidate_suppressed;
        }

        unique = g_shf_logged_rtm_candidate_natives.size();
        suppressed = g_shf_rtm_candidate_suppressed;
    }

    if (log_unique && desc) {
        SPDLOG_WARN("[SHf][RTM] accepting unique texture candidate #{} source={} tex={:x} native={:x} [{}x{} fmt={} flags=0x{:x}] current_rt={:x}",
            seen, source, (uintptr_t)texture, (uintptr_t)native, desc->Width, desc->Height, (uint32_t)desc->Format,
            (uint32_t)desc->Flags, (uintptr_t)rtm->get_render_target());
    } else if (log_unique) {
        SPDLOG_WARN("[SHf][RTM] accepting unique texture candidate #{} source={} tex={:x} native={:x} desc=<unavailable> current_rt={:x}",
            seen, source, (uintptr_t)texture, (uintptr_t)native, (uintptr_t)rtm->get_render_target());
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][RTM] texture candidate summary seen={} unique_natives={} duplicate_suppressed={} last_source={} last_tex={:x} last_native={:x} current_rt={:x}",
            seen, unique, suppressed, source, (uintptr_t)texture, (uintptr_t)native, (uintptr_t)rtm->get_render_target());
    }
}

bool shf_can_reuse_current_ui_target(VRRenderTargetManager_Base* rtm, uint32_t expected_width, uint32_t expected_height) {
    if (!shf_is_current_game() || !g_framework->is_dx12() || rtm == nullptr || expected_width == 0 || expected_height == 0) {
        return false;
    }

    auto* ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || ui_target == rtm->get_render_target()) {
        return false;
    }

    const auto desc = shf_try_get_d3d12_desc(ui_target);

    if (!desc || desc->Width != expected_width || desc->Height != expected_height) {
        return false;
    }

    SPDLOG_INFO_EVERY_N_SEC(2,
        "[SHf] Reusing stable UI texture {:x} [{}x{} fmt={}]; skipping duplicate UI texture creation",
        (uintptr_t)ui_target, desc->Width, desc->Height, (uint32_t)desc->Format);

    return true;
}

void ue51_log_rt_churn_summary_locked(const char* reason) {
    const auto now = std::chrono::steady_clock::now();

    if (g_ue51_rt_churn.last_log.time_since_epoch().count() != 0 &&
        now - g_ue51_rt_churn.last_log < std::chrono::seconds(30))
    {
        return;
    }

    g_ue51_rt_churn.last_log = now;

    uint32_t current_width = 0;
    uint32_t current_height = 0;

    if (g_framework != nullptr) {
        const auto size = g_framework->get_d3d12_rt_size();
        current_width = (uint32_t)size.x;
        current_height = (uint32_t)size.y;
    }

    SPDLOG_INFO(
        "[UE5.1][RTChurn] summary reason={} alloc_seen={} ui_created={} ui_reused={} last_alloc={:x} last_create={:x} last_ui={:x} last_ui_size={}x{} current_rt_size={}x{}",
        reason != nullptr ? reason : "<unknown>",
        g_ue51_rt_churn.allocate_seen,
        g_ue51_rt_churn.ui_created,
        g_ue51_rt_churn.ui_reused,
        g_ue51_rt_churn.last_allocate_return_address,
        g_ue51_rt_churn.last_ui_create_return_address,
        g_ue51_rt_churn.last_ui_texture,
        g_ue51_rt_churn.last_ui_width,
        g_ue51_rt_churn.last_ui_height,
        current_width,
        current_height);
}

void ue51_note_rt_allocation(uintptr_t relative_return_address) {
    if (!is_ue_5_1_dx12_backend()) {
        return;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};
    ++g_ue51_rt_churn.allocate_seen;
    g_ue51_rt_churn.last_allocate_return_address = relative_return_address;
    ue51_log_rt_churn_summary_locked("allocate");
}

void ue51_note_ui_created(FRHITexture2D* ui_texture, uint32_t width, uint32_t height) {
    if (!is_ue_5_1_dx12_backend() || ui_texture == nullptr) {
        return;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};
    ++g_ue51_rt_churn.ui_created;
    g_ue51_rt_churn.last_ui_create_return_address = g_ue51_rt_churn.last_allocate_return_address;
    g_ue51_rt_churn.last_ui_texture = (uintptr_t)ui_texture;
    g_ue51_rt_churn.last_ui_width = width;
    g_ue51_rt_churn.last_ui_height = height;
    ue51_log_rt_churn_summary_locked("ui_created");
}

bool ue51_can_reuse_current_ui_target(VRRenderTargetManager_Base* rtm, uint32_t expected_width, uint32_t expected_height) {
    if (!is_ue_5_1_dx12_backend() || rtm == nullptr || expected_width == 0 || expected_height == 0) {
        return false;
    }

    const auto* ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || ui_target == rtm->get_render_target()) {
        return false;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};

    if (g_ue51_rt_churn.ui_created == 0 ||
        g_ue51_rt_churn.last_allocate_return_address == 0 ||
        g_ue51_rt_churn.last_allocate_return_address != g_ue51_rt_churn.last_ui_create_return_address ||
        g_ue51_rt_churn.last_ui_texture != (uintptr_t)ui_target ||
        g_ue51_rt_churn.last_ui_width != expected_width ||
        g_ue51_rt_churn.last_ui_height != expected_height)
    {
        return false;
    }

    ++g_ue51_rt_churn.ui_reused;
    ue51_log_rt_churn_summary_locked("ui_reused");

    return true;
}

void shf_log_texture_probe_candidate(
    const char* source,
    const char* base_name,
    uintptr_t base,
    uintptr_t offset,
    int32_t array_index,
    FRHITexture2D* texture,
    const D3D12_RESOURCE_DESC& desc)
{
    const auto key = ((uintptr_t)texture >> 4) ^
                     (base << 9) ^
                     (offset << 21) ^
                     ((uintptr_t)(array_index + 1) << 53);

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};

        if (g_shf_logged_texture_probe_keys.contains(key)) {
            return;
        }

        g_shf_logged_texture_probe_keys.insert(key);
    }

    if (array_index >= 0) {
        SPDLOG_WARN("[SHf] FSceneViewport probe {} {}+0x{:x}[{}] -> tex {:x} [{}x{} fmt={} flags=0x{:x}]",
            source, base_name, offset, array_index, (uintptr_t)texture, desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)desc.Flags);
    } else {
        SPDLOG_WARN("[SHf] FSceneViewport probe {} {}+0x{:x} -> tex {:x} [{}x{} fmt={} flags=0x{:x}]",
            source, base_name, offset, (uintptr_t)texture, desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)desc.Flags);
    }
}

void shf_probe_scene_viewport_memory(sdk::FViewport* viewport, const char* source, FRHITexture2D* known_texture) {
    if (viewport == nullptr || IsBadReadPtr(viewport, sizeof(void*))) {
        return;
    }

    void* required_vtable = nullptr;

    if (known_texture != nullptr && !IsBadReadPtr(known_texture, sizeof(void*))) {
        required_vtable = *(void**)known_texture;
    }

    if (required_vtable == nullptr) {
        required_vtable = FRHITexture2D::get_vtable();
    }

    if (required_vtable == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto viewport_base = (uintptr_t)viewport;

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};
        auto& last_probe = g_shf_last_texture_probe_by_base[viewport_base];

        if (last_probe.time_since_epoch().count() != 0 && now - last_probe < std::chrono::seconds(2)) {
            return;
        }

        last_probe = now;
    }

    auto probe_base = [&](uintptr_t base, const char* base_name) {
        if (base == 0 || IsBadReadPtr((void*)base, 0x340)) {
            return;
        }

        for (uintptr_t offset = 0; offset <= 0x330; offset += sizeof(void*)) try {
            const auto field = base + offset;

            if (IsBadReadPtr((void*)field, sizeof(void*))) {
                continue;
            }

            const auto texture = *(FRHITexture2D**)field;

            if (shf_is_valid_texture_with_vtable(texture, required_vtable)) {
                if (const auto desc = shf_try_get_d3d12_desc(texture)) {
                    shf_log_texture_probe_candidate(source, base_name, base, offset, -1, texture, *desc);
                }
            }

            if (IsBadReadPtr((void*)field, sizeof(void*) + sizeof(int32_t) * 2)) {
                continue;
            }

            const auto array_data = *(FRHITexture2D***)field;
            const auto array_count = *(int32_t*)(field + sizeof(void*));
            const auto array_capacity = *(int32_t*)(field + sizeof(void*) + sizeof(int32_t));

            if (array_data == nullptr || array_count <= 0 || array_count > 8 || array_capacity < array_count ||
                IsBadReadPtr(array_data, sizeof(FRHITexture2D*) * array_count))
            {
                continue;
            }

            for (int32_t i = 0; i < array_count; ++i) {
                const auto array_texture = array_data[i];

                if (!shf_is_valid_texture_with_vtable(array_texture, required_vtable)) {
                    continue;
                }

                if (const auto desc = shf_try_get_d3d12_desc(array_texture)) {
                    shf_log_texture_probe_candidate(source, base_name, base, offset, i, array_texture, *desc);
                }
            }
        } catch (...) {
        }
    };

    probe_base(viewport_base, "FViewport");

    if (viewport_base > 0x1000) {
        probe_base(viewport_base - sizeof(void*), "FSceneViewport");
    }
}

void shf_force_scene_viewport_separate_rt(const sdk::FViewport& viewport, const char* source) {
    if (!shf_is_current_game() || g_framework == nullptr || !g_framework->is_game_data_intialized()) {
        return;
    }

    const auto vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active() || vr->is_stereo_emulation_enabled() || vr->is_extreme_compatibility_mode_enabled()) {
        return;
    }

    constexpr uintptr_t fviewport_base_offset = 0x08;
    constexpr uintptr_t b_use_separate_rt_full_offset = 0x287;
    constexpr uintptr_t b_force_separate_rt_full_offset = 0x288;
    constexpr uintptr_t b_use_separate_rt_fviewport_offset = b_use_separate_rt_full_offset - fviewport_base_offset;
    constexpr uintptr_t b_force_separate_rt_fviewport_offset = b_force_separate_rt_full_offset - fviewport_base_offset;

    const auto viewport_base = reinterpret_cast<uintptr_t>(&viewport);

    if (viewport_base <= fviewport_base_offset ||
        IsBadReadPtr(reinterpret_cast<void*>(viewport_base - fviewport_base_offset), sizeof(void*)) ||
        IsBadReadPtr(reinterpret_cast<void*>(viewport_base + b_force_separate_rt_fviewport_offset), sizeof(uint8_t)))
    {
        return;
    }

    const auto fscene_viewport_vtable = *reinterpret_cast<uintptr_t*>(viewport_base - fviewport_base_offset);

    if (fscene_viewport_vtable == 0 || !utility::get_module_within(fscene_viewport_vtable).has_value()) {
        return;
    }

    auto* use_separate_rt = reinterpret_cast<uint8_t*>(viewport_base + b_use_separate_rt_fviewport_offset);
    auto* force_separate_rt = reinterpret_cast<uint8_t*>(viewport_base + b_force_separate_rt_fviewport_offset);

    if (*use_separate_rt > 1 || *force_separate_rt > 1) {
        SPDLOG_WARN_ONCE("[SHf] Refusing to force separate RT from {}; unexpected FSceneViewport bool bytes use={} force={}",
            source, *use_separate_rt, *force_separate_rt);
        return;
    }

    if (*use_separate_rt == 0 || *force_separate_rt == 0) {
        SPDLOG_WARN_ONCE("[SHf] Forcing FSceneViewport separate RT from {} at viewport {:x} use+0x{:x} force+0x{:x}",
            source, viewport_base, b_use_separate_rt_fviewport_offset, b_force_separate_rt_fviewport_offset);
    }

    *use_separate_rt = 1;
    *force_separate_rt = 1;
}

constexpr auto UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY = "ue57_prefer_slate_thread";
constexpr auto UE57_VIEW_EXTENSION_DISCOVERY_CACHE_KEY = "ue57_view_extension_discovery";

bool load_ue57_slate_thread_preference() {
    if (!is_ue_5_7_or_newer()) {
        return false;
    }

    if (const auto cached = sdk::discovery_cache::load_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY, utility::get_executable())) {
        return cached->value("prefer_slate_thread", false);
    }

    return false;
}

void save_ue57_slate_thread_preference(bool prefer) {
    if (!is_ue_5_7_or_newer()) {
        return;
    }

    if (prefer) {
        sdk::discovery_cache::save_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY, utility::get_executable(), {
            {"prefer_slate_thread", true}
        });
    } else {
        sdk::discovery_cache::invalidate_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY);
    }
}

enum class UE57RenderTargetLoadAction : uint32_t {
    NoAction = 0,
    Load = 1,
    Clear = 2,
};

struct UE57SlateDrawElementsPassInputsHead {
    FRDGTexture* stencil_texture;
    FRDGTexture* elements_texture;
    FRDGTexture* scene_viewport_texture;
    UE57RenderTargetLoadAction elements_load_action;
};

bool looks_like_ue57_slate_draw_elements_inputs(const UE57SlateDrawElementsPassInputsHead* inputs) {
    if (inputs == nullptr || !is_readable_process_range((uintptr_t)inputs, sizeof(UE57SlateDrawElementsPassInputsHead))) {
        return false;
    }

    const auto action = static_cast<uint32_t>(inputs->elements_load_action);

    if (action > static_cast<uint32_t>(UE57RenderTargetLoadAction::Clear)) {
        return false;
    }

    const auto scene_viewport_texture = inputs->scene_viewport_texture;
    const auto elements_texture = inputs->elements_texture;

    if (scene_viewport_texture == nullptr || elements_texture == nullptr) {
        return false;
    }

    if (!is_readable_process_range((uintptr_t)scene_viewport_texture, sizeof(void*)) ||
        !is_readable_process_range((uintptr_t)elements_texture, sizeof(void*))) {
        return false;
    }

    return true;
}

using RegisterExternalTextureFromRHIFn = FRDGTexture* (*)(FRDGBuilder&, FRHITexture*, const wchar_t*);

bool looks_like_nontrivial_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;
    size_t call_count = 0;
    bool saw_terminator = false;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 512; ) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded) {
            break;
        }

        decoded_bytes += decoded->Length;

        if (std::string_view{decoded->Mnemonic}.starts_with("CALL")) {
            ++call_count;
        }

        if (std::string_view{decoded->Mnemonic}.starts_with("RET") || std::string_view{decoded->Mnemonic}.starts_with("INT3")) {
            saw_terminator = true;
            break;
        }

        ip += decoded->Length;
    }

    return saw_terminator && decoded_bytes >= 64 && call_count >= 1;
}

bool looks_like_callable_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 256;) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        decoded_bytes += decoded->Length;

        const std::string_view mnemonic{decoded->Mnemonic};

        if (mnemonic.starts_with("RET")) {
            return decoded_bytes > 0;
        }

        if (mnemonic.starts_with("JMP")) {
            return decoded_bytes <= 16;
        }

        if (mnemonic.starts_with("INT3")) {
            return false;
        }

        ip += decoded->Length;
    }

    return false;
}

bool looks_like_post_init_properties_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;
    size_t call_count = 0;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 256;) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        decoded_bytes += decoded->Length;
        const std::string_view mnemonic{decoded->Mnemonic};

        if (mnemonic.starts_with("CALL")) {
            ++call_count;
        }

        if (mnemonic.starts_with("RET")) {
            return decoded_bytes > 8 || call_count > 0;
        }

        if (mnemonic.starts_with("JMP")) {
            return decoded_bytes > 8 || call_count > 0;
        }

        if (mnemonic.starts_with("INT3")) {
            return false;
        }

        ip += decoded->Length;
    }

    return call_count > 0;
}

std::optional<uint32_t> validate_source_informed_post_init_slot(
    uintptr_t* object_vtable,
    uintptr_t* localplayer_vtable,
    uint32_t slot,
    const char* source_note,
    bool require_inherited_uobject_slot,
    bool allow_localplayer_callable_thunk = false)
{
    if (IsBadReadPtr(&object_vtable[slot], sizeof(uintptr_t)) ||
        IsBadReadPtr(&localplayer_vtable[slot], sizeof(uintptr_t)))
    {
        SPDLOG_WARN("[PostInitProperties] {} slot {} is not readable", source_note, slot);
        return std::nullopt;
    }

    const auto object_fn = object_vtable[slot];
    const auto localplayer_fn = localplayer_vtable[slot];

    const auto localplayer_looks_valid = allow_localplayer_callable_thunk
        ? looks_like_callable_virtual(localplayer_fn)
        : looks_like_post_init_properties_virtual(localplayer_fn);

    if (!looks_like_post_init_properties_virtual(object_fn) || !localplayer_looks_valid)
    {
        SPDLOG_WARN("[PostInitProperties] {} slot {} did not look callable object_fn={:x} localplayer_fn={:x}",
            source_note,
            slot,
            object_fn,
            localplayer_fn);
        return std::nullopt;
    }

    if (require_inherited_uobject_slot && object_fn != localplayer_fn) {
        SPDLOG_WARN("[PostInitProperties] {} slot {} did not inherit UObject function object_fn={:x} localplayer_fn={:x}",
            source_note,
            slot,
            object_fn,
            localplayer_fn);
        return std::nullopt;
    }

    SPDLOG_INFO("[PostInitProperties] Resolved {} slot {} object_fn={:x} localplayer_fn={:x}",
        source_note,
        slot,
        object_fn,
        localplayer_fn);
    return slot;
}

std::optional<uint32_t> resolve_post_init_properties_index_from_uobject(uintptr_t localplayer) {
    auto* object_class = sdk::UObject::static_class();

    if (object_class == nullptr) {
        SPDLOG_WARN("[PostInitProperties] UObject::static_class() is not ready");
        return std::nullopt;
    }

    auto* object_cdo = object_class->get_class_default_object<sdk::UObject>();

    if (object_cdo == nullptr || IsBadReadPtr(object_cdo, sizeof(void*))) {
        SPDLOG_WARN("[PostInitProperties] UObject CDO is not ready");
        return std::nullopt;
    }

    const auto object_vtable = *(uintptr_t**)object_cdo;
    const auto localplayer_vtable = *(uintptr_t**)localplayer;

    if (object_vtable == nullptr || localplayer_vtable == nullptr ||
        IsBadReadPtr(object_vtable, sizeof(void*)) || IsBadReadPtr(localplayer_vtable, sizeof(void*)))
    {
        SPDLOG_WARN("[PostInitProperties] UObject or LocalPlayer vtable is invalid");
        return std::nullopt;
    }

    // UE5.1 source plus Stalker2/SOE PDBs place UObject::PostInitProperties at
    // slot 10 for shipped game layouts. Some UE5.1 games put a LocalPlayer
    // override/thunk at the same slot, so validate UObject strictly and only
    // require the LocalPlayer target to be callable.
    if (is_ue_5_1_dx_backend()) {
        constexpr uint32_t UE51_POST_INIT_PROPERTIES_SLOT = 10;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE51_POST_INIT_PROPERTIES_SLOT,
                "UE5.1 UObject::PostInitProperties",
                false,
                true))
        {
            return UE51_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[PostInitProperties] UE5.1 slot 10 did not validate; skipping LocalPlayer bootstrap");
        return std::nullopt;
    }

    // UE 5.4.4, 5.5.4 and 5.6.1 source/PDB put UObject::PostInitProperties at slot 10
    // for shipped game layouts:
    // UObjectBase has 4 virtuals, UObjectBaseUtility has 5, then UObject adds
    // GetDetailedInfoInternal at 9 and PostInitProperties at 10.
    if (is_ue_5_4_dx_backend() || is_ue_5_5_dx_backend() || is_ue_5_6_dx12_backend() || is_ue_5_7_or_newer()) {
        constexpr uint32_t UE54_PLUS_POST_INIT_PROPERTIES_SLOT = 10;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE54_PLUS_POST_INIT_PROPERTIES_SLOT,
                "UE5.4+ UObject::PostInitProperties",
                false,
                is_ue_5_4_dx_backend()))
        {
            return UE54_PLUS_POST_INIT_PROPERTIES_SLOT;
        }
    }

    // Keep the nearby slots as a fail-closed fallback for unusual/custom layouts.
    constexpr std::array<uint32_t, 4> candidate_slots{10, 9, 8, 11};

    for (const auto slot : candidate_slots) {
        if (IsBadReadPtr(&object_vtable[slot], sizeof(uintptr_t)) ||
            IsBadReadPtr(&localplayer_vtable[slot], sizeof(uintptr_t)))
        {
            continue;
        }

        const auto object_fn = object_vtable[slot];
        const auto localplayer_fn = localplayer_vtable[slot];

        if (object_fn == 0 || localplayer_fn == 0) {
            continue;
        }

        if (!looks_like_post_init_properties_virtual(object_fn) ||
            !looks_like_post_init_properties_virtual(localplayer_fn))
        {
            continue;
        }

        SPDLOG_INFO("[PostInitProperties] Resolved UObject::PostInitProperties through nearby fallback slot {} object_fn={:x} localplayer_fn={:x}",
            slot,
            object_fn,
            localplayer_fn);
        return slot;
    }

    SPDLOG_WARN("[PostInitProperties] Could not validate the expected UObject::PostInitProperties slots on this build");
    return std::nullopt;
}
}

namespace {
bool is_writable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool is_readable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_READONLY ||
           protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool is_executable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_EXECUTE ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool looks_like_virtual_function_table(uintptr_t table) {
    if (!is_readable_process_range(table, sizeof(uintptr_t) * 12)) {
        return false;
    }

    auto executable_entries = 0;

    for (auto i = 0; i < 12; ++i) {
        const auto fn = ((uintptr_t*)table)[i];

        if (fn == 0 || !is_executable_process_range(fn, 1)) {
            continue;
        }

        ++executable_entries;
    }

    return executable_entries >= 6;
}

template <typename T>
bool safe_read_value(uintptr_t address, T& out) {
    if (!is_readable_process_range(address, sizeof(T))) {
        return false;
    }

    memcpy(&out, (void*)address, sizeof(T));
    return true;
}

bool avowed_is_live_uobject(uintptr_t object, uintptr_t* out_vtable = nullptr, uintptr_t* out_class = nullptr) {
    if (!avowed_is_current_game() || object == 0) {
        return false;
    }

    uintptr_t vtable{};
    if (!safe_read_value(object, vtable) || !looks_like_virtual_function_table(vtable)) {
        return false;
    }

    uintptr_t cls{};
    if (!safe_read_value(object + sdk::UObjectBase::get_class_private_offset(), cls) || cls == 0) {
        return false;
    }

    uint32_t internal_index{};
    if (!safe_read_value(object + sdk::UObjectBase::get_internal_index_offset(), internal_index)) {
        return false;
    }

    auto object_array = sdk::FUObjectArray::get();
    if (object_array == nullptr) {
        return false;
    }

    const auto object_count = object_array->get_object_count();
    if (object_count <= 0 || internal_index >= (uint32_t)object_count) {
        return false;
    }

    auto item = object_array->get_object((int32_t)internal_index);
    if (item == nullptr || !is_readable_process_range((uintptr_t)item, sizeof(sdk::FUObjectItem))) {
        return false;
    }

    uintptr_t item_object{};
    if (!safe_read_value((uintptr_t)item + sdk::FUObjectArray::get_item_object_offset(), item_object) || item_object != object) {
        return false;
    }

    if (out_vtable != nullptr) {
        *out_vtable = vtable;
    }

    if (out_class != nullptr) {
        *out_class = cls;
    }

    return true;
}

std::optional<uintptr_t> locate_vtable_from_constructor_rip_references(uintptr_t constructor) {
    constexpr auto MAX_CONSTRUCTOR_SCAN_BYTES = 0x800;
    auto best_candidate = std::optional<uintptr_t>{};

    for (auto ip = constructor; ip < constructor + MAX_CONSTRUCTOR_SCAN_BYTES;) {
        const auto decoded = utility::decode_one((uint8_t*)ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        if (decoded->OperandsCount >= 2 &&
            decoded->IsRipRelative &&
            (decoded->Instruction == ND_INS_LEA || decoded->Instruction == ND_INS_MOV) &&
            decoded->Operands[0].Type == ND_OP_REG &&
            decoded->Operands[1].Type == ND_OP_MEM)
        {
            const auto referenced_addr = utility::resolve_displacement(ip);

            if (referenced_addr && looks_like_virtual_function_table(*referenced_addr)) {
                SPDLOG_INFO("Found FFakeStereoRendering vtable candidate via constructor RIP reference at {:x} -> {:x}",
                            ip, *referenced_addr);
                best_candidate = *referenced_addr;
                break;
            }
        }

        if (std::string_view{decoded->Mnemonic}.starts_with("RET")) {
            break;
        }

        ip += decoded->Length;
    }

    return best_candidate;
}
}

// Scan through function instructions to detect usage of double
// floating point precision instructions.
bool is_using_double_precision(uintptr_t addr) {
    SPDLOG_INFO("Scanning function at {:x} for double precision usage", addr);

    bool result = false;

    utility::exhaustive_decode((uint8_t*)addr, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
        if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
            return utility::ExhaustionResult::STEP_OVER;
        }

        if (ix.Instruction == ND_INS_MOVSD && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision MOVSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        if (ix.Instruction == ND_INS_ADDSD) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision ADDSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        return utility::ExhaustionResult::CONTINUE;
    });

    return result;
}

FFakeStereoRenderingHook::FFakeStereoRenderingHook() {
    g_hook = this;
    m_prefer_slate_thread_for_session = load_ue57_slate_thread_preference();
    setup_options();
}

void FFakeStereoRenderingHook::on_frame() {
    attempt_hook_game_engine_tick();
    attempt_hook_slate_thread();
    attempt_hook_fsceneview_constructor();
    attempt_hook_daysgone_slate_intermediate_buffer();
    attempt_hook_daysgone_bend_taa_composite();
    update_daysgone_ui_telemetry();
    update_daysgone_bend_ui_placement_fix();

    // Ideally we want to do all hooking
    // from game engine tick. if it fails
    // we will fall back to doing it here.
    if (!m_hooked_game_engine_tick && m_attempted_hook_game_engine_tick) {
        attempt_hooking();
    }
}


void FFakeStereoRenderingHook::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Stereo Hook Options")) {
        m_asynchronous_scan->draw("Asynchronous Code Scanning");
        m_recreate_textures_on_reset->draw("Recreate Textures on Reset");
        m_frame_delay_compensation->draw("Frame Delay Compensation");
        m_use_fmalloc_scene_view_extensions->draw("Use FMalloc for ISceneViewExtensions");
        m_safe_tick_hook->draw("Use Safe Tick Hooking");

        if (m_tracking_system_hook != nullptr) {
            m_tracking_system_hook->on_draw_ui();
        }

#if 0
        if (ImGui::Button("Spawn scene capture")) {
            get_render_target_manager()->create_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy scene capture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Create texture")) {
            get_render_target_manager()->create_scene_capture_texture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy texture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        bool status = false;

        if (get_render_target_manager()->get_scene_capture_utexture() != nullptr) {
            if (UObjectHook::get()->exists(get_render_target_manager()->get_scene_capture_utexture())) {
                status = true;
            }
        }
        ImGui::Text("Scene Capture Texture: %s", status ? "Exists" : "Does not exist");
#endif

        auto& data = m_viewport_rt_hook_data;
        std::scoped_lock _{data.retaddr_mutex};

        std::vector<uintptr_t> retaddrs{};
        std::vector<std::string> items{};
        for (auto& addr : data.seen_retaddrs) {
            items.push_back(fmt::format("{:x}", addr));
            retaddrs.push_back(addr);
        }
        
        std::vector<const char*> citems{};
        for (auto& item : items) {
            citems.push_back(item.c_str());
        }

        if (!items.empty()) {
            if (ImGui::BeginCombo("GetRenderTargetTexture Retaddrs", items[data.selected_retaddr].c_str())) {
                for (int n = 0; n < items.size(); n++) {
                    ImGui::PushID(n);
                    auto retaddr = retaddrs[n];
                    const bool is_selected = (data.selected_retaddr == n);

                    // Calculate the text size for the current item
                    const auto text_size = ImGui::CalcTextSize(items[n].c_str(), NULL, true);
                    const auto padding = ImGui::GetStyle().ItemSpacing.x;
                    const auto selectable_size = ImVec2{text_size.x + padding, text_size.y};

                    if (ImGui::Selectable(items[n].c_str(), is_selected, ImGuiSelectableFlags_None, selectable_size)) {
                        data.selected_retaddr = n;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Call Original")) {
                        data.call_original_retaddrs.insert(retaddr);
                        data.redirected_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Redirect")) {
                        data.redirected_retaddrs.insert(retaddr);
                        data.call_original_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (data.call_original_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Calling Original]");
                    } else if (data.redirected_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Redirected]");
                    } else {
                        ImGui::Text("[Default]");
                    }

                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

        }

        ImGui::TreePop();
    }

    if (daysgone_is_current_game()) {
        draw_daysgone_bend_ui_controls();
    }

    ImGui::Separator();
}

void FFakeStereoRenderingHook::draw_daysgone_bend_ui_controls() {
    if (!daysgone_is_current_game()) {
        ImGui::TextWrapped("Days Gone UI tuning is only available in DaysGone.exe.");
        return;
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::TreeNode("Days Gone Bend UI Placement")) {
        return;
    }

    auto vr = VR::get();
    const bool active = vr != nullptr && vr->is_daysgone_bend_ui_placement_fix_enabled();
    ImGui::TextWrapped("Status: %s | menu=%llx widget=%llx applies=%llu restores=%llu",
        active ? "enabled" : "disabled",
        (unsigned long long)m_daysgone_bend_ui_last_menu3d.load(),
        (unsigned long long)m_daysgone_bend_ui_last_widget_main.load(),
        (unsigned long long)m_daysgone_bend_ui_apply_count.load(),
        (unsigned long long)m_daysgone_bend_ui_restore_count.load());
    ImGui::TextWrapped("Use Root Viewport Slot controls for the visible MainMenu/HUD/subtitle roots.");
    ImGui::TextWrapped("Composite seen=%llu crop_suppressed=%llu extent_overrides=%llu shader_overrides=%llu",
        (unsigned long long)m_daysgone_bend_taa_composite_seen.load(),
        (unsigned long long)m_daysgone_bend_taa_composite_crop_suppressed.load(),
        (unsigned long long)m_daysgone_bend_taa_composite_extent_overrides.load(),
        (unsigned long long)m_daysgone_bend_taa_shader_param_overrides.load());
    ImGui::TextWrapped("Captured Slate UI native=%llx size=%ux%u",
        (unsigned long long)m_daysgone_slate_native_ui_target.load(),
        m_daysgone_slate_native_ui_width.load(),
        m_daysgone_slate_native_ui_height.load());

    if (ImGui::Button("Recommended Stable UMG Mode")) {
        m_daysgone_bend_ui_mode->value() = 2;
        m_daysgone_bend_ui_force_player_camera->value() = false;
        m_daysgone_bend_ui_override_widget_transform->value() = true;
        m_daysgone_bend_ui_override_root_transform->value() = false;
        m_daysgone_bend_ui_force_widget_refresh->value() = true;
        m_daysgone_bend_ui_viewport_slot_fix->value() = true;
        m_daysgone_bend_ui_live_watchdog->value() = false;
        m_daysgone_bend_ui_apply_child_render_transform->value() = false;
        m_daysgone_bend_ui_use_slate_overlay->value() = false;
        m_daysgone_bend_ui_suppress_in_scene_composite->value() = false;
        m_daysgone_bend_ui_split_overlay->value() = false;
        m_daysgone_bend_ui_disable_taa_crop->value() = true;
        m_daysgone_bend_ui_viewport_slot_offset_x->value() = -240.0f;
        m_daysgone_bend_ui_viewport_slot_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_viewport_slot_scale->value() = 0.85f;
        m_daysgone_bend_ui_viewport_slot_opacity->value() = 1.0f;
        m_daysgone_bend_ui_distance_from_camera->value() = -1371.022f;
        m_daysgone_bend_ui_camera_fov->value() = 70.0f;
        m_daysgone_bend_ui_widget_loc_x->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_y->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_z->value() = -1371.022f;
        m_daysgone_bend_ui_widget_rot_pitch->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_yaw->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_roll->value() = 0.0f;
        m_daysgone_bend_ui_widget_scale->value() = 1.0f;
        m_daysgone_bend_ui_screen_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_screen_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_screen_scale->value() = 1.0f;
        m_daysgone_bend_ui_draw_scale->value() = 1.0f;
        m_daysgone_bend_ui_key_opacity->value() = 0.0f;
        m_daysgone_bend_ui_manual_apply_generation.fetch_add(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Extracted Overlay Diagnostic Mode")) {
        m_daysgone_bend_ui_use_slate_overlay->value() = true;
        m_daysgone_bend_ui_suppress_in_scene_composite->value() = false;
        m_daysgone_bend_ui_split_overlay->value() = true;
        m_daysgone_bend_ui_key_threshold->value() = 0.025f;
        m_daysgone_bend_ui_key_softness->value() = 0.045f;
        m_daysgone_bend_ui_key_opacity->value() = 1.0f;
        m_daysgone_bend_ui_screen_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_screen_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_screen_scale->value() = 1.0f;
        m_daysgone_bend_ui_draw_scale->value() = 1.0f;
    }
    ImGui::TextWrapped("Stable UMG mode is the current useful path. The extracted overlay is diagnostic; only enable suppression if you want to hide the live game UI and use the copied overlay only.");

    ImGui::SeparatorText("Extracted Slate UI Overlay");
    m_daysgone_bend_ui_use_slate_overlay->draw("Use Extracted Slate UI Overlay");
    ImGui::TextWrapped("Copies the captured Bend SlateIntermediateBuffer into UEVR's OpenXR UI layer. This keeps the scene in the normal synced/native path and does not force global 2D mode.");
    m_daysgone_bend_ui_suppress_in_scene_composite->draw("Suppress Bend In-Scene Slate Composite");
    ImGui::TextWrapped("Leave suppression off first. Enable it only if the extracted overlay works but the original glued/duplicated game UI still remains visible.");
    m_daysgone_bend_ui_split_overlay->draw("Split Menu/Footer Extracted Overlay");
    ImGui::TextWrapped("Draws the footer/bottom band and the upper-right menu as separate crops. This lets the main menu move/scale independently from the footer.");
    m_daysgone_bend_ui_key_threshold->draw_drag("Overlay Key Threshold", 0.001f, "%.3f");
    m_daysgone_bend_ui_key_softness->draw_drag("Overlay Key Softness", 0.001f, "%.3f");
    m_daysgone_bend_ui_key_opacity->draw_drag("Overlay Opacity", 0.01f, "%.3f");
    m_daysgone_bend_ui_screen_offset_x->draw_drag("Overlay Offset X", 1.0f, "%.1f");
    m_daysgone_bend_ui_screen_offset_y->draw_drag("Overlay Offset Y", 1.0f, "%.1f");
    m_daysgone_bend_ui_screen_scale->draw_drag("Overlay Scale", 0.01f, "%.3f");
    m_daysgone_bend_ui_draw_scale->draw_drag("Overlay Fine Scale", 0.01f, "%.3f");
    m_daysgone_bend_ui_menu_offset_x->draw_drag("Upper Menu Offset X", 1.0f, "%.1f");
    m_daysgone_bend_ui_menu_offset_y->draw_drag("Upper Menu Offset Y", 1.0f, "%.1f");
    m_daysgone_bend_ui_menu_scale->draw_drag("Upper Menu Scale", 0.01f, "%.3f");

    if (ImGui::TreeNode("Extracted overlay crop tuning")) {
        m_daysgone_bend_ui_menu_src_x->draw_drag("Upper Menu Source X", 0.001f, "%.3f");
        m_daysgone_bend_ui_menu_src_y->draw_drag("Upper Menu Source Y", 0.001f, "%.3f");
        m_daysgone_bend_ui_menu_src_w->draw_drag("Upper Menu Source W", 0.001f, "%.3f");
        m_daysgone_bend_ui_menu_src_h->draw_drag("Upper Menu Source H", 0.001f, "%.3f");
        m_daysgone_bend_ui_footer_src_y->draw_drag("Footer Source Y", 0.001f, "%.3f");
        m_daysgone_bend_ui_footer_src_h->draw_drag("Footer Source H", 0.001f, "%.3f");
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Active SlateHUD / UMG Root Viewport Slot");
    m_daysgone_bend_ui_viewport_slot_fix->draw("Root UUserWidget Viewport Slot Fix");
    m_daysgone_bend_ui_viewport_slot_offset_x->draw_drag("Root Slot Offset X", 1.0f, "%.1f");
    m_daysgone_bend_ui_viewport_slot_offset_y->draw_drag("Root Slot Offset Y", 1.0f, "%.1f");
    m_daysgone_bend_ui_viewport_slot_scale->draw_drag("Root Render Scale", 0.01f, "%.3f");
    m_daysgone_bend_ui_viewport_slot_opacity->draw_drag("Root Opacity", 0.01f, "%.3f");
    if (ImGui::Button("Apply Current UI Tuning Once")) {
        m_daysgone_bend_ui_manual_apply_generation.fetch_add(1);
    }
    ImGui::SameLine();
    m_daysgone_bend_ui_live_watchdog->draw("Live Reapply Watchdog");
    ImGui::TextWrapped("Targets UI_MainMenuWidget, OptionsMenuWidget, OptionsTopMenuWidget, UI_HudWidget, UI_SubtitleWidget, and UI_MegaMenu roots. The slot stays 1920x1080; scale is applied as a render transform to avoid clipping/cropping.");
    m_daysgone_bend_ui_disable_taa_crop->draw("Disable Bend TAA Slate Crop");
    ImGui::TextWrapped("Only active when the extracted Slate overlay is enabled and the in-scene composite is suppressed. This avoids touching Bend TAA crops during normal gameplay.");
    ImGui::TextWrapped("This now applies only when settings change or when Apply Current UI Tuning Once is pressed. The watchdog is off by default; only enable it if the game recreates the menu widgets and you need periodic reapplication.");

    if (ImGui::TreeNode("Advanced legacy BP_Menu3D/widget controls")) {
        int mode = std::clamp(m_daysgone_bend_ui_mode->value(), 0, 2);
        constexpr const char* kModeItems = "Player-camera fields only\0Widget transform only\0Player-camera + widget transform\0";
        if (ImGui::Combo("Placement Mode", &mode, kModeItems)) {
            m_daysgone_bend_ui_mode->value() = mode;
        }

        m_daysgone_bend_ui_force_player_camera->draw("Force BP_Menu3D UsePlayerCamera");
        m_daysgone_bend_ui_distance_from_camera->draw_drag("Distance From Camera", 5.0f, "%.1f");
        m_daysgone_bend_ui_camera_fov->draw_drag("Camera FOV", 0.25f, "%.2f");

        ImGui::SeparatorText("BendWidgetMain");
        m_daysgone_bend_ui_override_widget_transform->draw("Override Widget Transform");
        m_daysgone_bend_ui_widget_loc_x->draw_drag("Widget Loc X", 5.0f, "%.1f");
        m_daysgone_bend_ui_widget_loc_y->draw_drag("Widget Loc Y", 5.0f, "%.1f");
        m_daysgone_bend_ui_widget_loc_z->draw_drag("Widget Loc Z", 5.0f, "%.1f");
        m_daysgone_bend_ui_widget_rot_pitch->draw_drag("Widget Rot Pitch", 0.25f, "%.2f");
        m_daysgone_bend_ui_widget_rot_yaw->draw_drag("Widget Rot Yaw", 0.25f, "%.2f");
        m_daysgone_bend_ui_widget_rot_roll->draw_drag("Widget Rot Roll", 0.25f, "%.2f");
        m_daysgone_bend_ui_widget_scale->draw_drag("Widget Uniform Scale", 0.01f, "%.3f");

        ImGui::SeparatorText("Child Render Transform");
        m_daysgone_bend_ui_apply_child_render_transform->draw("Experimental Child Render Transform");
        m_daysgone_bend_ui_screen_offset_x->draw_drag("Child Render Offset X", 1.0f, "%.1f");
        m_daysgone_bend_ui_screen_offset_y->draw_drag("Child Render Offset Y", 1.0f, "%.1f");
        m_daysgone_bend_ui_screen_scale->draw_drag("Child Render Scale", 0.01f, "%.3f");
        m_daysgone_bend_ui_draw_scale->draw_drag("Child Draw Scale", 0.01f, "%.3f");
        m_daysgone_bend_ui_force_widget_refresh->draw("Force Redraw / Offscreen Safe Flags");

        ImGui::SeparatorText("DefaultSceneRoot1");
        m_daysgone_bend_ui_override_root_transform->draw("Override Root Location");
        m_daysgone_bend_ui_root_loc_x->draw_drag("Root Loc X", 5.0f, "%.1f");
        m_daysgone_bend_ui_root_loc_y->draw_drag("Root Loc Y", 5.0f, "%.1f");
        m_daysgone_bend_ui_root_loc_z->draw_drag("Root Loc Z", 5.0f, "%.1f");
        ImGui::TreePop();
    }

    if (ImGui::Button("Observed Defaults")) {
        m_daysgone_bend_ui_mode->value() = 2;
        m_daysgone_bend_ui_force_player_camera->value() = false;
        m_daysgone_bend_ui_override_widget_transform->value() = true;
        m_daysgone_bend_ui_override_root_transform->value() = false;
        m_daysgone_bend_ui_use_slate_overlay->value() = false;
        m_daysgone_bend_ui_suppress_in_scene_composite->value() = false;
        m_daysgone_bend_ui_split_overlay->value() = true;
        m_daysgone_bend_ui_menu_src_x->value() = 0.52f;
        m_daysgone_bend_ui_menu_src_y->value() = 0.0f;
        m_daysgone_bend_ui_menu_src_w->value() = 0.48f;
        m_daysgone_bend_ui_menu_src_h->value() = 0.48f;
        m_daysgone_bend_ui_menu_offset_x->value() = -450.0f;
        m_daysgone_bend_ui_menu_offset_y->value() = -650.0f;
        m_daysgone_bend_ui_menu_scale->value() = 1.0f;
        m_daysgone_bend_ui_footer_src_y->value() = 0.68f;
        m_daysgone_bend_ui_footer_src_h->value() = 0.32f;
        m_daysgone_bend_ui_key_threshold->value() = 0.025f;
        m_daysgone_bend_ui_key_softness->value() = 0.045f;
        m_daysgone_bend_ui_key_opacity->value() = 1.0f;
        m_daysgone_bend_ui_disable_taa_crop->value() = true;
        m_daysgone_bend_ui_distance_from_camera->value() = -1371.022f;
        m_daysgone_bend_ui_camera_fov->value() = 70.0f;
        m_daysgone_bend_ui_widget_loc_x->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_y->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_z->value() = -1371.022f;
        m_daysgone_bend_ui_widget_rot_pitch->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_yaw->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_roll->value() = 0.0f;
        m_daysgone_bend_ui_widget_scale->value() = 1.0f;
        m_daysgone_bend_ui_viewport_slot_fix->value() = true;
        m_daysgone_bend_ui_live_watchdog->value() = false;
        m_daysgone_bend_ui_apply_child_render_transform->value() = false;
        m_daysgone_bend_ui_viewport_slot_offset_x->value() = -240.0f;
        m_daysgone_bend_ui_viewport_slot_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_viewport_slot_scale->value() = 0.85f;
        m_daysgone_bend_ui_viewport_slot_opacity->value() = 1.0f;
        m_daysgone_bend_ui_screen_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_screen_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_screen_scale->value() = 1.0f;
        m_daysgone_bend_ui_draw_scale->value() = 1.0f;
        m_daysgone_bend_ui_override_composite_extent->value() = false;
        m_daysgone_bend_ui_composite_width->value() = 1920.0f;
        m_daysgone_bend_ui_composite_height->value() = 1080.0f;
        m_daysgone_bend_ui_override_shader_params->value() = false;
        m_daysgone_bend_ui_shader_param_target->value() = 3;
        m_daysgone_bend_ui_shader_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_shader_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_shader_scale_x->value() = 1.0f;
        m_daysgone_bend_ui_shader_scale_y->value() = 1.0f;
        m_daysgone_bend_ui_root_loc_x->value() = 0.0f;
        m_daysgone_bend_ui_root_loc_y->value() = 0.0f;
        m_daysgone_bend_ui_root_loc_z->value() = -1200.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Recenter Test Preset")) {
        m_daysgone_bend_ui_mode->value() = 2;
        m_daysgone_bend_ui_force_player_camera->value() = true;
        m_daysgone_bend_ui_override_widget_transform->value() = true;
        m_daysgone_bend_ui_use_slate_overlay->value() = false;
        m_daysgone_bend_ui_suppress_in_scene_composite->value() = false;
        m_daysgone_bend_ui_split_overlay->value() = true;
        m_daysgone_bend_ui_menu_src_x->value() = 0.52f;
        m_daysgone_bend_ui_menu_src_y->value() = 0.0f;
        m_daysgone_bend_ui_menu_src_w->value() = 0.48f;
        m_daysgone_bend_ui_menu_src_h->value() = 0.48f;
        m_daysgone_bend_ui_menu_offset_x->value() = -450.0f;
        m_daysgone_bend_ui_menu_offset_y->value() = -650.0f;
        m_daysgone_bend_ui_menu_scale->value() = 1.0f;
        m_daysgone_bend_ui_footer_src_y->value() = 0.68f;
        m_daysgone_bend_ui_footer_src_h->value() = 0.32f;
        m_daysgone_bend_ui_key_threshold->value() = 0.025f;
        m_daysgone_bend_ui_key_softness->value() = 0.045f;
        m_daysgone_bend_ui_key_opacity->value() = 1.0f;
        m_daysgone_bend_ui_disable_taa_crop->value() = true;
        m_daysgone_bend_ui_distance_from_camera->value() = -1200.0f;
        m_daysgone_bend_ui_camera_fov->value() = 70.0f;
        m_daysgone_bend_ui_widget_loc_x->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_y->value() = 0.0f;
        m_daysgone_bend_ui_widget_loc_z->value() = -1200.0f;
        m_daysgone_bend_ui_widget_rot_pitch->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_yaw->value() = 90.0f;
        m_daysgone_bend_ui_widget_rot_roll->value() = 0.0f;
        m_daysgone_bend_ui_widget_scale->value() = 1.0f;
        m_daysgone_bend_ui_viewport_slot_fix->value() = true;
        m_daysgone_bend_ui_live_watchdog->value() = false;
        m_daysgone_bend_ui_apply_child_render_transform->value() = false;
        m_daysgone_bend_ui_viewport_slot_offset_x->value() = -240.0f;
        m_daysgone_bend_ui_viewport_slot_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_viewport_slot_scale->value() = 0.85f;
        m_daysgone_bend_ui_viewport_slot_opacity->value() = 1.0f;
        m_daysgone_bend_ui_screen_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_screen_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_screen_scale->value() = 1.0f;
        m_daysgone_bend_ui_draw_scale->value() = 1.0f;
        m_daysgone_bend_ui_override_composite_extent->value() = false;
        m_daysgone_bend_ui_composite_width->value() = 1920.0f;
        m_daysgone_bend_ui_composite_height->value() = 1080.0f;
        m_daysgone_bend_ui_override_shader_params->value() = false;
        m_daysgone_bend_ui_shader_param_target->value() = 3;
        m_daysgone_bend_ui_shader_offset_x->value() = 0.0f;
        m_daysgone_bend_ui_shader_offset_y->value() = 0.0f;
        m_daysgone_bend_ui_shader_scale_x->value() = 1.0f;
        m_daysgone_bend_ui_shader_scale_y->value() = 1.0f;
    }

    ImGui::TreePop();
}

bool FFakeStereoRenderingHook::invalidate_ue57_resolution_dependent_state(
    uint32_t old_width,
    uint32_t old_height,
    uint32_t new_width,
    uint32_t new_height) {
    if (!is_ue_5_7_or_newer()) {
        return false;
    }

    SPDLOG_INFO(
        "[UE5.7][OpenXR] Resolution changed [{}x{}]->[{}x{}]; invalidating Slate/UI render targets",
        old_width,
        old_height,
        new_width,
        new_height);

    m_wants_texture_recreation = true;
    m_skip_next_adjust_view_rect = true;

    // Force the UE5.7 Slate/UI path to observe fresh post-resize DrawWindow and
    // PreRenderViewFamily traffic instead of reusing candidates captured at the
    // previous OpenXR scale.
    m_has_seen_stable_slate_draw = false;
    m_has_seen_prerender_viewfamily = false;
    m_first_stable_slate_draw_at = {};

    m_rtm.invalidate_resolution_dependent_targets();
    m_rtm_418.invalidate_resolution_dependent_targets();
    m_rtm_special.invalidate_resolution_dependent_targets();

    return true;
}

void FFakeStereoRenderingHook::attempt_hooking() {
    if (m_finished_hooking || m_tried_hooking) {
        return;
    }

    // TODO: see if this can be threaded; it might not be able to because of TLS or something
    if (!VR::get()->should_skip_uobjectarray_init()) {
        sdk::FName::get_constructor();
        sdk::FName::get_to_string();
        sdk::FUObjectArray::get();
    }

    if (!m_injected_stereo_at_runtime) {
        attempt_runtime_inject_stereo();
        m_injected_stereo_at_runtime = true;
    }
    
    m_hooked = hook();
}

namespace detail{
bool pre_find_engine_tick() {
    ZoneScopedN(__FUNCTION__);
    sdk::UGameEngine::get_tick_address(); // this takes a LONG time to find
    sdk::UGameEngine::get_initialize_hmd_device_address();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_game_engine_tick(uintptr_t return_address) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_engine_tick);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_game_engine_tick) {
        return;
    }

    if (return_address == 0 && m_attempted_hook_game_engine_tick) {
        return;
    }
    
    SPDLOG_INFO("Attempting to hook UGameEngine::Tick!");

    m_attempted_hook_game_engine_tick = true;

    auto func = sdk::UGameEngine::get_tick_address();

    if (!func) {
        if (return_address == 0) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick");
            return;
        }

        const auto engine_module = sdk::get_ue_module(L"Engine");
        static const auto negative_delta_time_strings = 
            utility::scan_strings(engine_module, L"Negative delta time!");
        
        if (negative_delta_time_strings.empty()) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick (Negative delta time! not found)");
            return;
        }

        static std::vector<uintptr_t> negative_delta_time_funcs = [&]() {
            std::vector<uintptr_t> out{};

            for (auto str : negative_delta_time_strings) {
                const auto ref = utility::scan_displacement_reference(engine_module, str);

                if (!ref) {
                    continue;
                }
                //
                const auto func_start = utility::find_virtual_function_start(*ref);

                if (!func_start) {
                    continue;
                }

                SPDLOG_INFO("Negative delta time string function @ {:x}", *func_start);

                out.push_back(*func_start);
            }

            return out;
        }();

        const auto return_address_func = utility::find_virtual_function_start(return_address);

        if (!return_address_func) {
            SPDLOG_ERROR("Return address is not within a valid function!");
            return;
        }

        // Check if the return address is within one of the negative delta time functions.
        // If it is, then it's UGameEngine::Tick. Set func to the return_address_func.
        for (auto potential : negative_delta_time_funcs) {
            if (potential == *return_address_func) {
                SPDLOG_INFO("Found UGameEngine::Tick @ {:x}", *return_address_func);
                func = *return_address_func;
                break;
            }
        }

        if (!func) {
            SPDLOG_ERROR("Return address is not the correct function!");
            return;
        }
    }

    // TODO: move this to a better place
    m_tick_hook = safetyhook::create_inline((void*)*func, &engine_tick_hook, safetyhook::InlineHook::StartDisabled);

    if (!m_tick_hook) {
        SPDLOG_ERROR("Failed to hook UGameEngine::Tick!");
        return;
    }

    if (auto tick_hook_enable = m_tick_hook.enable(); !tick_hook_enable.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameEngine::Tick hook! {}", (int)tick_hook_enable.error().type);
        return;
    }

    m_hooked_game_engine_tick = true;

    SPDLOG_INFO("Hooked UGameEngine::Tick!");
}

void* FFakeStereoRenderingHook::engine_tick_hook(sdk::UGameEngine* engine, float delta, bool idle) {
    ZoneScopedN("UGameEngine::Tick Hook");
    FrameMarkStart("UGameEngine::Tick");

    sdk::UEngine::set_runtime_engine(engine);

    auto hook = g_hook;
    
    hook->m_in_engine_tick = true;

    utility::ScopeGuard _{[]() {
        g_hook->m_in_engine_tick = false;
        FrameMarkEnd("UGameEngine::Tick");
    }};
    
    static bool once = true;

    if (once) {
        SPDLOG_INFO("First time calling UGameEngine::Tick!");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        if (hook->m_safe_tick_hook->value()) {
            return hook->m_tick_hook.call<void*>(engine, delta, idle);
        }

        // This allocates memory on the stack.
        static bool check_canary_once = true;
        volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
        if (check_canary_once) {
#endif
            std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
        }
#endif
        // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
        void* result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

        // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
        // But only do it once in release builds.
#ifdef NDEBUG
        if (check_canary_once) {
#endif
            for (size_t i = 0; i < 64; ++i) {
                if (shadow_space[i] != 0) {
                    SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                }
            }

#ifdef NDEBUG
            check_canary_once = false;
        }
#endif

        return result;
    }

    hook->attempt_hooking();

    // Best place to run game thread jobs.
    GameThreadWorker::get().execute();

    if (hook->m_ignore_next_engine_tick) {
        hook->m_ignored_engine_delta = delta;
        hook->m_ignore_next_engine_tick = false;
        return nullptr;
    }
    
    g_framework->enable_engine_thread();
    g_framework->run_imgui_frame(false);

    delta += hook->m_ignored_engine_delta;
    hook->m_ignored_engine_delta = 0.0f;

    if (hook->m_tracking_system_hook != nullptr) {
        hook->m_tracking_system_hook->on_pre_engine_tick(engine, delta);
    }

    const auto& mods = g_framework->get_mods()->get_mods();
    for (auto& mod : mods) {
        mod->on_pre_engine_tick(engine, delta);
    }

    void* result = nullptr;

    {
        if (hook->m_safe_tick_hook->value()) {
            result = hook->m_tick_hook.call<void*>(engine, delta, idle);
        } else {
            // This allocates memory on the stack.
            static bool check_canary_once = true;
            volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
            if (check_canary_once) {
#endif
                std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
            }
#endif
            // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
            result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

            // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
            // But only do it once in release builds.
#ifdef NDEBUG
            if (check_canary_once) {
#endif
                for (size_t i = 0; i < 64; ++i) {
                    if (shadow_space[i] != 0) {
                        SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                    }
                }

#ifdef NDEBUG
                check_canary_once = false;
            }
#endif
        }
    }

    for (auto& mod : mods) {
        mod->on_post_engine_tick(engine, delta);
    }

    if (hook->m_tracking_system_hook != nullptr) {
        hook->m_tracking_system_hook->on_post_engine_tick(engine, delta);
    }

    return result;
}

namespace detail{
bool pre_find_slate_thread() {
    sdk::slate::locate_draw_window_renderthread_fn(); // Can take a while to find
    sdk::slate::locate_draw_window_renderthread_fn_alternate();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_slate_thread(uintptr_t return_address, bool alternate) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_slate_thread);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_slate_thread && !alternate) {
        return;
    }

    const auto attempted = alternate ? m_attempted_hook_slate_thread_alternate : m_attempted_hook_slate_thread;

    if (return_address == 0 && attempted) {
        return;
    }

    SPDLOG_INFO("Attempting to hook FSlateRHIRenderer::DrawWindow_RenderThread!");

    if (alternate) {
        SPDLOG_INFO("Using alternate method to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        m_attempted_hook_slate_thread_alternate = true;
    } else {
        m_attempted_hook_slate_thread = true;
    }

    auto func = alternate ? sdk::slate::locate_draw_window_renderthread_fn_alternate() : sdk::slate::locate_draw_window_renderthread_fn();

    if (!func && !alternate) {
        func = sdk::slate::locate_draw_window_renderthread_fn_alternate();

        if (func) {
            SPDLOG_INFO("Using alternate SlateRHIRenderer::DrawWindow_RenderThread scan result after primary scan failed");
            m_attempted_hook_slate_thread_alternate = true;
        }
    }

    if (!func && return_address == 0) {
        SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread");
        return;
    }

    if (return_address != 0) {
        func = utility::find_function_start_with_call(return_address);

        if (!func) {
            SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method");
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Checking if the assembly listing for {:X} is really small", *func);

        // Check if the assembly listing for this function is really small. It shouldn't be really small.
        // This will happen on UE 5.5+ where RenderTexture_RenderThread is enqueued inside of a lambda.
        size_t distance_to_ret = 0;
        utility::exhaustive_decode((uint8_t*)*func, 1000, [&](utility::ExhaustionContext& ctx2) -> utility::ExhaustionResult {
            ++distance_to_ret;

            if (ctx2.instrux.BranchInfo.IsBranch && std::string_view{ctx2.instrux.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (distance_to_ret < 50) {
            SPDLOG_ERROR("FSlateRHIRenderer::DrawWindow_RenderThread function is too small! Distance to RET: {}", distance_to_ret);
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Found FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method: {:x}", *func);
    }

    m_slate_thread_hook = safetyhook::create_inline((void*)*func, &FFakeStereoRenderingHook::slate_draw_window_render_thread, safetyhook::InlineHook::StartDisabled);
    m_hooked_slate_thread = true;

    if (!m_slate_thread_hook) {
        SPDLOG_ERROR("Failed to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        return;
    }

    if (auto enable_result = m_slate_thread_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSlateRHIRenderer::DrawWindow_RenderThread hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSlateRHIRenderer::DrawWindow_RenderThread @ 0x{:x}!", *func);

    // UE5.7 elements-pass inspection is a fallback only. Let the DrawWindow path
    // try the dedicated UI target first before probing extra callsites.
}

void FFakeStereoRenderingHook::attempt_hook_ue57_slate_elements_pass() {
    if (!is_ue_5_7_or_newer() || !g_framework->is_dx12()) {
        return;
    }

    if (m_attempted_hook_ue57_slate_elements_pass) {
        return;
    }

    if (const auto rtm = get_render_target_manager(); rtm != nullptr && rtm->has_dedicated_ui_target()) {
        SPDLOG_INFO_ONCE("Skipping UE 5.7 Slate elements-pass fallback because the dedicated UI target is active");
        return;
    }

    m_attempted_hook_ue57_slate_elements_pass = true;

    const auto draw_window = g_hook->m_slate_thread_hook.target_address();

    if (draw_window == 0) {
        SPDLOG_ERROR("Cannot scan UE 5.7 DrawWindow_RenderThread for Slate callsites because the Slate hook has no target address");
        return;
    }

    const auto module_within = utility::get_module_within(draw_window);

    if (!module_within.has_value()) {
        SPDLOG_ERROR("Cannot scan UE 5.7 DrawWindow_RenderThread for Slate callsites because the module was not resolved");
        return;
    }

    const auto& slate_symbols = vrmod::get_ue57_slate_symbols();

    if (slate_symbols.add_slate_draw_elements_pass != 0 &&
        is_executable_process_range(slate_symbols.add_slate_draw_elements_pass, 1)) {
        const auto symbol_module = utility::get_module_within(reinterpret_cast<void*>(slate_symbols.add_slate_draw_elements_pass));

        if (symbol_module.has_value()) {
            auto hook_result = safetyhook::create_mid(
                reinterpret_cast<void*>(slate_symbols.add_slate_draw_elements_pass),
                &FFakeStereoRenderingHook::ue57_add_slate_draw_elements_pass_hook);

            if (hook_result) {
                m_ue57_slate_elements_hooks.emplace_back(std::move(hook_result));
                m_hooked_ue57_slate_elements_pass = true;
                SPDLOG_INFO("Hooked UE 5.7 AddSlateDrawElementsPass by symbol at {:x}", slate_symbols.add_slate_draw_elements_pass);
                return;
            }

            SPDLOG_WARN("Failed to hook UE 5.7 AddSlateDrawElementsPass symbol at {:x}", slate_symbols.add_slate_draw_elements_pass);
        } else {
            SPDLOG_INFO("Ignoring UE 5.7 AddSlateDrawElementsPass symbol {:x} because its module could not be resolved",
                slate_symbols.add_slate_draw_elements_pass);
        }
    }

    struct DirectCall {
        uintptr_t callsite{};
        uintptr_t target{};
        uint8_t length{};
    };

    std::vector<DirectCall> direct_calls{};

    utility::exhaustive_decode((uint8_t*)draw_window, 0x1200, [&](utility::ExhaustionContext& ctx) -> utility::ExhaustionResult {
        const auto mnemonic = std::string_view{ctx.instrux.Mnemonic};

        if (!mnemonic.starts_with("CALL")) {
            return utility::ExhaustionResult::CONTINUE;
        }

        const auto target = utility::resolve_displacement(ctx.addr);

        if (!target.has_value()) {
            return utility::ExhaustionResult::CONTINUE;
        }

        const auto called_module = utility::get_module_within((void*)*target);

        if (!called_module.has_value() || *called_module != *module_within) {
            return utility::ExhaustionResult::CONTINUE;
        }

        direct_calls.push_back({ctx.addr, *target, (uint8_t)ctx.instrux.Length});
        return utility::ExhaustionResult::CONTINUE;
    });

    std::unordered_map<uintptr_t, std::vector<DirectCall>> grouped_calls{};

    for (const auto& call : direct_calls) {
        grouped_calls[call.target].push_back(call);
    }

    std::vector<DirectCall> candidate_calls{};
    std::unordered_set<uintptr_t> candidate_calls_seen{};

    for (const auto& [target, calls] : grouped_calls) {
        if (calls.size() >= 2) {
            for (const auto& call : calls) {
                if (candidate_calls_seen.insert(call.callsite).second) {
                    candidate_calls.push_back(call);
                }
            }
        }
    }

    // UE5.7.3 may only emit one direct call to AddSlateDrawElementsPass in optimized shipping builds.
    // Hook a capped set of single-call candidates and let the hook validate the r8 input shape at runtime.
    for (const auto& call : direct_calls) {
        if (candidate_calls_seen.insert(call.callsite).second) {
            candidate_calls.push_back(call);
        }
    }

    if (candidate_calls.empty()) {
        const auto rtm = get_render_target_manager();

        if (rtm != nullptr && rtm->has_dedicated_ui_target()) {
            SPDLOG_INFO("No direct-call candidates inside UE 5.7 DrawWindow_RenderThread; dedicated UI target is already active");
        } else {
            SPDLOG_WARN("Could not find direct-call candidates inside UE 5.7 DrawWindow_RenderThread for ElementsTexture inspection");
        }

        return;
    }

    size_t hooked_count{};
    constexpr size_t max_ue57_slate_callsite_hooks = 32;

    for (const auto& call : candidate_calls) {
        if (hooked_count >= max_ue57_slate_callsite_hooks) {
            break;
        }

        auto hook_result = safetyhook::create_mid(
            reinterpret_cast<void*>(call.callsite),
            &FFakeStereoRenderingHook::ue57_add_slate_draw_elements_pass_hook);

        if (!hook_result) {
            SPDLOG_WARN("Failed to hook UE 5.7 DrawWindow_RenderThread callsite {:x} -> {:x}", call.callsite, call.target);
            continue;
        }

        m_ue57_slate_elements_hooks.emplace_back(std::move(hook_result));
        ++hooked_count;
    }

    if (hooked_count == 0) {
        SPDLOG_WARN("Failed to hook any UE 5.7 DrawWindow_RenderThread callsites for ElementsTexture inspection");
        return;
    }

    m_hooked_ue57_slate_elements_pass = true;
    SPDLOG_INFO(
        "Hooked {} UE 5.7 DrawWindow_RenderThread callsites for validated ElementsTexture inspection ({} same-module calls found)",
        hooked_count,
        direct_calls.size());
}

void FFakeStereoRenderingHook::attempt_hook_ue55_slate_output_texture_register() {
    if (!supports_ue55_dedicated_ui_target_for_current_game()) {
        return;
    }

    if (m_attempted_hook_ue55_slate_output_texture_register) {
        return;
    }

    m_attempted_hook_ue55_slate_output_texture_register = true;

    const auto draw_window = g_hook != nullptr ? g_hook->m_slate_thread_hook.target_address() : 0;

    if (draw_window == 0) {
        SPDLOG_ERROR("[UE5.5][SlateUI] Cannot scan DrawWindow_RenderThread because the Slate hook has no target address");
        return;
    }

    uintptr_t slate_output_ref_ip{};
    uintptr_t register_callsite{};
    uintptr_t register_target{};
    bool after_slate_output_ref = false;

    // Decode linearly here. exhaustive_decode follows early CALL branches in this
    // function and can miss the straight-line RegisterExternalTexture callsite.
    for (auto* ip = (uint8_t*)draw_window; (uintptr_t)ip < draw_window + 0x3000;) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded) {
            break;
        }

        if (!after_slate_output_ref) {
            const auto referenced = utility::resolve_displacement((uintptr_t)ip);

            if (referenced &&
                is_readable_process_range(*referenced, sizeof(wchar_t) * 19) &&
                std::wstring_view{(const wchar_t*)*referenced, 18}.starts_with(L"SlateOutputTexture"))
            {
                slate_output_ref_ip = (uintptr_t)ip;
                after_slate_output_ref = true;
                SPDLOG_INFO("[UE5.5][SlateUI] found SlateOutputTexture reference at {:x}", slate_output_ref_ip);
            }

            ip += decoded->Length;
            continue;
        }

        const auto mnemonic = std::string_view{decoded->Mnemonic};

        if (mnemonic.starts_with("CALL")) {
            const auto target = utility::resolve_displacement((uintptr_t)ip);

            if (target.has_value()) {
                register_callsite = (uintptr_t)ip;
                register_target = *target;
                break;
            }
        }

        if (decoded->Instruction == ND_INS_RETN || decoded->Instruction == ND_INS_INT3) {
            break;
        }

        ip += decoded->Length;
    }

    if (slate_output_ref_ip == 0 || register_callsite == 0) {
        SPDLOG_ERROR("[UE5.5][SlateUI] Failed to find SlateOutputTexture RegisterExternalTexture callsite in DrawWindow_RenderThread");
        return;
    }

    auto hook_result = safetyhook::create_mid(
        reinterpret_cast<void*>(register_callsite),
        &FFakeStereoRenderingHook::ue55_slate_output_texture_register_hook);

    if (!hook_result) {
        SPDLOG_ERROR("[UE5.5][SlateUI] Failed to hook SlateOutputTexture RegisterExternalTexture callsite {:x}", register_callsite);
        return;
    }

    m_ue55_slate_output_texture_register_hook = std::move(hook_result);
    m_hooked_ue55_slate_output_texture_register = true;

    SPDLOG_WARN("[UE5.5][SlateUI] Hooked SlateOutputTexture RegisterExternalTexture callsite {:x} -> {:x}", register_callsite, register_target);
}

namespace detail{
bool pre_find_fsceneview_constructor() {
    sdk::FSceneView::get_constructor_address(); // Can take a while to find
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_fsceneview_constructor() {
    if (m_attempted_hook_fsceneview_constructor) {
        return;
    }
    
    // just try to find it before ghosting fix is even enabled
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_fsceneview_constructor);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() && !vr->is_splitscreen_compatibility_enabled() && !vr->is_sceneview_compatibility_enabled() && !vr->is_native_stereo_fix_enabled()) {
        return;
    }

    utility::ScopeGuard _{[&]() {
        m_attempted_hook_fsceneview_constructor = true;
    }};

    SPDLOG_INFO("Attempting to hook FSceneView::FSceneView constructor!");
    const auto constructor = sdk::FSceneView::get_constructor_address();

    if (!constructor) {
        SPDLOG_ERROR("Cannot hook FSceneView::FSceneView constructor");
        return;
    }

    g_hook->m_sceneview_data.constructor_hook = safetyhook::create_inline(*constructor, (uintptr_t)&sceneview_constructor, safetyhook::InlineHook::StartDisabled);

    if (!g_hook->m_sceneview_data.constructor_hook) {
        SPDLOG_ERROR("Failed to hook FSceneView::FSceneView constructor!");
        return;
    }

    if (auto enable_result = g_hook->m_sceneview_data.constructor_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSceneView::FSceneView constructor hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSceneView::FSceneView constructor!");
}

bool FFakeStereoRenderingHook::hook_ue418_oculus_pixel_density_sink() {
    if (m_ue418_oculus_pixel_density_hook) {
        return true;
    }

    const auto engine_version = sdk::search_for_version(utility::get_executable()).value_or(L"");
    if (!engine_version.starts_with(L"4.18")) {
        return false;
    }

    // UE4.18 OculusHMD registers a CVar sink that can be called with stale
    // settings after UEVR redirects stereo rendering. Guard the exact old sink
    // before it writes through an invalid FSettings pointer.
    const auto update_pixel_density = utility::scan(
        utility::get_executable(),
        "40 53 48 83 EC 20 80 B9 44 02 00 00 00 48 8B D9 75 ? 65 48 8B 04 25 58 00 00 00");

    if (!update_pixel_density) {
        return false;
    }

    SPDLOG_INFO("[UE4.18 Oculus] Hooking FSettings::UpdatePixelDensityFromScreenPercentage at {:x}", *update_pixel_density);
    m_ue418_oculus_pixel_density_hook = safetyhook::create_inline((void*)*update_pixel_density, &ue418_oculus_update_pixel_density_hook);

    if (!m_ue418_oculus_pixel_density_hook) {
        SPDLOG_WARN("[UE4.18 Oculus] Failed to hook FSettings::UpdatePixelDensityFromScreenPercentage");
        return false;
    }

    return true;
}

bool FFakeStereoRenderingHook::ue418_oculus_update_pixel_density_hook(void* settings) {
    const auto settings_addr = (uintptr_t)settings;

    // The CTTS crash passed a UEVRBackend code/shadow-vtable address as
    // FSettings*. The original function writes floats at +0x238/+0x23c/+0x240.
    if (!is_writable_process_range(settings_addr + 0x238, 0x10)) {
        static std::atomic<uint32_t> suppressed_invalid_calls{};
        const auto count = ++suppressed_invalid_calls;

        if (count == 1 || (count % 120) == 0) {
            SPDLOG_WARN("[UE4.18 Oculus] Suppressed invalid pixel-density sink call settings={:x} count={}",
                        settings_addr, count);
        }

        return true;
    }

    return g_hook->m_ue418_oculus_pixel_density_hook.call<bool>(settings);
}

bool FFakeStereoRenderingHook::hook() {
    SPDLOG_INFO("Entering FFakeStereoRenderingHook::hook");

    m_tried_hooking = true;

    // Locking the hook monitor mutex stops our code from trying to re-hook DX11 and 12 after
    // Long pauses in code execution, due to us doing massive scans for code in this function.
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    hook_ue418_oculus_pixel_density_sink();

    const auto vtable = locate_fake_stereo_rendering_vtable();

    // This happens if games have intentionally removed the stereo initialization functions and stereo emulation classes.
    // So we need to manually create the stereo device.
    if (!vtable) {
        SPDLOG_ERROR("Failed to locate Fake Stereo Rendering VTable, attempting to perform nonstandard hook");

        auto check_file_version = [](uint32_t ms, uint32_t ls) {
            try {
                const auto full_path = utility::get_module_pathw(utility::get_executable());

                if (!full_path) {
                    SPDLOG_ERROR("Failed to get executable path, falling back");
                    return false;
                }

                const auto file_version_size = GetFileVersionInfoSizeW(full_path->c_str(), nullptr);

                if (file_version_size == 0) {
                    SPDLOG_ERROR("Failed to get file version info size, falling back");
                    return false;
                }

                std::vector<uint8_t> file_version_data(file_version_size);
                GetFileVersionInfoW(full_path->c_str(), 0, file_version_size, file_version_data.data());

                UINT size{};
                VS_FIXEDFILEINFO* fixed_file_info{};

                if (VerQueryValueA(file_version_data.data(), "\\", (LPVOID*)&fixed_file_info, &size) && fixed_file_info != nullptr) {
                    SPDLOG_INFO("MS: {:x}, LS: {:x}", fixed_file_info->dwFileVersionMS, fixed_file_info->dwFileVersionLS);

                    if (fixed_file_info->dwFileVersionMS == ms && fixed_file_info->dwFileVersionLS == ls) {
                        SPDLOG_INFO("Found matching executable, attempting to perform nonstandard hook");
                        return true;
                    } else {
                        SPDLOG_INFO("File does not match requested version, falling back");
                    }
                } else {
                    SPDLOG_ERROR("Failed to get file version info, falling back");
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to get file version info, falling back");
            }

            return false;
        };

        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (!found_version) {
            SPDLOG_WARN("Failed to find version in executable");
        }

        // Check for version 4.27.2.0
        // 4.26 also works here
        if (check_file_version(0x4001B, 0x20000) || found_version.value_or(L"") == L"4.26") {
            return nonstandard_create_stereo_device_hook_4_27();
        }

        // Check for version 4.22.3.0
        if (check_file_version(0x40016, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_22();
        }

        // Check for version 4.18.3.0
        if (check_file_version(0x40012, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_18();
        }

        return nonstandard_create_stereo_device_hook();
    }

    return standard_fake_stereo_hook(*vtable);
}

bool FFakeStereoRenderingHook::standard_fake_stereo_hook(uintptr_t vtable) {
    ZoneScopedN(__FUNCTION__);
    SPDLOG_INFO("Performing standard fake stereo hook");

    const auto game = sdk::get_ue_module(L"Engine");
    std::array<uint8_t, 0x1000> og_vtable{};
    memcpy(og_vtable.data(), (void*)vtable, og_vtable.size()); // to perform tests on.

    const auto module_vtable_within = utility::get_module_within(vtable);

    // In 4.18 the destructor virtual doesn't exist or is at the very end of the vtable.
    const auto is_stereo_enabled_index = sdk::is_vfunc_pattern(*(uintptr_t*)vtable, "B0 01") ? 0 : 1;
    const auto is_stereo_enabled_func_ptr = &((uintptr_t*)vtable)[is_stereo_enabled_index];

    SPDLOG_INFO("IsStereoEnabled Index: {}", is_stereo_enabled_index);

    const auto stereo_view_offset_index = get_stereo_view_offset_index(vtable);

    if (!stereo_view_offset_index) {
        SPDLOG_ERROR("Failed to locate Stereo View Offset Index");
        return false;
    }

    // Some compiler optimizations cause 31 C0 (xor eax, eax) to be used.
    bool uses_33_c0 = false;

    for (size_t i = 0; i < 30; ++i) try {
        const auto fn = ((uintptr_t*)vtable)[i];

        if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
            SPDLOG_WARN("Found null function pointer at index {}", i);
            break;
        }

        if (sdk::is_vfunc_pattern(fn, "33 C0")) {
            uses_33_c0 = true;
            SPDLOG_INFO("Found 33 C0 pattern at index {}", i);
            break;
        }
    } catch(...) {

    }

    const auto stereo_projection_matrix_index = *stereo_view_offset_index + 1;
    const auto is_4_18_or_lower = *stereo_view_offset_index <= 6;

    const auto& stereo_view_offset_func = ((uintptr_t*)vtable)[*stereo_view_offset_index];

    auto render_texture_render_thread_func = utility::find_virtual_function_from_string_ref(game, L"RenderTexture_RenderThread");

    // Seems more robust than simply just checking the vtable index.
    m_uses_old_rendertarget_manager = *stereo_view_offset_index <= 11 && !render_texture_render_thread_func;

    SPDLOG_INFO("Using old rendertarget manager: {}", m_uses_old_rendertarget_manager);

    if (!render_texture_render_thread_func) {
        // Fallback scan to checking for the first non-default virtual function (<= 4.18)
        SPDLOG_INFO("Failed to find RenderTexture_RenderThread, falling back to first non-default virtual function");

        for (auto i = 2; i < 10; ++i) {
            const auto func = ((uintptr_t*)vtable)[stereo_projection_matrix_index + i];

            // Some protectors can fool this check, so we also check for the vfunc pattern (emulates the code)
            if (!utility::is_stub_code((uint8_t*)func) && 
                !sdk::is_vfunc_pattern(func, "33 C0") &&
                !sdk::is_vfunc_pattern(func, "32 C0"))
            {
                render_texture_render_thread_func = func;
                break;
            }
        }

        if (!render_texture_render_thread_func) {
            SPDLOG_ERROR("Failed to find RenderTexture_RenderThread");
            return false;
        }
    }

    SPDLOG_INFO("RenderTexture_RenderThread: {:x}", (uintptr_t)*render_texture_render_thread_func);

    // Scan for the function pointer, it should be in the middle of the vtable.
    auto rendertexture_fn_vtable_middle = utility::scan_ptr(vtable + ((stereo_projection_matrix_index + 2) * sizeof(void*)), 50 * sizeof(void*), *render_texture_render_thread_func);

    if (!rendertexture_fn_vtable_middle) {
        SPDLOG_ERROR("Failed to find RenderTexture_RenderThread VTable Middle");
        return false;
    }

    auto rendertexture_fn_vtable_index = (*rendertexture_fn_vtable_middle - vtable) / sizeof(uintptr_t);
    SPDLOG_INFO("RenderTexture_RenderThread VTable Middle: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*rendertexture_fn_vtable_middle);

    auto render_target_manager_vtable_index = rendertexture_fn_vtable_index + 1 + (2 * (size_t)is_4_18_or_lower);

    // verify first that the render target manager index is returning a null pointer
    // and if not, scan forward until we run into a vfunc that returns a null pointer
    auto get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

    bool is_4_11 = false;

    //if (!sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0")) {
        //SPDLOG_INFO("Expected GetRenderTargetManager function at index {} does not return null, scanning forward for return nullptr.", render_target_manager_vtable_index);

        for (;;++render_target_manager_vtable_index) {
            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

            if (IsBadReadPtr(*(void**)get_render_target_manager_func_ptr, 1)) {
                SPDLOG_ERROR("Failed to find GetRenderTargetManager vtable index, a crash is imminent");
                return false;
            }

            if (sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0") || (!uses_33_c0 && sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "31 C0"))) {
                const auto distance_from_rendertexture_fn = render_target_manager_vtable_index - rendertexture_fn_vtable_index;

                // means it's 4.17 I think. 12 means 4.11.
                if (distance_from_rendertexture_fn == 10 || distance_from_rendertexture_fn == 11 || distance_from_rendertexture_fn == 12) {
                    is_4_11 = distance_from_rendertexture_fn == 12;
                    m_rendertarget_manager_embedded_in_stereo_device = true;
                    SPDLOG_INFO("Render target manager appears to be directly embedded in the stereo device vtable");
                } else {
                    // Now this may potentially be the correct index, but we're not quite done yet.
                    // On 4.19 (and possibly others), the index is 1 higher than it should be.
                    // We can tell by checking how many functions in front of this index return null.
                    // if there are two functions in front of this index that return null, we need to add 1 to the index.
                    SPDLOG_INFO("Found potential GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                    SPDLOG_INFO("Double checking GetRenderTargetManager index...");

                    int32_t count = 0;
                    for (auto i = render_target_manager_vtable_index + 1; i < render_target_manager_vtable_index + 5; ++i) {
                        const auto addr_of_func = (uintptr_t)&((uintptr_t*)vtable)[i];
                        const auto func = ((uintptr_t*)vtable)[i];

                        if (func == 0 || IsBadReadPtr((void*)func, 1)) {
                            break;
                        }

                        // Make sure we didn't cross over into another vtable's boundaries.
                        const auto module_within = utility::get_module_within(addr_of_func);

                        if (module_within && utility::scan_displacement_reference(*module_within, addr_of_func)) {
                            SPDLOG_INFO("Crossed over into another vtable's boundaries, aborting double check");
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (!sdk::is_vfunc_pattern(func, "33 C0") && !sdk::is_vfunc_pattern(func, "31 C0")) {
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (++count >= 2) {
                            ++render_target_manager_vtable_index;
                            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

                            SPDLOG_INFO("Adjusted GetRenderTargetManager index to {}", render_target_manager_vtable_index);
                            break;
                        }
                    }

                    SPDLOG_INFO("Distance: {}", distance_from_rendertexture_fn);
                }

                break;
            } else {
                try {
                    using GetRenderTargetManagerFn = IStereoRenderTargetManager* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
                    const auto func = (GetRenderTargetManagerFn)(*get_render_target_manager_func_ptr);
    
                    // On UE5.5+ FFakeStereoRendering has a valid GetRenderTargetManager that doesn't return null.
                    if (!is_4_18_or_lower && func(og_vtable.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == (IStereoRenderTargetManager*)&og_vtable[sizeof(void*)]) {
                        m_uses_old_rendertarget_manager = false; // nope
                        SPDLOG_INFO("Found UE5.5+ variant of GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                        SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
                        break;
                    }
                } catch(...) {
                    SPDLOG_WARN("Unknown exception while checking GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                }
            }
        }
    //} else {
        //SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
    //}
    
    const auto get_stereo_layers_func_ptr = (uintptr_t)(get_render_target_manager_func_ptr + sizeof(void*));

    if (get_render_target_manager_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetRenderTargetManager");
        return false;
    }

    if (get_stereo_layers_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetStereoLayers");
        return false;
    }

    SPDLOG_INFO("GetRenderTargetManagerptr: {:x}", (uintptr_t)get_render_target_manager_func_ptr);
    SPDLOG_INFO("GetStereoLayersptr: {:x}", (uintptr_t)get_stereo_layers_func_ptr);

    const auto adjust_view_rect_distance = is_4_18_or_lower ? 2 : 3;
    const auto adjust_view_rect_index = *stereo_view_offset_index - adjust_view_rect_distance;

    SPDLOG_INFO("AdjustViewRect Index: {}", adjust_view_rect_index);
    
    auto calculate_stereo_projection_matrix_index = *stereo_view_offset_index + 1;

    // While generally most of the time the stereo projection matrix func is the next one after the stereo view offset func,
    // it's not always the case. We can scan for a call to the tanf function in one of the virtual functions to find it.
    for (auto i = 0; i < 10; ++i) {
        const auto potential_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index + i];
        if (potential_func == 0 || IsBadReadPtr((void*)potential_func, 1) || utility::is_stub_code((uint8_t*)potential_func)) {
            continue;
        }

        auto ip = (uint8_t*)potential_func;
        if (*(uint8_t*)ip == 0xE9) {
            ip = (uint8_t*)utility::calculate_absolute(potential_func + 1);
            SPDLOG_INFO("Found JMP at {:x}, jumping to {:x}", (uintptr_t)potential_func, (uintptr_t)ip);
        }

        bool found = false;

        SPDLOG_INFO("Scanning {:x}...", (uintptr_t)ip);

        for (auto j = 0; j < 50; ++j) {
            INSTRUX ix{};

            const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

            if (!ND_SUCCESS(status)) {
                SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
                break;
            }

            if (ix.Category == ND_CAT_RET || ix.InstructionBytes[0] == 0xE9) {
                SPDLOG_INFO("Encountered RET or JMP at {:x}, aborting scan", (uintptr_t)ip);
                break;
            }

            if (ix.InstructionBytes[0] == 0xE8) {
                auto called_func = (uintptr_t)(ip + ix.Length + (int32_t)ix.RelativeOffset);
                auto inner_ins = utility::decode_one((uint8_t*)called_func);

                SPDLOG_INFO("called {:x}", (uintptr_t)called_func);
                uintptr_t final_func = 0;

                // Fully resolve the pointer jmps until we reach another module.
                while (inner_ins && inner_ins->InstructionBytes[0] == 0xFF && inner_ins->InstructionBytes[1] == 0x25) {
                    const auto called_func_ptr = (uintptr_t*)(called_func + inner_ins->Length + (int32_t)inner_ins->Displacement);
                    const auto called_func_ptr_val = *called_func_ptr;

                    SPDLOG_INFO("called ptr {:x}", (uintptr_t)called_func_ptr_val);

                    inner_ins = utility::decode_one((uint8_t*)called_func_ptr_val);
                    final_func = called_func_ptr_val;
                    called_func = called_func_ptr_val;
                }

                // Check if this function is jmping into the "tanf" export in ucrtbase.dll
                if (final_func != 0) {
                    const auto module_within = utility::get_module_within(final_func);

                    if (module_within &&
                        (final_func == (uintptr_t)GetProcAddress(*module_within, "tanf") ||
                        final_func == (uintptr_t)GetProcAddress(*module_within, "tan"))) 
                    {
                        SPDLOG_INFO("Found CalculateStereoProjectionMatrix: {} {:x}", calculate_stereo_projection_matrix_index + i, potential_func);
                        calculate_stereo_projection_matrix_index += i;
                        found = true;
                        break;
                    } else {
                        SPDLOG_INFO("Function did not call tanf, skipping");
                    }
                } else {
                    SPDLOG_INFO("Failed to resolve inner pointer");
                }
            }

            ip += ix.Length;
        }

        if (found) {
            break;
        }
    }

    const auto init_canvas_index = calculate_stereo_projection_matrix_index + 1;

    const auto adjust_view_rect_func = ((uintptr_t*)vtable)[adjust_view_rect_index];
    const auto calculate_stereo_projection_matrix_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index];
    const auto init_canvas_func_ptr = &((uintptr_t*)vtable)[init_canvas_index];
    // const auto render_texture_render_thread_func = ((uintptr_t*)*vtable)[*stereo_view_offset_index + 3];
    

    SPDLOG_INFO("AdjustViewRect: {:x}", (uintptr_t)adjust_view_rect_func);
    SPDLOG_INFO("CalculateStereoProjectionMatrix: {:x}", (uintptr_t)calculate_stereo_projection_matrix_func);
    SPDLOG_INFO("CalculateStereoViewOffset: {:x}", (uintptr_t)stereo_view_offset_func);
    SPDLOG_INFO("IsStereoEnabled: {:x}", (uintptr_t)*is_stereo_enabled_func_ptr);

    m_has_double_precision = is_using_double_precision(stereo_view_offset_func) || is_using_double_precision(calculate_stereo_projection_matrix_func);

    if (!m_has_double_precision && windrose_is_current_game() && is_ue_5_6_or_newer()) {
        m_has_double_precision = true;
        SPDLOG_WARN("[Windrose][R5] Forcing UE5.6 double-precision view math because function-scan detection missed it");
    }

    {
        m_adjust_view_rect_hook = safetyhook::create_inline((void*)adjust_view_rect_func, adjust_view_rect);
        m_calculate_stereo_view_offset_hook_inline = safetyhook::create_inline((void*)stereo_view_offset_func, calculate_stereo_view_offset);
        m_calculate_stereo_projection_matrix_hook = safetyhook::create_inline((void*)calculate_stereo_projection_matrix_func, calculate_stereo_projection_matrix);
    }
    
    if (!m_adjust_view_rect_hook) {
        SPDLOG_ERROR("Failed to create AdjustViewRect hook");
    }

    if (!m_calculate_stereo_view_offset_hook_inline) {
        SPDLOG_ERROR("Failed to create CalculateStereoViewOffset hook, falling back to pointer hook");
        m_calculate_stereo_view_offset_hook_ptr = std::make_unique<PointerHook>((void**)&stereo_view_offset_func, (void*)calculate_stereo_view_offset);
    }

    if (!m_calculate_stereo_projection_matrix_hook) {
        SPDLOG_ERROR("Failed to create CalculateStereoProjectionMatrix hook");
    }

    // This requires a pointer hook because the virtual just returns false
    // compiler optimization makes that function get re-used in a lot of places
    // so it's not feasible to just detour it, we need to replace the pointer in the vtable.
    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);

        if (!m_render_texture_render_thread_hook) {
            SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
        }

        // Seems to exist in 4.18+
        m_get_render_target_manager_hook = std::make_unique<PointerHook>((void**)get_render_target_manager_func_ptr, (void*)&get_render_target_manager_hook);
    } else {
        // When the render target manager is embedded in the stereo device, it just means
        // that all of the virtuals are now part of FFakeStereoRendering
        // instead of being a part of IStereoRenderTargetManager and being returned via GetRenderTargetManager.
        // Only seen in 4.17 and below.
        SPDLOG_INFO("Performing hooks on embedded RenderTargetManager");

        // Scan forward from the alleged RenderTexture_RenderThread function to find the
        // real RenderTexture_RenderThread function, because it is different when the
        // render target manager is embedded in the stereo device.
        // When it's embedded, it seems like it's the first function right after
        // a set of functions that return false sequentially.
        bool prev_function_returned_false = false;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find real RenderTexture_RenderThread");
                return false;
            }
            
            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                prev_function_returned_false = true;
            } else {
                if (prev_function_returned_false) {
                    render_texture_render_thread_func = func;
                    rendertexture_fn_vtable_index = i;
                    m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);
                    if (!m_render_texture_render_thread_hook) {
                        SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
                    }
                    SPDLOG_INFO("Real RenderTexture_RenderThread: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*render_texture_render_thread_func);
                    break;
                }

                prev_function_returned_false = false;
            }
        }

        // Scan backwards from RenderTexture_RenderThread for the first virtual that just returns
        int32_t calculate_render_target_size_index = 0;

        for (auto i = rendertexture_fn_vtable_index - 1; i > 0; --i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find calculate render target size index, falling back to hardcoded index");
                calculate_render_target_size_index = rendertexture_fn_vtable_index - 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "C3") || sdk::is_vfunc_pattern(func, "C2 00 00")) {
                SPDLOG_INFO("Dynamically found CalculateRenderTargetSize index: {}", i);
                calculate_render_target_size_index = i;
                break;
            }
        }

        const auto calculate_render_target_size_func_ptr = &((uintptr_t*)vtable)[calculate_render_target_size_index];
        SPDLOG_INFO("CalculateRenderTargetSize index: {}", calculate_render_target_size_index);

        // To be seen if this one needs automated analysis
        const auto need_reallocate_viewport_render_target_index = calculate_render_target_size_index + 1;
        const auto need_reallocate_viewport_render_target_func_ptr = &((uintptr_t*)vtable)[need_reallocate_viewport_render_target_index];

        // To be seen if this one needs automated analysis
        const auto should_use_separate_render_target_index = calculate_render_target_size_index + 2;
        const auto should_use_separate_render_target_func_ptr = &((uintptr_t*)vtable)[should_use_separate_render_target_index];

        // Log a warning if NeedReallocateViewportRenderTarget or ShouldUseSeparateRenderTarget are not
        // functions that plainly return false, but do not fail entirely.
        bool need_reallocate_viewport_render_target_is_bad = false;
        bool should_use_separate_render_target_is_bad = false;

        if (!sdk::is_vfunc_pattern(*need_reallocate_viewport_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("NeedReallocateViewportRenderTarget is not a function that returns false");
            need_reallocate_viewport_render_target_is_bad = true;
        }

        if (!sdk::is_vfunc_pattern(*should_use_separate_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("ShouldUseSeparateRenderTarget is not a function that returns false");
            should_use_separate_render_target_is_bad = true;
        }

        SPDLOG_INFO("NeedReallocateViewportRenderTarget index: {}", need_reallocate_viewport_render_target_index);
        SPDLOG_INFO("ShouldUseSeparateRenderTarget index: {}", should_use_separate_render_target_index);

        // Scan forward from RenderTexture_RenderThread for the first virtual that returns false
        int32_t allocate_render_target_index = 0;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find allocate render target index, falling back to hardcoded index");
                allocate_render_target_index = render_target_manager_vtable_index + 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                SPDLOG_INFO("Dynamically found AllocateRenderTarget index: {}", i);
                allocate_render_target_index = i;
                break;
            }
        }

        const auto allocate_render_target_func_ptr = &((uintptr_t*)vtable)[allocate_render_target_index];
        SPDLOG_INFO("AllocateRenderTarget index: {}", allocate_render_target_index);

        m_embedded_rtm.calculate_render_target_size_hook = 
            std::make_unique<PointerHook>((void**)calculate_render_target_size_func_ptr, +[](void* self, const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("CalculateRenderTargetSize (embedded)");
            #else
                SPDLOG_INFO_ONCE("CalculateRenderTargetSize (embedded)");
            #endif

                return g_hook->get_render_target_manager()->calculate_render_target_size(viewport, x, y);
            }
        );

        m_embedded_rtm.allocate_render_target_texture_hook = 
            std::make_unique<PointerHook>((void**)allocate_render_target_func_ptr, +[](void* self, 
                uint32_t index, uint32_t w, uint32_t h, uint8_t format, uint32_t num_mips,
                ETextureCreateFlags lags, ETextureCreateFlags targetable_texture_flags, FTexture2DRHIRef& out_texture,
                FTexture2DRHIRef& out_shader_resource, uint32_t num_samples) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif

                return g_hook->get_render_target_manager()->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &out_texture, &out_shader_resource);
            }
        );
    
        m_embedded_rtm.should_use_separate_render_target_hook = 
            std::make_unique<PointerHook>((void**)should_use_separate_render_target_func_ptr, +[](void* self) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif
            
                auto vr = VR::get();

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    return false;
                }

                if (vr->is_hmd_active() && !vr->is_stereo_emulation_enabled()) {
                    g_hook->get_embedded_rtm().should_use_separate_rt_called = true;
                    return true;
                }

                return false;
            }
        );

        if (!need_reallocate_viewport_render_target_is_bad) {
            m_embedded_rtm.need_reallocate_viewport_render_target_hook = 
                std::make_unique<PointerHook>((void**)need_reallocate_viewport_render_target_func_ptr, +[](void* self, sdk::FViewport* viewport) -> bool {
                #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                    SPDLOG_INFO("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #else
                    SPDLOG_INFO_ONCE("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #endif

                    if (g_hook->get_render_target_manager()->need_reallocate_view_target(*viewport)) {
                        g_hook->get_embedded_rtm().need_reallocate_viewport_render_target_called = true;
                        g_hook->get_embedded_rtm().last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
                        return true;
                    }

                    return false;
                }
            );
        }
    }
    
    m_is_stereo_enabled_hook = std::make_unique<PointerHook>((void**)is_stereo_enabled_func_ptr, (void*)&is_stereo_enabled);

    // scan for GetDesiredNumberOfViews function, we use this function to perform AFR if needed
    SPDLOG_INFO("Searching for GetDesiredNumberOfViews function...");
    std::optional<uint32_t> get_desired_number_of_views_index{};

    for (auto i = 1; i < 20; ++i) {
        auto func_ptr = &((uintptr_t*)vtable)[i];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetDesiredNumberOfViews function, this is okay, not really needed");
            break;
        }

        // pretty consistent patterns
        if (sdk::is_vfunc_pattern(*func_ptr, "0F B6 C2 FF C0 C3") ||
            sdk::is_vfunc_pattern(*func_ptr, "33 C0 84 D2 0F 95 C0 FF C0 C3") || 
            sdk::is_vfunc_pattern(*func_ptr, "84 D2 74 04 8B 41 ? C3 B8 01") ||
            sdk::is_vfunc_pattern(*func_ptr, "B8 01 00 00 00 84 D2 74 03 8B 41 ? C3"))
        {
            SPDLOG_INFO("Found GetDesiredNumberOfViews function at index: {}", i);
            get_desired_number_of_views_index = i;
            m_get_desired_number_of_views_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_desired_number_of_views_hook);
            break;
        }
    }

    // If double precision detected, it means it's >= UE 5.0.3
    if (m_has_double_precision && get_desired_number_of_views_index) {
        SPDLOG_INFO("Searching for GetViewPassForIndex function...");

        // Pretty simple, it's at +1, to be seen if this needs automation
        const auto get_view_pass_for_index_index = *get_desired_number_of_views_index + 1;

        auto func_ptr = &((uintptr_t*)vtable)[get_view_pass_for_index_index];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetViewPassForIndex function. A crash may occur.");
        } else {
            SPDLOG_INFO("Found GetViewPassForIndex function at index: {}", get_view_pass_for_index_index);
            m_get_view_pass_for_index_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_view_pass_for_index_hook);
        }
    } else if (m_has_double_precision) {
        SPDLOG_INFO("Could not locate GetViewPassForIndex function because GetDesiredNumberOfViews function was not found. A crash may occur.");
    }

    SPDLOG_INFO("Leaving FFakeStereoRenderingHook::hook");

    const auto renderer_module = sdk::get_ue_module(L"Renderer");
    const auto backbuffer_format_cvar = sdk::find_cvar_by_description(L"Defines the default back buffer pixel format.", L"r.DefaultBackBufferPixelFormat", 4, renderer_module);
    m_pixel_format_cvar_found = backbuffer_format_cvar.has_value();

    // In 4.18 this doesn't exist. Not much we can do about that.
    if (backbuffer_format_cvar) {
        SPDLOG_INFO("Backbuffer Format CVar: {:x}", (uintptr_t)*backbuffer_format_cvar);
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0) = 0;   // 8bit RGBA, which is what VR headsets support
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0x4) = 0; // 8bit RGBA, which is what VR headsets support
    } else {
        SPDLOG_ERROR("Failed to find backbuffer format cvar, continuing anyways...");
    }

    // make a shadow copy of FFakeStereoRendering's vtable to get past weird compiler optimizations
    // that cause the hook to not work, reason being that the compiler will optimize
    // if the vtable pointer is equal to the original vtable pointer, and it will
    // not call the hook function, so we make a shadow copy of the vtable
    auto active_stereo_device = locate_active_stereo_rendering_device();
    
    // We need to manually insert a stereo device at this point if it's not already.
    // This is what the "nonstandard" hooks did, but those did not have access to FFakeStereoRendering's vtable.
    // All we need to do in this instance is get the engine offset to the stereo device, create a fake pointer with our own vtable,
    // and just overwrite the engine's (null) stereo device pointer with our fake one.
    // It is very rare that this should need to be done.
    if (!active_stereo_device) {
        SPDLOG_INFO("Attempting to create a stereo device without InitializeHMDDevice...");
        const auto device_offset = sdk::UEngine::get_stereo_rendering_device_offset();

        if (device_offset) {
            auto engine = sdk::UGameEngine::get();

            if (engine != nullptr) {
                m_fallback_device.vtable = (void*)vtable;
                *(uintptr_t*)((uintptr_t)engine + *device_offset) = (uintptr_t)&m_fallback_device;

                active_stereo_device = (uintptr_t)&m_fallback_device;
                s_stereo_rendering_device_offset = *device_offset; // Set it up if it's not already
            }
        } else {
            SPDLOG_ERROR("Could not create a new stereo device, VR may not work!");
        }
    }

    if (active_stereo_device) {
        SPDLOG_INFO("Found active stereo device: {:x}", (uintptr_t)*active_stereo_device);
        SPDLOG_INFO("Overwriting vtable...");

        static std::vector<uintptr_t> shadow_vtable{};
        auto& vtable = *(uintptr_t**)*active_stereo_device;

        for (auto i = 0; i < 100; i++) {
            shadow_vtable.push_back(vtable[i]);
        }

        vtable = shadow_vtable.data();
    } else {
        SPDLOG_INFO("Current stereo device is null, cannot overwrite vtable");
        patch_vtable_checks(); // fallback to patching vtable checks
    }

    setup_view_extensions();
    hook_game_viewport_client();

    m_finished_hooking = true;

    SPDLOG_INFO("Finished hooking FFakeStereoRendering!");

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook() {
    // This may only work on one game for now, but it should be a good placeholder
    // for creating a stereo device for games that don't have one.
    // We can figure out how to make it work for other games when we run into one
    // that needs this same functionality.

    // The reason why this function is needed is because in the one game that
    // the FFakeStereoRenderingHook doesn't work through the standard method,
    // is because the VR pipeline seems to have been heavily modified,
    // and so the -emulatestereo command line argument doesn't work, and
    // the FFakeStereoRendering vtable does not seem to exist
    // However the StereoRenderingDevice within GEngine seems to still exist
    // so we can take advantage of that and create our own stereo device
    // the downside is it will be much more difficult to figure out the 
    // proper vtable indices for the functions we need to hook
    // and we will need to actually implement some of the functions
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method");
    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    // Actually implement the ones we care about now.
    auto idx = 0;
    //m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect


    ++idx; // idk waht this is.

    // in this version the index is passed...?
    /*m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, uint32_t index, Vector2f* bounds) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetTextSafeRegionBounds called");
#endif

        bounds->x = 0.75f;
        bounds->y = 0.75f;

        return bounds;
    };*/ // GetTextSafeRegionBounds

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    
    idx++;

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, void* a2) {
        // do nothing
    }; // not sure what this one is. think it sets the FOV. Not present in newer UE4 versions.

    idx++; // just leave this one as a placeholder for now. Returns false.

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    idx++; // just leave this one as a placeholder for now. Probably SetClippingPlanes.

    m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager
    //m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return nullptr; }; // GetRenderTargetManager

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    //m_418_detected = true;
    m_special_detected = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        if (avowed_is_current_game()) {
            SPDLOG_ERROR("[Avowed] StereoRenderingDevice offset discovery failed; refusing legacy 0xAC8 nonstandard fallback");
            return false;
        }

        stereo_rendering_device_offset = 0xAC8; // fallback for the engine this was originally made for.
    }

    *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset) = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_27() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.27)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;

    constexpr auto DEVICE_IS_STEREO_EYE_PASS_INDEX = 7;
    constexpr auto DEVICE_IS_STEREO_EYE_VIEW_INDEX = 8;
    constexpr auto DEVICE_IS_A_PRIMARY_PASS_INDEX = 9;
    constexpr auto DEVICE_IS_A_PRIMARY_VIEW_INDEX = 10;
    constexpr auto DEVICE_IS_A_SECONDARY_PASS_INDEX = 11;
    constexpr auto DEVICE_IS_A_SECONDARY_VIEW_INDEX = 12;
    constexpr auto DEVICE_IS_AN_ADDITIONAL_PASS_INDEX = 13; // not necessary...?
    constexpr auto DEVICE_IS_AN_ADDITIONAL_VIEW_INDEX = 14; // not necessary...?
    constexpr auto DEVICE_GET_LOD_VIEW_INDEX_INDEX = 15; // not necessary...?

    constexpr auto ADJUST_VIEW_RECT_INDEX = 16;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = 19;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = 20;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = 22;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = 23;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xB18; // fallback for the engine this was originally made for.
    }

    static constexpr auto FSCENEVIEW_STEREO_PASS_OFFSET = 0xAF0;
    static auto get_stereo_pass = [](const sdk::FSceneView& view) -> EStereoscopicPass {
        return (EStereoscopicPass)*(uint8_t*)((uintptr_t)&view + FSCENEVIEW_STEREO_PASS_OFFSET);
    };

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyePass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyeView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) == EStereoscopicPass::eSSP_FULL || get_stereo_pass(view) == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return !(pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY);
    }; // DeviceIsASecondaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) > EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsASecondaryView

    m_special_detected_4_27 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_22() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.22)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;
    constexpr auto IS_STEREO_EYE_PASS_INDEX = 7;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 8;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 3;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 2;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 1;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAB8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[IS_STEREO_EYE_PASS_INDEX ] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    };

    m_special_detected_4_22 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_18() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.18)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto IS_STEREO_ENABLED_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 1;
    constexpr auto ENABLE_STEREO_INDEX = 2;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 3;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 2;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 3;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 3;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAE8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_special_detected_4_18 = true;
    m_uses_old_rendertarget_manager = true; // this engine has a funny render target manager.
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::hook_game_viewport_client() try {
    SPDLOG_INFO("Attempting to hook UGameViewportClient::Draw...");

    // We need to cache the canvas index before we hook the draw function or else this doesn't work.
    sdk::FViewport::get_debug_canvas_index();
    auto game_viewport_client_draw = sdk::UGameViewportClient::get_draw_function();

    if (!game_viewport_client_draw) {
        SPDLOG_ERROR("Failed to find UGameViewportClient::Draw!");
        m_has_game_viewport_client_draw_hook = false;
        return false;
    }

    m_gameviewportclient_draw_hook = safetyhook::create_inline((void*)*game_viewport_client_draw, &game_viewport_client_draw_hook, safetyhook::InlineHook::StartDisabled);
    m_has_game_viewport_client_draw_hook = true;

    if (!m_gameviewportclient_draw_hook) {
        SPDLOG_ERROR("Failed to hook UGameViewportClient::Draw!");
        return false;
    }

    if (auto enable_result = m_gameviewportclient_draw_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameViewportClient::Draw hook!");
        return false;
    }

    return true;
} catch(std::exception& e) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient: {}", e.what());
    return false;
} catch(...) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient!");
    return false;
}

void* FFakeStereoRenderingHook::viewport_destructor_hook(void* viewport, void* a2, void* a3, void* a4) {
    ZoneScopedN(__FUNCTION__);

    SPDLOG_INFO("FViewport::~FViewport called: {:x}", (uintptr_t)_ReturnAddress());

    // Call the original destructor.
    auto call_orig = [&]() -> void* {
        ZoneScopedN("FViewport::~FViewport");
        auto res = g_hook->m_viewport_destructor_hook->get_original<decltype(&viewport_destructor_hook)>()(viewport, a2, a3, a4);
        g_hook->m_last_destroyed_viewport = viewport;

        return res;
    };

    if (!g_framework->is_game_data_intialized()) {
        return call_orig();
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        return call_orig();
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Destructor called for the first time.");
        once = false;
    }

    return call_orig();
}

void FFakeStereoRenderingHook::viewport_draw_hook(void* viewport, bool should_present) {
    ZoneScopedN(__FUNCTION__);

    g_hook->m_last_viewport_vtable = *(void***)viewport;

    auto call_orig = [&]() {
        ZoneScopedN("FViewport::Draw");
        g_hook->m_viewport_draw_hook.call(viewport, should_present);
    };

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    if (g_hook->m_viewport_destructor_hook == nullptr) {
        static bool already_tried = false;

        if (!already_tried) {
            already_tried = true;
            auto& vtable = *(void***)viewport;

            if (vtable != nullptr && vtable[0] != nullptr) {
                // Destructors usually have some kind of test reg8, 01 instruction within them.
                if (utility::find_pattern_in_path((uint8_t*)vtable[0], 0x100, false, "F6 ? 01")) {
                    SPDLOG_INFO("Found TEST mnemonic for FViewport destructor at {:x}", (uintptr_t)vtable[0]);
                    SPDLOG_INFO("Hooking FViewport::~FViewport at {:x}", (uintptr_t)vtable[0]);
                    g_hook->m_viewport_destructor_hook = std::make_unique<PointerHook>(&vtable[0], &viewport_destructor_hook);
                } else {
                    SPDLOG_ERROR("Failed to find FViewport destructor pattern at {:x}", (uintptr_t)vtable[0]);
                }
            }
        }
    }

    if (g_hook->m_ignore_next_viewport_draw) {
        g_hook->m_ignore_next_viewport_draw = false;
        return;
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Draw called for the first time.");
        once = false;
    }

    call_orig();
}

// This function needs some more work for more rigorous filtering
// However it does its job on the relevant titles
// This is only used for the UI compatibility mode.
FRHITexture2D** FFakeStereoRenderingHook::viewport_get_render_target_texture_hook(sdk::FViewport* viewport) {
    const auto retaddr = (uintptr_t)_ReturnAddress();

    SPDLOG_INFO_ONCE("FViewport::GetRenderTargetTexture called!");
    const auto og = g_hook->m_viewport_get_render_target_texture_hook->get_original<decltype(&viewport_get_render_target_texture_hook)>();
    const auto& vr = VR::get();

    if (!vr->is_ahud_compatibility_enabled() || !vr->is_hmd_active() || g_hook->m_slate_draw_window_thread_id == 0) {
        return og(viewport);
    }

    auto& data = g_hook->m_viewport_rt_hook_data;

    {
        std::scoped_lock _{data.retaddr_mutex};
        utility::ScopeGuard guard{[&](){ data.seen_retaddrs.insert(retaddr); }};

        if (data.call_original_retaddrs.contains(retaddr)) {
            return og(viewport);
        }

        std::optional<size_t> func_start{};

        // ALWAYS check the retaddr for ViewFamilyTexture first and never skip it
        // This will fix the case where we run into some other texture initially.
        if (!data.seen_retaddrs.contains(retaddr)) {
            SPDLOG_INFO("FViewport::GetRenderTargetTexture called from {:x}", retaddr);

            func_start = utility::find_function_start(retaddr);

            if (!func_start) {
                func_start = retaddr;
            }

            // The function that has this string reference should ALWAYS get passed
            // back to the original function, this is the actual scene render target.
            // Everything else we will redirect to the UI render target.
            if (utility::find_string_reference_in_path(*func_start, L"ViewFamilyTexture", false) || utility::find_string_reference_in_path(*func_start, L"ViewFamilyTarget", false)) {
                SPDLOG_INFO("Found view family texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                data.has_view_family_tex = true;
                return og(viewport);
            }

            // We should always allow the viewport when used in a post processing context to go through.
            // There's two because this function stops itself at 200 instructions
            // doing a second one from the retaddr allows us to go further.
            if (utility::find_string_reference_in_path(*func_start, L"FinalPostProcessColor", false) || utility::find_string_reference_in_path(retaddr, L"FinalPostProcessColor", false)) {
                SPDLOG_INFO("Found FinalPostProcessColor reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            const auto next_fn_call = utility::scan_disasm(retaddr, 0x30, "E8 ? ? ? ?");

            if (next_fn_call) {
                const auto fn = utility::calculate_absolute(*next_fn_call + 1);

                // I don't know of any other way to check this. I'm not sure what this function is.
                // It seems like deep within a threaded or function for enqueueing a render command.
                if (utility::scan(fn, 0x50, "01 01 01 01") && utility::scan(fn, 0x50, "22 00 00 00")) {
                    SPDLOG_INFO("Found unknown screen space rendering call @ {:x}", retaddr);
                    data.redirected_retaddrs.insert(retaddr);
                }
            }

            // There are multiple other HAL references we can use too.
            static const auto hal_clear_solid_rectangle_fn = utility::find_function_from_string_ref(utility::get_executable(), "HAL::ClearSolidRectangle");
            static std::unordered_set<uintptr_t> scaleform_hal_vtable_functions{};

            const auto is_scaleform = hal_clear_solid_rectangle_fn.has_value();

            if (hal_clear_solid_rectangle_fn.has_value() && scaleform_hal_vtable_functions.empty()) try {
                scaleform_hal_vtable_functions.insert(*hal_clear_solid_rectangle_fn);

                SPDLOG_INFO("Found HAL::ClearSolidRectangle function @ {:x}", *hal_clear_solid_rectangle_fn);
                std::vector<uintptr_t> scaleform_hal_vtable_refs{};
                const auto module_size = utility::get_module_size(utility::get_executable()).value_or(0);
                const auto start = (uintptr_t)utility::get_executable();
                const auto end = (uintptr_t)utility::get_executable() + module_size;
                const auto hal_module = utility::get_module_within(*hal_clear_solid_rectangle_fn).value_or(nullptr);

                // There are multiple HAL vtable, so just collect all of them.
                for (auto i = start; i < end - 0x1000; i += sizeof(uintptr_t)) {
                    const auto remaining = end - i;
                    const auto function_ptr = utility::scan_ptr(i, remaining - 0x1000, *hal_clear_solid_rectangle_fn);

                    if (!function_ptr.has_value()) {
                        break;
                    }

                    i = *function_ptr;

                    SPDLOG_INFO("Found HAL::ClearSolidRectangle function pointer @ {:x}", *function_ptr);
                    for (auto j = 0; j < 100; ++j) {
                        const auto entry = *(uintptr_t*)(*function_ptr + (j * sizeof(uintptr_t)));

                        if (entry == 0 || IsBadReadPtr((void*)entry, sizeof(uintptr_t))) {
                            break;
                        }

                        const auto is_same_module = utility::get_module_within(entry).value_or(nullptr) == hal_module;

                        if (!is_same_module) {
                            break;
                        }

                        scaleform_hal_vtable_functions.insert(entry);
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to find Scaleform HAL vtable functions!");
            }

            if (is_scaleform && !scaleform_hal_vtable_functions.empty()) try {
                // Walk the stack, get function starts and check if any are in the vtable
                constexpr auto max_stack_depth = 100;
                uintptr_t stack[max_stack_depth]{};

                const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO(" Stack[{}]: {:x}", i, stack[i]);
                }

                bool found = false;

                for (auto i = 1; i < std::min<uint16_t>(7, depth); ++i) {
                    const auto scaleform_func_start = utility::find_virtual_function_start(stack[i]);

                    if (!scaleform_func_start) {
                        continue;
                    }

                    if (scaleform_hal_vtable_functions.contains(*scaleform_func_start)) {
                        SPDLOG_INFO("Found Scaleform HAL vtable function reference @ {:x}", retaddr);
                        data.redirected_retaddrs.insert(retaddr);
                        found = true;
                        break;
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to walk stack for scaleform vtable functions!");
            }
        }

        // Hacky way to allow the first texture to go through
        // For the games that are using something other than ViewFamilyTexture as the scene RT.
        if (!data.call_original_retaddrs.empty() && !data.redirected_retaddrs.contains(retaddr) && !data.has_view_family_tex) {
            return og(viewport);
        }

        if (!data.redirected_retaddrs.contains(retaddr) && !data.call_original_retaddrs.contains(retaddr)) {
            if (!func_start) {
                func_start = utility::find_function_start(retaddr);

                if (!func_start) {
                    func_start = retaddr;
                }
            }

            // Probably NOT...
            /*if (utility::find_string_reference_in_path(*func_start, L"r.RHICmdAsyncRHIThreadDispatch")) {
                SPDLOG_INFO("Found RHICmdAsyncRHIThreadDispatch reference @ {:x}", retaddr);
                call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }*/

            // TODO? this needs some more rigorous filtering
            // some games are insane and have multiple "UnknownTexture" references...
            if (utility::find_string_reference_in_path(*func_start, L"UnknownTexture", false)) {
                SPDLOG_INFO("Found unknown texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            SPDLOG_INFO("Redirecting FViewport::GetRenderTargetTexture call to UI render target @ {:x}", retaddr);
            data.redirected_retaddrs.insert(retaddr);
        }
    }

    // Finally redirect the call to the UI render target.
    auto& ui_target = g_hook->get_render_target_manager()->get_effective_ui_target_ref();

    if (ui_target != nullptr) {
        return &ui_target;
    }

    return og(viewport);
}

void FFakeStereoRenderingHook::try_adopt_scene_viewport_render_target(sdk::FViewport* viewport, const char* source) {
    constexpr bool allow_scene_viewport_rt_adoption = false;

    if (is_ue_5_7_or_newer() || !g_framework->is_dx12()) {
        return;
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active() || viewport == nullptr || IsBadReadPtr(viewport, sizeof(void*))) {
        return;
    }

    auto rtm = get_render_target_manager();

    if (rtm == nullptr || rtm->get_render_target() != nullptr) {
        return;
    }

    auto candidate = viewport->get_scene_viewport_render_target_texture_direct();

    if (candidate == nullptr) {
        shf_probe_scene_viewport_memory(viewport, source, nullptr);
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] FSceneViewport render target is not available yet from {}", source);
        return;
    }

    ID3D12Resource* native_resource = nullptr;
    D3D12_RESOURCE_DESC desc{};

    if (is_ue_5_6_dx12_backend()) {
        if (!ue56_dx12_try_get_native_resource(candidate, source, &native_resource, &desc)) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[UE5.6][RT] Failing closed for FSceneViewport render target from {}; waiting for D3D12 texture/backbuffer hooks", source);
            return;
        }
    } else {
        try {
            native_resource = (ID3D12Resource*)candidate->get_native_resource();
        } catch (const std::exception& e) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[SHf] Rejected FSceneViewport render target from {} because GetNativeResource failed: {}", source, e.what());
            return;
        } catch (...) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[SHf] Rejected FSceneViewport render target from {} because GetNativeResource threw", source);
            return;
        }

        if (native_resource != nullptr && !IsBadReadPtr(native_resource, sizeof(void*))) {
            desc = native_resource->GetDesc();
        }
    }

    if (native_resource == nullptr || IsBadReadPtr(native_resource, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] FSceneViewport render target from {} has no native D3D12 resource yet", source);
        return;
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) {
        SPDLOG_WARNING_EVERY_N_SEC(2,
            "[SHf] Rejected FSceneViewport render target from {} because desc is invalid: dim={} size={}x{} fmt={}",
            source, (uint32_t)desc.Dimension, desc.Width, desc.Height, (uint32_t)desc.Format);
        return;
    }

    if (!allow_scene_viewport_rt_adoption) {
        shf_probe_scene_viewport_memory(viewport, source, candidate);
        SPDLOG_WARNING_EVERY_N_SEC(2,
            "[SHf] Found FSceneViewport render target candidate from {} at {:x} [{}x{} fmt={}] but not adopting it yet",
            source, (uintptr_t)candidate, desc.Width, desc.Height, (uint32_t)desc.Format);
        return;
    }

    rtm->set_render_target(candidate);

    SPDLOG_WARN_ONCE("[SHf] Adopted real FSceneViewport render target from {} at {:x} [{}x{} fmt={}]",
        source, (uintptr_t)candidate, desc.Width, desc.Height, (uint32_t)desc.Format);
}

void FFakeStereoRenderingHook::game_viewport_client_draw_hook(sdk::UGameViewportClient* viewport_client, sdk::FViewport* viewport, sdk::FCanvas* canvas, void* a4) {
    ZoneScopedN(__FUNCTION__);

    // UI compatibility mode
    // Tries to redirect calls to GetRenderTargetTexture to point towards our UI
    // texture instead of the scene render target, if it's not the scene itself/the view family texture.
    // This usually isn't needed but sometimes there are bespoke changes to the rendering pipeline
    // or uses of the AHUD class that make it necessary.
    if (g_framework->is_game_data_intialized() && VR::get()->is_ahud_compatibility_enabled() && viewport != nullptr) {
        if (g_hook->m_viewport_get_render_target_texture_hook == nullptr) {
            SPDLOG_INFO("Hooking FViewport::GetRenderTargetTexture...");
            void** vp_vtable = *(void***)viewport;
            g_hook->m_viewport_get_render_target_texture_hook = std::make_unique<PointerHook>(&vp_vtable[1], &viewport_get_render_target_texture_hook);
            SPDLOG_INFO("Hooked FViewport::GetRenderTargetTexture!");
        }
    }

    auto call_orig = [=]() {
        ZoneScopedN("UGameViewportClient::Draw");
        g_hook->m_gameviewportclient_draw_hook.call(viewport_client, viewport, canvas, a4);
    };

    SPDLOG_INFO_ONCE("UGameViewportClient::Draw called for the first time.");

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    g_hook->m_in_viewport_client_draw = true;
    g_hook->m_was_in_viewport_client_draw = false;
    g_hook->get_render_target_manager()->set_viewport(viewport);
    if (viewport != nullptr) {
        shf_force_scene_viewport_separate_rt(*viewport, "UGameViewportClient::Draw");
    }
    g_hook->try_adopt_scene_viewport_render_target(viewport, "UGameViewportClient::Draw viewport");

    utility::ScopeGuard _{ 
        []() { 
            g_hook->m_in_viewport_client_draw = false;
            g_hook->m_was_in_viewport_client_draw = false;
        } 
    };

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static uint32_t hook_attempts = 0;
    static bool run_anyways = false;

    if (hook_attempts < 100 && !g_hook->m_hooked_game_engine_tick && g_hook->m_attempted_hook_game_engine_tick) {
        ZoneScopedN("UGameViewportClient::Draw (hook UGameEngine::Tick)");
        SPDLOG_INFO("Performing alternative UGameEngine::Tick hook for synced AFR.");

        ++hook_attempts;

        // Go up the stack and find the viewport draw function.
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

        for (auto i = 0; i < depth; ++i) {
            SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);
        }

        for (auto i = 3; i < depth; ++i) {
            const auto ret = stack[i];

            g_hook->attempt_hook_game_engine_tick(ret);

            if (g_hook->m_hooked_game_engine_tick) {
                SPDLOG_INFO("Successfully hooked UGameEngine::Tick for synced AFR.");
                break;
            }
        }
    } else {
        run_anyways = !g_hook->m_hooked_game_engine_tick;
    }

    const auto in_engine_tick = g_hook->m_in_engine_tick;

    if (run_anyways || in_engine_tick) {
        if (g_hook->m_has_view_extension_hook) {
            g_frame_count = vr->get_runtime()->internal_frame_count;
            vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
        } else {
            vr->update_hmd_state(false);
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (const auto& mod : mods) {
        mod->on_pre_viewport_client_draw(viewport_client, viewport, canvas);
    }

    call_orig();

    // Perform synced eye rendering (synced AFR)
    if (in_engine_tick && vr->is_using_synchronized_afr()) {
        static bool hooked_viewport_draw = false;

        // Hook for FViewport::Draw
        if (g_hook->m_hooked_game_engine_tick && !hooked_viewport_draw) {
            hooked_viewport_draw = true;

            // Go up the stack and find the viewport draw function.
            constexpr auto max_stack_depth = 100;
            uintptr_t stack[max_stack_depth]{};

            const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
            if (depth >= 2) {
                // Log the stack functions
                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO("(Stack[{}]: {:x}", i, stack[i]);
                }

                auto try_hook_index = [&](uint32_t index) -> bool {
                    SPDLOG_INFO("Attempting to locate FViewport::Draw function @ stack[{}]", index);

                    const auto viewport_draw_middle = stack[index];
                    const auto viewport_draw = utility::find_function_start_with_call(viewport_draw_middle);

                    if (!viewport_draw) {
                        SPDLOG_ERROR("Failed to find viewport draw function @ {}", index);
                        return false;
                    }

                    SPDLOG_INFO("Found FViewport::Draw function at {:x}", (uintptr_t)*viewport_draw); 

                    g_hook->m_viewport_draw_hook = safetyhook::create_inline((void*)*viewport_draw, &viewport_draw_hook);

                    if (!g_hook->m_viewport_draw_hook) {
                        SPDLOG_ERROR("Failed to hook FViewport::Draw function!");
                        return false;
                    }

                    return true;
                };

                if (!try_hook_index(1)) {
                    // Fallback to index 3, on some UE4 games the viewport draw function is called from a different stack index.
                    if (!try_hook_index(2)) {
                        SPDLOG_ERROR("Failed to find viewport draw function! Cannot perform synced AFR!");
                    }
                }
            }
        }
    }

    // This is how synchronized AFR works. it forces a world draw
    // on the start of the next engine tick, before the world ticks again.
    // that will allow both views and the world to be drawn in sync with no artifacts.
    if (in_engine_tick && vr->is_using_synchronized_afr() && g_frame_count % 2 == 0) {
        GameThreadWorker::get().enqueue([=]() {
            if (g_hook->m_viewport_draw_hook && viewport != g_hook->m_last_destroyed_viewport) {
                __try {
                    if (*(void***)viewport != g_hook->m_last_viewport_vtable) {
                        SPDLOG_ERROR("FViewport::Draw called on a viewport with a different vtable! This is not expected!");
                        return;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    SPDLOG_ERROR("FViewport::Draw called with a bad viewport pointer! This is not expected!");
                    return;
                }

                const auto viewport_draw = (void (*)(void*, bool))g_hook->m_viewport_draw_hook.target();
                viewport_draw(viewport, true);

                auto& vr = VR::get();
                const auto method = vr->get_synced_sequential_method();
                
                if (method == VR::SyncedSequentialMethod::SKIP_TICK) {
                    g_hook->m_ignore_next_engine_tick = true;
                    //g_hook->m_ignore_next_viewport_draw = true;
                } else if (method == VR::SyncedSequentialMethod::SKIP_DRAW) {
                    g_hook->m_ignore_next_viewport_draw = true;
                }
            }
        });
    }

    for (const auto& mod : mods) {
        mod->on_post_viewport_client_draw(viewport_client, viewport, canvas);
    }
}

static std::array<uintptr_t, 50> g_view_extension_vtable{};
struct SceneViewExtensionAnalyzer;

// Analyzes all of the virtual functions for ISceneViewExtension
// We create the ISceneViewExtension ourselves and overwrite all of the virtual functions
// The class will count how many times each virtual is getting called
// and then when a threshold is reached, it finds the most called one
// the most called one is IsActiveThisFrame which we need to activate the ISceneViewExtension
struct SceneViewExtensionAnalyzer {
    template<int N>
    struct FillVtable {
        static void fill(std::array<uintptr_t, 50>& table);
        static void fill2(std::array<uintptr_t, 50>& table);
    };

    template<>
    struct FillVtable<-1> {
        static void fill(std::array<uintptr_t, 50>& table) {}
        static void fill2(std::array<uintptr_t, 50>& table) {}
    };

    struct AnalyzedFunction {
        uint32_t call_count{0};
        uint32_t frame_count_a2{0};
        uint32_t frame_count_a3{0};
        uint32_t frame_count_offset_a2{0};
        uint32_t frame_count_offset_a3{0};
        uint32_t times_frame_count_correct_a2{0};
        uint32_t times_frame_count_correct_a3{0};
        std::array<uint8_t, 0x100> a2_data{};
        std::array<uint8_t, 0x100> a3_data{};
    };

    static inline std::recursive_mutex dummy_mutex{};
    static inline uint32_t total_call_count{};
    static inline std::unordered_map<uint32_t, AnalyzedFunction> functions{};
    static inline bool has_found_is_active_this_frame_index{false};
    static inline bool has_found_begin_render_viewfamily{false};
    static inline bool index_0_called{false};
    
    static inline uint32_t is_active_this_frame_index{0};
    static inline uint32_t begin_render_viewfamily_index{0};
    static inline uint32_t pre_render_viewfamily_renderthread_index{0};
    static inline uint32_t frame_count_offset{0};

    static bool validate_cached_discovery(void** original_vtable, const nlohmann::json& cached);
    static bool try_apply_cached_discovery(void** original_vtable);
    static void save_cached_discovery();

    template<int N>
    static bool analysis_dummy_stage1(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (N == 0) {
            index_0_called = true;
        }

        if (has_found_is_active_this_frame_index) {
            return false;
        }

        std::scoped_lock _{dummy_mutex};

        auto& func = functions[N];

        ++total_call_count;
        ++functions[N].call_count;

        if (total_call_count >= 50) {
            // Find the most called index, it's going to be IsActiveThisFrame
            uint32_t max_count = 0;
            uint32_t max_index = 0;

            for (const auto& func : functions) {
                const auto count = func.second.call_count;
                const auto index = func.first;

                if (count > max_count) {
                    max_count = count;
                    max_index = index;
                }
            }

            SPDLOG_INFO("[Stage 1] Found most called index to be {} with {} calls", max_index, max_count);

            functions.clear();
            FillVtable<g_view_extension_vtable.size() - 1>::fill2(g_view_extension_vtable);

            // Force the function to return true
            g_view_extension_vtable[max_index] = (uintptr_t)+[](ISceneViewExtension* ext) -> bool {
                return true;
            };

            has_found_is_active_this_frame_index = true;
            is_active_this_frame_index = max_index;
        } else {
            if (functions[N].call_count == 1) {
                SPDLOG_INFO("[Stage 1] ISceneViewExtension Index {} called for the first time!", N);
            }
        }

        return false;
    };

    template<int N>
    static bool analysis_dummy_stage2(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (has_found_begin_render_viewfamily) {
            return false;
        }

        if (N == 0) {
            index_0_called = true;
        }

        std::scoped_lock _{dummy_mutex};

        if (functions.contains(N)) {
            auto& func = functions[N];

            if (func.call_count++ == 0) {
                SPDLOG_INFO("[Stage 2] SceneViewExtension Index {} called for the first time!", N);
            }

            const auto& last_view_family_data_a2 = func.a2_data;
            const auto view_family_a2 = (uintptr_t)a2;

            if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a2.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a2[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a2)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a2 + 1 == b) {
                            SPDLOG_DEBUG("[A2] Function index {} Found frame count offset at {:x}, ({})", N, i, b);

                            func.frame_count_offset_a2 = i;
                            ++func.times_frame_count_correct_a2;

                            // func_next is one of the functions ahead of N and has the frame count in a3
                            AnalyzedFunction* func_next = nullptr;
                            uint32_t next_index = 0;

                            for (auto j = N + 1; j < g_view_extension_vtable.size(); j++) {
                                if (functions.contains(j)) {
                                    const auto& next = functions[j];

                                    if (next.times_frame_count_correct_a3 >= 10) {
                                        func_next = &functions[j];
                                        next_index = j;
                                        break;
                                    }
                                }
                            }

                            if (func_next != nullptr) {
                                if (func.times_frame_count_correct_a2 >= 50 && 
                                    func_next->times_frame_count_correct_a3 >= 50 && 
                                    func.frame_count_offset_a2 == func_next->frame_count_offset_a3 &&
                                    std::abs((int32_t)func.frame_count_a2 - (int32_t)func_next->frame_count_a3) <= 3) // In some games, the frame delta is really high but the same offset (so, it's wrong)
                                {
                                    SPDLOG_INFO("Found final frame count offset at {:x}", i);
                                    SPDLOG_INFO("Found BeginRenderViewFamily at index {}", N);
                                    SPDLOG_INFO("Found PreRenderViewFamily_RenderThread at index {}", next_index);
                                    has_found_begin_render_viewfamily = true;
                                    begin_render_viewfamily_index = N;
                                    pre_render_viewfamily_renderthread_index = next_index;

                                    frame_count_offset = i;
                                    sdk::FSceneViewFamily::set_frame_count_offset(frame_count_offset);
                                    save_cached_discovery();

                                    setup_view_extension_hook();
                                    return false;
                                }   
                            }
                        }

                        func.frame_count_a2 = b;
                        break;
                    }
                }
            }

            const auto& last_view_family_data_a3 = func.a3_data;
            const auto view_family_a3 = (uintptr_t)a3;

            if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a3.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a3[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a3)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a3 + 1 == b) {
                            SPDLOG_DEBUG("[A3] Function index {} Found frame count offset at {:x} ({})", N, i, b);
                            ++func.times_frame_count_correct_a3;
                        }

                        func.frame_count_a3 = b;
                        func.frame_count_offset_a3 = i;
                        break;
                    }
                }
            }
        }

        if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
            memcpy(functions[N].a2_data.data(), (void*)a2, 0x100);
        }

        if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
            memcpy(functions[N].a3_data.data(), (void*)a3, 0x100);
        }

        return false;
    }

    static inline std::recursive_mutex vtable_mutex{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, void**> original_vtables{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, uint32_t> cmd_frame_counts{};

    // Meant to be called after analysis has been completed
    static void setup_view_extension_hook() {
        std::scoped_lock _{dummy_mutex};

        SPDLOG_INFO("Setting up BeginRenderViewFamily hook...");

        const auto setup_view_family_index = index_0_called ? 0 : 1;

        g_view_extension_vtable[setup_view_family_index] = (uintptr_t)&FFakeStereoRenderingHook::setup_view_family;
        g_view_extension_vtable[begin_render_viewfamily_index] = (uintptr_t)&FFakeStereoRenderingHook::begin_render_viewfamily;

        if (!index_0_called && (setup_view_family_index + 2) != begin_render_viewfamily_index) {
            g_view_extension_vtable[setup_view_family_index + 2] = (uintptr_t)&FFakeStereoRenderingHook::setup_viewpoint;
        }

        // PreRenderViewFamily_RenderThread
        g_view_extension_vtable[pre_render_viewfamily_renderthread_index] = (uintptr_t)&FFakeStereoRenderingHook::pre_render_viewfamily_renderthread;

        SPDLOG_INFO("Done setting up BeginRenderViewFamily hook!");
    }

    static inline std::unordered_set<int> tested_execute_indices{};
    static inline int correct_execute_index{0};
    static inline bool found_correct_execute{false};

    template<int N>
    static void* hooked_command_fn(sdk::FRHICommandBase_New* cmd, sdk::FRHICommandListBase* cmd_list, void* debug_context, void* r9, void* stack_1, void* stack_2, void* stack_3, void* stack_4, void* stack_5, void* stack_6, void* stack_7, void* stack_8) {
        std::scoped_lock _{vtable_mutex};
        //std::scoped_lock __{VR::get()->get_vr_mutex()};

        static bool once = true;

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list! {}", N);
        }

        if (g_hook != nullptr) {
            g_hook->note_successful_command_list_hijack();
        }

        const auto original_vtable = original_vtables[cmd];
        const auto original_func = original_vtable[N];

        const auto func = (decltype(hooked_command_fn<N>)*)original_func;
        const auto frame_count = cmd_frame_counts[cmd];

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Command list frame count: {}", frame_count);
            SPDLOG_INFO("[ISceneViewExtension] Original vtable: {:x}", (uintptr_t)original_vtable);
            once = false;
        }

        if (!found_correct_execute && !tested_execute_indices.contains(N) && VR::get()->get_present_thread_id() != 0) {
            tested_execute_indices.insert(N);

            // N == 0 is a pretty safe heuristic
            // Otherwise if >= 1 gets called first, we can assume if the thread is the same
            // as the DXGI present thread, then it's the correct execute function
            if (N == 0 || GetCurrentThreadId() == VR::get()->get_present_thread_id()) {
                correct_execute_index = N;
                found_correct_execute = true;
                SPDLOG_INFO("[ISceneViewExtension] Found correct execute index: {}", N);
            }
        }

        auto& vr = VR::get();
        auto runtime = vr->get_runtime();

        auto call_orig = [=]() {
            const auto result = func(cmd, cmd_list, debug_context, r9, stack_1, stack_2, stack_3, stack_4, stack_5, stack_6, stack_7, stack_8);

            if (N == correct_execute_index) {
                runtime->enqueue_render_poses(frame_count);
            }

            return result;
        };

        if (N != correct_execute_index) {
            return call_orig();
        }

        // set the vtable back
        *(void**)cmd = original_vtable;
        original_vtables.erase(cmd);
        cmd_frame_counts.erase(cmd);

        RHIThreadWorker::get().execute();

        if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
            if (runtime->is_openxr()) {
                if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                    if (!runtime->got_first_sync || runtime->synchronize_frame(frame_count, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }  
                } else if (runtime->synchronize_frame(frame_count, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }

                vr->get_openxr_runtime()->begin_frame();
            } else {
                if (runtime->synchronize_frame(frame_count, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }
            }
        }

        return call_orig();
    }

    static void hook_new_rhi_command(sdk::FRHICommandBase_New* last_command, uint32_t frame_count) {
        std::scoped_lock __{vtable_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        if (last_command == nullptr || *(void**)last_command == nullptr) {
            SPDLOG_INFO("Cannot hook command with no vtable, falling back to passing current frame count to runtime");
            runtime->enqueue_render_poses(frame_count);
            return;
        }

        // Whichever one gets called first is the winner winner chicken dinner
        static std::array<uintptr_t, 7> new_vtable{
            (uintptr_t)&hooked_command_fn<0>,
            (uintptr_t)&hooked_command_fn<1>,
            (uintptr_t)&hooked_command_fn<2>,
            (uintptr_t)&hooked_command_fn<3>,
            (uintptr_t)&hooked_command_fn<4>,
            (uintptr_t)&hooked_command_fn<5>,
            (uintptr_t)&hooked_command_fn<6>
        };

        cmd_frame_counts[last_command] = frame_count;

        if (original_vtables.contains(last_command) || *(void**)last_command == new_vtable.data()) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            static uint64_t suppressed_count = 0;
            const auto now = std::chrono::high_resolution_clock::now();
            ++suppressed_count;
            
            if (last_log_time.time_since_epoch().count() == 0 || now - last_log_time > std::chrono::seconds(30)) {
                SPDLOG_WARN(
                    "Something strange is going on, the vtable is already hooked, maybe previous frame was not rendered? suppressed={}",
                    suppressed_count);
                suppressed_count = 0;
                last_log_time = now;
            }

            if (auto vr = VR::get(); vr != nullptr) {
                vr->note_stalker2_transition_stress("duplicate_rhi_command");
            }

            return;
        }

        original_vtables[last_command] = *(void***)last_command;
        *(void***)last_command = (void**)new_vtable.data();
    }

    static void hook_old_rhi_command(sdk::FRHICommandBase_Old* last_command, uint32_t frame_count) {
        static std::recursive_mutex func_mutex{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, sdk::FRHICommandBase_Old::Func> original_funcs{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, uint32_t> cmd_frame_counts{};

        std::scoped_lock __{func_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        cmd_frame_counts[last_command] = frame_count;

        if (original_funcs.contains(last_command)) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            static uint64_t suppressed_count = 0;
            const auto now = std::chrono::high_resolution_clock::now();
            ++suppressed_count;
            
            if (last_log_time.time_since_epoch().count() == 0 || now - last_log_time > std::chrono::seconds(30)) {
                SPDLOG_WARN(
                    "Something strange is going on, the function is already hooked, maybe previous frame was not rendered? suppressed={}",
                    suppressed_count);
                suppressed_count = 0;
                last_log_time = now;
            }

            if (auto vr = VR::get(); vr != nullptr) {
                vr->note_stalker2_transition_stress("duplicate_old_rhi_command");
            }

            return;
        }

        static auto func_override = (sdk::FRHICommandBase_Old::Func)+[](sdk::FRHICommandListBase* cmd_list, sdk::FRHICommandBase_Old* cmd) {
            std::scoped_lock _{func_mutex};
            //std::scoped_lock __{VR::get()->get_vr_mutex()};

            static bool once = true;

            if (once) {
                SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list!");
                once = false;
            }

            if (g_hook != nullptr) {
                g_hook->note_successful_command_list_hijack();
            }

            auto& vr = VR::get();
            auto runtime = vr->get_runtime();

            const auto func = original_funcs[cmd];
            const auto frame_count = cmd_frame_counts[cmd];

            runtime->enqueue_render_poses(frame_count);
            runtime->on_pre_render_rhi_thread(frame_count);

            auto call_orig = [&]() {
                func(*cmd_list, cmd);
            };

            cmd->func = func;
            original_funcs.erase(cmd);
            cmd_frame_counts.erase(cmd);

            RHIThreadWorker::get().execute();

            if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
                if (runtime->is_openxr()) {
                    if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                        if (!runtime->got_first_sync || runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                            return call_orig();
                        }  
                    } else if (runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }

                    vr->get_openxr_runtime()->begin_frame();
                } else {
                    if (runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::VREarlyRHICommand) != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }
                }
            }

            return call_orig();
        };

        original_funcs[last_command] = last_command->func;
        last_command->func = func_override;
    }
};

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage1<N>;
    FillVtable<N - 1>::fill(table);
}

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill2(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage2<N>;
    FillVtable<N - 1>::fill2(table);
}

bool SceneViewExtensionAnalyzer::validate_cached_discovery(void** original_vtable, const nlohmann::json& cached) {
    if (!cached.is_object() || original_vtable == nullptr) {
        return false;
    }

    const auto cached_is_active = cached.value("is_active_this_frame_index", UINT32_MAX);
    const auto cached_begin = cached.value("begin_render_viewfamily_index", UINT32_MAX);
    const auto cached_pre_render = cached.value("pre_render_viewfamily_renderthread_index", UINT32_MAX);
    const auto cached_frame_count_offset = cached.value("frame_count_offset", UINT32_MAX);

    constexpr auto max_index = g_view_extension_vtable.size();

    if (cached_is_active >= max_index || cached_begin >= max_index || cached_pre_render >= max_index ||
        cached_begin == cached_pre_render || cached_frame_count_offset == 0 || cached_frame_count_offset > 0x4000)
    {
        return false;
    }

    const auto base_module = utility::get_module_within((void*)original_vtable[0]).value_or(nullptr);

    for (const auto index : {cached_is_active, cached_begin, cached_pre_render}) {
        const auto fn = original_vtable[index];

        if (fn == nullptr || IsBadReadPtr(fn, sizeof(void*))) {
            return false;
        }

        const auto fn_module = utility::get_module_within((void*)fn).value_or(nullptr);

        if (base_module != nullptr && fn_module != base_module) {
            return false;
        }
    }

    return true;
}

bool SceneViewExtensionAnalyzer::try_apply_cached_discovery(void** original_vtable) {
    if (!is_ue_5_7_or_newer()) {
        return false;
    }

    const auto cached = sdk::discovery_cache::load_entry(UE57_VIEW_EXTENSION_DISCOVERY_CACHE_KEY, utility::get_executable());

    if (!cached) {
        return false;
    }

    if (!validate_cached_discovery(original_vtable, *cached)) {
        sdk::discovery_cache::invalidate_entry(UE57_VIEW_EXTENSION_DISCOVERY_CACHE_KEY);
        return false;
    }

    functions.clear();
    total_call_count = 0;
    has_found_is_active_this_frame_index = true;
    has_found_begin_render_viewfamily = true;
    index_0_called = cached->value("index_0_called", false);
    is_active_this_frame_index = cached->value("is_active_this_frame_index", 0u);
    begin_render_viewfamily_index = cached->value("begin_render_viewfamily_index", 0u);
    pre_render_viewfamily_renderthread_index = cached->value("pre_render_viewfamily_renderthread_index", 0u);
    frame_count_offset = cached->value("frame_count_offset", 0u);
    sdk::FSceneViewFamily::set_frame_count_offset(frame_count_offset);

    FillVtable<g_view_extension_vtable.size() - 1>::fill2(g_view_extension_vtable);
    g_view_extension_vtable[is_active_this_frame_index] = (uintptr_t)+[](ISceneViewExtension* ext) -> bool {
        return true;
    };

    SPDLOG_INFO("[UE 5.7] Reused cached SceneViewExtension discovery");
    setup_view_extension_hook();
    return true;
}

void SceneViewExtensionAnalyzer::save_cached_discovery() {
    if (!is_ue_5_7_or_newer() || !has_found_is_active_this_frame_index || !has_found_begin_render_viewfamily || frame_count_offset == 0) {
        return;
    }

    sdk::discovery_cache::save_entry(UE57_VIEW_EXTENSION_DISCOVERY_CACHE_KEY, utility::get_executable(), {
        {"index_0_called", index_0_called},
        {"is_active_this_frame_index", is_active_this_frame_index},
        {"begin_render_viewfamily_index", begin_render_viewfamily_index},
        {"pre_render_viewfamily_renderthread_index", pre_render_viewfamily_renderthread_index},
        {"frame_count_offset", frame_count_offset}
    });
}

// 4.25something to 4.27
// TODO: Add support for all versions via PDB dumps
constexpr auto INIT_OPTIONS_OFFSET = 0x50;

bool FFakeStereoRenderingHook::is_in_viewport_client_draw() const {
    return m_in_viewport_client_draw && GameThreadWorker::get().is_same_thread();
}

// FSceneView constructor hook
sdk::FSceneView* FFakeStereoRenderingHook::sceneview_constructor(sdk::FSceneView* view, sdk::FSceneViewInitOptions* init_options, void* a3, void* a4) {
    SPDLOG_INFO_ONCE("Called FSceneView constructor for the first time");

    auto& vr = VR::get();

    if (!g_hook->is_in_viewport_client_draw() || !vr->is_hmd_active()) {
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    if (g_hook->m_analyzing_view_extensions || !g_hook->m_has_view_extensions_installed) {
        SPDLOG_INFO_ONCE("FSceneView constructor was called before view extensions were installed, aborting");
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    std::scoped_lock ___{g_hook->m_sceneview_data.mtx};

    const auto retaddr = (uintptr_t)_ReturnAddress();

    if (!g_hook->m_sceneview_data.seen_retaddrs.contains(retaddr)) {
        g_hook->m_sceneview_data.seen_retaddrs.insert(retaddr);
        SPDLOG_INFO("FSceneView constructor called from {:x}", retaddr);
    }

    sdk::FSceneViewInitOptionsBase::update_offsets(init_options);

    if (!is_ue_5_7_or_newer()) {
        if (auto view_family = init_options->get_view_family(); view_family != nullptr) {
            if (sdk::FSceneViewFamily::update_offsets(view_family, nullptr)) {
                g_hook->note_scene_view_family_offsets_ready();
            }
        }
    }

    const auto is_ue5 = g_hook->has_double_precision();
    auto init_options_ue5 = (sdk::FSceneViewInitOptionsUE5*)init_options;

    const auto init_options_scene_state = init_options->get_scene_state();

    if (init_options_scene_state != nullptr) {
        if (is_ue5) {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue5[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE5));
        } else {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue4[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE4));
        }
    }

    auto& known_scene_states = g_hook->m_sceneview_data.known_scene_states;
    auto& last_frame_count = g_hook->m_sceneview_data.last_frame_count;
    auto& last_index = g_hook->m_sceneview_data.last_index;

    if (last_frame_count != g_frame_count || last_index > 1) {
        last_index = 0;
    }

    last_frame_count = g_frame_count;

    const auto true_index = vr->is_using_afr() ? (g_frame_count + last_index) % 2 : last_index;

    if (vr->is_splitscreen_compatibility_enabled() || vr->is_sceneview_compatibility_enabled()) {
        int32_t w = vr->get_hmd_width();
        int32_t h = vr->get_hmd_height();

        int32_t x = 0;
        int32_t y = 0;

        if (!vr->is_using_afr() && true_index == 1 && !vr->is_native_stereo_fix_enabled()) {
            x += w;
        }

        FIntRect view_rect{x, y, x + w, y + h};

        vr->get_runtime()->update_matrices(0.1f, 10000.0f);

        const auto proj_mat = vr->get_projection_matrix((VRRuntime::Eye)(true_index));

        auto& init_options_view_origin = is_ue5 ? *(glm::vec3*)&init_options_ue5->view_origin : init_options->view_origin;
        auto& init_options_view_rect = is_ue5 ? init_options_ue5->view_rect : init_options->view_rect;
        auto& init_options_constrained_view_rect = is_ue5 ? init_options_ue5->constrained_view_rect : init_options->constrained_view_rect;
        auto& init_options_projection_matrix = init_options->projection_matrix;
        auto& init_options_projection_matrix_ue5 = init_options_ue5->projection_matrix;

        auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
        auto& init_options_view_rotation_matrix_ue5 = init_options_ue5->view_rotation_matrix;

        const auto conversion_mat = glm::mat4 {
            0, 0, 1, 0,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1
        };

        const auto conversion_mat_inverse = glm::inverse(conversion_mat);

        // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
        // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
        glm::vec3 euler{};

        if (is_ue5) {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * glm::mat4{init_options_view_rotation_matrix_ue5}));
        } else {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
        }

        auto euler_d = glm::vec<3, double>{euler};
        auto euler_pointer = is_ue5 ? (Rotator<float>*)&euler_d : (Rotator<float>*)&euler;

        float scene_world_to_meters = 100.0f;

        if (vr->is_sceneview_compatibility_enabled()) {
            // SceneView compatibility manually rebuilds the eye views here. Keep its legacy
            // 100uu/m basis so UE5.6+ world-scale changes do not flatten the stereo view.
            SPDLOG_INFO_ONCE("[SceneViewCompat] Using fixed 100.0 world-to-meters for manual view offset");
        } else {
            scene_world_to_meters = init_options->get_world_to_meters_scale().value_or(100.0f);
            if (!std::isfinite(scene_world_to_meters) || scene_world_to_meters <= 0.0f || scene_world_to_meters > 100000.0f) {
                scene_world_to_meters = 100.0f;
            }
        }

        g_hook->calculate_stereo_view_offset_(true_index + 1, euler_pointer, scene_world_to_meters, &init_options_view_origin);

        if (is_ue5) {
            euler = euler_d;
        }

        const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);

        *(FIntRect*)&init_options_view_rect = view_rect;
        *(FIntRect*)&init_options_constrained_view_rect = view_rect;

        if (is_ue5) {
            init_options_view_rotation_matrix_ue5 = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix_ue5 = proj_mat;
            }
        } else {
            init_options_view_rotation_matrix = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix = proj_mat;
            }
        }
    }

    const auto init_options_stereo_pass = init_options->get_stereo_pass();

    std::optional<uint32_t> views_original_count{};

    if (vr->is_native_stereo_fix_enabled() && vr->is_native_stereo_fix_same_pass_enabled() && init_options_stereo_pass > EStereoscopicPass::eSSP_PRIMARY) {
        if (g_hook->get_render_target_manager()->get_scene_capture_render_target() != nullptr) {
            init_options->set_stereo_pass(EStereoscopicPass::eSSP_PRIMARY);

            auto view_family = init_options->get_view_family();
            auto views = view_family != nullptr ? view_family->get_views() : nullptr;

            if (views != nullptr) {
                // Hide the fact that we have multiple views from the FSceneView constructor.
                // At least 1 view causes special stereo logic to run in the constructor.
                // Notably I've seen more than 1 view causing crashes on UE5 with the native stereo fix without doing this.
                views_original_count = views->count;
                views->count = 0;
            }
        }
    }

    bool new_scene_state_inserted_this_frame = false;

    if (init_options_scene_state != nullptr && !g_hook->m_sceneview_data.known_scene_states.contains(init_options_scene_state)) {
        SPDLOG_INFO("Inserting new scene state {:x}", (uintptr_t)init_options_scene_state);
        known_scene_states.insert(init_options_scene_state);
        new_scene_state_inserted_this_frame = true;
    } else if (init_options_scene_state == nullptr) {
        SPDLOG_ERROR_ONCE("Scene state passed to FSceneView constructor is null");

        if ((int32_t)init_options_stereo_pass < 0) {
            SPDLOG_ERROR_ONCE("Stereo pass is negative");
        }
    }

    if (init_options_scene_state != nullptr && !new_scene_state_inserted_this_frame && vr->is_ghosting_fix_enabled() && !known_scene_states.empty() && vr->is_using_afr() && true_index == 1) {
        init_options->set_stereo_pass(EStereoscopicPass::eSSP_PRIMARY);

        // Set the scene state to the one that isn't the current one
        for (auto scene_state : known_scene_states) {
            if (scene_state != init_options_scene_state) {
                SPDLOG_INFO_ONCE("Setting scene state to {:x}", (uintptr_t)scene_state);
                init_options->set_scene_state(scene_state);
                break;
            }
        }
    }

    last_index++;

    auto result = g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);

    // Reset the view count back to what it was.
    if (views_original_count.has_value()) {
        auto view_family = init_options->get_view_family();
        auto views = view_family != nullptr ? view_family->get_views() : nullptr;

        if (views != nullptr) {
            views->count = views_original_count.value();
        }
    }

    return result;
}

void FFakeStereoRenderingHook::setup_view_family(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("SetupViewFamily");

    static bool once = true;

    if (once) {
        SPDLOG_INFO("Called SetupViewFamily for the first time");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    //vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
}

void FFakeStereoRenderingHook::setup_viewpoint(ISceneViewExtension* extension, void* player_controller, void* view_info) {
    ZoneScopedN("SetupViewPoint");
    SPDLOG_INFO_ONCE("Called SetupViewPoint for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() || g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    // Using this as a way to get to the localplayer
    static bool attempted_hook{false};

    // Fix localplayer view count
    if (!attempted_hook) {
        SPDLOG_INFO("Attempting to find caller of ISceneViewExtension::SetupViewPoint");

        attempted_hook = true;
        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto caller = utility::find_virtual_function_start(return_address);

        if (!caller) {
            SPDLOG_ERROR("Failed to find caller of ISceneViewExtension::SetupViewPoint");
            return;
        }

        // No need to StartDisabled on this because we're on the same thread.
        g_hook->m_localplayer_get_viewpoint_hook = safetyhook::create_inline(*caller, (uintptr_t)&localplayer_setup_viewpoint);
        
        if (!g_hook->m_localplayer_get_viewpoint_hook) {
            SPDLOG_ERROR("Failed to hook ISceneViewExtension::SetupViewPoint");
            return;
        }

        SPDLOG_INFO("Hooked ISceneViewExtension::SetupViewPoint");
    }
}

void FFakeStereoRenderingHook::localplayer_setup_viewpoint(void* localplayer, void* view_info, void* pass) {
    ZoneScopedN("LocalPlayerSetupViewPoint");
    SPDLOG_INFO_ONCE("Called LocalPlayerSetupViewPoint for the first time");

    if (!g_hook->m_fixed_localplayer_view_count) {
        static bool attempted = false;

        if (!attempted) {
            attempted = true;

            if (localplayer != nullptr && !IsBadReadPtr(localplayer, sizeof(void*))) try {
                g_hook->post_init_properties((uintptr_t)localplayer);
            } catch(...) {
                SPDLOG_ERROR("[LocalPlayerSetupViewPoint] Failed to post init properties");
            }
        }
    }

    g_hook->m_localplayer_get_viewpoint_hook.call<void>(localplayer, view_info, pass);
}

void FFakeStereoRenderingHook::begin_render_viewfamily_real(void* render_module, sdk::FCanvas* canvas, sdk::FSceneViewFamily* view_family_candidate) {
    ZoneScopedN("BeginRenderViewFamilyReal");
    const auto begin_render_viewfamily_real_start = std::chrono::steady_clock::now();
    utility::ScopeGuard begin_render_viewfamily_real_timing_guard{[&]() {
        g_begin_render_viewfamily_real_timing.add(std::chrono::steady_clock::now() - begin_render_viewfamily_real_start);
        log_engine_render_timing_if_needed();
    }};

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamilyReal for the first time");

    if (!g_framework->is_game_data_intialized()) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    auto& vr = VR::get();
    auto rtm = g_hook->get_render_target_manager();

    if (!vr->is_hmd_active() || !vr->is_native_stereo_fix_enabled()) {
        avowed_native_fix_gate_reset("hmd inactive or native stereo fix disabled");
        rtm->destroy_scene_capture();

        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    struct TArrayViewViewFamily {
        sdk::FSceneViewFamily** data;
        uint32_t count;
    };

    const auto uses_tarrayview = sdk::FSceneViewFamily::has_vtable() && *(void**)view_family_candidate != sdk::FSceneViewFamily::get_vtable_ptr();
    const auto ue5_view_family_array = (TArrayViewViewFamily*)view_family_candidate;

    if (uses_tarrayview && ue5_view_family_array->data == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    // UE5 passes an TArrayView of ViewFamily pointers instead of a single ViewFamily
    sdk::FSceneViewFamily* view_family = uses_tarrayview ? ue5_view_family_array->data[0] : view_family_candidate;

    auto views_ptr = view_family->get_views();
    if (views_ptr == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    auto& views = *views_ptr;
    const auto prev_count = views.count;

    if (auto view_family_target = view_family->get_render_target(); view_family_target != nullptr) {
        g_hook->try_adopt_scene_viewport_render_target(
            reinterpret_cast<sdk::FViewport*>(view_family_target),
            "BeginRenderingViewFamily RenderTarget");
    }

    const auto rt = rtm->get_scene_capture_utexture();
    const auto rtrsrc = rt != nullptr ? (sdk::FTextureRenderTargetResource*)rt->get_resource() : nullptr;
    const auto rtfrt = rtrsrc != nullptr ? rtrsrc->as_render_target() : nullptr;
    const auto scene_capture_rhi = rtm->get_scene_capture_render_target();
    const auto scene_capture_native = avowed_try_get_native_resource(scene_capture_rhi);

    if (avowed_is_current_game()) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] BeginRenderViewFamilyReal: uses_tarrayview={} views={} scene_utexture={} resource={} render_target={} capture_rhi={:x} capture_native={:x} fixed_localplayer={}",
            uses_tarrayview,
            views.count,
            rt != nullptr,
            rtrsrc != nullptr,
            rtfrt != nullptr,
            (uintptr_t)scene_capture_rhi,
            scene_capture_native,
            g_hook->m_fixed_localplayer_view_count);
    }

    if (rtfrt == nullptr) {
        avowed_native_fix_gate_update(
            (uintptr_t)view_family->get_scene_interface(),
            0,
            (uintptr_t)scene_capture_rhi,
            scene_capture_native,
            false);

        // This is fine to call constantly because we use an in-flight render target
        // that gets unset after the texture is fully created. This function exits early otherwise.
        rtm->create_scene_capture();
        views.count = 1;
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        views.count = prev_count;
        return;
    }

    auto view_family_target = view_family->get_render_target();
    const auto view_family_scene = view_family->get_scene_interface();

    if (view_family_target == nullptr) {
        avowed_native_fix_gate_update(
            (uintptr_t)view_family_scene,
            0,
            (uintptr_t)scene_capture_rhi,
            scene_capture_native,
            false);

        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    uint32_t avowed_gate_stable_frames = 0;
    uint32_t avowed_gate_required_frames = AVOWED_NATIVE_FIX_STABLE_FRAMES;
    const auto avowed_gate_ready = avowed_native_fix_gate_update(
        (uintptr_t)view_family_scene,
        (uintptr_t)view_family_target,
        (uintptr_t)scene_capture_rhi,
        scene_capture_native,
        rtfrt != nullptr && scene_capture_rhi != nullptr,
        &avowed_gate_stable_frames,
        &avowed_gate_required_frames);

    bool wants_swap = false;
    if (views.count > 1) {
        views.count = 1;
        wants_swap = !avowed_is_current_game() || avowed_gate_ready;

        if (avowed_is_current_game() && !avowed_gate_ready) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Suppressing right-eye pass during render transition stabilization stable={}/{}",
                avowed_gate_stable_frames,
                avowed_gate_required_frames);
        }

        if (wants_swap) {
            auto runtime = vr->get_runtime();
            const auto frame_count = runtime->internal_frame_count;

            // We need to clone the VR state from last frame to this frame
            if (runtime->is_openxr()) {
                auto openxr = (runtimes::OpenXR*)runtime;
                std::scoped_lock __{ openxr->sync_assignment_mtx };

                const auto last_frame = (frame_count) % runtimes::OpenXR::QUEUE_SIZE;
                const auto now_frame = (frame_count + 1) % runtimes::OpenXR::QUEUE_SIZE;
                openxr->pipeline_states[now_frame] = openxr->pipeline_states[last_frame];
                openxr->pipeline_states[now_frame].frame_count = now_frame;
            } else {
                auto openvr = (runtimes::OpenVR*)runtime;
                std::unique_lock __{ openvr->pose_mtx };

                const auto last_frame = (frame_count) % openvr->pose_queue.size();
                const auto now_frame = (frame_count + 1) % openvr->pose_queue.size();
                openvr->pose_queue[now_frame] = openvr->pose_queue[last_frame];
            }
        }

        /*auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[0] + INIT_OPTIONS_OFFSET);
        init_options->stereo_pass = 0;

        auto init_options2 = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[1] + INIT_OPTIONS_OFFSET);
        init_options2->stereo_pass = 0;

        std::array<uint8_t, 0x500> init_options_copy{};
        std::array<uint8_t, 0x500> init_options_copy2{};

        memcpy(init_options_copy.data(), init_options, 0x500);
        view_family.views.data[0]->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well

        memcpy(init_options_copy2.data(), init_options2, 0x500);
        view_family.views.data[1]->constructor((sdk::FSceneViewInitOptions*)init_options_copy2.data());*/
    }

    g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);

    if (wants_swap) {
        if (avowed_is_current_game()) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[Avowed][NativeStereoFix] Executing right-eye second render pass into scene capture target");
        }

        // Swap out the existing render target for our custom one
        // Also, the entire point of swapping the render target
        // instead of "just" re-using the existing one is that doing that causes a 90% FPS drop
        // because the engine is still working on the old render target
        const auto original_target = view_family_target;

        view_family->set_render_target(rtfrt);

        auto scene = (sdk::FScene*)view_family->get_scene_interface();

        if (scene != nullptr) {
            // We decrement the frame count because it fixes motion vectors in the right eye.
            scene->decrement_frame_count();
        }
        
        std::swap(views[0], views[1]);

        // Call it again
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);

        std::swap(views[0], views[1]);

        view_family->set_render_target(original_target);
    }

    views.count = prev_count;
}

void FFakeStereoRenderingHook::begin_render_viewfamily(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("BeginRenderViewFamily");
    const auto begin_render_viewfamily_start = std::chrono::steady_clock::now();
    utility::ScopeGuard begin_render_viewfamily_timing_guard{[&]() {
        g_begin_render_viewfamily_timing.add(std::chrono::steady_clock::now() - begin_render_viewfamily_start);
        log_engine_render_timing_if_needed();
    }};

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamily for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    if (!g_hook->has_scene_view_family_offsets_ready() &&
        sdk::FSceneViewFamily::update_offsets(&view_family, g_hook->get_render_target_manager()->get_viewport()))
    {
        g_hook->note_scene_view_family_offsets_ready();
    }

    if (auto view_family_target = view_family.get_render_target(); view_family_target != nullptr) {
        g_hook->try_adopt_scene_viewport_render_target(
            reinterpret_cast<sdk::FViewport*>(view_family_target),
            "FSceneViewFamily::RenderTarget");
    }

    auto si = view_family.get_scene_interface();

    if (si != nullptr) {
        sdk::FScene::update_offsets((sdk::FScene*)si);
    }

    if (!g_hook->has_engine_tick_hook()) {
        // Alternative place of running game thread work.
        GameThreadWorker::get().execute();
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    auto views_ptr = view_family.get_views();

    auto runtime = vr->get_runtime();
    runtime->internal_frame_count = frame_count;
    runtime->on_pre_render_game_thread(frame_count);

    // This is a HACKHACKHACK to get splitscreen working on around 4.20 to 4.27 something
    // This is completely borked on UE5
    // We can probably do it better inside the sceneview constructor hook, but that needs to be handled with care
    if (vr->is_splitscreen_compatibility_enabled() && views_ptr != nullptr) {
        auto& views = *views_ptr;
        
        // B = dst, A = src
        static auto copy_init_options_from = [](const sdk::FSceneView& a, sdk::FSceneView& b) {
            std::scoped_lock _{g_hook->m_sceneview_data.mtx};
            auto init_options_a = (sdk::FSceneViewInitOptions*)((uintptr_t)&a + INIT_OPTIONS_OFFSET);
            auto init_options_b = (sdk::FSceneViewInitOptions*)((uintptr_t)&b + INIT_OPTIONS_OFFSET);

            auto& cached_init_options = g_hook->m_sceneview_data.view_init_options_ue4;

            if (auto it = cached_init_options.find(init_options_a->scene_view_state); it != cached_init_options.end()) {
                const auto& vio_entry = it->second;
                //memcpy(init_options_b, &vio_entry, sizeof(sdk::FSceneViewInitOptionsUE4));
                init_options_b->view_origin = vio_entry.view_origin;
                init_options_b->view_rotation_matrix = vio_entry.view_rotation_matrix;
                *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&vio_entry.view_rect;
                *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&vio_entry.constrained_view_rect;
                init_options_b->projection_matrix = vio_entry.projection_matrix;
                return;
            }

            // Otherwise just do this crap
            init_options_b->view_origin = init_options_a->view_origin;
            init_options_b->view_rotation_matrix = init_options_a->view_rotation_matrix;
            *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&init_options_a->view_rect;
            *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&init_options_a->constrained_view_rect;
            init_options_b->projection_matrix = init_options_a->projection_matrix;
        };

        auto do_splitscreen = [&](int32_t view_index) {
            int32_t w = vr->get_hmd_width();
            int32_t h = vr->get_hmd_height();

            int32_t x = 0;
            int32_t y = 0;

            const auto true_index = vr->is_using_afr() ? (frame_count + 1) % 2 : view_index;

            if (!vr->is_using_afr() && true_index == 1) {
                x += w;
            }

            auto view = views.data[view_index % views.count];

            FIntRect view_rect{x, y, x + w, y + h};

            auto& vr = VR::get();

            VR::get()->get_runtime()->update_matrices(0.1f, 10000.0f);

            const auto proj_mat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));

            std::array<uint8_t, 0x500> init_options_copy{};

            auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view + INIT_OPTIONS_OFFSET);

            auto& init_options_view_origin = init_options->view_origin;
            auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
            auto& init_options_view_rect = *(FIntRect*)&init_options->view_rect;
            auto& init_options_constrained_view_rect = *(FIntRect*)&init_options->constrained_view_rect;
            auto& init_options_projection_matrix = init_options->projection_matrix;
            auto& init_options_stereo_pass = init_options->stereo_pass;

            // ADDENDUM: The sceneview constructor hook handles the rotation logic now.
            /*const auto conversion_mat = glm::mat4 {
                0, 0, 1, 0,
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 0, 1
            };

            const auto conversion_mat_inverse = glm::inverse(conversion_mat);*/

            // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
            // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
            //auto euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
            //g_hook->calculate_stereo_view_offset_(true_index + 1, (Rotator<float>*)&euler, 100.0f, &init_options_view_origin);
            //const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);
            //init_options_view_rotation_matrix = view_rot_mat;

            init_options_view_rect = view_rect;
            init_options_constrained_view_rect = view_rect;
            init_options_projection_matrix = proj_mat;

            memcpy(init_options_copy.data(), init_options, 0x500);
            view->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well
        };

        const auto requested_index = vr->get_requested_splitscreen_index();
        const auto final_index = std::min<uint32_t>(views.count - 1, requested_index);
        const auto other_index = final_index != 0 ? 0 : 1;

        if (final_index > 0) {
            if (views.count > 1) {
                copy_init_options_from(*views.data[final_index], *views.data[other_index]);
            }

            if (!vr->is_using_afr()) {
                if (views.count > 1) {
                    do_splitscreen(other_index);
                } else {
                    do_splitscreen(0);
                }
            } else {
                do_splitscreen(0);
            }
        }
    }

    // If we couldn't find GetDesiredNumberOfViews, we need to set the view count to 1 as a workaround
    // TODO: Check if this can cause a memory leak, I don't know who is resonsible
    // for destroying the views in the array
    // This check might seem kind of arbitrary, but sometimes (rarely) the offset
    // for the views can be wrong so if the count is some sane number
    // then we can assume that the offset is correct
    if (vr->is_using_afr() && views_ptr != nullptr && views_ptr->count >= 2 && views_ptr->count <= 4) {
        SPDLOG_INFO_ONCE("Setting view count to 1 (from {})", views_ptr->count);
        views_ptr->count = 1;
    }


    using BeginRenderViewFamilyRealFn = void(*)(void*, sdk::FCanvas*, sdk::FSceneViewFamily*);
    static BeginRenderViewFamilyRealFn begin_rendering_view_family_real_fn = nullptr;
    static bool already_tried = false;
    if (begin_rendering_view_family_real_fn == nullptr && !already_tried && vr->is_native_stereo_fix_enabled()) {
        already_tried = true;

        // Get callstack
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
        uintptr_t mid = 0;

        for (int i = 1; i < depth; i++) {
            SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
            mid = stack[i];
            break;
        }

        if (mid != 0) {
            const auto candidate = utility::find_virtual_function_start(mid);

            if (candidate) {
                begin_rendering_view_family_real_fn = (BeginRenderViewFamilyRealFn)*candidate;

                if (begin_rendering_view_family_real_fn != nullptr) {
                    SPDLOG_INFO("Found BeginRenderingViewFamily real function at {:x}", (uintptr_t)begin_rendering_view_family_real_fn);

                    g_hook->m_render_module_begin_render_viewfamily_hook = safetyhook::create_inline((uintptr_t)begin_rendering_view_family_real_fn, (uintptr_t)&begin_render_viewfamily_real);

                    if (g_hook->m_render_module_begin_render_viewfamily_hook) {
                        SPDLOG_INFO("Hooked BeginRenderingViewFamily real function");
                    } else {
                        SPDLOG_ERROR("Failed to hook BeginRenderingViewFamily real function");
                    }
                } else {
                    SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
                }
            } else {
                SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
            }
        }
    }
}

void FFakeStereoRenderingHook::pre_render_viewfamily_renderthread(ISceneViewExtension* extension, sdk::FRHICommandListBase* cmd_list, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("PreRenderViewFamily_RenderThread");
    const auto prerender_viewfamily_rt_start = std::chrono::steady_clock::now();
    utility::ScopeGuard prerender_viewfamily_rt_timing_guard{[&]() {
        g_prerender_viewfamily_rt_timing.add(std::chrono::steady_clock::now() - prerender_viewfamily_rt_start);
        log_engine_render_timing_if_needed();
    }};

    utility::ScopeGuard _{[]() {
        RenderThreadWorker::get().execute();
    }};
    
    SPDLOG_INFO_ONCE("Called PreRenderViewFamily_RenderThread for the first time");
    
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    g_hook->note_prerender_viewfamily_seen();

    static size_t execution_count{0};

    // This should 100% only get executed if the headset is on, because
    // FFakeStereoRenderingHook::render_texture_render_thread is the first fallback for hooking
    // And we don't want to miss that unintentionally
    if (g_hook->m_attempted_hook_slate_thread && !g_hook->m_slate_thread_hook && !g_hook->m_attempted_hook_slate_thread_alternate && execution_count++ >= 50) {
        SPDLOG_INFO("DrawWindow_RenderThread was not hooked after {} render calls, trying alternative hook", execution_count);

        g_hook->attempt_hook_slate_thread(0, true);
    }

    if (vr->is_stereo_emulation_enabled()) {
        return;
    }

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    static uint32_t last_frame = 0;

    // We only want to run this logic on the first "frame" (left eye) passed through here
    // When using Native Stereo Fix
    if (vr->is_native_stereo_fix_enabled() && frame_count == last_frame) {
        return;
    }

    last_frame = frame_count;

    static bool is_ue5_rdg_builder = false;
    static uint32_t ue5_command_offset = 0;
    static bool analyzed_root_already = false;
    static bool is_old_command_base = false;

    if (is_ue5_rdg_builder) {
        cmd_list = *(sdk::FRHICommandListBase**)((uintptr_t)cmd_list + ue5_command_offset);
    }

    const auto compensation = g_hook->get_frame_delay_compensation();

    // Using slate's draw window hook is the safest way to do this without
    // false positives on the command list in this function
    // otherwise we can attempt to use the command list here and hook it
    // in the slate hook, a guaranteed proper command list is passed to the function
    // so we can use that to hook the command list
    // The main inspiration for this is UE5.0.3 because it passes an FRDGBuilder
    // which *does* contain the command list in it, but for whatever reason I can't
    // seem to hook it properly, so I'm using the slate hook instead
    // ADDENDUM: For now, I'm only using the slate hook for UE5.0.3.
    // But I'll use it as a fallback as well for when the command list appears to be empty
    // Reason being the slate hook doesn't appear to run every frame, so it's not a perfect solution
    auto enqueue_poses_on_slate_thread = [&]() {
        g_hook->get_slate_thread_worker()->enqueue([=](FRHICommandListImmediate* command_list) {
            static bool once_slate = true;

            if (once_slate) {
                SPDLOG_INFO("Called enqueued function on the Slate thread for the first time! Frame count: {}", frame_count);
                once_slate = false;
            }

            static size_t actual_offset = 0;
            auto l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + actual_offset);
            const auto is_ue5 = g_hook->has_double_precision();

            if (l != nullptr && l->root != nullptr && ((uintptr_t)l->root & (sizeof(void*) - 1)) == 0) {
                auto new_root = (sdk::FRHICommandBase_New*)l->root;
                if (!analyzed_root_already) try {
                    // so all of this might seem really overkill but
                    // it's a good way to detect whether we have an FMemStack at the top of the command list
                    // which we need to skip on UE5.5+
                    if (utility::get_module_within(*(void**)l->root).value_or(nullptr) == nullptr || 
                        IsBadReadPtr(*(void**)l->root, sizeof(void*)) || 
                        utility::get_module_within(**(void***)l->root).value_or(nullptr) == nullptr ||
                        (!IsBadReadPtr(new_root->next, sizeof(void*)) && (utility::get_module_within(*(void**)new_root->next).value_or(nullptr) == nullptr || utility::get_module_within(**(void***)new_root->next).value_or(nullptr) == nullptr))
                    )
                {
                        if (is_ue5) {
                            // UE5 is NOT an old command list, we need to bruteforce the offset
                            // Start at 0x10 because that's usually where the pointers in FMemStack end.
                            for (size_t i = 0x10; i < 0x50; i += sizeof(void*)) try {
                                const auto cur_l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + i);
                                if (utility::get_module_within(*(void**)cur_l->root).value_or(nullptr) != nullptr) {
                                    actual_offset = i;
                                    l = cur_l;
                                    SPDLOG_INFO("Found UE5.5+ command list at offset 0x{:x}", i);
                                    break;
                                }
                            } catch(...) {

                            }
                        } else {
                            SPDLOG_INFO("Old FRHICommandBase detected");
                            is_old_command_base = true;
                        }
                    } else {
                        SPDLOG_INFO("New FRHICommandBase detected");
                    }

                    analyzed_root_already = true;
                } catch(...) {
                    SPDLOG_ERROR("Failed to analyze FRHICommandBase");
                    analyzed_root_already = true;
                }

                if (!is_old_command_base) {
                    SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)l->root, frame_count + compensation);
                } else {
                    SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)l->root, frame_count + compensation);
                }
            } else {
                // welp
                vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
            }
        });
    };

    auto fall_back_to_slate_thread = [&]() {
        if (is_ue_5_7_or_newer()) {
            g_hook->m_prefer_slate_thread_for_session = true;
            save_ue57_slate_thread_preference(true);
        }

        if (is_ue_5_7_or_newer()) {
            g_hook->get_render_target_manager()->try_schedule_dedicated_ui_creation();
        }

        if (g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else {
            vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
        }
    };

    if (is_ue_5_7_or_newer() &&
        g_hook->has_slate_hook() &&
        g_hook->has_seen_stable_slate_draw() &&
        !g_hook->has_successful_command_list_hijack() &&
        !g_hook->m_prefer_slate_thread_for_session &&
        g_hook->m_first_stable_slate_draw_at.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - g_hook->m_first_stable_slate_draw_at > std::chrono::seconds(2))
    {
        SPDLOG_WARN_ONCE("[UE 5.7] Command-list path did not stabilize after the first Slate draw; preferring Slate-thread startup");
        fall_back_to_slate_thread();
        return;
    }

    if (is_ue_5_7_or_newer() && g_hook->m_prefer_slate_thread_for_session && g_hook->has_slate_hook()) {
        enqueue_poses_on_slate_thread();
        return;
    }

    // okay well I think this evaluates to false all the time
    // but apparently it has been working for a LONG TIME so I'm not going to touch this until after release
    // (the else statement still handles everything... fine?)
    const auto has_good_root = 
        cmd_list != nullptr &&
        ((uintptr_t)cmd_list & 1 == 0) &&
        cmd_list->root != nullptr &&
        ((uintptr_t)cmd_list->root & 1 == 0);

    // Hijack the top command in the command list so we can enqueue the render poses on the RHI thread
    if (has_good_root) {
        SPDLOG_INFO_ONCE("Command list root is good");

        if (!analyzed_root_already) try {
            auto root = cmd_list->root;

            auto analyze_for_ue5 = [&]() {
                // Find the real command list.
                is_ue5_rdg_builder = true;
                const auto rdg_builder = (uintptr_t)cmd_list;

                for (auto i = 0x10; i <= 0x100; i += sizeof(void*)) try {
                    const auto value = *(uintptr_t*)(rdg_builder + i);

                    if (value == 0 || IsBadReadPtr((void*)value, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value).has_value()) {
                        continue;
                    }

                    const auto value_deref = *(uintptr_t*)value;

                    if (value_deref == 0 || IsBadReadPtr((void*)value_deref, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value_deref).has_value()) {
                        continue;
                    }

                    const auto root_vtable = *(uintptr_t*)value_deref;

                    if (root_vtable == 0 || IsBadReadPtr((void*)root_vtable, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)root_vtable).has_value()) {
                        continue;
                    }

                    // Check that there is a valid function in the vtable
                    const auto first_function = *(uintptr_t*)root_vtable;

                    if (first_function == 0 || IsBadReadPtr((void*)first_function, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)first_function).has_value()) {
                        continue;
                    }

                    SPDLOG_INFO("Possible UE5 command list found at offset 0x{:x}", i);
                    ue5_command_offset = i;
                    cmd_list = (sdk::FRHICommandListBase*)value;
                    break;
                } catch(...) {
                    spdlog::error("Exception occurred while analyzing UE5 command list");
                }
            };

            // If we read the pointer at the start of the root and it's not a module, then it's the old FRHICommandBase
            // this is because all vtables reside within a module
            if (utility::get_module_within(*(void**)root).value_or(nullptr) == nullptr) {
                // UE5
                if (g_hook->has_double_precision()) {
                    analyze_for_ue5();

                    if (ue5_command_offset == 0) {
                        SPDLOG_ERROR("Failed to find UE5 command list, trying again next frame");
                        return;
                    }
                } else {
                    SPDLOG_INFO("Old FRHICommandBase detected");
                    is_old_command_base = true;
                }
            } else {
                SPDLOG_INFO("New FRHICommandBase detected");
            }

            analyzed_root_already = true;
        } catch(...) {
            SPDLOG_ERROR("Failed to analyze root command");
            analyzed_root_already = true;
        }

        if (g_hook->get_render_target_manager()->is_ue_5_0_3() && g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else try {
            if (!is_old_command_base) {
                SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)cmd_list->root, frame_count + compensation);
            } else {
                SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)cmd_list->root, frame_count + compensation);
            }
        } catch(...) {
            SPDLOG_INFO_ONCE("Failed to hook command list, falling back to Slate thread hook");
            fall_back_to_slate_thread();
        }
    } else {
        SPDLOG_INFO_ONCE("Bad root or command list, falling back to Slate thread hook");
        fall_back_to_slate_thread();
    }
}

bool FFakeStereoRenderingHook::setup_view_extensions() try {
    SPDLOG_INFO("Attempting to set up view extensions...");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot set up view extensions!");
        return false;
    }

    const auto active_stereo_device = locate_active_stereo_rendering_device();

    if (!active_stereo_device || !s_stereo_rendering_device_offset) {
        SPDLOG_ERROR("Failed to locate active stereo rendering device!");
        return false;
    }

    // This is a proof of concept at the moment for newer UE versions
    // older versions may not work or crash.
    // TODO: Figure out older versions.
    constexpr auto weak_ptr_size = sizeof(TWeakPtr<void*>);
    static const auto potential_hmd_device_offset = s_stereo_rendering_device_offset + weak_ptr_size;
    static const uintptr_t potential_hmd_device = (uintptr_t)engine + potential_hmd_device_offset;
    static const uintptr_t potential_view_extensions = (uintptr_t)engine + s_stereo_rendering_device_offset + (weak_ptr_size * 2); // 2 to skip over the XRSystem

    // This can happen if the game left a VR plugin in it
    // Usually this isn't an issue, but some games can leave a valid HMDDevice or XRSystem laying around for whatever reason
    // If this isn't cleaned up, the game will crash because it tries to gather view extensions from the existing device
    // and the view extensions it gathered will cause a crash when calling them. also the HMD device itself can cause a crash, it's not actually initialized.
    if (*(void**)potential_hmd_device != nullptr) {
        // Double check that we're actually replacing a pointer and not an integer or something
        if (!IsBadReadPtr(*(void**)potential_hmd_device, sizeof(void*))) {
            SPDLOG_INFO("Found an existing HMDDevice or XRSystem, nullifying it...");
            static std::vector<uintptr_t> replacement_vtable{};

            for (auto i = 0; i < 200; ++i) {
                replacement_vtable.push_back((uintptr_t)+[]() { return nullptr; });
            }

            //**(void***)potential_hmd_device = replacement_vtable.data();
            *(void**)potential_hmd_device = nullptr;
            m_fixed_localplayer_view_count = true; // If this is already allocated, then there's already a second view for us to use
        }

        if (!IsBadReadPtr(*(void**)(potential_hmd_device + sizeof(void*)), sizeof(void*))) {
            *(void**)(potential_hmd_device + sizeof(void*)) = nullptr;
        }
    }

    m_tracking_system_hook = std::make_unique<IXRTrackingSystemHook>(this, potential_hmd_device_offset);
    m_components.push_back(m_tracking_system_hook.get());

    // Add a vectored exception handler that catches attempted dereferences of a null XRSystem or HMDDevice
    // The exception handler will then patch out the instructions causing the crash and continue execution
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS exception) -> LONG {
        static std::vector<Patch::Ptr> xrsystem_patches{};
        static std::unordered_set<uintptr_t> ignored_addresses{};

        if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            const auto exception_address = exception->ContextRecord->Rip;

            const auto daysgone_current = daysgone_is_current_game();

            if (ignored_addresses.contains(exception_address)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (exception_address == 0) {
                SPDLOG_INFO("[Exception Handler] Exception address is null");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (IsBadReadPtr((void*)exception_address, sizeof(void*))) {
                SPDLOG_INFO("[Exception Handler] Bad read pointer at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto decoded = utility::decode_one((uint8_t*)exception_address);

            if (!decoded) {
                SPDLOG_ERROR("[Exception Handler] Failed to decode instruction at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto& op2 = decoded->Operands[1];

            if (decoded->OperandsCount != 2 || 
                 op2.Type != ND_OP_MEM      || 
                !op2.Info.Memory.HasBase)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Encountered attempted dereference of null pointer at {:x}", exception_address);

            const auto fault_target = exception->ExceptionRecord->NumberParameters > 1
                ? static_cast<uintptr_t>(exception->ExceptionRecord->ExceptionInformation[1])
                : std::numeric_limits<uintptr_t>::max();

            const auto is_rax_base = [](auto reg) {
                return reg == NDR_RAX || reg == NDR_EAX;
            };

            const auto reg_to_index = [](auto reg) -> std::optional<uint8_t> {
                if (reg == NDR_RAX || reg == NDR_EAX) return 0;
                if (reg == NDR_RCX || reg == NDR_ECX) return 1;
                if (reg == NDR_RDX || reg == NDR_EDX) return 2;
                if (reg == NDR_RBX || reg == NDR_EBX) return 3;
                if (reg == NDR_RSP || reg == NDR_ESP) return 4;
                if (reg == NDR_RBP || reg == NDR_EBP) return 5;
                if (reg == NDR_RSI || reg == NDR_ESI) return 6;
                if (reg == NDR_RDI || reg == NDR_EDI) return 7;
                if (reg == NDR_R8 || reg == NDR_R8D) return 8;
                if (reg == NDR_R9 || reg == NDR_R9D) return 9;
                if (reg == NDR_R10 || reg == NDR_R10D) return 10;
                if (reg == NDR_R11 || reg == NDR_R11D) return 11;
                if (reg == NDR_R12 || reg == NDR_R12D) return 12;
                if (reg == NDR_R13 || reg == NDR_R13D) return 13;
                if (reg == NDR_R14 || reg == NDR_R14D) return 14;
                if (reg == NDR_R15 || reg == NDR_R15D) return 15;
                return std::nullopt;
            };

            auto make_zero_register_patch = [&reg_to_index](auto reg) -> std::vector<int16_t> {
                const auto index = reg_to_index(reg);

                if (!index) {
                    return {};
                }

                const auto dest = *index;
                std::vector<int16_t> patch{};

                if (dest < 8) {
                    patch.push_back(0x31);
                    patch.push_back(static_cast<int16_t>(0xC0 | (dest << 3) | dest));
                } else {
                    patch.push_back(0x45);
                    patch.push_back(0x31);
                    const auto r = static_cast<uint8_t>(dest - 8);
                    patch.push_back(static_cast<int16_t>(0xC0 | (r << 3) | r));
                }

                return patch;
            };

            auto set_context_register = [&reg_to_index](CONTEXT* context, auto reg, DWORD64 value) -> bool {
                const auto index = reg_to_index(reg);

                if (!index) {
                    return false;
                }

                switch (*index) {
                case 0: context->Rax = value; return true;
                case 1: context->Rcx = value; return true;
                case 2: context->Rdx = value; return true;
                case 3: context->Rbx = value; return true;
                case 4: context->Rsp = value; return true;
                case 5: context->Rbp = value; return true;
                case 6: context->Rsi = value; return true;
                case 7: context->Rdi = value; return true;
                case 8: context->R8 = value; return true;
                case 9: context->R9 = value; return true;
                case 10: context->R10 = value; return true;
                case 11: context->R11 = value; return true;
                case 12: context->R12 = value; return true;
                case 13: context->R13 = value; return true;
                case 14: context->R14 = value; return true;
                case 15: context->R15 = value; return true;
                default: return false;
                }
            };

            const auto exception_module = utility::get_module_within(exception_address).value_or(nullptr);
            const auto executable_base = reinterpret_cast<uintptr_t>(utility::get_executable());
            const auto exception_rva =
                exception_module == utility::get_executable() && executable_base != 0 && exception_address >= executable_base
                    ? exception_address - executable_base
                    : 0;

            const auto is_daysgone_fname_block_lookup =
                daysgone_current &&
                exception_module == utility::get_executable() &&
                exception_rva == 0x19fd30a &&
                exception->ContextRecord->Rax == 0 &&
                decoded->Operands[0].Type == ND_OP_REG &&
                decoded->Operands[0].Info.Register.Reg == NDR_RDX &&
                std::string_view{decoded->Mnemonic}.starts_with("MOV") &&
                op2.Type == ND_OP_MEM &&
                op2.Info.Memory.HasBase &&
                is_rax_base(op2.Info.Memory.Base);

            if (is_daysgone_fname_block_lookup) {
                constexpr uintptr_t daysgone_fname_to_string_return_rva = 0x19fd370;

                SPDLOG_WARN_ONCE(
                    "[DaysGone] Recovering invalid UE4 FName block lookup at RVA 0x19fd30a; returning empty name string");

                exception->ContextRecord->Rax = exception->ContextRecord->Rbx;
                exception->ContextRecord->Rip = executable_base + daysgone_fname_to_string_return_rva;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            const auto is_daysgone_view_extension_null_chain =
                daysgone_current &&
                exception_module == utility::get_executable() &&
                fault_target <= 0x20 &&
                decoded->Operands[0].Type == ND_OP_REG &&
                std::string_view{decoded->Mnemonic}.starts_with("MOV") &&
                op2.Info.Memory.HasDisp &&
                is_rax_base(op2.Info.Memory.Base) &&
                (op2.Info.Memory.Disp == 0x8 || op2.Info.Memory.Disp == 0x10);

            if (is_daysgone_view_extension_null_chain) {
                auto patch_bytes = make_zero_register_patch(decoded->Operands[0].Info.Register.Reg);

                if (!patch_bytes.empty() && patch_bytes.size() <= decoded->Length) {
                    for (size_t i = patch_bytes.size(); i < decoded->Length; ++i) {
                        patch_bytes.push_back(0x90);
                    }

                    SPDLOG_WARN(
                        "[DaysGone] Patching UE4.10/4.11 SceneViewExtension null chain at {:x}: fault={:x} disp={:x}",
                        exception_address,
                        fault_target,
                        op2.Info.Memory.Disp);

                    xrsystem_patches.push_back(Patch::create(exception_address, patch_bytes));
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                SPDLOG_ERROR(
                    "[DaysGone] Failed to create safe zero-register patch at {:x}; patch_len={} instr_len={}",
                    exception_address,
                    patch_bytes.size(),
                        decoded->Length);
            }

            const auto is_rcx_base = [](auto reg) {
                return reg == NDR_RCX || reg == NDR_ECX;
            };

            const auto is_rbx_base = [](auto reg) {
                return reg == NDR_RBX || reg == NDR_EBX;
            };

            const auto is_daysgone_null_projection_source =
                daysgone_current &&
                exception_module == utility::get_executable() &&
                fault_target == 0 &&
                decoded->Operands[0].Type == ND_OP_REG &&
                decoded->Operands[0].Info.Register.Reg == NDR_RAX &&
                op2.Type == ND_OP_MEM &&
                op2.Info.Memory.HasBase &&
                is_rcx_base(op2.Info.Memory.Base) &&
                (!op2.Info.Memory.HasDisp || op2.Info.Memory.Disp == 0);

            if (is_daysgone_null_projection_source) {
                const auto next_instruction_addr = exception_address + decoded->Length;
                const auto next_instruction = utility::decode_one((uint8_t*)next_instruction_addr);
                const auto is_expected_vcall =
                    next_instruction &&
                    std::string_view{next_instruction->Mnemonic}.starts_with("CALL") &&
                    next_instruction->OperandsCount >= 1 &&
                    next_instruction->Operands[0].Type == ND_OP_MEM &&
                    next_instruction->Operands[0].Info.Memory.HasBase &&
                    next_instruction->Operands[0].Info.Memory.Base == NDR_RAX &&
                    next_instruction->Operands[0].Info.Memory.HasDisp &&
                    next_instruction->Operands[0].Info.Memory.Disp == 0x20;

                if (is_expected_vcall) {
                    alignas(16) static const std::array<float, 20> daysgone_fallback_matrix{
                        1.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 1.0f,
                        0.0f, 0.0f, 0.0f, 0.0f,
                    };

                    SPDLOG_WARN(
                        "[DaysGone] Recovering null projection-source call at {:x}; using fallback matrix and skipping vcall {:x}",
                        exception_address,
                        next_instruction_addr);

                    exception->ContextRecord->Rax = reinterpret_cast<DWORD64>(daysgone_fallback_matrix.data());
                    exception->ContextRecord->Rip = next_instruction_addr + next_instruction->Length;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                SPDLOG_ERROR(
                    "[DaysGone] Null projection-source pattern at {:x} did not match expected vcall",
                    exception_address);
            }

            const auto is_daysgone_null_texture_output_ref =
                daysgone_current &&
                exception_module == utility::get_executable() &&
                fault_target == 0x10 &&
                decoded->Operands[0].Type == ND_OP_REG &&
                op2.Type == ND_OP_MEM &&
                op2.Info.Memory.HasBase &&
                is_rbx_base(op2.Info.Memory.Base) &&
                op2.Info.Memory.HasDisp &&
                op2.Info.Memory.Disp == 0x10;

            if (is_daysgone_null_texture_output_ref) {
                const auto next_instruction_addr = exception_address + decoded->Length;
                const auto next_instruction = utility::decode_one((uint8_t*)next_instruction_addr);
                const auto next_base_is_expected = [&]() {
                    if (!next_instruction ||
                        next_instruction->OperandsCount < 2 ||
                        next_instruction->Operands[1].Type != ND_OP_MEM ||
                        !next_instruction->Operands[1].Info.Memory.HasBase)
                    {
                        return false;
                    }

                    const auto base = next_instruction->Operands[1].Info.Memory.Base;
                    return base == NDR_R12 || base == NDR_R13 || base == NDR_R14 || base == NDR_R15 || base == NDR_RBP;
                }();
                const auto is_expected_followup =
                    next_instruction &&
                    std::string_view{next_instruction->Mnemonic}.starts_with("MOV") &&
                    next_instruction->OperandsCount >= 2 &&
                    next_instruction->Operands[0].Type == ND_OP_REG &&
                    next_instruction->Operands[1].Type == ND_OP_MEM &&
                    next_instruction->Operands[1].Info.Memory.HasBase &&
                    next_base_is_expected &&
                    next_instruction->Operands[1].Info.Memory.HasDisp &&
                    next_instruction->Operands[1].Info.Memory.Disp == 0x28;

                if (is_expected_followup &&
                    set_context_register(exception->ContextRecord, decoded->Operands[0].Info.Register.Reg, 0))
                {
                    SPDLOG_WARN(
                        "[DaysGone] Recovering null post-process texture output ref at {:x}; dest_reg={} next_base={} using null resource ref",
                        exception_address,
                        (int)decoded->Operands[0].Info.Register.Reg,
                        (int)next_instruction->Operands[1].Info.Memory.Base);

                    exception->ContextRecord->Rip = next_instruction_addr;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                SPDLOG_ERROR(
                    "[DaysGone] Null texture-output-ref pattern at {:x} did not match expected followup",
                    exception_address);
            }

            const auto is_daysgone_null_scene_render_target_output_ref =
                daysgone_current &&
                exception_module == utility::get_executable() &&
                fault_target == 0x10 &&
                decoded->Operands[0].Type == ND_OP_REG &&
                op2.Type == ND_OP_MEM &&
                op2.Info.Memory.HasBase &&
                (is_rcx_base(op2.Info.Memory.Base) || is_rax_base(op2.Info.Memory.Base)) &&
                op2.Info.Memory.HasDisp &&
                op2.Info.Memory.Disp == 0x10;

            if (is_daysgone_null_scene_render_target_output_ref) {
                const auto previous_instruction = utility::resolve_instruction(exception_address - 1);
                const auto scene_target_slot = [&]() -> std::optional<int64_t> {
                    if (!previous_instruction ||
                        !std::string_view{previous_instruction->instrux.Mnemonic}.starts_with("MOV") ||
                        previous_instruction->instrux.OperandsCount < 2 ||
                        previous_instruction->instrux.Operands[0].Type != ND_OP_REG ||
                        previous_instruction->instrux.Operands[1].Type != ND_OP_MEM ||
                        !previous_instruction->instrux.Operands[1].Info.Memory.HasBase ||
                        !previous_instruction->instrux.Operands[1].Info.Memory.HasDisp)
                    {
                        return std::nullopt;
                    }

                    const auto loaded_reg = previous_instruction->instrux.Operands[0].Info.Register.Reg;
                    const auto source_base = previous_instruction->instrux.Operands[1].Info.Memory.Base;
                    const auto source_disp = previous_instruction->instrux.Operands[1].Info.Memory.Disp;
                    const auto current_base = op2.Info.Memory.Base;

                    const auto feeds_current_null_ref =
                        (is_rcx_base(current_base) && loaded_reg == NDR_RCX) ||
                        (is_rax_base(current_base) && loaded_reg == NDR_RAX);

                    if (!feeds_current_null_ref) {
                        return std::nullopt;
                    }

                    const auto source_is_scene_targets =
                        is_rax_base(source_base) ||
                        source_base == NDR_R15 ||
                        source_base == NDR_R15D;

                    if (!source_is_scene_targets) {
                        return std::nullopt;
                    }

                    switch (source_disp) {
                    case 0xA8:
                    case 0xB0:
                    case 0x188:
                        return source_disp;
                    default:
                        return std::nullopt;
                    }
                }();

                if (scene_target_slot &&
                    set_context_register(exception->ContextRecord, decoded->Operands[0].Info.Register.Reg, 0))
                {
                    SPDLOG_WARN(
                        "[DaysGone] Recovering null scene-render-target output ref at {:x}; dest_reg={} from FSceneRenderTargets slot 0x{:x}",
                        exception_address,
                        (int)decoded->Operands[0].Info.Register.Reg,
                        *scene_target_slot);

                    exception->ContextRecord->Rip = exception_address + decoded->Length;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                SPDLOG_ERROR(
                    "[DaysGone] Null scene-render-target output-ref pattern at {:x} did not match expected FSceneRenderTargets load",
                    exception_address);
            }

            ignored_addresses.insert(exception_address);

            // Get the start of the previous instruction
            auto previous_instruction = utility::resolve_instruction(exception_address - 1);

            if (!previous_instruction) {
                SPDLOG_ERROR("Could not resolve previous instruction at {:x}", exception_address - 1);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (previous_instruction->instrux.Operands[0].Type != ND_OP_REG ||
                previous_instruction->instrux.Operands[0].Info.Register.Reg != op2.Info.Memory.Base)
            {
                const auto can_use_stalker2_backscan = stalker2_is_current_game() && is_ue_5_1_dx12_backend();

                if (!can_use_stalker2_backscan) {
                    SPDLOG_ERROR("Previous instruction does not use the same register as the dereference");
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                std::optional<utility::Resolved> xr_hmd_load{};
                const auto prior_instructions = utility::get_disassembly_behind(exception_address);

                for (auto it = prior_instructions.rbegin(); it != prior_instructions.rend(); ++it) {
                    const auto& candidate = *it;
                    const auto& candidate_ix = candidate.instrux;

                    if ((exception_address - candidate.addr) > 0x40) {
                        break;
                    }

                    if (candidate_ix.OperandsCount < 2 ||
                        candidate_ix.Operands[0].Type != ND_OP_REG ||
                        candidate_ix.Operands[0].Info.Register.Reg != op2.Info.Memory.Base)
                    {
                        continue;
                    }

                    const auto& candidate_op2 = candidate_ix.Operands[1];

                    if (candidate_op2.Type == ND_OP_MEM &&
                        candidate_op2.Info.Memory.HasBase &&
                        candidate_op2.Info.Memory.HasDisp &&
                        candidate_op2.Info.Memory.Disp == potential_hmd_device_offset)
                    {
                        xr_hmd_load = candidate;
                        break;
                    }
                }

                if (!xr_hmd_load) {
                    SPDLOG_ERROR("Previous instruction does not use the same register as the dereference");
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                SPDLOG_INFO(
                    "[Stalker2][UE5.1] Matched non-adjacent XRSystem/HMDDevice load at {:x} for null dereference at {:x}",
                    xr_hmd_load->addr,
                    exception_address);

                previous_instruction = *xr_hmd_load;
            }

            const auto prev_op2 = previous_instruction->instrux.Operands[1];

            if (previous_instruction->instrux.OperandsCount < 2 ||
                prev_op2.Type != ND_OP_MEM ||
                !prev_op2.Info.Memory.HasBase)
            {
                SPDLOG_ERROR("Previous instruction is not a memory dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (!prev_op2.Info.Memory.HasDisp) {
                SPDLOG_ERROR("Previous instruction does not have a displacement");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (prev_op2.Info.Memory.Disp != potential_hmd_device_offset) {
                SPDLOG_ERROR("Previous instruction is not the XRSystem or HMDDevice dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Found the dereference of the XRSystem or HMDDevice at {:x}", previous_instruction->addr);

            // Patch the initial instruction that caused the crash
            SPDLOG_INFO("Creating first patch...");

            std::vector<int16_t> first_patch{};

            for (auto i = 0; i < decoded->Length; ++i) {
                first_patch.push_back(0x90);
            }

            xrsystem_patches.push_back(Patch::create(exception_address, first_patch));

            const auto next_instruction_addr = exception_address + decoded->Length;
            const auto next_instruction = utility::decode_one((uint8_t*)next_instruction_addr);

            if (!next_instruction) {
                SPDLOG_ERROR("Could not decode next instruction at {:x}", exception_address + decoded->Length);
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (!std::string_view{next_instruction->Mnemonic}.starts_with("CALL")) {
                SPDLOG_ERROR("Next instruction is not a call, continuing anyways since we patched the dereference");
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Patch the next instruction if it's a call
            SPDLOG_INFO("Creating second patch...");

            std::vector<int16_t> second_patch{};

            for (auto i = 0; i < next_instruction->Length; ++i) {
                second_patch.push_back(0x90);
            }

            xrsystem_patches.push_back(Patch::create(next_instruction_addr, second_patch));

            SPDLOG_INFO("Finished creating patches, continuing execution. Hopefully we don't crash...");
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    });

    // The TWeakPtr version is for >= 4.11 UE versions
    TWeakPtr<FSceneViewExtensions>& view_extensions_tweakptr = 
        *(TWeakPtr<FSceneViewExtensions>*)potential_view_extensions;

    // This means it's an old version of UE
    // so the view extensions are a TArray and not a TWeakPtr<TArray>
    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        if (view_extensions_tweakptr.reference == nullptr) {
            view_extensions_tweakptr.allocate_naive(m_use_fmalloc_scene_view_extensions->value());
        }
    }

    FSceneViewExtensions& view_extensions = m_rendertarget_manager_embedded_in_stereo_device ?  
                                            *(FSceneViewExtensions*)potential_view_extensions : *view_extensions_tweakptr.reference;

    SPDLOG_INFO("Current ext ptr: {:x}", (uintptr_t)view_extensions.extensions.data);
    SPDLOG_INFO("Current ext count: {}", view_extensions.extensions.count);
    SPDLOG_INFO("Current ext capacity: {}", view_extensions.extensions.capacity);

    // Verifications on the current memory of the FSceneViewExtensions, because pre-4.10 (?) the view extensions array did not actually exist
    if (m_rendertarget_manager_embedded_in_stereo_device) {
        SPDLOG_INFO("Performing verifications on the current memory of the FSceneViewExtensions...");

        const auto& current_view_extensions_ptr_value = view_extensions.extensions;

        // Check if current value is non zero and points to invalid memory
        if (current_view_extensions_ptr_value.data != nullptr && IsBadReadPtr((void*)current_view_extensions_ptr_value.data, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions pointer is non-zero but points to invalid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if count is greater than capacity, which is not possible
        if ((uint32_t)current_view_extensions_ptr_value.count > (uint32_t)current_view_extensions_ptr_value.capacity) {
            SPDLOG_ERROR("Usual view extensions count is greater than capacity! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if count or capacity is negative, which is not possible
        if ((int32_t)current_view_extensions_ptr_value.count < 0 || (int32_t)current_view_extensions_ptr_value.capacity < 0) {
            SPDLOG_ERROR("Usual view extensions count or capacity is negative! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }
        
        // Check if the memory at count treated as a pointer points to valid memory, which is not possible
        const auto count_as_ptr = *(void**)&current_view_extensions_ptr_value.count;
        if (count_as_ptr != nullptr && !IsBadReadPtr(count_as_ptr, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions count is actually a pointer to valid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if the data pointer is null but capacity is greater than 0, which is not possible
        if (current_view_extensions_ptr_value.data == nullptr && current_view_extensions_ptr_value.capacity > 0) {
            SPDLOG_INFO("Usual view extensions data pointer is null but capacity is greater than 0! Cannot set up view extensions!");
            SPDLOG_INFO("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
        }

        // Check if the data pointer is non-null but the capacity is 0, which is not possible
        if (current_view_extensions_ptr_value.data != nullptr && current_view_extensions_ptr_value.capacity == 0) {
            SPDLOG_ERROR("Usual view extensions data pointer is non-null but capacity is 0! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if any current entries in the array within the count are invalid, which is not possible
        if (current_view_extensions_ptr_value.data != nullptr) {
            for (auto i = 0; i < current_view_extensions_ptr_value.count; ++i) {
                const auto ext = current_view_extensions_ptr_value.data[i].reference;

                if (IsBadReadPtr((void*)ext, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an invalid entry! Cannot set up view extensions!");
                    SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }

                const auto ext_vtable = *(void**)ext;

                if (IsBadReadPtr((void*)ext_vtable, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an entry with an invalid vtable! Cannot set up view extensions!");
                    SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }
            }
        }
    }

    // Allocate a completely new array if the current one is null or empty
    if (view_extensions.extensions.data == nullptr || view_extensions.extensions.data[0].reference == nullptr || view_extensions.extensions.count == 0) {
        SPDLOG_INFO("Allocating new view extensions array...");

        auto& exts = view_extensions.extensions;

        // Allocate a bunch more than necessary to prevent crashes when the engine tries to add new entries
        const auto new_capacity = 32;

        if (!m_use_fmalloc_scene_view_extensions->value()) {
            exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity]{};
        } else {
            if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                exts.data = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                for (auto i = 0; i < new_capacity; ++i) {
                    new (&exts.data[i]) TWeakPtr<ISceneViewExtension>();
                }
            } else {
                SPDLOG_ERROR("Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity]{};
            }
        }

        exts.count = 0;
        exts.capacity = new_capacity;

        ZeroMemory(exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else if (view_extensions.extensions.data != nullptr && view_extensions.extensions.count <= view_extensions.extensions.capacity) {
        auto& exts = view_extensions.extensions;

        // TODO: Use FMemory::Realloc (or whatever its called) instead of new/delete cuz game crashes when reallocating/closing the game
        if (exts.count == exts.capacity) {
            SPDLOG_INFO("Extending view extensions array...");

            const auto new_capacity = exts.capacity * 4;
            const auto old_capacity = exts.capacity;

            TWeakPtr<ISceneViewExtension>* new_exts = nullptr;

            if (!m_use_fmalloc_scene_view_extensions->value()) {
                new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
            } else {
                if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                    new_exts = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                    for (auto i = 0; i < new_capacity; ++i) {
                        new (&new_exts[i]) TWeakPtr<ISceneViewExtension>();
                    }
                } else {
                    SPDLOG_ERROR("Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                    new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
                }
            }

            ZeroMemory(new_exts, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
            memcpy(new_exts, exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * old_capacity);

            // dont delete it cuz its owned by the games allocator... for now
            //delete[] exts.data;

            exts.data = new_exts;
            exts.capacity = new_capacity;
        } else {
            SPDLOG_INFO("Allocating new view extension entry onto existing array...");
        }

        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else {
        SPDLOG_INFO("None of the previous conditions were met, so we're not allocating a new view extensions array");
    }

    if (view_extensions.extensions.count > 0 && view_extensions.extensions.data != nullptr) {
        // Replace the vtable of the first entry
        auto& entry = view_extensions.extensions.data[view_extensions.extensions.count-1];

        if (entry.reference == nullptr) {
            SPDLOG_ERROR("Failed to get first view extension entry!");
            return false;
        }

        auto& vtable = *(uintptr_t**)entry.reference;
        const auto original_vtable = (void**)vtable;

        g_hook->m_analyze_view_extensions_start_time = std::chrono::high_resolution_clock::now();
        g_hook->m_analyzing_view_extensions = true;

        if (SceneViewExtensionAnalyzer::try_apply_cached_discovery(original_vtable)) {
            vtable = g_view_extension_vtable.data();
            g_hook->m_analyzing_view_extensions = false;
            g_hook->m_has_view_extensions_installed = true;
            m_has_view_extension_hook = true;
            return true;
        }

        if (!m_rendertarget_manager_embedded_in_stereo_device) {
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size()-1>::fill(g_view_extension_vtable);
        } else {
            // Skip straight to stage 2.
            SPDLOG_INFO("Skipping view extension stage 1...");
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size()-1>::fill2(g_view_extension_vtable);
        }

        // Will get called when the view extensions are finally hooked.
        RenderThreadWorker::get().enqueue([this]() {
            this->m_analyzing_view_extensions = false;
            this->m_has_view_extensions_installed = true;
        });

        // overwrite the vtable
        vtable = g_view_extension_vtable.data();
        m_has_view_extension_hook = true;
    } else {
        // TODO: Allocate a new one.
        m_has_view_extension_hook = false;

        SPDLOG_INFO("Failed to set up view extensions! (not yet implemented to allocate a new one)");
    }

    return true;
} catch(...) {
    SPDLOG_ERROR("Unknown exception while setting up view extensions!");
    return false;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_constructor() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    const auto engine_dll = sdk::get_ue_module(L"Engine");

    auto fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationHeight");

    if (!fake_stereo_rendering_constructor) {
        fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationFOV");

        if (!fake_stereo_rendering_constructor) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
            return std::nullopt;
        }
    }

    if (!fake_stereo_rendering_constructor) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering constructor: {:x}", (uintptr_t)*fake_stereo_rendering_constructor);
    cached_result = *fake_stereo_rendering_constructor;

    return *fake_stereo_rendering_constructor;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_vtable() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    if (g_hook->m_manually_constructed) {
        cached_result = *(uintptr_t*)((uintptr_t)sdk::UGameEngine::get() + s_stereo_rendering_device_offset);
        return cached_result;
    }

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();

    if (!fake_stereo_rendering_constructor) {
        // If this happened, then that's bad news, the UE version is probably extremely old
        // so we have to use this fallback method.
        SPDLOG_INFO("Failed to locate FFakeStereoRendering constructor, using fallback method");
        const auto initialize_hmd_device = sdk::UEngine::get_initialize_hmd_device_address();

        if (!initialize_hmd_device) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method");
            return std::nullopt;
        }

        // To be seen if this needs to be adjusted. At first glance it doesn't look very reliable.
        // maybe perform emulation or something in the future?
        const auto instruction = utility::scan_disasm(*initialize_hmd_device, 100, "48 8D 05 ? ? ? ?");

        if (!instruction) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (2)");
            return std::nullopt;
        }

        const auto result = utility::calculate_absolute(*instruction + 3);

        if (!result) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (3)");
            return std::nullopt;
        }

        SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)result);
        cached_result = result;

        return result;
    }

    const auto vtable_ref = utility::scan(*fake_stereo_rendering_constructor, 100, "48 8D 05 ? ? ? ?");

    if (!vtable_ref) {
        SPDLOG_WARN("Failed to find FFakeStereoRendering VTable Reference through legacy pattern, trying constructor RIP-reference scan");

        if (const auto vtable_from_constructor = locate_vtable_from_constructor_rip_references(*fake_stereo_rendering_constructor)) {
            SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", *vtable_from_constructor);
            cached_result = vtable_from_constructor;
            return vtable_from_constructor;
        }

        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable Reference");
        return std::nullopt;
    }

    const auto vtable = utility::calculate_absolute(*vtable_ref + 3);

    if (!vtable) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)vtable);
    cached_result = vtable;

    return vtable;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_active_stereo_rendering_device() {
    auto engine = (uintptr_t)sdk::UEngine::get();

    if (engine == 0) {
        SPDLOG_ERROR("GEngine does not appear to be instantiated, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    SPDLOG_INFO("Checking engine pointers for StereoRenderingDevice...");
    auto fake_stereo_device_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_device_vtable) {
        SPDLOG_ERROR("Failed to locate fake stereo rendering device vtable, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    if (s_stereo_rendering_device_offset != 0) {
        const auto result = *(uintptr_t*)(engine + s_stereo_rendering_device_offset);

        if (result == 0) {
            return std::nullopt;
        }

        return result;
    }

    for (auto i = 0; i < 0x2000; i += sizeof(void*)) {
        const auto addr_of_ptr = engine + i;

        if (IsBadReadPtr((void*)addr_of_ptr, sizeof(void*))) {
            SPDLOG_INFO("Reached end of engine pointers at offset {:x}", i);
            break;
        }

        const auto ptr = *(uintptr_t*)addr_of_ptr;

        if (ptr == 0 || IsBadReadPtr((void*)ptr, sizeof(void*))) {
            continue;
        }

        auto potential_vtable = *(uintptr_t*)ptr;

        if (potential_vtable == *fake_stereo_device_vtable) {
            SPDLOG_INFO("Found fake stereo rendering device at offset {:x} -> {:x}", i, ptr);
            s_stereo_rendering_device_offset = i;
            return ptr;
        }
    }

    SPDLOG_ERROR("Failed to find stereo rendering device");
    return std::nullopt;
}

std::optional<uint32_t> FFakeStereoRenderingHook::get_stereo_view_offset_index(uintptr_t vtable) {
    for (auto i = 0; i < 30; ++i) {
        auto func = ((uintptr_t*)vtable)[i];

        if (func == 0 || IsBadReadPtr((void*)func, sizeof(void*))) {
            continue;
        }

        // Resolve jmps if needed.
        while (*(uint8_t*)func == 0xE9) {
            SPDLOG_INFO("VFunc at index {} contains a jmp, resolving...", i);
            func = utility::calculate_absolute(func + 1);
        }

        bool found = false;
        uint32_t xmm_register_usage_count = 0;

        // We do an exhaustive decode (disassemble all possible code paths) that correctly follows the control flow
        // because some games are obfuscated and do huge jumps across gaps of junk code.
        // so we can't just linearly scan forward as the disassembler will fail at some point.
        utility::exhaustive_decode((uint8_t*)func, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (found) {
                return utility::ExhaustionResult::BREAK;
            }

            if (ix.BranchInfo.IsBranch && !ix.BranchInfo.IsConditional && std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            char txt[ND_MIN_BUF_SIZE]{};
            NdToText(&ix, 0, sizeof(txt), txt);

            if (std::string_view{txt}.find("xmm") != std::string_view::npos && ++xmm_register_usage_count >= 10) {
                found = true;
                return utility::ExhaustionResult::BREAK;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (found) {
            SPDLOG_INFO("Found Stereo View Offset Index: {}", i);
            return i;
        }
    }

    return std::nullopt;
}

// DISCLAIMER: I've only seen this in one game so far...
// So, there's some kind of compiler optimization for inlined virtuals
// that checks whether the vtable pointer matches the base FFakeStereoRendering class.
// if it matches, it just calls an inlined version of the function.
// otherwise it actually calls the function within the vtable.
bool FFakeStereoRenderingHook::patch_vtable_checks() {
    SPDLOG_INFO("Attempting to patch inlined vtable checks...");

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();
    const auto fake_stereo_rendering_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_rendering_constructor || !fake_stereo_rendering_vtable) {
        SPDLOG_ERROR("Cannot patch vtables, constructor or vtable not found!");
        return false;
    }

    const auto vtable_module_within = utility::get_module_within(*fake_stereo_rendering_vtable);
    const auto module_size = utility::get_module_size(*vtable_module_within);
    const auto module_end = (uintptr_t)*vtable_module_within + *module_size;

    SPDLOG_INFO("{:x} {:x} {:x}", *fake_stereo_rendering_vtable, (uintptr_t)*vtable_module_within, *module_size);

    for (auto ref = utility::scan_displacement_reference(*vtable_module_within, *fake_stereo_rendering_vtable); 
        ref.has_value();
        ref = utility::scan_displacement_reference((uintptr_t)*ref + 4, (module_end - *ref) - sizeof(void*), *fake_stereo_rendering_vtable)) 
    {
        const auto distance_from_constructor = *ref - *fake_stereo_rendering_constructor;

        // We don't want to mess with the one within the constructor.
        if (distance_from_constructor < 0x100) {
            SPDLOG_INFO("Skipping vtable reference within constructor");
            continue;
        }

        // Change the bytes to be some random number
        // this causes the vtable check to fail and will call the function within the vtable.
        DWORD old{};
        VirtualProtect((void*)*ref, 4, PAGE_EXECUTE_READWRITE, &old);
        *(uint32_t*)*ref = 0x12345678;
        VirtualProtect((void*)*ref, 4, old, &old);
        SPDLOG_INFO("Patched vtable check at {:x}", (uintptr_t)*ref);
    }

    SPDLOG_INFO("Finished patching inlined vtable checks.");
    return true;
}

bool FFakeStereoRenderingHook::attempt_runtime_inject_stereo() {
    // This attempts to create a new StereoRenderingDevice in the GEngine
    // if it doesn't already exist via using -emulatestereo.
    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to locate GEngine, cannot inject stereo rendering device at runtime.");
        return false;
    }

    if (everwind_is_current_game()) {
        // Everwind's updated UE5.5 build can crash inside InitializeHMDDevice
        // before UEVR reaches its fallback stereo-device path. The old build
        // already relied on that fallback after InitializeHMDDevice failed to
        // create a device, so avoid the unsafe engine call for all Everwind builds.
        SPDLOG_WARN_ONCE("[Everwind] Skipping runtime InitializeHMDDevice; using fallback stereo-device injection");
        return false;
    }

    static auto enable_stereo_emulation_cvar = sdk::vr::get_enable_stereo_emulation_cvar();

    if (!locate_active_stereo_rendering_device()) {
        SPDLOG_INFO("Calling InitializeHMDDevice...");

        //utility::ThreadSuspender _{};

        engine->initialize_hmd_device();

        SPDLOG_INFO("Called InitializeHMDDevice.");

        if (!locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Previous call to InitializeHMDDevice did not setup the stereo rendering device, attempting to call again...");

            auto patch_emulate_stereo_flag = []() {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                SPDLOG_INFO("r.EnableStereoEmulation cvar not found, using fallback method of forcing -emulatestereo flag.");
                
                const auto emulate_stereo_string_ref = sdk::UGameEngine::get_emulatestereo_string_ref_address();

                if (emulate_stereo_string_ref) {
                    const auto resolved = utility::resolve_instruction(*emulate_stereo_string_ref);

                    if (resolved) {
                        // Scan forward for a call instruction, this call checks the command line for "emulatestereo".
                        const auto call = utility::scan_disasm(resolved->addr, 20, "E8 ? ? ? ?");

                        if (call) {
                            // Patch the instruction to mov al, 1
                            SPDLOG_INFO("Patching instruction at {:x} to mov al, 1", (uintptr_t)*call);
                            static auto patch = Patch::create(*call, { 0xB0, 0x01, 0x90, 0x90, 0x90 });
                        }
                    }
                }
            };

            // We don't call this before because the cvar will not be set up
            // until it's referenced once. after we set this we need to call the function again.
            if (enable_stereo_emulation_cvar) {
                try {
                    enable_stereo_emulation_cvar->set<int>(1);
                } catch(...) {
                    SPDLOG_ERROR("Access violation occurred when writing to r.EnableStereoEmulation, the address may be incorrect!");
                    patch_emulate_stereo_flag();
                }
            } else {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                patch_emulate_stereo_flag();
            }

            SPDLOG_INFO("Calling InitializeHMDDevice... AGAIN");

            engine->initialize_hmd_device();

            SPDLOG_INFO("Called InitializeHMDDevice again.");
        }

        if (locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Stereo rendering device setup successfully.");
        } else {
            SPDLOG_ERROR("Failed to setup stereo rendering device.");
            return false;
        }
    } else {
        SPDLOG_INFO("Not necessary to call InitializeHMDDevice, stereo rendering device is already setup.");
        m_fixed_localplayer_view_count = true; // Everything was set up beforehand, we don't need to do anything, so just set it to true.
    }

    return true;
}

bool FFakeStereoRenderingHook::is_stereo_enabled(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("is stereo enabled called!");
#else
    SPDLOG_INFO_ONCE("is stereo enabled called!");
#endif

    // wait!!!
    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        g_hook->set_should_recreate_textures(true);
        return true;
    }
    
    /*if (g_hook->m_analyzing_view_extensions) {
        const auto now = std::chrono::high_resolution_clock::now();

        if (now - g_hook->m_analyze_view_extensions_start_time > std::chrono::seconds(15)) {
            SPDLOG_INFO("Timed out waiting for view extensions to be analyzed.");
            g_hook->m_analyzing_view_extensions = false;
        }

        return false;
    }*/

    static std::atomic<bool> last_state = false;
    auto hook = g_hook;

    // The best way to enable stereo rendering without causing crashes
    // while also allowing the desktop view to initially display
    // if the HMD is not on at the start. It only allows
    // stereo to be enabled if it starts from the first call to IsStereoEnabled inside UGameViewportClient::Draw.
    if (hook->m_has_game_viewport_client_draw_hook) {
        if (GameThreadWorker::get().is_same_thread()) {
            if (hook->m_in_viewport_client_draw && !hook->m_was_in_viewport_client_draw) {
                const auto is_hmd_active = VR::get()->is_hmd_active();

                if (!last_state && is_hmd_active) {
                    VR::get()->wait_for_present();
                    hook->set_should_recreate_textures(true);
                }

                last_state = is_hmd_active;
            }

            hook->m_was_in_viewport_client_draw = hook->m_in_viewport_client_draw;
        }

        return last_state;
    }

    static uint32_t count = 0;

    // Forcefully return true the first few times to let stuff initialize.
    if (count < 50) {
        if (count == 0) {
            hook->set_should_recreate_textures(true);
        }

        ++count;
        last_state = true;
        return true;
    }

    const auto result = !VR::get()->get_runtime()->got_first_sync || VR::get()->is_hmd_active();

    if (result && !last_state) {
        hook->set_should_recreate_textures(true);
    }

    last_state = result;

    return result;
}

void FFakeStereoRenderingHook::adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("adjust view rect called! {}", index);
    SPDLOG_INFO(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#else
    SPDLOG_INFO_ONCE("adjust view rect called! {}", index);
    SPDLOG_INFO_ONCE(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    static bool index_starts_from_one = true;

    if (index == 2) {
        index_starts_from_one = true;
    } else if (index == 0) {
        index_starts_from_one = false;
    }

    // The purpose of this is to prevent the game from crashing in IDirect3D12CommandList::Close
    // Because the game will try to copy a texture region that is out of bounds.
    if (g_hook->m_skip_next_adjust_view_rect) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        g_hook->m_skip_next_adjust_view_rect = false;
        g_hook->m_skip_next_adjust_view_rect_count = 1;
        return;
    }

    if (g_hook->m_skip_next_adjust_view_rect_count > 0) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        --g_hook->m_skip_next_adjust_view_rect_count;
        return;
    }

    if (VR::get()->is_stereo_emulation_enabled()) {
        *w *= 2;
    } else {
        *w = VR::get()->get_hmd_width() * 2;
        *h = VR::get()->get_hmd_height();
    }


    *w = *w / 2;

    const auto true_index = index_starts_from_one ? ((index + 1) % 2) : (index % 2);

    if (!VR::get()->is_native_stereo_fix_enabled()) {
        *x += *w * true_index;
    }
}

__forceinline void FFakeStereoRenderingHook::calculate_stereo_view_offset(
    FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, 
    const float world_to_meters, Vector3f* view_location)
{
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo view offset called! {}", view_index);
#else
    SPDLOG_INFO_ONCE("calculate stereo view offset called! {}", view_index);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto vr = VR::get();
    //std::scoped_lock _{vr->get_vr_mutex()};

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;
    static bool index_was_ever_negative = false;

    if (view_index == -1) {
        index_was_ever_negative = true;
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index -1 (INDEX_NONE), ignoring.");
        return;
    }

    const bool synced_ue56_zero_view_is_eye =
        vr->is_using_synchronized_afr() &&
        g_hook->m_has_double_precision &&
        is_ue_5_6_or_newer() &&
        // SceneView compatibility handles eye offsets manually in sceneview_constructor.
        // Do not let the normal UE callback change legacy SceneView pass handling.
        !vr->is_sceneview_compatibility_enabled() &&
        view_index == 0;

    if (synced_ue56_zero_view_is_eye) {
        SPDLOG_INFO_ONCE("[SyncedSequential][UE5.6+] Treating CalculateStereoViewOffset view_index 0 as the alternating AFR eye pass");
    }

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    if (index_was_ever_two && view_index == 0 && !synced_ue56_zero_view_is_eye) {
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index 0 after 2, ignoring.");
        return;
    }

    vr->set_world_to_meters(world_to_meters);

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0 && !index_was_ever_two) {
        index_starts_from_one = false;
    }

    const auto is_full_pass = view_index == 0 && !index_was_ever_two && !index_was_ever_negative && !synced_ue56_zero_view_is_eye;

    auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
    const auto has_double_precision = g_hook->m_has_double_precision;
    const auto rot_d = (Rotator<double>*)view_rotation;

    if (vr->is_using_afr() && !is_full_pass) {
        true_index = g_frame_count % 2;

        if (!vr->is_using_synchronized_afr()) {
            if (g_hook->m_has_double_precision) {
                if (true_index == 1) {
                    *rot_d = g_hook->m_last_afr_rotation_double;
                } else {
                    g_hook->m_last_afr_rotation_double = *rot_d;
                }
            } else {
                if (true_index == 1) {
                    *view_rotation = g_hook->m_last_afr_rotation;
                } else {
                    g_hook->m_last_afr_rotation = *view_rotation;
                }
            }
        }
    }

    if (true_index == 0 && !is_full_pass) {
        if (has_double_precision) {
            g_hook->m_last_pre_rotation_double = *rot_d;
        } else {
            g_hook->m_last_pre_rotation = *view_rotation;
        }

        //vr->wait_for_present();
        
        if (!g_hook->m_has_view_extension_hook && !g_hook->m_has_game_viewport_client_draw_hook) {
            vr->update_hmd_state();
        }
    }

    /*if (view_index % 2 == 1 && VR::get()->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
        std::scoped_lock _{ vr->get_runtime()->render_mtx };
        SPDLOG_INFO("SYNCING!!!");
        //vr->get_runtime()->synchronize_frame();
        vr->update_hmd_state();
    }*/

    // if we were unable to hook UGameEngine::Tick, we can run our game thread jobs here instead.
    if (!is_full_pass && !g_hook->m_has_view_extension_hook && g_hook->m_attempted_hook_game_engine_tick && !g_hook->m_hooked_game_engine_tick) {
        GameThreadWorker::get().execute();
    }

    if (vr->is_sceneview_compatibility_enabled() && !g_hook->m_inside_manual_view_offset) {
        return;
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_early_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        for (auto& mod : mods) {
            mod->on_pre_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }
    }

    const auto view_d = (Vector3d*)view_location;

    const auto view_mat = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians((float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians((float)rot_d->roll));

    const auto view_mat_inverse = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(-view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(-view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians(-(float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians(-(float)rot_d->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    const auto view_quat = glm::quat {
        view_mat
    };

    const auto quat_converter = glm::quat{Matrix4x4f {
        0, 0, -1, 0,
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1
    }};

    auto vqi_norm = glm::normalize(view_quat_inverse);

    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        vr->set_pre_flattened_rotation(vqi_norm);
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto camera_forward_offset = vr->get_camera_forward_offset();
    const auto camera_right_offset = vr->get_camera_right_offset();
    const auto camera_up_offset = vr->get_camera_up_offset();
    const auto camera_forward = quat_converter * (vqi_norm * glm::vec3{0, 0, camera_forward_offset});
    const auto camera_right = quat_converter * (vqi_norm * glm::vec3{-camera_right_offset, 0, 0});
    const auto camera_up = quat_converter * (vqi_norm * glm::vec3{0, -camera_up_offset, 0});

    const auto world_scale = world_to_meters * vr->get_world_scale();

    if (has_double_precision) {
        *view_d += camera_forward;
        *view_d += camera_right;
        *view_d += camera_up;
    } else {
        *view_location += camera_forward;
        *view_location += camera_right;
        *view_location += camera_up;
    }

    // Don't apply any headset transformations
    // if we have stereo emulation mode enabled
    // it is only for debugging purposes
    if (!vr->is_stereo_emulation_enabled()) {
        const auto is_2d_screen = vr->is_using_2d_screen();

        const auto rotation_offset = vr->get_rotation_offset();
        const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{vr->get_rotation(0)});
        const auto current_eye_rotation_offset = glm::normalize(glm::quat{vr->get_eye_transform(true_index)});

        const auto new_rotation = glm::normalize(vqi_norm * current_hmd_rotation * current_eye_rotation_offset);
        const auto eye_offset = glm::vec3{vr->get_eye_offset((VRRuntime::Eye)(true_index))};


        const auto standing_delta = vr->get_position(0) - vr->get_standing_origin();
        const auto standing_delta_flat = glm::vec3{standing_delta.x, 0, standing_delta.z};

        const auto pos = glm::vec3{rotation_offset * standing_delta};
        const auto pos_flat = glm::vec3{rotation_offset * standing_delta_flat};

        const auto head_offset = quat_converter * (vqi_norm * (pos * world_scale));
        const auto head_offset_flat = quat_converter * (vqi_norm * (pos_flat * world_scale));
        const auto eye_separation = quat_converter * (glm::normalize(new_rotation) * (eye_offset * world_scale));

        if (!has_double_precision) {
            if (!is_2d_screen) {
                *view_location -= head_offset;
            }

            *view_location -= eye_separation;
        } else {
            if (!is_2d_screen) {
                *view_d -= head_offset;
            }

            *view_d -= eye_separation;
        }

        if (!is_2d_screen) {
            const auto euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

            if (!has_double_precision) {
                view_rotation->pitch = euler.x;
                view_rotation->yaw = euler.y;
                view_rotation->roll = euler.z;
            } else {
                rot_d->pitch = euler.x;
                rot_d->yaw = euler.y;
                rot_d->roll = euler.z;
            }
        }

        // Roomscale movement
        // only do it on the right eye pass
        // if we did it on the left, there would be eye desyncs when the right eye is rendered
        if (true_index == 1 && (vr->is_roomscale_enabled() || vr->is_aim_pawn_control_rotation_enabled())) {
            const auto world = sdk::UEngine::get()->get_world();

            if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
                const auto pawn = controller->get_acknowledged_pawn();

                static bool was_pawn_rotation_enabled = false;

                if (pawn != nullptr && vr->is_aim_pawn_control_rotation_enabled()) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, true);
                            was_pawn_rotation_enabled = true;
                        }
                    }
                } else if (pawn != nullptr && was_pawn_rotation_enabled) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, false);
                            was_pawn_rotation_enabled = false;
                        }
                    }
                }

                if (pawn != nullptr && vr->is_roomscale_enabled()) {
                    const auto pawn_pos = pawn->get_actor_location();
                    const auto new_pos = pawn_pos - head_offset_flat;

                    // Roomscale sweep option allows the actor to affect the world
                    // like push doors open, and prevent them from clipping through walls
                    pawn->set_actor_location(new_pos, vr->is_roomscale_sweep_enabled(), false);

                    // Recenter the standing origin
                    auto current_standing_origin = vr->get_standing_origin();
                    const auto hmd_pos = vr->get_position(0);
                    // dont touch the Y axis
                    current_standing_origin.x = hmd_pos.x;
                    current_standing_origin.z = hmd_pos.z;
                    vr->set_standing_origin(current_standing_origin);
                }
            }
        }

        // Process snapturn    
        vr->process_snapturn();
    }

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_post_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        if (true_index == 0) {
            if (has_double_precision) {
                g_hook->m_last_rotation_double = *rot_d;
            } else {
                g_hook->m_last_rotation = *view_rotation;
            }
        }

        // Modify Player Control Rotation
        const auto controller_camera_guard_active = vr->is_controller_camera_conflict_guard_active();
        const auto direct_aim_compatibility_fallback =
            vr->is_hmd_active() &&
            !controller_camera_guard_active &&
            (is_deadzone_ue56_executable() || vr->is_direct_aim_compatibility_enabled()) &&
            (vr->is_headlocked_aim_enabled() ||
                (vr->is_controller_aim_enabled() && vr->is_using_controllers()));

        if (true_index == 1 &&
            vr->is_any_aim_method_active() &&
            !controller_camera_guard_active &&
            (vr->is_aim_modify_player_control_rotation_enabled() || direct_aim_compatibility_fallback))
        {
            if (g_hook->m_tracking_system_hook != nullptr) {
                g_hook->m_tracking_system_hook->manual_update_control_rotation();
            }
        }
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo view offset!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo view offset!");
#endif
}

__forceinline Matrix4x4f* FFakeStereoRenderingHook::calculate_stereo_projection_matrix(FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#else
    SPDLOG_INFO_ONCE("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#endif

    auto& vr = VR::get();

    // Only call PostInitProperties if ghosting fix enabled or native stereo is being used.
    // Also, if we don't have a hook on GetDesiredNumberOfViews, we need to call PostInitProperties
    //if (!vr->is_using_afr() || vr->is_ghosting_fix_enabled() || !g_hook->m_get_desired_number_of_views_hook) {
    if (!vr->should_skip_post_init_properties()) {
        if (!g_hook->m_fixed_localplayer_view_count) {
            if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                const auto return_address = (uintptr_t)_ReturnAddress();

                if (subnautica2_is_current_game() && !g_hook->m_hooked_alternative_localplayer_scan) {
                    // Subnautica 2 can wedge if we patch the immediate return address
                    // during first startup projection. Use the safer GetProjectionData
                    // pre-hook path and let the next frame provide the LocalPlayer.
                    constexpr auto max_stack_depth = 100;
                    uintptr_t stack[max_stack_depth]{};

                    const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
                    g_hook->m_projection_matrix_stack.clear();

                    for (int i = 0; i < depth; i++) {
                        g_hook->m_projection_matrix_stack.push_back(stack[i]);
                    }

                    if (g_hook->m_projection_matrix_stack.size() >= 3) {
                        const auto post_get_projection_data = g_hook->m_projection_matrix_stack[2];
                        const auto get_projection_data_candidate_1 = utility::find_function_start_with_call(post_get_projection_data);
                        const auto get_projection_data_candidate_2 = utility::find_virtual_function_start(post_get_projection_data);
                        std::optional<uintptr_t> get_projection_data{};

                        if (get_projection_data_candidate_1 && get_projection_data_candidate_2) {
                            const auto candidate_1_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_1);
                            const auto candidate_2_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_2);
                            get_projection_data = candidate_1_distance < candidate_2_distance ? get_projection_data_candidate_1 : get_projection_data_candidate_2;
                        } else if (get_projection_data_candidate_1) {
                            get_projection_data = get_projection_data_candidate_1;
                        } else if (get_projection_data_candidate_2) {
                            get_projection_data = get_projection_data_candidate_2;
                        } else {
                            get_projection_data = utility::find_function_start(post_get_projection_data);
                        }

                        if (get_projection_data) {
                            SPDLOG_INFO("[Subnautica2] Hooking GetProjectionData at {:x} instead of CalculateStereoProjectionMatrix return address", *get_projection_data);
                            auto hook = safetyhook::create_mid((void*)*get_projection_data, &FFakeStereoRenderingHook::pre_get_projection_data);

                            if (hook) {
                                g_hook->m_get_projection_data_pre_hook = std::move(hook);
                                g_hook->m_hooked_alternative_localplayer_scan = true;
                            } else {
                                SPDLOG_WARN("[Subnautica2] Failed to hook GetProjectionData; disabling LocalPlayer bootstrap for this session");
                                g_hook->m_fixed_localplayer_view_count = true;
                            }
                        } else {
                            SPDLOG_WARN("[Subnautica2] Failed to locate GetProjectionData; disabling LocalPlayer bootstrap for this session");
                            g_hook->m_fixed_localplayer_view_count = true;
                        }
                    } else {
                        SPDLOG_WARN("[Subnautica2] Projection stack was too shallow for GetProjectionData hook; disabling LocalPlayer bootstrap for this session");
                        g_hook->m_fixed_localplayer_view_count = true;
                    }

                    g_hook->m_projection_matrix_stack.clear();
                }

                if (g_hook->m_hooked_alternative_localplayer_scan || g_hook->m_fixed_localplayer_view_count) {
                    // The alternative LocalPlayer bootstrap path is installed or this
                    // session has failed closed; do not patch the return address.
                } else {
                    SPDLOG_INFO("Inserting midhook after CalculateStereoProjectionMatrix... @ {:x}", return_address);

                    constexpr auto max_stack_depth = 100;
                    uintptr_t stack[max_stack_depth]{};

                    const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                    for (int i = 0; i < depth; i++) {
                        g_hook->m_projection_matrix_stack.push_back(stack[i]);
                        SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
                    }

                    g_hook->m_calculate_stereo_projection_matrix_post_hook = safetyhook::create_mid((void*)return_address, &FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix);

                    if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                        SPDLOG_ERROR("Failed to insert midhook after CalculateStereoProjectionMatrix!");
                    }
                }
            }
        } else if (g_hook->m_calculate_stereo_projection_matrix_post_hook) {
            SPDLOG_INFO("Removing midhook after CalculateStereoProjectionMatrix, job is done...");
            g_hook->m_calculate_stereo_projection_matrix_post_hook = {};
            g_hook->m_get_projection_data_pre_hook = {};
        }   
    }

    if (!g_framework->is_game_data_intialized()) {
        if (g_hook->m_calculate_stereo_projection_matrix_hook) {
            return g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
        }

        return out;
    }

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    // or maybe we should, this could be used for WorldToScreen.
    /*if (index_was_ever_two && view_index == 0) {
        SPDLOG_INFO_ONCE("Index was ever two, and now it's zero. This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.");
        return out;
    }*/

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0) {
        index_starts_from_one = false;
    }

    // Can happen if we hooked this differently.
    if (g_hook->m_calculate_stereo_projection_matrix_hook) {
        g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
    } else {
        if (g_hook->m_has_double_precision) {
            (*(Matrix4x4d*)out)[3][2] = (double)sdk::globals::get_near_clipping_plane();
        } else {
            (*out)[3][2] = sdk::globals::get_near_clipping_plane();
        }
    }

    if (VR::get()->is_using_2d_screen()) {
        float fov = 90.0f; // todo, get from FMinimalViewInfo

        const float width = VR::get()->get_hmd_width();
        const float height = VR::get()->get_hmd_height();
        const float half_fov = glm::radians(fov) / 2.0f;
        const float xs = 1.0f / glm::tan(half_fov);
        const float ys = width / glm::tan(half_fov) / height;
        const float near_z = sdk::globals::get_near_clipping_plane();

        if (g_hook->m_has_double_precision) {
            (*(Matrix4x4d*)out) = Matrix4x4d {
                xs, 0.0, 0.0, 0.0,
                0.0, ys, 0.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
                0.0, 0.0, near_z, 0.0
            };
        } else {
            *out = Matrix4x4f {
                xs, 0.0f, 0.0f, 0.0f,
                0.0f, ys, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, near_z, 0.0f
            };
        }

        return out;
    }

    // SPDLOG_INFO("NearZ: {}", old_znear);

    if (out != nullptr) {
        auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
    
        if (vr->is_using_afr()) {
            true_index = g_frame_count % 2;
        }

        auto& double_matrix = *(Matrix4x4d*)out;

        if (!g_hook->m_has_double_precision) {
            float old_znear = (*out)[3][2];
            VR::get()->m_nearz = old_znear;            
            VR::get()->get_runtime()->update_matrices(old_znear, 10000.0f);
        } else {
            double old_znear = (double_matrix)[3][2];
            VR::get()->m_nearz = (float)old_znear;
            VR::get()->get_runtime()->update_matrices((float)old_znear, 10000.0f);
        }

        if (!g_hook->m_has_double_precision) {
            *out = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
        } else {
            const auto fmat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
            double_matrix = fmat;
        }
    } else {
        SPDLOG_ERROR("CalculateStereoProjectionMatrix returned nullptr!");
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo projection matrix!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo projection matrix!");
#endif
    
    return out;
}

__forceinline void FFakeStereoRenderingHook::render_texture_render_thread(FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list,
    FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) 
{
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("render texture render thread called!");
#else
    SPDLOG_INFO_ONCE("render texture render thread called!");
#endif


    if (!g_hook->is_slate_hooked() && g_hook->has_attempted_to_hook_slate()) {
        SPDLOG_INFO("Attempting to hook SlateRHIRenderer::DrawWindow_RenderThread using RenderTexture_RenderThread return address...");
        const auto return_address = (uintptr_t)_ReturnAddress();
        SPDLOG_INFO(" Return address: {:x}", return_address);
        g_hook->attempt_hook_slate_thread(return_address);
    }

    g_hook->get_slate_thread_worker()->execute(rhi_command_list);

    /*const auto return_address = (uintptr_t)_ReturnAddress();
    const auto slate_cvar_usage_location = sdk::vr::get_slate_draw_to_vr_render_target_usage_location();

    if (slate_cvar_usage_location) {
        const auto distance_from_usage = (intptr_t)(return_address - *slate_cvar_usage_location);

        if (distance_from_usage <= 0x200) {
            //SPDLOG_INFO("Ret: {:x} Distance: {:x}", return_address, distance_from_usage);

            auto& d3d11_vr = VR::get()->m_d3d11;
            auto& hook = g_framework->get_d3d11_hook();
            auto device = hook->get_device();
            ComPtr<ID3D11DeviceContext> context{};

            device->GetImmediateContext(&context);
            context->CopyResource(d3d11_vr.get_test_tex().Get(), (ID3D11Resource*)src_texture->get_native_resource());
            context->Flush();
        }
    }*/

    //g_hook->m_rtm.set_render_target(src_texture);

    /*if (g_hook->m_rtm.get_scene_target() != src_texture) {
        g_hook->m_rtm.set_render_target(src_texture);
    }*/

    // SPDLOG_INFO("{:x}", (uintptr_t)src_texture->GetNativeResource());

    // maybe the window size is actually a pointer we will find out later.
    /*if (g_hook->m_render_texture_render_thread_hook) {
        g_hook->m_render_texture_render_thread_hook->call<void*>(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    }*/
}

void FFakeStereoRenderingHook::init_canvas(FFakeStereoRendering* stereo, sdk::FSceneView* view, UCanvas* canvas) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("init canvas called!");
#else
    SPDLOG_INFO_ONCE("init canvas called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    // Since the FSceneView and UCanvas structures will probably vary wildly
    // in terms of field offsets and size, we will need to dynamically scan
    // from the return address of this function to find the ViewProjectionMatrix offset.
    // in the FSceneView and also the UCanvas.
    // it happens in the else block of the conditional statement that calls this function
    static uint32_t fsceneview_viewproj_offset = 0;
    static uint32_t ucanvas_viewproj_offset = 0;

    if (fsceneview_viewproj_offset == 0 || ucanvas_viewproj_offset == 0) {
        SPDLOG_INFO("Searching for FSceneView and UCanvas offsets...");
        SPDLOG_INFO("Canvas: {:x}", (uintptr_t)canvas);

        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto containing_function = utility::find_function_start(return_address);

        SPDLOG_INFO("Found containing function at {:x}", *containing_function);

        auto find_offsets = [](uintptr_t start, uintptr_t end) -> bool {
            for (auto ip = (uintptr_t)start; ip < end + 0x100;) {
                const auto ix = utility::decode_one((uint8_t*)ip);

                if (!ix) {
                    SPDLOG_ERROR("Failed to decode instruction at {:x}", ip);
                    break;
                }

                // The initial instructions look something like this
                /*
                0F 28 86 C0 03 00 00                          movaps  xmm0, xmmword ptr [rsi+3C0h]
                41 0F 11 87 80 02 00 00                       movups  xmmword ptr [r15+280h], xmm0
                */
                if (std::string_view{ix->Mnemonic} == "MOVAPS" && ix->Operands[1].Type == ND_OP_MEM) {
                    const auto next = utility::decode_one((uint8_t*)(ip + ix->Length));

                    if (next) {
                        if (std::string_view{next->Mnemonic} == "MOVUPS" && next->Operands[0].Type == ND_OP_MEM) {
                            fsceneview_viewproj_offset = ix->Operands[1].Info.Memory.Disp;
                            ucanvas_viewproj_offset = next->Operands[0].Info.Memory.Disp;
                            
                            SPDLOG_INFO("Found at {:x}", ip);
                            SPDLOG_INFO("Found FSceneView ViewProjectionMatrix offset: {:x}", fsceneview_viewproj_offset);
                            SPDLOG_INFO("Found UCanvas ViewProjectionMatrix offset: {:x}", ucanvas_viewproj_offset);
                            return true;
                            break;
                        }
                    }
                }

                ip += ix->Length;
            }

            return false;
        };

        if (!find_offsets(*containing_function, return_address)) {
            // If we still didn't find it at this stage, re-scan from the previous function from the previous function call instead.
            const auto potential_func = utility::calculate_absolute(return_address - 4);
            if (!find_offsets(potential_func, potential_func + 0x100)) {
                SPDLOG_ERROR("Failed to find offsets!");
                return;
            }
        }
    }

    //*(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset) = VR::get()->get_projection_matrix(VRRuntime::Eye::LEFT);
    *(Matrix4x4f*)((uintptr_t)canvas + ucanvas_viewproj_offset) = *(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset);
}

uint32_t FFakeStereoRenderingHook::get_desired_number_of_views_hook(FFakeStereoRendering* stereo, bool is_stereo_enabled) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get desired number of views hook called!");
#else
    SPDLOG_INFO_ONCE("get desired number of views hook called!");
#endif

    auto& vr = VR::get();

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        return 2;
    }

    if (!is_stereo_enabled || (vr->is_using_afr() && !vr->is_splitscreen_compatibility_enabled())) {
        // We need to know about the second scene state to fix ghosting, so set the view count to 2
        // after we know about it, we can continue returning 1.
        if (is_stereo_enabled && vr->is_ghosting_fix_enabled() && vr->is_using_afr() &&
            g_hook->m_sceneview_data.known_scene_states.size() < 2 && g_hook->m_fixed_localplayer_view_count &&
            !!g_hook->m_sceneview_data.constructor_hook && g_hook->m_has_view_extensions_installed)
        {
            // Only works correctly if view extensions are installed, so we can reset the view count to 1 without crashing
            return 2;
        }

        return 1;
    }

    if (vr->is_native_stereo_fix_enabled()) {
        auto rtm = g_hook->get_render_target_manager();
        const auto scene_capture_rt_ready = rtm->get_scene_capture_render_target() != nullptr;
        const auto scene_capture_utexture_ready = rtm->get_scene_capture_utexture() != nullptr;
        const auto fsceneview_hook_ready = !!g_hook->m_sceneview_data.constructor_hook;
        const auto begin_viewfamily_hook_ready = !!g_hook->m_render_module_begin_render_viewfamily_hook;

        if (avowed_is_current_game()) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] GetDesiredNumberOfViews state: stereo_enabled={} scene_rt={} scene_utexture={} fsceneview_hook={} begin_viewfamily_hook={} fixed_localplayer={}",
                is_stereo_enabled,
                scene_capture_rt_ready,
                scene_capture_utexture_ready,
                fsceneview_hook_ready,
                begin_viewfamily_hook_ready,
                g_hook->m_fixed_localplayer_view_count);
        }

        if ((!scene_capture_rt_ready || !fsceneview_hook_ready || !begin_viewfamily_hook_ready)) {
            if (rtm->get_scene_capture_utexture() == nullptr) {
                rtm->create_scene_capture();
            }

            if (avowed_is_current_game()) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[Avowed][NativeStereoFix] Returning one view while native-fix prerequisites initialize");
            }

            return 1; // wait for the scene capture render target to be set and FSceneView constructor to be hooked
        }

        if (avowed_is_current_game()) {
            uint32_t stable_frames = 0;
            uint32_t required_frames = AVOWED_NATIVE_FIX_STABLE_FRAMES;

            if (!avowed_native_fix_gate_ready(&stable_frames, &required_frames)) {
                SPDLOG_INFO_EVERY_N_SEC(
                    2,
                    "[Avowed][NativeStereoFix] Returning one view while render transition stabilizes stable={}/{}",
                    stable_frames,
                    required_frames);
                return 1;
            }
        }
    }

    if (avowed_is_current_game() && vr->is_native_stereo_fix_enabled()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[Avowed][NativeStereoFix] Returning two views for native stereo fix");
    }

    return 2;
}

// Only really necessary for 5.0.3 because for some reason negative view index gets passed into it
// but 5.0.3 doesn't account for this and thinks it's a secondary pass
// so the purpose of the hook (mostly) is to make those return eSSP_FULL to fix a crash
EStereoscopicPass FFakeStereoRenderingHook::get_view_pass_for_index_hook(FFakeStereoRendering* stereo, bool stereo_requested, int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get view pass for index hook called! {} {}", stereo_requested, view_index);
#else
    SPDLOG_INFO_ONCE("get view pass for index hook called! {} {}", stereo_requested, view_index);
#endif

    // On 5.0.3 this check is not here, it was only added in 5.1
    // So we need to imitate it here to prevent a crash
    if (!stereo_requested || view_index < 0) {
        return EStereoscopicPass::eSSP_FULL;
    }

    return view_index % 2 == 0 ? EStereoscopicPass::eSSP_PRIMARY : EStereoscopicPass::eSSP_SECONDARY;
}

IStereoRenderTargetManager* FFakeStereoRenderingHook::get_render_target_manager_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get render target manager hook called!");
#else
    SPDLOG_INFO_ONCE("get render target manager hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    auto vr = VR::get();

    if (vr->is_stereo_emulation_enabled() || vr->is_extreme_compatibility_mode_enabled()) {
        return nullptr;
    }

    if (!vr->get_runtime()->got_first_poses || vr->is_hmd_active()) {
        if (g_hook->m_uses_old_rendertarget_manager) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_418;
        }

        if (g_hook->m_special_detected) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_special;
        }

        return &g_hook->m_rtm;
    }

    return nullptr;
}

IStereoLayers* FFakeStereoRenderingHook::get_stereo_layers_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get stereo layers hook called!");
#else
    SPDLOG_INFO_ONCE("get stereo layers hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    if (!VR::get()->get_runtime()->got_first_poses || VR::get()->is_hmd_active()) {
        /*static uint8_t fake_data[0x100]{};

        if (*(uintptr_t*)&fake_data == 0) {
            *(uintptr_t*)&fake_data = (uintptr_t)utility::get_executable() + 0x3D13420; // test
        }

        //return &g_hook->m_sl;
        return (IStereoLayers*)&fake_data;*/
    }

    return nullptr;
}

void FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("post calculate stereo projection matrix called!");
#else
    SPDLOG_INFO_ONCE("post calculate stereo projection matrix called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count || g_hook->m_hooked_alternative_localplayer_scan) {
        return;
    }

    auto vfunc = utility::find_virtual_function_start(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address());

    if (!vfunc) {
        // attempt to hook GetProjectionData instead to get the localplayer
        SPDLOG_INFO("Failed to find virtual function start for CalculateStereoProjectionMatrix, attempting to hook GetProjectionData instead...");

        if (!g_hook->m_projection_matrix_stack.empty() && g_hook->m_projection_matrix_stack.size() >= 3) {
            const auto post_get_projection_data = g_hook->m_projection_matrix_stack[2];

            const auto get_projection_data_candidate_1 = utility::find_function_start_with_call(post_get_projection_data);
            const auto get_projection_data_candidate_2 = utility::find_virtual_function_start(post_get_projection_data);

            // Select whichever one is closest to post_get_projection_data
            std::optional<uintptr_t> get_projection_data{};

            if (get_projection_data_candidate_1 && get_projection_data_candidate_2) {
                const auto candidate_1_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_1);
                const auto candidate_2_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_2);

                if (candidate_1_distance < candidate_2_distance) {
                    get_projection_data = get_projection_data_candidate_1;
                } else {
                    get_projection_data = get_projection_data_candidate_2;
                }
            } else if (get_projection_data_candidate_1) {
                get_projection_data = get_projection_data_candidate_1;
            } else if (get_projection_data_candidate_2) {
                get_projection_data = get_projection_data_candidate_2;
            } else {
                // emergency fallback
                SPDLOG_INFO("Failed to find GetProjectionData, falling back to emergency fallback (this may not work)");
                get_projection_data = utility::find_function_start(post_get_projection_data);
            }

            if (get_projection_data) {
                SPDLOG_INFO("Successfully found GetProjectionData at {:x}", *get_projection_data);

                g_hook->m_hooked_alternative_localplayer_scan = true;

                g_hook->m_get_projection_data_pre_hook = safetyhook::create_mid((void*)*get_projection_data, &FFakeStereoRenderingHook::pre_get_projection_data);
                g_hook->m_projection_matrix_stack.clear();

                if (g_hook->m_get_projection_data_pre_hook) {
                    SPDLOG_INFO("Successfully hooked GetProjectionData");
                    return;
                } else {
                    SPDLOG_ERROR("Failed to hook GetProjectionData");
                }
            } else {
                SPDLOG_ERROR("Failed to find GetProjectionData!");
            }
        }
    }

    if (!vfunc) {
        SPDLOG_INFO("Could not find function via normal means, scanning for int3s...");

        const auto ref = utility::scan_reverse(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address(), 0x2000, "CC CC CC");

        if (ref) {
            vfunc = *ref + 3;
        }

        if (!vfunc) {
            g_hook->m_fixed_localplayer_view_count = true;
            SPDLOG_ERROR("Failed to find virtual function start for post calculate_stereo_projection_matrix!");
            return;
        }
    }

    // Scan forward until we find an assignment of the RCX register into a storage register.
    std::unordered_map<uint32_t, uintptr_t*> register_to_context {
        { NDR_RBX, &ctx.rbx },
        { NDR_RCX, &ctx.rcx },
        { NDR_RDX, &ctx.rdx },
        { NDR_RSI, &ctx.rsi },
        { NDR_RDI, &ctx.rdi },
        { NDR_RBP, &ctx.rbp },
        { NDR_RSP, &ctx.rsp },
        { NDR_R8, &ctx.r8 },
        { NDR_R9, &ctx.r9 },
        { NDR_R10, &ctx.r10 },
        { NDR_R11, &ctx.r11 },
        { NDR_R12, &ctx.r12 },
        { NDR_R13, &ctx.r13 },
        { NDR_R14, &ctx.r14 },
        { NDR_R15, &ctx.r15 },
    };

    INSTRUX ix{};
    std::optional<uint32_t> found_register{};
    auto ip = (uint8_t*)vfunc.value_or(0);

    while (true) {
        const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

        if (!ND_SUCCESS(status)) {
            SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
            break;
        }

        if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_REG && ix.Operands[1].Info.Register.Reg == NDR_RCX) {
            SPDLOG_INFO("Found assignment of RCX to storage register at {:x} ({})!", (uintptr_t)ip, ix.Operands[0].Info.Register.Reg);
            found_register = ix.Operands[0].Info.Register.Reg;
            break;
        }

        ip += ix.Length;
    }

    if (!found_register) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find assignment of RCX to storage register!");
        return;
    }

    const auto localplayer = *register_to_context[found_register.value_or(0)];
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    if (avowed_is_current_game() && !avowed_is_live_uobject(localplayer)) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] CalculateStereoProjectionMatrix yielded non-UObject LocalPlayer candidate {:x}; switching to GetProjectionData scan",
            localplayer);

        auto try_hook_get_projection_data = [&]() -> bool {
            if (g_hook->m_hooked_alternative_localplayer_scan || g_hook->m_fixed_localplayer_view_count) {
                return false;
            }

            if (g_hook->m_projection_matrix_stack.size() < 3) {
                SPDLOG_WARNING_EVERY_N_SEC(
                    2,
                    "[Avowed][NativeStereoFix] Cannot install GetProjectionData LocalPlayer scan; projection stack has {} entries",
                    g_hook->m_projection_matrix_stack.size());
                return false;
            }

            const auto post_get_projection_data = g_hook->m_projection_matrix_stack[2];
            const auto get_projection_data_candidate_1 = utility::find_function_start_with_call(post_get_projection_data);
            const auto get_projection_data_candidate_2 = utility::find_virtual_function_start(post_get_projection_data);
            std::optional<uintptr_t> get_projection_data{};

            if (get_projection_data_candidate_1 && get_projection_data_candidate_2) {
                const auto candidate_1_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_1);
                const auto candidate_2_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_2);
                get_projection_data = candidate_1_distance < candidate_2_distance ? get_projection_data_candidate_1 : get_projection_data_candidate_2;
            } else if (get_projection_data_candidate_1) {
                get_projection_data = get_projection_data_candidate_1;
            } else if (get_projection_data_candidate_2) {
                get_projection_data = get_projection_data_candidate_2;
            } else {
                get_projection_data = utility::find_function_start(post_get_projection_data);
            }

            if (!get_projection_data) {
                SPDLOG_WARNING_EVERY_N_SEC(2, "[Avowed][NativeStereoFix] Failed to locate GetProjectionData; disabling LocalPlayer bootstrap spam");
                return false;
            }

            SPDLOG_INFO("[Avowed][NativeStereoFix] Hooking GetProjectionData at {:x}", *get_projection_data);

            auto hook = safetyhook::create_mid((void*)*get_projection_data, &FFakeStereoRenderingHook::pre_get_projection_data);
            if (!hook) {
                SPDLOG_ERROR("[Avowed][NativeStereoFix] Failed to hook GetProjectionData");
                return false;
            }

            g_hook->m_get_projection_data_pre_hook = std::move(hook);
            g_hook->m_hooked_alternative_localplayer_scan = true;
            g_hook->m_projection_matrix_stack.clear();
            SPDLOG_INFO("[Avowed][NativeStereoFix] Installed GetProjectionData LocalPlayer scan");
            return true;
        };

        if (!try_hook_get_projection_data()) {
            g_hook->m_fixed_localplayer_view_count = true;
        }

        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::pre_get_projection_data(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("pre get projection data called!");
#else
    SPDLOG_INFO_ONCE("pre get projection data called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    const auto localplayer = ctx.rcx;
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    if (avowed_is_current_game() && !avowed_is_live_uobject(localplayer)) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] GetProjectionData yielded non-UObject LocalPlayer candidate {:x}; disabling LocalPlayer bootstrap",
            localplayer);
        g_hook->m_fixed_localplayer_view_count = true;
        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::post_init_properties(uintptr_t localplayer) {
    SPDLOG_INFO("Searching for PostInitProperties virtual function...");

    if (is_deadzone_ue56_executable()) {
        // Deadzone Rogue's UE5.6.1 retail build can OOM while resolving
        // UObject::StaticClass() through FName/object scans during this
        // bootstrap. The game already reaches the stereo path without this
        // call, so fail closed instead of hammering the unsafe resolver.
        SPDLOG_WARN_ONCE("[Deadzone][PostInitProperties] Skipping UObject-based PostInitProperties resolver to avoid unsafe FName scan");
        g_hook->m_sceneview_data.known_scene_states.clear();
        g_hook->m_fixed_localplayer_view_count = true;
        return;
    }

    std::optional<uint32_t> idx{};
    const auto engine = sdk::UEngine::get_lvalue();

    if (engine == nullptr) {
        SPDLOG_ERROR("Cannot proceed without engine!");
        return;
    }

    uintptr_t vtable_address{};

    if (avowed_is_current_game()) {
        uintptr_t class_address{};
        if (!avowed_is_live_uobject(localplayer, &vtable_address, &class_address)) {
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Refusing PostInitProperties on unsafe LocalPlayer candidate {:x}",
                localplayer);
            g_hook->m_fixed_localplayer_view_count = true;
            return;
        }

        SPDLOG_INFO_ONCE("[Avowed][NativeStereoFix] LocalPlayer candidate validated via GUObjectArray (object={:x}, class={:x}, vtable={:x})",
            localplayer,
            class_address,
            vtable_address);
    } else {
        vtable_address = *(uintptr_t*)localplayer;
    }

    const auto vtable = (uintptr_t*)vtable_address;

    if (vtable == nullptr || IsBadReadPtr((void*)vtable, sizeof(void*))) {
        if (everwind_is_current_game()) {
            // Everwind's UE5.5 projection path can hand us a stable non-LocalPlayer
            // pointer every frame. Do not keep hammering PostInitProperties scans.
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[Everwind][PostInitProperties] Refusing invalid LocalPlayer candidate {:x}; disabling LocalPlayer bootstrap",
                localplayer);
            g_hook->m_fixed_localplayer_view_count = true;
        } else {
            SPDLOG_ERROR("Cannot proceed, vtable for so-called \"local player\" is invalid!");
        }
        return;
    }

    const auto ue51_post_init = is_ue_5_1_dx_backend();
    const auto needs_source_informed_post_init = is_ue_5_7_or_newer() || is_ue_5_6_dx12_backend() || is_ue_5_5_dx_backend() || is_ue_5_4_dx_backend() || ue51_post_init;

    if (needs_source_informed_post_init) {
        idx = resolve_post_init_properties_index_from_uobject(localplayer);
    }

    if (ue51_post_init && !idx) {
        g_hook->m_sceneview_data.known_scene_states.clear();
        g_hook->m_fixed_localplayer_view_count = true;
        return;
    }

    for (auto i = 1; !idx && i < 25; ++i) {
        if (idx) {
            break;
        }

        SPDLOG_INFO("Analyzing index {}...", i);

        const auto vfunc = vtable[i];

        if (vfunc == 0 || IsBadReadPtr((void*)vfunc, 1)) {
            SPDLOG_ERROR("Encountered invalid vfunc at index {}!", i);
            break;
        }

        SPDLOG_INFO("Scanning vfunc at index {} ({:x})...", i, vfunc);

        utility::exhaustive_decode((uint8_t*)vfunc, 25, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (idx) {
                return utility::ExhaustionResult::BREAK;
            }

            if (const auto disp = utility::resolve_displacement(ip); disp) {
                // the second expression catches UE dynamic/debug builds
                if (*disp == (uintptr_t)engine || 
                    (!IsBadReadPtr((void*)*disp, sizeof(void*)) && *(uintptr_t*)*disp == (uintptr_t)*engine)) 
                {
                    SPDLOG_INFO("Found PostInitProperties through legacy body scan at {} {:x}!", i, (uintptr_t)vfunc);
                    idx = i;
                    return utility::ExhaustionResult::BREAK;
                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });
    }

    if (!idx) {
        if (needs_source_informed_post_init) {
            SPDLOG_WARN("Failed to find PostInitProperties virtual function on UE 5.4+/modern path; skipping LocalPlayer bootstrap for safety");
            g_hook->m_sceneview_data.known_scene_states.clear();
            g_hook->m_fixed_localplayer_view_count = true;
            return;
        }

        SPDLOG_ERROR("Failed to find PostInitProperties virtual function! A crash may occur!");
    }

    // Now call PostInitProperties.
    // The purpose of this is setting up the view for the other eye.
    // Just creating the StereoRenderingDevice does not automatically do it, so we have to do it manually.
    // Usually the game just calls this function near startup after calling InitializeHMDDevice.
    if (idx) {
        SPDLOG_INFO("Calling PostInitProperties on local player!");

        // Get PEB and set debugger present
        auto peb = (PEB*)__readgsqword(0x60);

        const auto old = peb->BeingDebugged;
        peb->BeingDebugged = true;

        // If the exception count exceeds a certain amount, we need to un-nop the function call because it was supposed to return a pointer.
        static auto exception_count = 0;
        static std::vector<Patch::Ptr> patches{};
        static std::vector<uintptr_t> patch_locations{};

        static std::vector<Patch::Ptr> assert_patches{};
        const void (*post_init_properties)(uintptr_t) = (*(decltype(post_init_properties)**)localplayer)[*idx];

        // Scan through all of the branches of PostInitProperties to find any assertions
        // The assertion we're looking for is easily identified by a string that it loads in RCX, named "!Reference"
        // If we dont do this, there's a possibility that the game will crash at some point or cause some sort of corruption
        utility::exhaustive_decode((uint8_t*)post_init_properties, 100, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (ix.Operands[1].Type == ND_OP_MEM) {
                const auto referenced_addr = utility::resolve_displacement(ip);

                if (referenced_addr) try {
                    if (std::string_view{(const char*)*referenced_addr}.starts_with("!Reference")) {
                        // Scan forward and patch out the first call or jmp we run into
                        utility::exhaustive_decode((uint8_t*)ip, 10, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
                            if (*(uint8_t*)ip == 0xE8) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0x90, 0x90, 0x90, 0x90, 0x90 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (*(uint8_t*)ip == 0xE9) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0xC3 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                std::vector<int16_t> nop{};
                                for (auto i = 0; i < ix.Length; ++i) {
                                    nop.push_back(0x90);
                                }

                                assert_patches.push_back(Patch::create(ip, nop));
                                return utility::ExhaustionResult::BREAK;
                            }

                            return utility::ExhaustionResult::CONTINUE;
                        });
                    }
                } catch(...) {

                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        // set up a handler to skip int3 assertions
        // we do this because debug builds assert when the views are already setup.
        const auto seh_handler = [](PEXCEPTION_POINTERS info) -> LONG {
            ++exception_count;

            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
                SPDLOG_INFO("Skipping int3 breakpoint at {:x}!", info->ContextRecord->Rip);
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    SPDLOG_INFO("Skipping {} bytes!", insn->Length);
                    info->ContextRecord->Rip += insn->Length;

                    // Nop out the next function call.
                    // It logs and does some other stuff and causes a crash later on.
                    // To be seen if this will cause any issues, does not appear to (on 4.9 debug builds)
                    const auto call = utility::scan_disasm((uintptr_t)info->ContextRecord->Rip, 20, "E8 ? ? ? ?");

                    if (call) {
                        patch_locations.push_back(*call);
                        patches.emplace_back(Patch::create(*call, {0x90, 0x90, 0x90, 0x90, 0x90}));
                    }

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            SPDLOG_INFO("Encountered exception {:x} at {:x}!", info->ExceptionRecord->ExceptionCode, info->ContextRecord->Rip);

            // This happens if we removed a call that shouldn't have been removed.
            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !patches.empty()) {
                SPDLOG_WARN("Access violation at {:x}! Removing patch at {:x}!", info->ContextRecord->Rip, patch_locations.back());

                exception_count = 0;
                info->ContextRecord->Rip = patch_locations.back();
                patches.pop_back();
                patch_locations.pop_back();
            } else {
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    info->ContextRecord->Rip += insn->Length;
                } else {
                    info->ContextRecord->Rip += 1;
                }
            }

            // yolo? idk xd
            return EXCEPTION_CONTINUE_EXECUTION;
        };

        const auto exception_handler = AddVectoredExceptionHandler(1, seh_handler);

        m_sceneview_data.inside_post_init_properties = true;
        post_init_properties(localplayer);
        m_sceneview_data.inside_post_init_properties = false;

        SPDLOG_INFO("PostInitProperties called!");

        // remove the handler
        RemoveVectoredExceptionHandler(exception_handler);
        peb->BeingDebugged = old;
    }

    g_hook->m_sceneview_data.known_scene_states.clear();
    g_hook->m_fixed_localplayer_view_count = true;
}

void FFakeStereoRenderingHook::ue57_add_slate_draw_elements_pass_hook(safetyhook::Context& ctx) {
    if (g_hook == nullptr || !is_ue_5_7_or_newer() || !g_framework->is_dx12()) {
        return;
    }

    if (!g_hook->m_inside_slate_draw_window || GetCurrentThreadId() != g_hook->m_slate_draw_window_thread_id) {
        return;
    }

    auto* inputs = reinterpret_cast<UE57SlateDrawElementsPassInputsHead*>(ctx.r8);

    if (!looks_like_ue57_slate_draw_elements_inputs(inputs)) {
        return;
    }

    const auto scene_viewport_texture = inputs->scene_viewport_texture;
    const auto elements_texture = inputs->elements_texture;

    static bool logged_valid_path{false};

    if (elements_texture != scene_viewport_texture) {
        static bool logged_separate_path{false};

        if (!logged_valid_path) {
            logged_valid_path = true;
            SPDLOG_INFO("[UE 5.7 Slate] Validated AddSlateDrawElementsPass inputs");
        }

        if (!logged_separate_path) {
            logged_separate_path = true;
            SPDLOG_INFO("[UE 5.7 Slate] DrawElements pass already has a separate ElementsTexture");
        }
    } else {
        if (!logged_valid_path) {
            logged_valid_path = true;
            SPDLOG_INFO("[UE 5.7 Slate] Validated AddSlateDrawElementsPass inputs");
        }

        SPDLOG_INFO_EVERY_N_SEC(5, "[UE 5.7 Slate] DrawElements pass is still aliasing ElementsTexture to SceneViewportTexture");
    }
}

namespace {
struct UE55SlateDrawWindowPassInputsHead {
    void* renderer;
    void* window_element_list;
    void* window;
    sdk::FViewportInfo* viewport_info;
};

struct UE55SlatePostProcessArrayView {
    void* data;
    uint64_t count;
};

struct UE55FIntPoint {
    int32_t x;
    int32_t y;
};

struct UE55FIntRect {
    UE55FIntPoint min;
    UE55FIntPoint max;
};

struct UE55SlateDrawWindowPassInputs {
    void* renderer;
    void* window_element_list;
    void* window;
    sdk::FViewportInfo* viewport_info;
    UE55SlatePostProcessArrayView post_process_requests;
    UE55FIntPoint cursor_position;
    UE55FIntRect scene_view_rect;
    float viewport_scale_ui;
};

struct UE55SlateDrawWindowPassOutputs {
    void* viewport_rhi;
    FRHITexture2D* viewport_texture_rhi;
    FRHITexture2D* output_texture_rhi;
};

struct UE55SlateDrawWindowsArrayView {
    UE55SlateDrawWindowPassInputs* data;
    int32_t count;
    int32_t padding;
};

struct UE55SlateExtent {
    uint32_t width{};
    uint32_t height{};
};

bool try_read_ue55_slate_draw_inputs(void* candidate, void* renderer, UE55SlateDrawWindowPassInputsHead& out) {
    if (!is_readable_process_range((uintptr_t)candidate, sizeof(UE55SlateDrawWindowPassInputsHead))) {
        return false;
    }

    memcpy(&out, candidate, sizeof(out));

    if (out.renderer != renderer || out.window == nullptr) {
        return false;
    }

    return is_readable_process_range((uintptr_t)out.window, sizeof(void*));
}

bool try_read_ue55_slate_draw_inputs_full(void* candidate, void* renderer, UE55SlateDrawWindowPassInputs& out) {
    if (!is_readable_process_range((uintptr_t)candidate, sizeof(UE55SlateDrawWindowPassInputs))) {
        return false;
    }

    memcpy(&out, candidate, sizeof(out));

    if (out.renderer != renderer || out.window == nullptr) {
        return false;
    }

    return is_readable_process_range((uintptr_t)out.window, sizeof(void*));
}

bool try_read_ue55_slate_draw_windows_first_input(
    void* candidate,
    void* renderer,
    UE55SlateDrawWindowPassInputsHead& out_head,
    UE55SlateDrawWindowPassInputs& out_full,
    bool& out_has_full)
{
    out_has_full = false;

    if (!is_readable_process_range((uintptr_t)candidate, sizeof(UE55SlateDrawWindowsArrayView))) {
        return false;
    }

    UE55SlateDrawWindowsArrayView windows{};
    memcpy(&windows, candidate, sizeof(windows));

    if (windows.data == nullptr || windows.count <= 0 || windows.count > 16) {
        return false;
    }

    if (!try_read_ue55_slate_draw_inputs(windows.data, renderer, out_head)) {
        return false;
    }

    out_has_full = try_read_ue55_slate_draw_inputs_full(windows.data, renderer, out_full);
    return true;
}

std::optional<UE55SlateExtent> ue55_get_slate_expected_extent(const UE55SlateDrawWindowPassInputs& inputs) {
    const auto width = inputs.scene_view_rect.max.x - inputs.scene_view_rect.min.x;
    const auto height = inputs.scene_view_rect.max.y - inputs.scene_view_rect.min.y;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return std::nullopt;
    }

    return UE55SlateExtent{(uint32_t)width, (uint32_t)height};
}

bool looks_like_vtable_object(void* object) {
    uintptr_t vtable{};
    return safe_read_value((uintptr_t)object, vtable) && looks_like_virtual_function_table(vtable);
}

bool try_call_slate_viewport_bool_slot(sdk::ISlateViewport* viewport, size_t slot, bool& out) {
    out = false;

    if (viewport == nullptr || !looks_like_vtable_object(viewport)) {
        return false;
    }

    uintptr_t vtable{};
    if (!safe_read_value((uintptr_t)viewport, vtable) ||
        !is_readable_process_range(vtable + (slot * sizeof(void*)), sizeof(void*)))
    {
        return false;
    }

    uintptr_t fn{};
    if (!safe_read_value(vtable + (slot * sizeof(void*)), fn) ||
        fn == 0 ||
        !is_executable_process_range(fn, 1))
    {
        return false;
    }

    using BoolVirtualFn = bool (*)(sdk::ISlateViewport*);

    __try {
        out = ((BoolVirtualFn)fn)(viewport);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

sdk::FSlateResource* try_get_slate_viewport_render_target_texture(sdk::ISlateViewport* viewport, bool& faulted) {
    faulted = false;

    __try {
        return viewport != nullptr ? viewport->GetViewportRenderTargetTexture() : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

bool ue55_is_valid_ui_texture_candidate(
    VRRenderTargetManager_Base* rtm,
    FRHITexture2D* texture,
    std::optional<UE55SlateExtent> expected_extent,
    const char* source)
{
    if (!supports_ue55_dedicated_ui_target_for_current_game() || rtm == nullptr || texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return false;
    }

    if (texture == rtm->get_render_target()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] rejecting {} candidate because it matches the scene render target: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    const auto desc = ue55_try_get_d3d12_desc(texture, source);
    if (!desc) {
        return false;
    }

    if (!expected_extent || expected_extent->width == 0 || expected_extent->height == 0) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[UE5.5][SlateUI] observing {} candidate tex={:x} [{}x{} fmt={} flags=0x{:x}] but no trusted Slate extent is available yet",
            source != nullptr ? source : "<unknown>",
            (uintptr_t)texture,
            desc->Width,
            desc->Height,
            (uint32_t)desc->Format,
            (uint32_t)desc->Flags);
        return false;
    }

    if (desc->Width != expected_extent->width || desc->Height != expected_extent->height) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[UE5.5][SlateUI] rejecting {} candidate tex={:x} because extent [{}x{}] != expected Slate [{}x{}]",
            source != nullptr ? source : "<unknown>",
            (uintptr_t)texture,
            desc->Width,
            desc->Height,
            expected_extent->width,
            expected_extent->height);
        return false;
    }

    SPDLOG_INFO_EVERY_N_SEC(2,
        "[UE5.5][SlateUI] accepted {} candidate tex={:x} [{}x{} fmt={} flags=0x{:x}]",
        source != nullptr ? source : "<unknown>",
        (uintptr_t)texture,
        desc->Width,
        desc->Height,
        (uint32_t)desc->Format,
        (uint32_t)desc->Flags);

    return true;
}

void ue55_promote_slate_outputs(
    VRRenderTargetManager_Base* rtm,
    void* outputs_ptr,
    std::optional<UE55SlateExtent> expected_extent)
{
    if (!supports_ue55_dedicated_ui_target_for_current_game() || rtm == nullptr || outputs_ptr == nullptr) {
        return;
    }

    if (!is_readable_process_range((uintptr_t)outputs_ptr, sizeof(UE55SlateDrawWindowPassOutputs))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] DrawWindow outputs are not readable yet: {:x}", (uintptr_t)outputs_ptr);
        return;
    }

    UE55SlateDrawWindowPassOutputs outputs{};
    memcpy(&outputs, outputs_ptr, sizeof(outputs));

    SPDLOG_INFO_EVERY_N_SEC(2,
        "[UE5.5][SlateUI] DrawWindow outputs viewport_rhi={:x} viewport_texture={:x} output_texture={:x} expected={}x{}",
        (uintptr_t)outputs.viewport_rhi,
        (uintptr_t)outputs.viewport_texture_rhi,
        (uintptr_t)outputs.output_texture_rhi,
        expected_extent ? expected_extent->width : 0,
        expected_extent ? expected_extent->height : 0);

    if (ue55_is_valid_ui_texture_candidate(rtm, outputs.viewport_texture_rhi, expected_extent, "DrawWindow viewport texture output")) {
        if (rtm->get_dedicated_ui_target() != outputs.viewport_texture_rhi) {
            rtm->set_dedicated_ui_target(outputs.viewport_texture_rhi, expected_extent->width, expected_extent->height);
            rtm->get_fallback_ui_target_ref() = nullptr;
            if (should_preserve_promoted_ue55_slate_target()) {
                rtm->cancel_dedicated_ui_creation_preserving_target("UE5.5 promoted DrawWindow viewport texture");
            }
            SPDLOG_WARN("[UE5.5][SlateUI] promoted DrawWindow viewport texture output as dedicated UI target");
        }
        return;
    }

    if (ue55_is_valid_ui_texture_candidate(rtm, outputs.output_texture_rhi, expected_extent, "DrawWindow output texture")) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[UE5.5][SlateUI] DrawWindow output texture is window-sized but not promoted; explicit SlateOutputTexture routing remains preferred");
    }
}

bool ue55_try_promote_fallback_ui_target(
    VRRenderTargetManager_Base* rtm,
    std::optional<UE55SlateExtent> expected_extent,
    const char* source)
{
    if (!(everwind_is_current_game() || is_deadzone_ue56_executable()) || rtm == nullptr || !expected_extent) {
        return false;
    }

    auto* fallback = rtm->get_fallback_ui_target_ref();

    if (!ue55_is_valid_ui_texture_candidate(rtm, fallback, expected_extent, source)) {
        return false;
    }

    if (rtm->get_dedicated_ui_target() != fallback) {
        rtm->set_dedicated_ui_target(fallback, expected_extent->width, expected_extent->height);
        rtm->get_fallback_ui_target_ref() = nullptr;
        rtm->cancel_dedicated_ui_creation_preserving_target(
            is_deadzone_ue56_executable() ? "Deadzone D3D12 UI fallback target" : "Everwind D3D12 UI fallback target");
        SPDLOG_WARN("[UE5.5][SlateUI] promoted {} D3D12 UI fallback target as dedicated UI target",
            is_deadzone_ue56_executable() ? "Deadzone" : "Everwind");
    }

    return true;
}
}

void FFakeStereoRenderingHook::ue55_slate_output_texture_register_hook(safetyhook::Context& ctx) {
    if (g_hook == nullptr || !supports_ue55_dedicated_ui_target_for_current_game()) {
        return;
    }

    if (!g_hook->m_inside_slate_draw_window || GetCurrentThreadId() != g_hook->m_slate_draw_window_thread_id) {
        return;
    }

    if (ctx.r8 == 0 ||
        !is_readable_process_range(ctx.r8, sizeof(wchar_t) * 19) ||
        !std::wstring_view{(const wchar_t*)ctx.r8, 18}.starts_with(L"SlateOutputTexture"))
    {
        return;
    }

    auto* rtm = g_hook->get_render_target_manager();
    auto* ui_target = rtm != nullptr ? rtm->get_dedicated_ui_target() : nullptr;

    if ((everwind_is_current_game() || is_deadzone_ue56_executable()) &&
        rtm != nullptr &&
        (ui_target == nullptr || ui_target == rtm->get_render_target()))
    {
        auto* fallback = rtm->get_fallback_ui_target_ref();

        if (fallback != nullptr && fallback != rtm->get_render_target() && !IsBadReadPtr(fallback, sizeof(void*))) {
            ui_target = fallback;
        }
    }

    if (rtm == nullptr || ui_target == nullptr || ui_target == rtm->get_render_target()) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[UE5.5][SlateUI] SlateOutputTexture call reached before a valid dedicated UI target exists");
        return;
    }

    const auto desc = ue55_try_get_d3d12_desc(ui_target, "dedicated UI target at SlateOutputTexture");
    if (!desc) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[UE5.5][SlateUI] dedicated UI target is not a valid D3D12 texture yet: {:x}", (uintptr_t)ui_target);
        return;
    }

    const auto expected_width = rtm->get_dedicated_ui_width();
    const auto expected_height = rtm->get_dedicated_ui_height();

    if (expected_width != 0 && expected_height != 0 &&
        (desc->Width != expected_width || desc->Height != expected_height))
    {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[UE5.5][SlateUI] refusing SlateOutputTexture replacement because dedicated UI target extent [{}x{}] != requested [{}x{}]",
            desc->Width,
            desc->Height,
            expected_width,
            expected_height);
        return;
    }

    const auto original = (FRHITexture2D*)ctx.rdx;
    const auto original_desc = ue55_try_get_d3d12_desc(original, "original SlateOutputTexture");

    SPDLOG_INFO_EVERY_N_SEC(1,
        "[UE5.5][SlateUI] routing SlateOutputTexture original={:x} [{}x{}] -> dedicated={:x} [{}x{} fmt={}]",
        (uintptr_t)original,
        original_desc ? original_desc->Width : 0,
        original_desc ? original_desc->Height : 0,
        (uintptr_t)ui_target,
        desc->Width,
        desc->Height,
        (uint32_t)desc->Format);

    ctx.rdx = (uintptr_t)ui_target;
}

namespace {
struct DaysGoneRawTArray {
    uintptr_t data{};
    int32_t count{};
    int32_t max{};
};

struct DaysGoneVec2 {
    float x{};
    float y{};
};

struct DaysGoneVec3 {
    float x{};
    float y{};
    float z{};
};

struct DaysGoneVec4 {
    float x{};
    float y{};
    float z{};
    float w{};
};

struct DaysGoneWidgetTransform {
    DaysGoneVec2 translation{};
    DaysGoneVec2 scale{1.0f, 1.0f};
    DaysGoneVec2 shear{};
    float angle{};
};

struct DaysGoneSlateWidgetOriginalState {
    uintptr_t widget{};
    bool captured{};
    DaysGoneWidgetTransform transform{};
    DaysGoneVec2 pivot{};
};

struct DaysGoneViewportRootOriginalState {
    uintptr_t widget{};
    bool captured{};
    DaysGoneVec4 color_and_opacity{1.0f, 1.0f, 1.0f, 1.0f};
};

struct DaysGoneIntPoint {
    int32_t x{};
    int32_t y{};
};

std::array<DaysGoneSlateWidgetOriginalState, 16> g_daysgone_slate_widget_originals{};
std::array<DaysGoneViewportRootOriginalState, 16> g_daysgone_viewport_root_originals{};
uint64_t g_daysgone_slate_widget_apply_count{};
uint64_t g_daysgone_slate_widget_restore_count{};
struct DaysGoneSlateCompositeCVarState {
    sdk::IConsoleVariable* cvar{};
    bool looked_up{};
    bool has_original{};
    int32_t original_value{};
    bool forced{};
    bool logged_missing{};
} g_daysgone_slate_composite_cvar{};

bool daysgone_object_pointer_is_readable(sdk::UObjectBase* object);
std::string daysgone_describe_uobject(sdk::UObjectBase* object);

struct DaysGoneD3D11TextureCandidate {
    ID3D11Texture2D* native{};
    D3D11_TEXTURE2D_DESC desc{};
    uintptr_t outer_offset{std::numeric_limits<uintptr_t>::max()};
    uintptr_t inner_offset{std::numeric_limits<uintptr_t>::max()};
};

bool daysgone_is_valid_d3d11_desc(const D3D11_TEXTURE2D_DESC& desc) {
    return desc.Width >= 640 && desc.Height >= 360 && desc.Width <= 8192 && desc.Height <= 8192;
}

bool daysgone_is_plausible_slate_ui_desc(const D3D11_TEXTURE2D_DESC& desc) {
    if (!daysgone_is_valid_d3d11_desc(desc)) {
        return false;
    }

    if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        return false;
    }

    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        break;
    default:
        return false;
    }

    // Days Gone exposes the Bend SlateIntermediateBuffer as an HMD-sized target
    // in VR. Accept both a normal 16:9 UI texture and the HMD-shaped buffer; the
    // D3D11 path keys black out before submitting it as a UEVR UI layer.
    const auto aspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
    const auto looks_like_flat_ui = desc.Width >= 1280 && desc.Height >= 720 && desc.Width > desc.Height && aspect >= 1.45f && aspect <= 2.25f;
    const auto looks_like_hmd_ui = desc.Width >= 1280 && desc.Height >= 720 && aspect >= 0.75f && aspect <= 1.35f;
    return looks_like_flat_ui || looks_like_hmd_ui;
}

bool daysgone_is_d3d11_or_dxgi_com_object(IUnknown* object) {
    if (object == nullptr || !is_readable_process_range((uintptr_t)object, sizeof(void*))) {
        return false;
    }

    auto* const vtable = *(void**)object;
    if (vtable == nullptr || !is_readable_process_range((uintptr_t)vtable, sizeof(void*))) {
        return false;
    }

    const auto module = utility::get_module_within(vtable);
    if (!module) {
        return false;
    }

    const auto module_path = utility::get_module_path(*module);
    if (!module_path) {
        return false;
    }

    auto lower = std::string{*module_path};
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("d3d11") != std::string::npos || lower.find("dxgi") != std::string::npos;
}

bool daysgone_try_query_d3d11_texture(IUnknown* object, ID3D11Texture2D*& out_native, D3D11_TEXTURE2D_DESC& out_desc) {
    out_native = nullptr;
    out_desc = {};

    if (!daysgone_is_d3d11_or_dxgi_com_object(object)) {
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();

        if (daysgone_is_valid_d3d11_desc(desc)) {
            out_native = texture;
            out_desc = desc;
            return true;
        }
    }

    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&srv))) && srv != nullptr) {
        ID3D11Resource* resource = nullptr;
        srv->GetResource(&resource);
        srv->Release();

        if (resource != nullptr) {
            texture = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                texture->Release();
                resource->Release();

                if (daysgone_is_valid_d3d11_desc(desc)) {
                    out_native = texture;
                    out_desc = desc;
                    return true;
                }

                return false;
            }

            resource->Release();
        }
    }

    return false;
}

bool daysgone_try_read_pointer(uintptr_t address, uintptr_t& out) {
    out = 0;
    if (address == 0 || !is_readable_process_range(address, sizeof(uintptr_t))) {
        return false;
    }

    out = *reinterpret_cast<const uintptr_t*>(address);
    return out >= 0x10000;
}

bool daysgone_try_get_d3d11_texture_candidate(FRHITexture2D* texture, DaysGoneD3D11TextureCandidate& out_candidate) {
    out_candidate = {};

    if (texture == nullptr || !is_readable_process_range((uintptr_t)texture, sizeof(void*))) {
        return false;
    }

    auto try_native_texture_pointer = [&](uintptr_t raw_native, uintptr_t outer_offset, uintptr_t inner_offset) {
        auto* native = reinterpret_cast<ID3D11Texture2D*>(raw_native);
        if (!daysgone_is_d3d11_or_dxgi_com_object(reinterpret_cast<IUnknown*>(native))) {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        native->GetDesc(&desc);
        if (!daysgone_is_valid_d3d11_desc(desc)) {
            return false;
        }

        out_candidate.native = native;
        out_candidate.desc = desc;
        out_candidate.outer_offset = outer_offset;
        out_candidate.inner_offset = inner_offset;
        return true;
    };

    // Live Days Gone evidence shows BendTemporalAA's SlateIntermediateBuffer
    // local is an UE4.11 wrapper: wrapper lives at +0x8/+0x10, and its native
    // ID3D11Texture2D lives at +0xA0. Keep this fixed and fail-closed; broad
    // COM probing here can destabilize injection.
    constexpr std::array<uintptr_t, 2> kWrapperOffsets{0x8, 0x10};
    constexpr uintptr_t kNativeTextureOffset = 0xA0;

    const auto base = reinterpret_cast<uintptr_t>(texture);
    for (const auto outer_offset : kWrapperOffsets) {
        uintptr_t wrapper{};
        if (!daysgone_try_read_pointer(base + outer_offset, wrapper)) {
            continue;
        }

        uintptr_t native{};
        if (daysgone_try_read_pointer(wrapper + kNativeTextureOffset, native) &&
            try_native_texture_pointer(native, outer_offset, kNativeTextureOffset))
        {
            return true;
        }
    }

    return false;
}

bool daysgone_try_get_d3d11_texture_desc(FRHITexture2D* texture, D3D11_TEXTURE2D_DESC& out_desc) {
    DaysGoneD3D11TextureCandidate candidate{};
    if (!daysgone_try_get_d3d11_texture_candidate(texture, candidate)) {
        return false;
    }

    out_desc = candidate.desc;
    return true;
}
template <typename T>
bool daysgone_read_value(uintptr_t address, T& out) {
    if (address == 0 || !is_readable_process_range(address, sizeof(T))) {
        return false;
    }

    out = *reinterpret_cast<const T*>(address);
    return true;
}

template <typename T>
bool daysgone_write_value(uintptr_t address, const T& value) {
    if (address == 0 || !is_writable_process_range(address, sizeof(T))) {
        return false;
    }

    *reinterpret_cast<T*>(address) = value;
    return true;
}

bool daysgone_write_bool_bit(uintptr_t address, bool value) {
    uint8_t current{};
    if (!daysgone_read_value(address, current)) {
        return false;
    }

    current = static_cast<uint8_t>((current & ~uint8_t{1}) | (value ? uint8_t{1} : uint8_t{0}));
    return daysgone_write_value(address, current);
}

void daysgone_set_disable_slate_composite(bool enabled) {
    try {
        auto& state = g_daysgone_slate_composite_cvar;

        if (!state.looked_up) {
            state.looked_up = true;
            const auto console_manager = sdk::FConsoleManager::get();
            if (console_manager != nullptr) {
                auto* object = console_manager->find(L"r.Bend.TemporalAA.DisableSlateComposite");
                if (object != nullptr && object->AsCommand() == nullptr) {
                    state.cvar = static_cast<sdk::IConsoleVariable*>(object);
                }
            }
        }

        if (state.cvar == nullptr) {
            if (!state.logged_missing) {
                state.logged_missing = true;
                SPDLOG_WARN("[DaysGone][SlateOverlay] r.Bend.TemporalAA.DisableSlateComposite unavailable");
            }
            return;
        }

        if (enabled) {
            if (!state.has_original) {
                state.original_value = state.cvar->GetInt();
                state.has_original = true;
            }

            if (state.cvar->GetInt() != 1) {
                state.cvar->Set(L"1");
            }

            if (!state.forced) {
                SPDLOG_INFO("[DaysGone][SlateOverlay] Disabled Bend in-scene Slate composite; using extracted UEVR UI layer");
                state.forced = true;
            }
            return;
        }

        if (state.forced || state.has_original) {
            const auto restore_value = state.has_original ? state.original_value : 0;
            if (state.cvar->GetInt() != restore_value) {
                state.cvar->Set(std::to_wstring(restore_value).c_str());
            }

            if (state.forced) {
                SPDLOG_INFO("[DaysGone][SlateOverlay] Restored Bend in-scene Slate composite to {}", restore_value);
            }
        }

        state.forced = false;
        state.has_original = false;
        state.original_value = 0;
    } catch (...) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[DaysGone][SlateOverlay] Failed to update r.Bend.TemporalAA.DisableSlateComposite");
    }
}

bool daysgone_capture_slate_widget_original(sdk::UObjectBase* widget) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return false;
    }

    const auto widget_address = reinterpret_cast<uintptr_t>(widget);
    for (auto& state : g_daysgone_slate_widget_originals) {
        if (state.captured && state.widget == widget_address) {
            return true;
        }
    }

    for (auto& state : g_daysgone_slate_widget_originals) {
        if (state.captured) {
            continue;
        }

        state.widget = widget_address;
        state.captured = true;
        daysgone_read_value(widget_address + 0xB0, state.transform);
        daysgone_read_value(widget_address + 0xCC, state.pivot);
        SPDLOG_INFO(
            "[DaysGone][SlateWidgetFix] Captured widget original {} transform=({:.1f},{:.1f}) scale=({:.3f},{:.3f}) pivot=({:.2f},{:.2f})",
            daysgone_describe_uobject(widget),
            state.transform.translation.x,
            state.transform.translation.y,
            state.transform.scale.x,
            state.transform.scale.y,
            state.pivot.x,
            state.pivot.y);
        return true;
    }

    SPDLOG_WARN_ONCE("[DaysGone][SlateWidgetFix] Original state cache is full; cannot capture more Slate widgets");
    return false;
}

void daysgone_call_widget_vec2_function(sdk::UObjectBase* widget, const wchar_t* function_name, DaysGoneVec2 value) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    struct Params {
        DaysGoneVec2 value;
    } params{value};

    widget->call_function(function_name, &params);
}

void daysgone_call_widget_transform_function(sdk::UObjectBase* widget, DaysGoneWidgetTransform value) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    struct Params {
        DaysGoneWidgetTransform value;
    } params{value};

    widget->call_function(L"SetRenderTransform", &params);
}

void daysgone_call_widget_position_in_viewport(sdk::UObjectBase* widget, DaysGoneVec2 position, bool remove_dpi_scale) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    struct Params {
        DaysGoneVec2 position;
        bool remove_dpi_scale;
    } params{position, remove_dpi_scale};

    widget->call_function(L"SetPositionInViewport", &params);
}

void daysgone_call_user_widget_color_and_opacity(sdk::UObjectBase* widget, DaysGoneVec4 value) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    struct Params {
        DaysGoneVec4 value;
    } params{value};

    widget->call_function(L"SetColorAndOpacity", &params);
}

void daysgone_call_widget_no_param_function(sdk::UObjectBase* widget, const wchar_t* function_name) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    uint8_t unused_params{};
    widget->call_function(function_name, &unused_params);
}

bool daysgone_widget_is_viewport_root_candidate(sdk::UObjectBase* widget) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return false;
    }

    const auto desc = daysgone_describe_uobject(widget);
    return desc.find("UI_MainMenuWidget_C") != std::string::npos ||
        desc.find("UI_HudWidget_C") != std::string::npos ||
        desc.find("UI_SubtitleWidget_C") != std::string::npos ||
        desc.find("UI_MegaMenu_C") != std::string::npos ||
        desc.find("OptionsMenuWidget_C") != std::string::npos ||
        desc.find("OptionsTopMenuWidget_C") != std::string::npos;
}

bool daysgone_capture_viewport_root_original(sdk::UObjectBase* widget) {
    if (!daysgone_widget_is_viewport_root_candidate(widget)) {
        return false;
    }

    const auto widget_address = reinterpret_cast<uintptr_t>(widget);
    for (auto& state : g_daysgone_viewport_root_originals) {
        if (state.captured && state.widget == widget_address) {
            return true;
        }
    }

    for (auto& state : g_daysgone_viewport_root_originals) {
        if (state.captured) {
            continue;
        }

        state.widget = widget_address;
        state.captured = true;
        daysgone_read_value(widget_address + 0x120, state.color_and_opacity);

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[DaysGone][ViewportSlotFix] captured root opacity {} rgba=({:.3f},{:.3f},{:.3f},{:.3f})",
            daysgone_describe_uobject(widget),
            state.color_and_opacity.x,
            state.color_and_opacity.y,
            state.color_and_opacity.z,
            state.color_and_opacity.w);
        return true;
    }

    SPDLOG_WARN_ONCE("[DaysGone][ViewportSlotFix] Root original-state cache is full; opacity restore may be incomplete");
    return false;
}

bool daysgone_restore_viewport_root_original_opacity(sdk::UObjectBase* widget) {
    if (!daysgone_widget_is_viewport_root_candidate(widget)) {
        return false;
    }

    const auto widget_address = reinterpret_cast<uintptr_t>(widget);
    for (auto& state : g_daysgone_viewport_root_originals) {
        if (!state.captured || state.widget != widget_address) {
            continue;
        }

        daysgone_write_value(widget_address + 0x120, state.color_and_opacity);
        daysgone_call_user_widget_color_and_opacity(widget, state.color_and_opacity);
        state = {};
        return true;
    }

    return false;
}

bool daysgone_apply_user_widget_viewport_slot(sdk::UObjectBase* widget, DaysGoneVec2 translation, float scale, float opacity) {
    if (!daysgone_widget_is_viewport_root_candidate(widget)) {
        return false;
    }

    daysgone_capture_viewport_root_original(widget);

    const auto clamped_scale = std::clamp(scale, 0.05f, 8.0f);
    const auto clamped_opacity = std::clamp(opacity, 0.0f, 2.0f);
    const DaysGoneVec2 alignment{0.5f, 0.5f};
    const DaysGoneVec2 desired_size{1920.0f, 1080.0f};
    const DaysGoneVec2 viewport_position{960.0f + translation.x, 540.0f + translation.y};
    const DaysGoneVec2 pivot{0.5f, 0.5f};
    const DaysGoneVec2 render_scale{clamped_scale, clamped_scale};
    const DaysGoneVec4 color_and_opacity{1.0f, 1.0f, 1.0f, clamped_opacity};

    daysgone_call_widget_vec2_function(widget, L"SetAlignmentInViewport", alignment);
    daysgone_call_widget_vec2_function(widget, L"SetDesiredSizeInViewport", desired_size);
    daysgone_call_widget_position_in_viewport(widget, viewport_position, false);
    daysgone_call_widget_vec2_function(widget, L"SetRenderTransformPivot", pivot);
    daysgone_call_widget_vec2_function(widget, L"SetRenderScale", render_scale);
    daysgone_write_value((uintptr_t)widget + 0x120, color_and_opacity);
    daysgone_call_user_widget_color_and_opacity(widget, color_and_opacity);
    daysgone_call_widget_no_param_function(widget, L"InvalidateLayoutAndVolatility");
    daysgone_call_widget_no_param_function(widget, L"ForceLayoutPrepass");

    SPDLOG_INFO_EVERY_N_SEC(
        2,
        "[DaysGone][ViewportSlotFix] applied {} pos=({:.1f},{:.1f}) size=({:.1f},{:.1f}) render_scale={:.3f} opacity={:.3f}",
        daysgone_describe_uobject(widget),
        viewport_position.x,
        viewport_position.y,
        desired_size.x,
        desired_size.y,
        clamped_scale,
        clamped_opacity);

    return true;
}

void daysgone_restore_user_widget_viewport_slot(sdk::UObjectBase* widget) {
    if (!daysgone_widget_is_viewport_root_candidate(widget)) {
        return;
    }

    daysgone_call_widget_vec2_function(widget, L"SetAlignmentInViewport", {0.0f, 0.0f});
    daysgone_call_widget_vec2_function(widget, L"SetDesiredSizeInViewport", {1920.0f, 1080.0f});
    daysgone_call_widget_position_in_viewport(widget, {0.0f, 0.0f}, false);
    daysgone_call_widget_vec2_function(widget, L"SetRenderTransformPivot", {0.0f, 0.0f});
    daysgone_call_widget_vec2_function(widget, L"SetRenderScale", {1.0f, 1.0f});
    if (!daysgone_restore_viewport_root_original_opacity(widget)) {
        const DaysGoneVec4 color_and_opacity{1.0f, 1.0f, 1.0f, 1.0f};
        daysgone_write_value((uintptr_t)widget + 0x120, color_and_opacity);
        daysgone_call_user_widget_color_and_opacity(widget, color_and_opacity);
    }
    daysgone_call_widget_no_param_function(widget, L"InvalidateLayoutAndVolatility");
    daysgone_call_widget_no_param_function(widget, L"ForceLayoutPrepass");
}

void daysgone_apply_slate_widget_transform(sdk::UObjectBase* widget, DaysGoneVec2 translation, float scale) {
    if (!daysgone_object_pointer_is_readable(widget) || !daysgone_capture_slate_widget_original(widget)) {
        return;
    }

    const auto clamped_scale = std::clamp(scale, 0.05f, 8.0f);
    DaysGoneWidgetTransform transform{};
    transform.translation = translation;
    transform.scale = {clamped_scale, clamped_scale};
    transform.shear = {};
    transform.angle = 0.0f;
    const DaysGoneVec2 pivot{0.5f, 0.5f};

    const auto widget_address = reinterpret_cast<uintptr_t>(widget);
    daysgone_write_value(widget_address + 0xB0, transform);
    daysgone_write_value(widget_address + 0xCC, pivot);

    // Also go through UMG's own invalidation path; direct memory writes alone
    // are not enough for Days Gone's SlateHUD widgets.
    daysgone_call_widget_transform_function(widget, transform);
    daysgone_call_widget_vec2_function(widget, L"SetRenderTranslation", translation);
    daysgone_call_widget_vec2_function(widget, L"SetRenderScale", {clamped_scale, clamped_scale});
    daysgone_call_widget_vec2_function(widget, L"SetRenderTransformPivot", pivot);
    daysgone_call_widget_no_param_function(widget, L"InvalidateLayoutAndVolatility");
    daysgone_call_widget_no_param_function(widget, L"ForceLayoutPrepass");

    ++g_daysgone_slate_widget_apply_count;
}

void daysgone_restore_slate_widget_originals() {
    for (auto& state : g_daysgone_slate_widget_originals) {
        if (!state.captured) {
            continue;
        }

        auto* widget = reinterpret_cast<sdk::UObjectBase*>(state.widget);
        if (daysgone_object_pointer_is_readable(widget)) {
            daysgone_write_value(state.widget + 0xB0, state.transform);
            daysgone_write_value(state.widget + 0xCC, state.pivot);
            daysgone_call_widget_transform_function(widget, state.transform);
            daysgone_call_widget_vec2_function(widget, L"SetRenderTranslation", state.transform.translation);
            daysgone_call_widget_vec2_function(widget, L"SetRenderScale", state.transform.scale);
            daysgone_call_widget_vec2_function(widget, L"SetRenderTransformPivot", state.pivot);
            daysgone_restore_user_widget_viewport_slot(widget);
            daysgone_call_widget_no_param_function(widget, L"InvalidateLayoutAndVolatility");
            daysgone_call_widget_no_param_function(widget, L"ForceLayoutPrepass");
            ++g_daysgone_slate_widget_restore_count;
        }

        state = {};
    }
}

bool daysgone_has_slate_widget_originals() {
    return std::any_of(
        g_daysgone_slate_widget_originals.begin(),
        g_daysgone_slate_widget_originals.end(),
        [](const auto& state) { return state.captured; });
}

bool daysgone_object_pointer_is_readable(sdk::UObjectBase* object) {
    if (object == nullptr || (uintptr_t)object < 0x10000) {
        return false;
    }

    if (!is_readable_process_range((uintptr_t)object, std::max<size_t>(sizeof(void*), sdk::UObjectBase::get_class_size()))) {
        return false;
    }

    sdk::UClass* klass{};
    if (!daysgone_read_value((uintptr_t)object + sdk::UObjectBase::get_class_private_offset(), klass)) {
        return false;
    }

    return klass != nullptr && is_readable_process_range((uintptr_t)klass, sizeof(void*));
}

std::string daysgone_trim_log_string(std::string value, size_t max_len = 96) {
    if (value.size() <= max_len) {
        return value;
    }

    value.resize(max_len);
    value += "...";
    return value;
}

std::string daysgone_describe_uobject(sdk::UObjectBase* object) {
    if (object == nullptr) {
        return "0";
    }

    if (!daysgone_object_pointer_is_readable(object)) {
        return fmt::format("{:x}:unreadable", (uintptr_t)object);
    }

    auto* klass = object->get_class();
    std::string class_name = klass != nullptr && is_readable_process_range((uintptr_t)klass, sizeof(void*))
        ? utility::narrow(klass->get_name_safe())
        : "<bad-class>";
    std::string object_name = utility::narrow(object->get_name_safe());

    if (class_name.empty()) {
        class_name = "<empty-class>";
    }

    if (object_name.empty()) {
        object_name = "<empty-name>";
    }

    return fmt::format(
        "{:x} {}:{}",
        (uintptr_t)object,
        daysgone_trim_log_string(class_name, 48),
        daysgone_trim_log_string(object_name, 72));
}

bool daysgone_is_default_object_name(const std::wstring& name) {
    return name.rfind(L"Default__", 0) == 0 || name.find(L"ClassDefaultObject") != std::wstring::npos;
}

sdk::UObjectBase* daysgone_find_first_tracked_object(const wchar_t* class_full_name) {
    auto* klass = sdk::find_uobject<sdk::UClass>(class_full_name, true);
    if (klass == nullptr || !daysgone_object_pointer_is_readable((sdk::UObjectBase*)klass)) {
        return nullptr;
    }

    auto hook = UObjectHook::get();
    if (hook == nullptr) {
        return nullptr;
    }

    auto objects = hook->get_objects_by_class(klass);
    for (auto* object : objects) {
        if (object == nullptr || !daysgone_object_pointer_is_readable(object)) {
            continue;
        }

        const auto name = object->get_name_safe();
        if (name.empty() || daysgone_is_default_object_name(name)) {
            continue;
        }

        return object;
    }

    return nullptr;
}

sdk::UObjectBase* daysgone_find_menu3d_object() {
    sdk::UObjectBase* menu3d{};
    if (auto* ui_manager = daysgone_find_first_tracked_object(L"Class /Script/BendGame.UIManager");
        daysgone_object_pointer_is_readable(ui_manager))
    {
        daysgone_read_value((uintptr_t)ui_manager + 0x530, menu3d);
    }

    if (daysgone_object_pointer_is_readable(menu3d)) {
        return menu3d;
    }

    return daysgone_find_first_tracked_object(L"Class /Script/BendGame.Menu3D");
}

bool daysgone_read_bend_widget_main(sdk::UObjectBase* menu3d, sdk::UObjectBase*& out_widget) {
    out_widget = nullptr;
    if (!daysgone_object_pointer_is_readable(menu3d)) {
        return false;
    }

    return daysgone_read_value((uintptr_t)menu3d + 0x408, out_widget) &&
        daysgone_object_pointer_is_readable(out_widget);
}

void daysgone_push_unique_widget(std::vector<sdk::UObjectBase*>& widgets, sdk::UObjectBase* widget) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    if (std::find(widgets.begin(), widgets.end(), widget) != widgets.end()) {
        return;
    }

    widgets.push_back(widget);
}

void daysgone_collect_known_daysgone_widget_children(sdk::UObjectBase* widget, std::vector<sdk::UObjectBase*>& widgets);

void daysgone_collect_tracked_widget_class(
    const wchar_t* class_full_name,
    std::vector<sdk::UObjectBase*>& widgets,
    bool collect_known_children)
{
    auto* klass = sdk::find_uobject<sdk::UClass>(class_full_name, true);
    if (klass == nullptr || !daysgone_object_pointer_is_readable((sdk::UObjectBase*)klass)) {
        return;
    }

    auto hook = UObjectHook::get();
    if (hook == nullptr) {
        return;
    }

    const auto objects = hook->get_objects_by_class(klass);
    for (auto* object : objects) {
        if (!daysgone_object_pointer_is_readable(object)) {
            continue;
        }

        const auto name = object->get_name_safe();
        if (name.empty() || daysgone_is_default_object_name(name)) {
            continue;
        }

        daysgone_push_unique_widget(widgets, object);
        if (collect_known_children) {
            daysgone_collect_known_daysgone_widget_children(object, widgets);
        }
    }
}

void daysgone_collect_widgets_from_slate_menu(sdk::UObjectBase* menu, std::vector<sdk::UObjectBase*>& widgets) {
    if (!daysgone_object_pointer_is_readable(menu)) {
        return;
    }

    sdk::UObjectBase* widget{};
    if (daysgone_read_value((uintptr_t)menu + 0x80, widget)) {
        daysgone_push_unique_widget(widgets, widget);
        daysgone_collect_known_daysgone_widget_children(widget, widgets);
    }

    // SlateHUD has a second strongly-typed HudWidget pointer at +0xB0. The
    // visible main/menu/options UI lives here in Days Gone; BP_Menu3D's
    // BendWidgetMain can exist but have widget=0/rt=0 and not affect output.
    sdk::UObjectBase* hud_widget{};
    if (daysgone_read_value((uintptr_t)menu + 0xB0, hud_widget)) {
        daysgone_push_unique_widget(widgets, hud_widget);
        daysgone_collect_known_daysgone_widget_children(hud_widget, widgets);
    }
}

void daysgone_collect_widget_field_at(sdk::UObjectBase* owner, uintptr_t offset, std::vector<sdk::UObjectBase*>& widgets) {
    if (!daysgone_object_pointer_is_readable(owner)) {
        return;
    }

    sdk::UObjectBase* child{};
    if (daysgone_read_value((uintptr_t)owner + offset, child)) {
        daysgone_push_unique_widget(widgets, child);
    }
}

void daysgone_collect_known_daysgone_widget_children(sdk::UObjectBase* widget, std::vector<sdk::UObjectBase*>& widgets) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return;
    }

    const auto desc = daysgone_describe_uobject(widget);

    if (desc.find("UI_HudWidget_C") != std::string::npos) {
        // The root UI_HudWidget did not move in live testing. These are the
        // real UMG subtrees Days Gone paints for HUD/menu/tutorial/subtitle
        // content, so include them in the same opt-in placement transform.
        constexpr std::array<uintptr_t, 16> kHudChildWidgetOffsets{
            0x450, // AccessibilityWrapper
            0x458, // AspectBars
            0x470, // CanvasWrapper
            0x478, // CoreElements
            0x480, // EdgeMarkers
            0x488, // HCM_BGBlack
            0x490, // HCM_BGBlack_0
            0x4A0, // HealthOverlay
            0x4A8, // HitEffects
            0x4B0, // HUDContents
            0x4B8, // HudWrapper
            0x4C0, // Loading
            0x4C8, // MissionPopup
            0x4F8, // SimpleTutorialOverlay
            0x548, // SurvivalWheel
            0x570  // Weapon
        };

        for (const auto offset : kHudChildWidgetOffsets) {
            daysgone_collect_widget_field_at(widget, offset, widgets);
        }
    }

    if (desc.find("UI_MegaMenu_C") != std::string::npos) {
        constexpr std::array<uintptr_t, 8> kMegaMenuChildWidgetOffsets{
            0x250, // 3D_Select_Left
            0x258, // Bars
            0x260, // Inventory_Menu
            0x268, // Map_Menu
            0x270, // OverlayContent
            0x278, // Skills_Menu
            0x280, // Storylines_Menu
            0x288  // UI_ModifiersMenu
        };

        for (const auto offset : kMegaMenuChildWidgetOffsets) {
            daysgone_collect_widget_field_at(widget, offset, widgets);
        }
    }
}

void daysgone_collect_menu3d_user_widgets(sdk::UObjectBase* menu3d, std::vector<sdk::UObjectBase*>& widgets) {
    if (!daysgone_object_pointer_is_readable(menu3d)) {
        return;
    }

    constexpr std::array<uintptr_t, 9> kMenu3DWidgetOffsets{
        0x5D8, // MapWidgetRef
        0x5F8, // Menu_Base_Skills
        0x600, // Menu_Base_Modifiers
        0x608, // Menu_Base_Inventory
        0x610, // Menu_Base_Storylines
        0x618, // Menu_Base_Map
        0x640, // Menu_Base_MegaMenu
        0x648, // Menu_Base_Start
        0x8C8  // Menu_Base_Selector
    };

    for (const auto offset : kMenu3DWidgetOffsets) {
        sdk::UObjectBase* widget{};
        if (daysgone_read_value((uintptr_t)menu3d + offset, widget)) {
            daysgone_push_unique_widget(widgets, widget);
            daysgone_collect_known_daysgone_widget_children(widget, widgets);
        }
    }
}

void daysgone_collect_widgets_from_menu_array(sdk::UObjectBase* owner, uintptr_t offset, std::vector<sdk::UObjectBase*>& widgets) {
    if (!daysgone_object_pointer_is_readable(owner)) {
        return;
    }

    DaysGoneRawTArray array{};
    if (!daysgone_read_value((uintptr_t)owner + offset, array) ||
        array.count < 0 || array.max < 0 || array.count > array.max ||
        array.count > 128 || array.data == 0)
    {
        return;
    }

    const auto bytes = static_cast<size_t>(array.count) * sizeof(uintptr_t);
    if (!is_readable_process_range(array.data, bytes)) {
        return;
    }

    for (int32_t i = 0; i < array.count; ++i) {
        sdk::UObjectBase* menu{};
        if (daysgone_read_value(array.data + (static_cast<uintptr_t>(i) * sizeof(uintptr_t)), menu)) {
            daysgone_collect_widgets_from_slate_menu(menu, widgets);
        }
    }
}

std::vector<sdk::UObjectBase*> daysgone_collect_active_slate_widgets() {
    std::vector<sdk::UObjectBase*> widgets{};
    widgets.reserve(8);

    auto* ui_manager = daysgone_find_first_tracked_object(L"Class /Script/BendGame.UIManager");
    if (daysgone_object_pointer_is_readable(ui_manager)) {
        daysgone_collect_widgets_from_menu_array(ui_manager, 0x470, widgets); // HudMenus
        daysgone_collect_widgets_from_menu_array(ui_manager, 0x480, widgets); // Menus
        daysgone_collect_widgets_from_menu_array(ui_manager, 0x490, widgets); // MenusCreatedThisFrame

        sdk::UObjectBase* menu3d{};
        if (daysgone_read_value((uintptr_t)ui_manager + 0x530, menu3d)) {
            daysgone_collect_widgets_from_slate_menu(menu3d, widgets);
            daysgone_collect_menu3d_user_widgets(menu3d, widgets);
        }

        sdk::UObjectBase* subtitle_widget{};
        if (daysgone_read_value((uintptr_t)ui_manager + 0x518, subtitle_widget)) {
            daysgone_push_unique_widget(widgets, subtitle_widget);
        }
    }

    if (auto* slate_hud = daysgone_find_first_tracked_object(L"Class /Script/BendGame.SlateHUD");
        daysgone_object_pointer_is_readable(slate_hud))
    {
        daysgone_collect_widgets_from_slate_menu(slate_hud, widgets);
    }

    if (auto* bend_hud = daysgone_find_first_tracked_object(L"Class /Script/BendGame.BendHUD");
        daysgone_object_pointer_is_readable(bend_hud))
    {
        sdk::UObjectBase* slate_hud{};
        if (daysgone_read_value((uintptr_t)bend_hud + 0x480, slate_hud)) {
            daysgone_collect_widgets_from_slate_menu(slate_hud, widgets);
        }
    }

    // The visible Days Gone menu/HUD is often a normal viewport UUserWidget
    // owned by BendGameInstance rather than a child of BP_Menu3D/SlateHUD.
    // Collect those roots explicitly so viewport-slot placement can affect the
    // actual MainMenu/HUD/subtitle widgets instead of only their child panels.
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/Menus/MainMenu/UI_MainMenuWidget.UI_MainMenuWidget_C",
        widgets,
        false);
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/Menus/_New_Menus/UI_MegaMenu.UI_MegaMenu_C",
        widgets,
        true);
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/HUD/UI_HudWidget.UI_HudWidget_C",
        widgets,
        true);
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/HUD/UI_SubtitleWidget.UI_SubtitleWidget_C",
        widgets,
        false);
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/Menus/Options/OptionsMenuWidget.OptionsMenuWidget_C",
        widgets,
        false);
    daysgone_collect_tracked_widget_class(
        L"WidgetBlueprintGeneratedClass /Game/Libraries/UI/Blueprints/Menus/Options/OptionsTopMenuWidget.OptionsTopMenuWidget_C",
        widgets,
        false);

    return widgets;
}

void daysgone_restore_viewport_root_slots() {
    const auto widgets = daysgone_collect_active_slate_widgets();
    size_t restored{};
    for (auto* widget : widgets) {
        if (!daysgone_widget_is_viewport_root_candidate(widget)) {
            continue;
        }

        daysgone_restore_user_widget_viewport_slot(widget);
        ++restored;
    }

    if (restored != 0) {
        SPDLOG_INFO("[DaysGone][ViewportSlotFix] restored {} root viewport widget slots", restored);
    }
}

std::string daysgone_describe_uobject_array(sdk::UObjectBase* owner, uintptr_t offset, const char* label, size_t max_entries = 6) {
    if (!daysgone_object_pointer_is_readable(owner)) {
        return fmt::format("{}=<owner-missing>", label);
    }

    DaysGoneRawTArray array{};
    if (!daysgone_read_value((uintptr_t)owner + offset, array)) {
        return fmt::format("{}=<unreadable-array>", label);
    }

    if (array.count < 0 || array.max < 0 || array.count > array.max || array.count > 512) {
        return fmt::format("{}=<bad-array data={:x} count={} max={}>", label, array.data, array.count, array.max);
    }

    std::string result = fmt::format("{}=count:{} max:{} data:{:x}", label, array.count, array.max, array.data);
    if (array.count == 0) {
        return result;
    }

    const auto shown = std::min<size_t>((size_t)array.count, max_entries);
    if (array.data == 0 || !is_readable_process_range(array.data, shown * sizeof(uintptr_t))) {
        return result + " entries:<unreadable>";
    }

    result += " entries:[";
    for (size_t i = 0; i < shown; ++i) {
        uintptr_t raw_object{};
        if (!daysgone_read_value(array.data + (i * sizeof(uintptr_t)), raw_object)) {
            result += fmt::format("{}:<bad-read>", i);
            continue;
        }

        if (i != 0) {
            result += "; ";
        }

        result += fmt::format("{}:{}", i, daysgone_describe_uobject((sdk::UObjectBase*)raw_object));
    }

    if ((size_t)array.count > shown) {
        result += fmt::format("; +{}", (size_t)array.count - shown);
    }

    result += "]";
    return result;
}

std::string daysgone_array_count_string(sdk::UObjectBase* owner, uintptr_t offset) {
    DaysGoneRawTArray array{};
    if (!daysgone_object_pointer_is_readable(owner) || !daysgone_read_value((uintptr_t)owner + offset, array)) {
        return "?";
    }

    if (array.count < 0 || array.max < 0 || array.count > array.max || array.count > 512) {
        return "bad";
    }

    return std::to_string(array.count);
}

std::string daysgone_describe_raw_pointer_field(sdk::UObjectBase* owner, uintptr_t offset, const char* label) {
    if (!daysgone_object_pointer_is_readable(owner)) {
        return fmt::format("{}=<owner-missing>", label);
    }

    sdk::UObjectBase* object{};
    if (!daysgone_read_value((uintptr_t)owner + offset, object)) {
        return fmt::format("{}=<unreadable-field>", label);
    }

    return fmt::format("{}={}", label, daysgone_describe_uobject(object));
}

std::string daysgone_format_vec2(const DaysGoneVec2& value) {
    return fmt::format("{:.3f},{:.3f}", value.x, value.y);
}

std::string daysgone_format_vec3(const DaysGoneVec3& value) {
    return fmt::format("{:.3f},{:.3f},{:.3f}", value.x, value.y, value.z);
}

bool daysgone_float_is_sane(float value, float abs_limit = 100000.0f) {
    return std::isfinite(value) && std::abs(value) <= abs_limit;
}

std::string daysgone_describe_suspect_uobject_pointer(uintptr_t raw) {
    if (raw == 0) {
        return "0";
    }

    auto* object = (sdk::UObjectBase*)raw;
    if (daysgone_object_pointer_is_readable(object)) {
        return daysgone_describe_uobject(object);
    }

    return fmt::format("{:x}:raw", raw);
}

std::string daysgone_describe_texture_render_target_2d(sdk::UObjectBase* render_target, const char* label) {
    if (!daysgone_object_pointer_is_readable(render_target)) {
        return fmt::format("{}={}", label, daysgone_describe_uobject(render_target));
    }

    int32_t size_x{};
    int32_t size_y{};
    uint8_t flags{};
    uint8_t override_format{};

    daysgone_read_value((uintptr_t)render_target + 0xB0, size_x);
    daysgone_read_value((uintptr_t)render_target + 0xB4, size_y);
    daysgone_read_value((uintptr_t)render_target + 0xCC, flags);
    daysgone_read_value((uintptr_t)render_target + 0xD0, override_format);

    return fmt::format(
        "{}={} size={}x{} rt_flags=0x{:02X} override_fmt={}",
        label,
        daysgone_describe_uobject(render_target),
        size_x,
        size_y,
        flags,
        override_format);
}

std::string daysgone_describe_scene_component_transform(sdk::UObjectBase* component, const char* label) {
    if (!daysgone_object_pointer_is_readable(component)) {
        return fmt::format("{}={}", label, daysgone_describe_uobject(component));
    }

    sdk::UObjectBase* attach_parent{};
    DaysGoneRawTArray attach_children{};
    uint8_t scene_flags0{};
    uint8_t scene_flags1{};
    DaysGoneVec3 relative_location{};
    DaysGoneVec3 relative_rotation{};
    DaysGoneVec3 relative_scale{};
    DaysGoneVec3 component_velocity{};

    daysgone_read_value((uintptr_t)component + 0xD0, attach_parent);
    daysgone_read_value((uintptr_t)component + 0xD8, attach_children);
    daysgone_read_value((uintptr_t)component + 0xF0, scene_flags0);
    daysgone_read_value((uintptr_t)component + 0xF1, scene_flags1);
    daysgone_read_value((uintptr_t)component + 0x170, relative_location);
    daysgone_read_value((uintptr_t)component + 0x17C, relative_rotation);
    daysgone_read_value((uintptr_t)component + 0x1B0, relative_scale);
    daysgone_read_value((uintptr_t)component + 0x1E0, component_velocity);

    return fmt::format(
        "{}={} rel_loc=({}) rel_rot=({}) rel_scale=({}) vel=({}) scene_flags=0x{:02X}/0x{:02X} attach_parent={} attach_children={}/{}",
        label,
        daysgone_describe_uobject(component),
        daysgone_format_vec3(relative_location),
        daysgone_format_vec3(relative_rotation),
        daysgone_format_vec3(relative_scale),
        daysgone_format_vec3(component_velocity),
        scene_flags0,
        scene_flags1,
        daysgone_describe_uobject(attach_parent),
        attach_children.count,
        attach_children.max);
}

std::string daysgone_describe_bend_widget_component(sdk::UObjectBase* component, const char* label) {
    if (!daysgone_object_pointer_is_readable(component)) {
        return fmt::format("{}={}", label, daysgone_describe_uobject(component));
    }

    uint8_t flags600{};
    uint8_t pooled_widget_type{};
    uint8_t space{};
    uint8_t timing_policy{};
    uint8_t world_orientation{};
    uint8_t use_image_texture{};
    uint8_t size_screen{};
    uint8_t size_from_widget{};
    uint8_t far_fade{};
    uint8_t stick_to_edge{};
    uint8_t disable_occlusion{};
    uint8_t manually_redraw{};
    uint8_t redraw_requested{};
    uint8_t force_redraw_requested{};
    uint8_t is_opaque{};
    uint8_t is_two_sided{};
    uint8_t tick_when_offscreen{};
    uint8_t blend_mode{};
    uint8_t use_custom_material{};
    uint8_t use_legacy_rotation{};
    uint8_t override_tick{};
    uint8_t tick_enabled{};
    uint8_t added_to_screen{};

    uintptr_t widget_class{};
    sdk::UObjectBase* image_texture{};
    sdk::UObjectBase* owner_player{};
    sdk::UObjectBase* widget{};
    sdk::UObjectBase* translucent_material{};
    sdk::UObjectBase* translucent_material_post_aa{};
    sdk::UObjectBase* custom_material{};
    sdk::UObjectBase* render_target{};
    sdk::UObjectBase* material_instance{};

    float screen_scale{};
    float draw_scale{};
    float far_fade_start{};
    float far_fade_end{};
    float opacity{};
    float redraw_time{};
    float max_interaction_distance{};
    DaysGoneVec2 screen_pixel_offset{};
    DaysGoneVec2 draw_size_f{};
    DaysGoneIntPoint draw_size_i{};
    DaysGoneVec2 pivot{};
    DaysGoneVec3 relative_location{};
    DaysGoneVec3 relative_rotation{};
    DaysGoneVec3 relative_scale{};

    const auto base = (uintptr_t)component;
    daysgone_read_value(base + 0x600, flags600);
    daysgone_read_value(base + 0x601, pooled_widget_type);
    daysgone_read_value(base + 0x602, space);
    daysgone_read_value(base + 0x604, timing_policy);
    daysgone_read_value(base + 0x605, world_orientation);
    daysgone_read_value(base + 0x608, use_image_texture);
    daysgone_read_value(base + 0x618, widget_class);
    daysgone_read_value(base + 0x620, size_screen);
    daysgone_read_value(base + 0x624, screen_scale);
    daysgone_read_value(base + 0x628, screen_pixel_offset);
    daysgone_read_value(base + 0x630, size_from_widget);
    daysgone_read_value(base + 0x634, draw_size_i);
    daysgone_read_value(base + 0x634, draw_size_f);
    daysgone_read_value(base + 0x63C, draw_scale);
    daysgone_read_value(base + 0x640, far_fade);
    daysgone_read_value(base + 0x644, far_fade_start);
    daysgone_read_value(base + 0x648, far_fade_end);
    daysgone_read_value(base + 0x64C, opacity);
    daysgone_read_value(base + 0x650, stick_to_edge);
    daysgone_read_value(base + 0x651, disable_occlusion);
    daysgone_read_value(base + 0x652, manually_redraw);
    daysgone_read_value(base + 0x653, redraw_requested);
    daysgone_read_value(base + 0x654, force_redraw_requested);
    daysgone_read_value(base + 0x658, redraw_time);
    daysgone_read_value(base + 0x668, pivot);
    daysgone_read_value(base + 0x670, max_interaction_distance);
    daysgone_read_value(base + 0x678, owner_player);
    daysgone_read_value(base + 0x690, blend_mode);
    daysgone_read_value(base + 0x691, is_opaque);
    daysgone_read_value(base + 0x692, is_two_sided);
    daysgone_read_value(base + 0x693, tick_when_offscreen);
    daysgone_read_value(base + 0x610, image_texture);
    daysgone_read_value(base + 0x6C0, widget);
    daysgone_read_value(base + 0x6C8, translucent_material);
    daysgone_read_value(base + 0x6D8, translucent_material_post_aa);
    daysgone_read_value(base + 0x718, use_custom_material);
    daysgone_read_value(base + 0x720, custom_material);
    daysgone_read_value(base + 0x728, render_target);
    daysgone_read_value(base + 0x730, material_instance);
    daysgone_read_value(base + 0x738, use_legacy_rotation);
    daysgone_read_value(base + 0x739, override_tick);
    daysgone_read_value(base + 0x73A, tick_enabled);
    daysgone_read_value(base + 0x73C, added_to_screen);
    daysgone_read_value(base + 0x170, relative_location);
    daysgone_read_value(base + 0x17C, relative_rotation);
    daysgone_read_value(base + 0x1B0, relative_scale);

    const auto draw_size_f_is_sane =
        daysgone_float_is_sane(draw_size_f.x, 32768.0f) &&
        daysgone_float_is_sane(draw_size_f.y, 32768.0f) &&
        draw_size_f.x >= 0.0f &&
        draw_size_f.y >= 0.0f;

    return fmt::format(
        "{}={} rel_loc=({}) rel_rot=({}) rel_scale=({}) enums pooled={} space={} timing={} orient={} blend={} "
        "screen_scale={:.3f} screen_offset=({}) draw_size_i={}x{} draw_size_f={} draw_scale={:.3f} pivot=({}) "
        "flags delay={} use_img={} size_screen={} size_widget={} far={} edge={} no_occ={} manual={} redraw={} force={} opaque={} two_sided={} offscreen={} custom_mat={} legacy_rot={} tick_override={} tick_enabled={} added={} "
        "fade={:.3f}/{:.3f} opacity={:.3f} redraw_time={:.3f} max_dist={:.3f} widget_class={} image={} owner={} widget={} trans_mat={} postaa_mat={} custom={} {} mid={}",
        label,
        daysgone_describe_uobject(component),
        daysgone_format_vec3(relative_location),
        daysgone_format_vec3(relative_rotation),
        daysgone_format_vec3(relative_scale),
        pooled_widget_type,
        space,
        timing_policy,
        world_orientation,
        blend_mode,
        screen_scale,
        daysgone_format_vec2(screen_pixel_offset),
        draw_size_i.x,
        draw_size_i.y,
        draw_size_f_is_sane ? fmt::format("({})", daysgone_format_vec2(draw_size_f)) : "<not-float>",
        draw_scale,
        daysgone_format_vec2(pivot),
        (flags600 & 1) != 0,
        (use_image_texture & 1) != 0,
        (size_screen & 1) != 0,
        (size_from_widget & 1) != 0,
        (far_fade & 1) != 0,
        (stick_to_edge & 1) != 0,
        (disable_occlusion & 1) != 0,
        (manually_redraw & 1) != 0,
        (redraw_requested & 1) != 0,
        (force_redraw_requested & 1) != 0,
        (is_opaque & 1) != 0,
        (is_two_sided & 1) != 0,
        (tick_when_offscreen & 1) != 0,
        (use_custom_material & 1) != 0,
        (use_legacy_rotation & 1) != 0,
        (override_tick & 1) != 0,
        (tick_enabled & 1) != 0,
        (added_to_screen & 1) != 0,
        far_fade_start,
        far_fade_end,
        opacity,
        redraw_time,
        max_interaction_distance,
        daysgone_describe_suspect_uobject_pointer(widget_class),
        daysgone_describe_uobject(image_texture),
        daysgone_describe_uobject(owner_player),
        daysgone_describe_uobject(widget),
        daysgone_describe_uobject(translucent_material),
        daysgone_describe_uobject(translucent_material_post_aa),
        daysgone_describe_uobject(custom_material),
        daysgone_describe_texture_render_target_2d(render_target, "rt"),
        daysgone_describe_uobject(material_instance));
}

std::string daysgone_describe_bp_menu3d_state(sdk::UObjectBase* menu3d) {
    if (!daysgone_object_pointer_is_readable(menu3d)) {
        return fmt::format("bpMenu3D={}", daysgone_describe_uobject(menu3d));
    }

    sdk::UObjectBase* bend_widget_main{};
    sdk::UObjectBase* bend_widget_bike_info{};
    sdk::UObjectBase* default_scene_root{};
    sdk::UObjectBase* root1{};
    sdk::UObjectBase* main{};
    sdk::UObjectBase* storylines{};
    sdk::UObjectBase* skills{};
    sdk::UObjectBase* inventory{};
    sdk::UObjectBase* map{};
    sdk::UObjectBase* menus{};
    sdk::UObjectBase* current_tween{};
    sdk::UObjectBase* current_drift{};
    sdk::UObjectBase* gray_background{};
    sdk::UObjectBase* white_background{};
    sdk::UObjectBase* world_map{};
    sdk::UObjectBase* duplicate_camera{};
    sdk::UObjectBase* map_widget{};
    sdk::UObjectBase* skill_menu{};
    sdk::UObjectBase* modifiers_menu{};
    sdk::UObjectBase* inventory_menu{};
    sdk::UObjectBase* storylines_menu{};
    sdk::UObjectBase* map_menu{};
    sdk::UObjectBase* mega_menu{};
    sdk::UObjectBase* start_menu{};
    sdk::UObjectBase* selector_menu{};

    uint8_t touch_flags{};
    uint8_t current_menu{};
    uint8_t next_menu{};
    uint8_t use_player_camera{};
    uint8_t tearing_down{};
    uint8_t tutorial_active{};
    uint8_t has_run_once{};
    uint8_t selected_menu{};
    uint8_t last_menu{};
    uint8_t previous_menu{};
    uint8_t override_menu{};
    uint8_t exit_transition{};
    int32_t tearing_down_frames{};
    float distance_from_camera{};
    float camera_fov{};
    float timeline_fade{};
    float timeline_main{};
    float map_drift_scale{};
    float drag_threshold_sq{};
    DaysGoneRawTArray allowed_menus{};
    DaysGoneVec3 menu_start_location{};
    DaysGoneVec3 touch_start{};
    DaysGoneVec3 touch_end{};
    DaysGoneVec3 default_map_location{};

    const auto base = (uintptr_t)menu3d;
    daysgone_read_value(base + 0x408, bend_widget_main);
    daysgone_read_value(base + 0x500, bend_widget_bike_info);
    daysgone_read_value(base + 0x4F0, default_scene_root);
    daysgone_read_value(base + 0x4F8, root1);
    daysgone_read_value(base + 0x508, main);
    daysgone_read_value(base + 0x510, storylines);
    daysgone_read_value(base + 0x518, skills);
    daysgone_read_value(base + 0x520, inventory);
    daysgone_read_value(base + 0x530, map);
    daysgone_read_value(base + 0x568, menus);
    daysgone_read_value(base + 0x570, current_tween);
    daysgone_read_value(base + 0x578, current_drift);
    daysgone_read_value(base + 0x580, gray_background);
    daysgone_read_value(base + 0x588, white_background);
    daysgone_read_value(base + 0x5A0, touch_flags);
    daysgone_read_value(base + 0x5A1, current_menu);
    daysgone_read_value(base + 0x5A2, next_menu);
    daysgone_read_value(base + 0x5A8, world_map);
    daysgone_read_value(base + 0x5B0, distance_from_camera);
    daysgone_read_value(base + 0x5B4, use_player_camera);
    daysgone_read_value(base + 0x5B8, default_map_location);
    daysgone_read_value(base + 0x5C8, duplicate_camera);
    daysgone_read_value(base + 0x5D8, map_widget);
    daysgone_read_value(base + 0x5E0, camera_fov);
    daysgone_read_value(base + 0x5E4, tearing_down);
    daysgone_read_value(base + 0x5E8, tearing_down_frames);
    daysgone_read_value(base + 0x5F8, skill_menu);
    daysgone_read_value(base + 0x600, modifiers_menu);
    daysgone_read_value(base + 0x608, inventory_menu);
    daysgone_read_value(base + 0x610, storylines_menu);
    daysgone_read_value(base + 0x618, map_menu);
    daysgone_read_value(base + 0x620, map_drift_scale);
    daysgone_read_value(base + 0x640, mega_menu);
    daysgone_read_value(base + 0x648, start_menu);
    daysgone_read_value(base + 0x660, tutorial_active);
    daysgone_read_value(base + 0x668, allowed_menus);
    daysgone_read_value(base + 0x678, has_run_once);
    daysgone_read_value(base + 0x688, last_menu);
    daysgone_read_value(base + 0x68C, drag_threshold_sq);
    daysgone_read_value(base + 0x690, selected_menu);
    daysgone_read_value(base + 0x6B8, previous_menu);
    daysgone_read_value(base + 0x8B0, override_menu);
    daysgone_read_value(base + 0x8C8, selector_menu);
    daysgone_read_value(base + 0x8F0, exit_transition);
    daysgone_read_value(base + 0x538, timeline_fade);
    daysgone_read_value(base + 0x548, timeline_main);
    daysgone_read_value(base + 0x558, menu_start_location);
    daysgone_read_value(base + 0x590, touch_start);
    daysgone_read_value(base + 0x598, touch_end);

    return fmt::format(
        "bpMenu3D={} menus cur={} next={} last={} prev={} selected={} override={} touch_blocked={} use_player_cam={} tearing={} tear_frames={} tutorial={} run_once={} exit={} dist={:.3f} fov={:.3f} fade={:.3f} main_tl={:.3f} map_drift={:.3f} drag_sq={:.3f} allowed={}/{} menu_start=({}) default_map=({}) touch_start=({}) touch_end=({}) "
        "main={} story={} skills={} inv={} map={} menusRoot={} tween={} drift={} gray={} white={} worldMap={} dupCam={} mapWidget={} baseSkills={} baseModifiers={} baseInventory={} baseStory={} baseMap={} mega={} start={} selector={} {} {} {} {}",
        daysgone_describe_uobject(menu3d),
        current_menu,
        next_menu,
        last_menu,
        previous_menu,
        selected_menu,
        override_menu,
        (touch_flags & 1) != 0,
        (use_player_camera & 1) != 0,
        (tearing_down & 1) != 0,
        tearing_down_frames,
        (tutorial_active & 1) != 0,
        (has_run_once & 1) != 0,
        (exit_transition & 1) != 0,
        distance_from_camera,
        camera_fov,
        timeline_fade,
        timeline_main,
        map_drift_scale,
        drag_threshold_sq,
        allowed_menus.count,
        allowed_menus.max,
        daysgone_format_vec3(menu_start_location),
        daysgone_format_vec3(default_map_location),
        daysgone_format_vec3(touch_start),
        daysgone_format_vec3(touch_end),
        daysgone_describe_uobject(main),
        daysgone_describe_uobject(storylines),
        daysgone_describe_uobject(skills),
        daysgone_describe_uobject(inventory),
        daysgone_describe_uobject(map),
        daysgone_describe_uobject(menus),
        daysgone_describe_uobject(current_tween),
        daysgone_describe_uobject(current_drift),
        daysgone_describe_uobject(gray_background),
        daysgone_describe_uobject(white_background),
        daysgone_describe_uobject(world_map),
        daysgone_describe_uobject(duplicate_camera),
        daysgone_describe_uobject(map_widget),
        daysgone_describe_uobject(skill_menu),
        daysgone_describe_uobject(modifiers_menu),
        daysgone_describe_uobject(inventory_menu),
        daysgone_describe_uobject(storylines_menu),
        daysgone_describe_uobject(map_menu),
        daysgone_describe_uobject(mega_menu),
        daysgone_describe_uobject(start_menu),
        daysgone_describe_uobject(selector_menu),
        daysgone_describe_scene_component_transform(default_scene_root, "defaultRoot"),
        daysgone_describe_scene_component_transform(root1, "root1"),
        daysgone_describe_bend_widget_component(bend_widget_main, "bendWidgetMain"),
        daysgone_describe_bend_widget_component(bend_widget_bike_info, "bendWidgetBike"));
}

std::string daysgone_describe_base_menu_widget(sdk::UObjectBase* widget, const char* label) {
    if (!daysgone_object_pointer_is_readable(widget)) {
        return fmt::format("{}={}", label, daysgone_describe_uobject(widget));
    }

    uint8_t flags{};
    int32_t z_order{};
    sdk::UObjectBase* popup{};
    sdk::UObjectBase* owning_menu{};

    daysgone_read_value((uintptr_t)widget + 0x330, flags);
    daysgone_read_value((uintptr_t)widget + 0x338, popup);
    daysgone_read_value((uintptr_t)widget + 0x340, owning_menu);
    daysgone_read_value((uintptr_t)widget + 0x348, z_order);

    return fmt::format(
        "{}={} flags=0x{:02X} z={} popup={} owning={}",
        label,
        daysgone_describe_uobject(widget),
        flags,
        z_order,
        daysgone_describe_uobject(popup),
        daysgone_describe_uobject(owning_menu));
}
}

void FFakeStereoRenderingHook::attempt_hook_daysgone_slate_intermediate_buffer() {
    if (m_attempted_hook_daysgone_slate_intermediate_buffer) {
        return;
    }

    if (!daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    m_attempted_hook_daysgone_slate_intermediate_buffer = true;

    // Days Gone's BendTemporalAA creates a real SlateIntermediateBuffer, then
    // composites it into the scene. Capturing that texture gives UEVR a true UI
    // target instead of treating the scene RT as UI.
    constexpr uintptr_t kBendTemporalAASlateIntermediatePostCallRva = 0x200CD9F;
    constexpr std::array<uint8_t, 8> kExpectedBytes{
        0x33, 0xD2,                         // xor edx, edx
        0xB9, 0x00, 0x01, 0x00, 0x00,       // mov ecx, 100h
        0xE8                                // call ...
    };

    const auto module = (uintptr_t)utility::get_executable();
    if (module == 0) {
        SPDLOG_WARN("[DaysGone][SlateUI] Cannot hook SlateIntermediateBuffer: executable module unavailable");
        return;
    }

    const auto hook_address = module + kBendTemporalAASlateIntermediatePostCallRva;
    if (!is_executable_process_range(hook_address, kExpectedBytes.size()) ||
        std::memcmp((void*)hook_address, kExpectedBytes.data(), kExpectedBytes.size()) != 0)
    {
        SPDLOG_WARN(
            "[DaysGone][SlateUI] Refusing SlateIntermediateBuffer hook at {:x}: byte signature mismatch",
            hook_address);
        return;
    }

    auto hook_result = safetyhook::create_mid((void*)hook_address, &FFakeStereoRenderingHook::daysgone_slate_intermediate_buffer_hook);
    if (!hook_result) {
        SPDLOG_WARN("[DaysGone][SlateUI] Failed to hook SlateIntermediateBuffer post-call at {:x}", hook_address);
        return;
    }

    m_daysgone_slate_intermediate_buffer_hook = std::move(hook_result);
    SPDLOG_INFO("[DaysGone][SlateUI] Hooked Bend SlateIntermediateBuffer post-call at {:x}", hook_address);
}

void FFakeStereoRenderingHook::daysgone_slate_intermediate_buffer_hook(safetyhook::Context& ctx) {
    if (g_hook == nullptr || !daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    auto* const rtm = g_hook->get_render_target_manager();
    if (rtm == nullptr) {
        return;
    }

    constexpr uintptr_t kSlateIntermediateLocalRspOffset = 0x60;
    const auto local_address = ctx.rsp + kSlateIntermediateLocalRspOffset;
    if (!is_readable_process_range(local_address, sizeof(FRHITexture2D*))) {
        return;
    }

    auto* slate_texture = *(FRHITexture2D**)local_address;
    if (slate_texture == nullptr || !is_readable_process_range((uintptr_t)slate_texture, sizeof(void*))) {
        return;
    }

    if (slate_texture == rtm->get_render_target()) {
        SPDLOG_WARN_ONCE("[DaysGone][SlateUI] Ignoring SlateIntermediateBuffer candidate because it matches the scene RT");
        return;
    }

    DaysGoneD3D11TextureCandidate candidate{};
    if (!daysgone_try_get_d3d11_texture_candidate(slate_texture, candidate)) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[DaysGone][SlateUI] SlateIntermediateBuffer candidate {:x} is not a readable nested D3D11 texture yet",
            (uintptr_t)slate_texture);
        return;
    }

    if (!daysgone_is_plausible_slate_ui_desc(candidate.desc)) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[DaysGone][SlateUI] Rejected non-UI SlateIntermediateBuffer candidate: wrapper={:x} native={:x} [{}x{} fmt={} bind=0x{:X}]",
            (uintptr_t)slate_texture,
            (uintptr_t)candidate.native,
            candidate.desc.Width,
            candidate.desc.Height,
            (uint32_t)candidate.desc.Format,
            candidate.desc.BindFlags);
        return;
    }

    const auto last_target = g_hook->m_daysgone_slate_intermediate_last_target.load();
    const auto last_native = g_hook->m_daysgone_slate_native_ui_target.load();
    if (last_target == (uintptr_t)slate_texture && last_native == (uintptr_t)candidate.native) {
        return;
    }

    // Store the captured texture so the opt-in D3D11 overlay path can draw it
    // into UEVR's UI layer without forcing global 2D screen mode.
    (void)rtm;
    g_hook->m_daysgone_slate_intermediate_last_target.store((uintptr_t)slate_texture);
    g_hook->m_daysgone_slate_native_ui_target.store((uintptr_t)candidate.native);
    g_hook->m_daysgone_slate_native_ui_width.store(candidate.desc.Width);
    g_hook->m_daysgone_slate_native_ui_height.store(candidate.desc.Height);

    SPDLOG_INFO_EVERY_N_SEC(
        5,
        "[DaysGone][SlateUI] Captured Bend SlateIntermediateBuffer native UI target: wrapper={:x} native={:x} path=+0x{:X}/+0x{:X} [{}x{} fmt={} bind=0x{:X}]",
        (uintptr_t)slate_texture,
        (uintptr_t)candidate.native,
        candidate.outer_offset == std::numeric_limits<uintptr_t>::max() ? 0 : candidate.outer_offset,
        candidate.inner_offset == std::numeric_limits<uintptr_t>::max() ? 0 : candidate.inner_offset,
        candidate.desc.Width,
        candidate.desc.Height,
        (uint32_t)candidate.desc.Format,
        candidate.desc.BindFlags);
}

void FFakeStereoRenderingHook::attempt_hook_daysgone_bend_taa_composite() {
    if (m_attempted_hook_daysgone_bend_taa_composite) {
        return;
    }

    if (!daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    m_attempted_hook_daysgone_bend_taa_composite = true;

    // Days Gone's visible UI is not a normal Slate overlay. BendTemporalAA
    // composites SlateIntermediateBuffer into the scene in this execute
    // routine; the crop flag at pass+0x76 is what pushes the menu/HUD toward
    // a bad HMD-sized quadrant in VR.
    constexpr uintptr_t kBendTemporalAACompositeExecuteRva = 0x2008550;
    constexpr std::array<uint8_t, 10> kExpectedBytes{
        0x4C, 0x89, 0x4C, 0x24, 0x20, // mov [rsp+20h], r9
        0x48, 0x89, 0x4C, 0x24, 0x08  // mov [rsp+08h], rcx
    };

    const auto module = (uintptr_t)utility::get_executable();
    if (module == 0) {
        SPDLOG_WARN("[DaysGone][BendTAA] Cannot hook BendTemporalAA composite: executable module unavailable");
        return;
    }

    const auto hook_address = module + kBendTemporalAACompositeExecuteRva;
    if (!is_executable_process_range(hook_address, kExpectedBytes.size()) ||
        std::memcmp((void*)hook_address, kExpectedBytes.data(), kExpectedBytes.size()) != 0)
    {
        SPDLOG_WARN(
            "[DaysGone][BendTAA] Refusing composite hook at {:x}: byte signature mismatch",
            hook_address);
        return;
    }

    auto hook_result = safetyhook::create_mid((void*)hook_address, &FFakeStereoRenderingHook::daysgone_bend_taa_composite_hook);
    if (!hook_result) {
        SPDLOG_WARN("[DaysGone][BendTAA] Failed to hook BendTemporalAA composite at {:x}", hook_address);
        return;
    }

    m_daysgone_bend_taa_composite_hook = std::move(hook_result);
    SPDLOG_INFO("[DaysGone][BendTAA] Hooked BendTemporalAA Slate composite at {:x}", hook_address);
}

void FFakeStereoRenderingHook::daysgone_bend_taa_composite_hook(safetyhook::Context& ctx) {
    if (g_hook == nullptr || !daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    const auto pass = (uintptr_t)ctx.rcx;
    if (!is_readable_process_range(pass, 0xD8)) {
        return;
    }

    auto vr = VR::get();
    const bool compatibility_enabled = vr != nullptr && vr->is_daysgone_bend_ui_placement_fix_enabled();
    const bool extracted_overlay_enabled = g_hook->m_daysgone_bend_ui_use_slate_overlay->value();
    const bool suppress_in_scene_composite = g_hook->m_daysgone_bend_ui_suppress_in_scene_composite->value();
    if (!compatibility_enabled ||
        !g_hook->m_daysgone_bend_ui_disable_taa_crop->value() ||
        !extracted_overlay_enabled ||
        !suppress_in_scene_composite)
    {
        return;
    }

    uint8_t crop_flag{};
    if (!daysgone_read_value(pass + 0x76, crop_flag)) {
        return;
    }

    const bool had_crop = (crop_flag & 1) != 0;
    g_hook->m_daysgone_bend_taa_composite_seen.fetch_add(1);

    if (had_crop && daysgone_write_value<uint8_t>(pass + 0x76, 0)) {
        g_hook->m_daysgone_bend_taa_composite_crop_suppressed.fetch_add(1);
    }

    SPDLOG_INFO_EVERY_N_SEC(
        5,
        "[DaysGone][BendTAA] crop suppression active pass={:x} had_crop={} seen={} suppressed={}",
        pass,
        had_crop,
        (unsigned long long)g_hook->m_daysgone_bend_taa_composite_seen.load(),
        (unsigned long long)g_hook->m_daysgone_bend_taa_composite_crop_suppressed.load());
}

void FFakeStereoRenderingHook::update_daysgone_ui_telemetry() {
    if (!daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    auto vr = VR::get();
    if (vr == nullptr || !vr->is_daysgone_bend_ui_placement_fix_enabled()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_daysgone_ui_telemetry_last_queue.time_since_epoch().count() != 0 &&
        now - m_daysgone_ui_telemetry_last_queue < std::chrono::seconds(5))
    {
        return;
    }

    if (m_daysgone_ui_telemetry_queued.exchange(true)) {
        return;
    }

    m_daysgone_ui_telemetry_last_queue = now;

    auto log_telemetry = [this]() {
        utility::ScopeGuard reset{[this]() {
            m_daysgone_ui_telemetry_queued.store(false);
        }};

        if (g_hook != this || !daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
            return;
        }

        log_daysgone_ui_telemetry_game_thread();
    };

    if (GameThreadWorker::get().is_same_thread()) {
        log_telemetry();
        return;
    }

    GameThreadWorker::get().enqueue(std::move(log_telemetry));
}

void FFakeStereoRenderingHook::log_daysgone_ui_telemetry_game_thread() {
    auto* ui_manager = daysgone_find_first_tracked_object(L"Class /Script/BendGame.UIManager");
    auto* bend_hud = daysgone_find_first_tracked_object(L"Class /Script/BendGame.BendHUD");
    auto* slate_hud = daysgone_find_first_tracked_object(L"Class /Script/BendGame.SlateHUD");
    auto* menu3d_class_instance = daysgone_find_first_tracked_object(L"Class /Script/BendGame.Menu3D");

    int32_t frame_count = -1;
    uint8_t options_menu_flags = 0;
    uint8_t menu3d_actor_flags = 0;
    float menu3d_flick_angle = 0.0f;
    sdk::UObjectBase* menu3d{};
    sdk::UObjectBase* map3d{};
    sdk::UObjectBase* subtitle_widget{};
    sdk::UObjectBase* devinfo_widget{};
    sdk::UObjectBase* bend_slate_hud{};
    sdk::UObjectBase* slate_widget{};
    sdk::UObjectBase* slate_hud_widget{};

    if (daysgone_object_pointer_is_readable(ui_manager)) {
        daysgone_read_value((uintptr_t)ui_manager + 0x46C, frame_count);
        daysgone_read_value((uintptr_t)ui_manager + 0x528, options_menu_flags);
        daysgone_read_value((uintptr_t)ui_manager + 0x530, menu3d);
        daysgone_read_value((uintptr_t)ui_manager + 0x538, map3d);
        daysgone_read_value((uintptr_t)ui_manager + 0x518, subtitle_widget);
        daysgone_read_value((uintptr_t)ui_manager + 0x520, devinfo_widget);
    }

    if (daysgone_object_pointer_is_readable(bend_hud)) {
        daysgone_read_value((uintptr_t)bend_hud + 0x480, bend_slate_hud);
    }

    if (!daysgone_object_pointer_is_readable(slate_hud) && daysgone_object_pointer_is_readable(bend_slate_hud)) {
        slate_hud = bend_slate_hud;
    }

    if (daysgone_object_pointer_is_readable(slate_hud)) {
        daysgone_read_value((uintptr_t)slate_hud + 0x80, slate_widget);
        daysgone_read_value((uintptr_t)slate_hud + 0xB0, slate_hud_widget);
    }

    if (!daysgone_object_pointer_is_readable(menu3d) && daysgone_object_pointer_is_readable(menu3d_class_instance)) {
        menu3d = menu3d_class_instance;
    }

    if (daysgone_object_pointer_is_readable(menu3d)) {
        daysgone_read_value((uintptr_t)menu3d + 0x8, menu3d_actor_flags);
        daysgone_read_value((uintptr_t)menu3d + 0x358, menu3d_flick_angle);
    }

    auto* const rtm = get_render_target_manager();
    const auto scene_rt = rtm != nullptr ? (uintptr_t)rtm->get_render_target() : 0;
    const auto fallback_ui = rtm != nullptr ? (uintptr_t)rtm->get_fallback_ui_target_ref() : 0;
    const auto dedicated_ui = rtm != nullptr ? (uintptr_t)rtm->get_dedicated_ui_target() : 0;
    const auto dedicated_ui_w = rtm != nullptr ? rtm->get_dedicated_ui_width() : 0;
    const auto dedicated_ui_h = rtm != nullptr ? rtm->get_dedicated_ui_height() : 0;
    const auto last_slate_intermediate = m_daysgone_slate_intermediate_last_target.load();

    uint8_t current_menu_sig{};
    uint8_t next_menu_sig{};
    uint8_t last_menu_sig{};
    uint8_t selected_menu_sig{};
    sdk::UObjectBase* bend_widget_main_sig{};
    sdk::UObjectBase* bend_widget_bike_sig{};
    sdk::UObjectBase* bend_widget_main_rt_sig{};
    DaysGoneIntPoint bend_widget_main_draw_size_sig{};
    float bend_widget_main_draw_scale_sig{};

    if (daysgone_object_pointer_is_readable(menu3d)) {
        daysgone_read_value((uintptr_t)menu3d + 0x5A1, current_menu_sig);
        daysgone_read_value((uintptr_t)menu3d + 0x5A2, next_menu_sig);
        daysgone_read_value((uintptr_t)menu3d + 0x688, last_menu_sig);
        daysgone_read_value((uintptr_t)menu3d + 0x690, selected_menu_sig);
        daysgone_read_value((uintptr_t)menu3d + 0x408, bend_widget_main_sig);
        daysgone_read_value((uintptr_t)menu3d + 0x500, bend_widget_bike_sig);
    }

    if (daysgone_object_pointer_is_readable(bend_widget_main_sig)) {
        daysgone_read_value((uintptr_t)bend_widget_main_sig + 0x634, bend_widget_main_draw_size_sig);
        daysgone_read_value((uintptr_t)bend_widget_main_sig + 0x63C, bend_widget_main_draw_scale_sig);
        daysgone_read_value((uintptr_t)bend_widget_main_sig + 0x728, bend_widget_main_rt_sig);
    }

    const auto hmd_width = VR::get() != nullptr ? VR::get()->get_hmd_width() : 0;
    const auto hmd_height = VR::get() != nullptr ? VR::get()->get_hmd_height() : 0;
    const auto bp_menu3d_state = daysgone_describe_bp_menu3d_state(menu3d);

    const auto signature = fmt::format(
        "ui={:x}|frame={}|opt=0x{:02X}|hud={:x}|slate={:x}|widget={:x}|hudwidget={:x}|menu3d={:x}|map3d={:x}|menus={}|hudmenus={}|created={}|m={}/{}/{}/{}|bw={:x}/{:x}/{:x}/{}x{}/{:.3f}|rt={:x}/{:x}/{:x}/{}x{}|hmd={}x{}",
        (uintptr_t)ui_manager,
        frame_count,
        options_menu_flags,
        (uintptr_t)bend_hud,
        (uintptr_t)slate_hud,
        (uintptr_t)slate_widget,
        (uintptr_t)slate_hud_widget,
        (uintptr_t)menu3d,
        (uintptr_t)map3d,
        daysgone_array_count_string(ui_manager, 0x480),
        daysgone_array_count_string(ui_manager, 0x470),
        daysgone_array_count_string(ui_manager, 0x490),
        current_menu_sig,
        next_menu_sig,
        last_menu_sig,
        selected_menu_sig,
        (uintptr_t)bend_widget_main_sig,
        (uintptr_t)bend_widget_bike_sig,
        (uintptr_t)bend_widget_main_rt_sig,
        bend_widget_main_draw_size_sig.x,
        bend_widget_main_draw_size_sig.y,
        bend_widget_main_draw_scale_sig,
        scene_rt,
        fallback_ui,
        dedicated_ui,
        dedicated_ui_w,
        dedicated_ui_h,
        hmd_width,
        hmd_height);

    ++m_daysgone_ui_telemetry_log_counter;
    if (signature == m_daysgone_ui_telemetry_last_signature && (m_daysgone_ui_telemetry_log_counter % 6) != 0) {
        return;
    }

    m_daysgone_ui_telemetry_last_signature = signature;

    SPDLOG_INFO(
        "[DaysGone][UITelemetry] ui={} frame={} opt_flags=0x{:02X} hmd={}x{} postprocess_raw=[{:016x},{:016x}] {} {} {} menu3d={} actor_flags=0x{:02X} flick={} map3d={} subtitle={} devinfo={} bend_hud={} bend_slate_hud={} slate_hud={} slate_widget={} slate_hud_widget={} {} {} {} scene_rt={:x} fallback_ui={:x} dedicated_ui={:x} dedicated_ui_size={}x{} slate_intermediate_last={:x}",
        daysgone_describe_uobject(ui_manager),
        frame_count,
        options_menu_flags,
        hmd_width,
        hmd_height,
        daysgone_object_pointer_is_readable(ui_manager) && is_readable_process_range((uintptr_t)ui_manager + 0x4C0, sizeof(uintptr_t))
            ? *(uintptr_t*)((uintptr_t)ui_manager + 0x4C0)
            : 0,
        daysgone_object_pointer_is_readable(ui_manager) && is_readable_process_range((uintptr_t)ui_manager + 0x4C8, sizeof(uintptr_t))
            ? *(uintptr_t*)((uintptr_t)ui_manager + 0x4C8)
            : 0,
        daysgone_describe_uobject_array(ui_manager, 0x470, "hudMenus"),
        daysgone_describe_uobject_array(ui_manager, 0x480, "menus"),
        daysgone_describe_uobject_array(ui_manager, 0x490, "createdThisFrame"),
        daysgone_describe_uobject(menu3d),
        menu3d_actor_flags,
        menu3d_flick_angle,
        daysgone_describe_uobject(map3d),
        daysgone_describe_uobject(subtitle_widget),
        daysgone_describe_uobject(devinfo_widget),
        daysgone_describe_uobject(bend_hud),
        daysgone_describe_uobject(bend_slate_hud),
        daysgone_describe_uobject(slate_hud),
        daysgone_describe_base_menu_widget(slate_widget, "slateWidget"),
        daysgone_describe_base_menu_widget(slate_hud_widget, "slateHudWidget"),
        daysgone_describe_raw_pointer_field(slate_widget, 0x340, "widgetOwningMenu"),
        daysgone_describe_raw_pointer_field(slate_hud_widget, 0x340, "hudWidgetOwningMenu"),
        bp_menu3d_state,
        scene_rt,
        fallback_ui,
        dedicated_ui,
        dedicated_ui_w,
        dedicated_ui_h,
        last_slate_intermediate);
}

void FFakeStereoRenderingHook::update_daysgone_bend_ui_placement_fix() {
    if (!daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
        return;
    }

    auto vr = VR::get();
    const bool enabled = vr != nullptr && vr->is_daysgone_bend_ui_placement_fix_enabled();
    if (!enabled && !m_daysgone_bend_ui_originals.captured && !daysgone_has_slate_widget_originals() &&
        !g_daysgone_slate_composite_cvar.forced)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (enabled) {
        const auto apply_signature = fmt::format(
            "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}|{:.3f}",
            m_daysgone_bend_ui_manual_apply_generation.load(),
            m_daysgone_bend_ui_mode->value(),
            m_daysgone_bend_ui_force_player_camera->value(),
            m_daysgone_bend_ui_override_widget_transform->value(),
            m_daysgone_bend_ui_override_root_transform->value(),
            m_daysgone_bend_ui_force_widget_refresh->value(),
            m_daysgone_bend_ui_viewport_slot_fix->value(),
            m_daysgone_bend_ui_apply_child_render_transform->value(),
            m_daysgone_bend_ui_use_slate_overlay->value(),
            m_daysgone_bend_ui_suppress_in_scene_composite->value(),
            m_daysgone_bend_ui_viewport_slot_offset_x->value(),
            m_daysgone_bend_ui_viewport_slot_offset_y->value(),
            m_daysgone_bend_ui_viewport_slot_scale->value(),
            m_daysgone_bend_ui_viewport_slot_opacity->value(),
            m_daysgone_bend_ui_distance_from_camera->value(),
            m_daysgone_bend_ui_camera_fov->value(),
            m_daysgone_bend_ui_widget_loc_x->value(),
            m_daysgone_bend_ui_widget_loc_y->value(),
            m_daysgone_bend_ui_widget_loc_z->value(),
            m_daysgone_bend_ui_widget_rot_pitch->value(),
            m_daysgone_bend_ui_widget_rot_yaw->value(),
            m_daysgone_bend_ui_widget_rot_roll->value(),
            m_daysgone_bend_ui_widget_scale->value(),
            m_daysgone_bend_ui_screen_offset_x->value(),
            m_daysgone_bend_ui_screen_offset_y->value(),
            m_daysgone_bend_ui_screen_scale->value(),
            m_daysgone_bend_ui_draw_scale->value(),
            m_daysgone_bend_ui_root_loc_x->value(),
            m_daysgone_bend_ui_root_loc_y->value(),
            m_daysgone_bend_ui_root_loc_z->value(),
            m_daysgone_bend_ui_key_opacity->value());
        const bool settings_changed = apply_signature != m_daysgone_bend_ui_last_apply_signature;
        const bool watchdog_due =
            m_daysgone_bend_ui_live_watchdog->value() &&
            m_daysgone_bend_ui_last_apply.time_since_epoch().count() != 0 &&
            now - m_daysgone_bend_ui_last_apply >= std::chrono::seconds(5);

        if (!settings_changed && !watchdog_due) {
            return;
        }

        constexpr auto apply_interval = std::chrono::milliseconds(75);
        if (m_daysgone_bend_ui_last_apply.time_since_epoch().count() != 0 &&
            now - m_daysgone_bend_ui_last_apply < apply_interval)
        {
            return;
        }

        if (m_daysgone_bend_ui_fix_queued.exchange(true)) {
            return;
        }

        m_daysgone_bend_ui_last_apply = now;
        m_daysgone_bend_ui_last_apply_signature = apply_signature;
    } else {
        m_daysgone_bend_ui_last_apply = {};
        m_daysgone_bend_ui_last_apply_signature.clear();
        if (m_daysgone_bend_ui_fix_queued.exchange(true)) {
            return;
        }
    }

    auto apply_or_restore = [this]() {
        utility::ScopeGuard reset{[this]() {
            m_daysgone_bend_ui_fix_queued.store(false);
        }};

        if (g_hook != this || !daysgone_is_current_game() || g_framework == nullptr || !g_framework->is_dx11()) {
            return;
        }

        auto vr = VR::get();
        if (vr != nullptr && vr->is_daysgone_bend_ui_placement_fix_enabled()) {
            apply_daysgone_bend_ui_placement_fix_game_thread();
        } else {
            restore_daysgone_bend_ui_placement_fix_game_thread();
        }
    };

    if (GameThreadWorker::get().is_same_thread()) {
        apply_or_restore();
        return;
    }

    GameThreadWorker::get().enqueue(std::move(apply_or_restore));
}

void FFakeStereoRenderingHook::restore_daysgone_bend_ui_placement_fix_game_thread() {
    daysgone_set_disable_slate_composite(false);
    daysgone_restore_slate_widget_originals();
    daysgone_restore_viewport_root_slots();

    auto& original = m_daysgone_bend_ui_originals;
    if (!original.captured) {
        return;
    }

    auto* menu3d = reinterpret_cast<sdk::UObjectBase*>(original.menu3d);
    auto* widget = reinterpret_cast<sdk::UObjectBase*>(original.widget_main);
    auto* root = reinterpret_cast<sdk::UObjectBase*>(original.default_root);

    if (daysgone_object_pointer_is_readable(menu3d)) {
        daysgone_write_value((uintptr_t)menu3d + 0x5B0, original.distance_from_camera);
        daysgone_write_bool_bit((uintptr_t)menu3d + 0x5B4, (original.use_player_camera & 1) != 0);
        daysgone_write_value((uintptr_t)menu3d + 0x5E0, original.camera_fov);
    }

    if (daysgone_object_pointer_is_readable(widget)) {
        daysgone_write_value((uintptr_t)widget + 0x170, DaysGoneVec3{original.widget_location.x, original.widget_location.y, original.widget_location.z});
        daysgone_write_value((uintptr_t)widget + 0x17C, DaysGoneVec3{original.widget_rotation.x, original.widget_rotation.y, original.widget_rotation.z});
        daysgone_write_value((uintptr_t)widget + 0x1B0, DaysGoneVec3{original.widget_scale.x, original.widget_scale.y, original.widget_scale.z});
        daysgone_write_value((uintptr_t)widget + 0x624, original.screen_scale);
        daysgone_write_value((uintptr_t)widget + 0x628, DaysGoneVec2{original.screen_offset.x, original.screen_offset.y});
        daysgone_write_value((uintptr_t)widget + 0x63C, original.draw_scale);
        daysgone_write_value((uintptr_t)widget + 0x668, DaysGoneVec2{original.pivot.x, original.pivot.y});
        daysgone_write_bool_bit((uintptr_t)widget + 0x651, (original.disable_occlusion & 1) != 0);
        daysgone_write_bool_bit((uintptr_t)widget + 0x693, (original.tick_when_offscreen & 1) != 0);
        daysgone_write_bool_bit((uintptr_t)widget + 0x739, (original.tick_override & 1) != 0);
        daysgone_write_bool_bit((uintptr_t)widget + 0x73A, (original.tick_enabled & 1) != 0);
    }

    if (daysgone_object_pointer_is_readable(root)) {
        daysgone_write_value((uintptr_t)root + 0x170, DaysGoneVec3{original.root_location.x, original.root_location.y, original.root_location.z});
        daysgone_write_value((uintptr_t)root + 0x17C, DaysGoneVec3{original.root_rotation.x, original.root_rotation.y, original.root_rotation.z});
        daysgone_write_value((uintptr_t)root + 0x1B0, DaysGoneVec3{original.root_scale.x, original.root_scale.y, original.root_scale.z});
    }

    SPDLOG_INFO(
        "[DaysGone][BendUIFix] Restored original placement menu={:x} widget={:x} root={:x}",
        original.menu3d,
        original.widget_main,
        original.default_root);

    m_daysgone_bend_ui_restore_count.fetch_add(1);
    m_daysgone_bend_ui_last_menu3d.store(0);
    m_daysgone_bend_ui_last_widget_main.store(0);
    original = {};
}

void FFakeStereoRenderingHook::apply_daysgone_bend_ui_placement_fix_game_thread() {
    const bool overlay_requested = m_daysgone_bend_ui_use_slate_overlay->value();
    const bool use_extracted_overlay = should_use_daysgone_slate_ui_overlay();
    const bool suppress_in_scene =
        use_extracted_overlay &&
        m_daysgone_bend_ui_suppress_in_scene_composite->value();
    if (overlay_requested && suppress_in_scene) {
        if (m_daysgone_bend_ui_originals.captured || daysgone_has_slate_widget_originals()) {
            restore_daysgone_bend_ui_placement_fix_game_thread();
        }

        daysgone_set_disable_slate_composite(true);

        m_daysgone_bend_ui_last_menu3d.store(0);
        m_daysgone_bend_ui_last_widget_main.store(0);

        SPDLOG_INFO_EVERY_N_SEC(
            10,
            "[DaysGone][SlateOverlay] requested target={} suppress_in_scene=true key=({:.3f},{:.3f},{:.3f}) offset=({:.1f},{:.1f}) scale={:.3f}; using extracted overlay only",
            use_extracted_overlay,
            m_daysgone_bend_ui_key_threshold->value(),
            m_daysgone_bend_ui_key_softness->value(),
            m_daysgone_bend_ui_key_opacity->value(),
            m_daysgone_bend_ui_screen_offset_x->value(),
            m_daysgone_bend_ui_screen_offset_y->value(),
            m_daysgone_bend_ui_screen_scale->value() * m_daysgone_bend_ui_draw_scale->value());
        return;
    }

    if (overlay_requested) {
        daysgone_set_disable_slate_composite(false);
        SPDLOG_INFO_EVERY_N_SEC(
            10,
            "[DaysGone][SlateOverlay] requested target={} suppress_in_scene=false key=({:.3f},{:.3f},{:.3f}) offset=({:.1f},{:.1f}) scale={:.3f}; keeping live UMG root tuning active",
            use_extracted_overlay,
            m_daysgone_bend_ui_key_threshold->value(),
            m_daysgone_bend_ui_key_softness->value(),
            m_daysgone_bend_ui_key_opacity->value(),
            m_daysgone_bend_ui_screen_offset_x->value(),
            m_daysgone_bend_ui_screen_offset_y->value(),
            m_daysgone_bend_ui_screen_scale->value() * m_daysgone_bend_ui_draw_scale->value());
    }

    daysgone_set_disable_slate_composite(false);

    const auto slate_widgets = daysgone_collect_active_slate_widgets();
    auto apply_slate_widgets = [this, &slate_widgets]() {
        const DaysGoneVec2 child_translation{
            m_daysgone_bend_ui_screen_offset_x->value(),
            m_daysgone_bend_ui_screen_offset_y->value()};
        const auto child_scale = std::max(0.01f, m_daysgone_bend_ui_screen_scale->value() * m_daysgone_bend_ui_draw_scale->value());
        const DaysGoneVec2 viewport_translation{
            m_daysgone_bend_ui_viewport_slot_offset_x->value(),
            m_daysgone_bend_ui_viewport_slot_offset_y->value()};
        const auto viewport_scale = std::max(0.01f, m_daysgone_bend_ui_viewport_slot_scale->value());
        const auto viewport_opacity = std::clamp(m_daysgone_bend_ui_viewport_slot_opacity->value(), 0.0f, 2.0f);

        size_t viewport_roots_applied{};
        size_t child_transforms_applied{};
        for (auto* widget : slate_widgets) {
            if (m_daysgone_bend_ui_viewport_slot_fix->value() &&
                daysgone_apply_user_widget_viewport_slot(widget, viewport_translation, viewport_scale, viewport_opacity))
            {
                ++viewport_roots_applied;
                continue;
            }

            if (m_daysgone_bend_ui_apply_child_render_transform->value()) {
                daysgone_apply_slate_widget_transform(widget, child_translation, child_scale);
                ++child_transforms_applied;
            }
        }

        if (!slate_widgets.empty()) {
            SPDLOG_INFO_EVERY_N_SEC(
                5,
                "[DaysGone][SlateWidgetFix] candidates={} viewport_roots={} child_transforms={} viewport_offset=({:.1f},{:.1f}) viewport_scale={:.3f} viewport_opacity={:.3f} child_offset=({:.1f},{:.1f}) child_scale={:.3f} total_apply={} total_restore={}",
                slate_widgets.size(),
                viewport_roots_applied,
                child_transforms_applied,
                viewport_translation.x,
                viewport_translation.y,
                viewport_scale,
                viewport_opacity,
                child_translation.x,
                child_translation.y,
                child_scale,
                g_daysgone_slate_widget_apply_count,
                g_daysgone_slate_widget_restore_count);
        }
    };

    auto* menu3d = daysgone_find_menu3d_object();
    sdk::UObjectBase* widget{};
    if (!daysgone_read_bend_widget_main(menu3d, widget)) {
        apply_slate_widgets();
        return;
    }

    sdk::UObjectBase* default_root{};
    daysgone_read_value((uintptr_t)menu3d + 0x4F0, default_root);

    auto& original = m_daysgone_bend_ui_originals;
    if (original.captured &&
        (original.menu3d != (uintptr_t)menu3d || original.widget_main != (uintptr_t)widget))
    {
        restore_daysgone_bend_ui_placement_fix_game_thread();
    }

    if (!original.captured) {
        DaysGoneVec3 v3{};
        DaysGoneVec2 v2{};

        original.menu3d = (uintptr_t)menu3d;
        original.widget_main = (uintptr_t)widget;
        original.default_root = (uintptr_t)default_root;
        daysgone_read_value((uintptr_t)menu3d + 0x5B0, original.distance_from_camera);
        daysgone_read_value((uintptr_t)menu3d + 0x5B4, original.use_player_camera);
        daysgone_read_value((uintptr_t)menu3d + 0x5E0, original.camera_fov);

        if (daysgone_read_value((uintptr_t)widget + 0x170, v3)) { original.widget_location = {v3.x, v3.y, v3.z}; }
        if (daysgone_read_value((uintptr_t)widget + 0x17C, v3)) { original.widget_rotation = {v3.x, v3.y, v3.z}; }
        if (daysgone_read_value((uintptr_t)widget + 0x1B0, v3)) { original.widget_scale = {v3.x, v3.y, v3.z}; }
        daysgone_read_value((uintptr_t)widget + 0x624, original.screen_scale);
        if (daysgone_read_value((uintptr_t)widget + 0x628, v2)) { original.screen_offset = {v2.x, v2.y}; }
        daysgone_read_value((uintptr_t)widget + 0x63C, original.draw_scale);
        if (daysgone_read_value((uintptr_t)widget + 0x668, v2)) { original.pivot = {v2.x, v2.y}; }
        daysgone_read_value((uintptr_t)widget + 0x651, original.disable_occlusion);
        daysgone_read_value((uintptr_t)widget + 0x693, original.tick_when_offscreen);
        daysgone_read_value((uintptr_t)widget + 0x739, original.tick_override);
        daysgone_read_value((uintptr_t)widget + 0x73A, original.tick_enabled);

        if (daysgone_object_pointer_is_readable(default_root)) {
            if (daysgone_read_value((uintptr_t)default_root + 0x170, v3)) { original.root_location = {v3.x, v3.y, v3.z}; }
            if (daysgone_read_value((uintptr_t)default_root + 0x17C, v3)) { original.root_rotation = {v3.x, v3.y, v3.z}; }
            if (daysgone_read_value((uintptr_t)default_root + 0x1B0, v3)) { original.root_scale = {v3.x, v3.y, v3.z}; }
        }

        original.captured = true;

        SPDLOG_INFO(
            "[DaysGone][BendUIFix] Captured original placement menu={:x} widget={:x} root={:x}",
            original.menu3d,
            original.widget_main,
            original.default_root);
    }

    const int mode = std::clamp(m_daysgone_bend_ui_mode->value(), 0, 2);
    const bool apply_player_camera = mode == 0 || mode == 2;
    const bool apply_widget = (mode == 1 || mode == 2) && m_daysgone_bend_ui_override_widget_transform->value();

    if (apply_player_camera) {
        daysgone_write_bool_bit((uintptr_t)menu3d + 0x5B4, m_daysgone_bend_ui_force_player_camera->value());
        daysgone_write_value((uintptr_t)menu3d + 0x5B0, m_daysgone_bend_ui_distance_from_camera->value());
        daysgone_write_value((uintptr_t)menu3d + 0x5E0, m_daysgone_bend_ui_camera_fov->value());
    }

    if (apply_widget) {
        const auto scale = std::max(0.01f, m_daysgone_bend_ui_widget_scale->value());
        daysgone_write_value((uintptr_t)widget + 0x170, DaysGoneVec3{
            m_daysgone_bend_ui_widget_loc_x->value(),
            m_daysgone_bend_ui_widget_loc_y->value(),
            m_daysgone_bend_ui_widget_loc_z->value()});
        daysgone_write_value((uintptr_t)widget + 0x17C, DaysGoneVec3{
            m_daysgone_bend_ui_widget_rot_pitch->value(),
            m_daysgone_bend_ui_widget_rot_yaw->value(),
            m_daysgone_bend_ui_widget_rot_roll->value()});
        daysgone_write_value((uintptr_t)widget + 0x1B0, DaysGoneVec3{scale, scale, scale});
        daysgone_write_value((uintptr_t)widget + 0x624, std::max(0.01f, m_daysgone_bend_ui_screen_scale->value()));
        daysgone_write_value((uintptr_t)widget + 0x628, DaysGoneVec2{
            m_daysgone_bend_ui_screen_offset_x->value(),
            m_daysgone_bend_ui_screen_offset_y->value()});
        daysgone_write_value((uintptr_t)widget + 0x63C, std::max(0.01f, m_daysgone_bend_ui_draw_scale->value()));
        daysgone_write_value((uintptr_t)widget + 0x668, DaysGoneVec2{0.5f, 0.5f});
    }

    if (m_daysgone_bend_ui_override_root_transform->value() && daysgone_object_pointer_is_readable(default_root)) {
        daysgone_write_value((uintptr_t)default_root + 0x170, DaysGoneVec3{
            m_daysgone_bend_ui_root_loc_x->value(),
            m_daysgone_bend_ui_root_loc_y->value(),
            m_daysgone_bend_ui_root_loc_z->value()});
    }

    if (m_daysgone_bend_ui_force_widget_refresh->value()) {
        daysgone_write_bool_bit((uintptr_t)widget + 0x651, true); // bDisableOcclusion
        daysgone_write_bool_bit((uintptr_t)widget + 0x653, true); // bRedrawRequested
        daysgone_write_bool_bit((uintptr_t)widget + 0x654, true); // bForceRedrawRequested
        daysgone_write_bool_bit((uintptr_t)widget + 0x693, true); // TickWhenOffscreen
        daysgone_write_bool_bit((uintptr_t)widget + 0x739, true); // bShouldOverrideComponentTick
    }

    m_daysgone_bend_ui_last_menu3d.store((uintptr_t)menu3d);
    m_daysgone_bend_ui_last_widget_main.store((uintptr_t)widget);
    m_daysgone_bend_ui_apply_count.fetch_add(1);

    SPDLOG_INFO_EVERY_N_SEC(
        5,
        "[DaysGone][BendUIFix] applied mode={} player={} widget={} menu={:x} widget={:x} loc=({:.1f},{:.1f},{:.1f}) rot=({:.1f},{:.1f},{:.1f}) scale={:.3f} screen_offset=({:.1f},{:.1f})",
        mode,
        apply_player_camera,
        apply_widget,
        (uintptr_t)menu3d,
        (uintptr_t)widget,
        m_daysgone_bend_ui_widget_loc_x->value(),
        m_daysgone_bend_ui_widget_loc_y->value(),
        m_daysgone_bend_ui_widget_loc_z->value(),
        m_daysgone_bend_ui_widget_rot_pitch->value(),
        m_daysgone_bend_ui_widget_rot_yaw->value(),
        m_daysgone_bend_ui_widget_rot_roll->value(),
        m_daysgone_bend_ui_widget_scale->value(),
        m_daysgone_bend_ui_screen_offset_x->value(),
        m_daysgone_bend_ui_screen_offset_y->value());

    apply_slate_widgets();
}

void* FFakeStereoRenderingHook::slate_draw_window_render_thread(void* renderer, void* a2, void* a3,
                                                                void* a4, void* params, void* unk1, void* unk2)
{
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("SlateRHIRenderer::DrawWindow_RenderThread called!");
#else
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread called!");
#endif

    g_framework->notify_render_activity();

    if (!g_framework->is_game_data_intialized() || a2 == nullptr) {
        return g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);
    }

    auto viewport_info = (sdk::FViewportInfo*)a3;
    sdk::ISlateViewport* slate_viewport = nullptr; // UE5.5+
    UE55SlateDrawWindowPassInputsHead ue55_inputs{};
    UE55SlateDrawWindowPassInputs ue55_inputs_full{};
    bool a4_is_ue_5_5_variant = try_read_ue55_slate_draw_inputs(a4, renderer, ue55_inputs);
    bool a4_has_ue_5_5_full_inputs = a4_is_ue_5_5_variant && try_read_ue55_slate_draw_inputs_full(a4, renderer, ue55_inputs_full);
    bool ue55_inputs_are_from_windows_array = false;
    void* ue55_draw_window_outputs_ptr = a2;

    if (!a4_is_ue_5_5_variant && try_read_ue55_slate_draw_inputs(a4, a2, ue55_inputs)) {
        // Some UE5.5 builds return FSlateDrawWindowPassOutputs by hidden sret
        // pointer, so RCX is the output struct and RDX is the renderer.
        a4_is_ue_5_5_variant = true;
        a4_has_ue_5_5_full_inputs = try_read_ue55_slate_draw_inputs_full(a4, a2, ue55_inputs_full);
        ue55_draw_window_outputs_ptr = renderer;
        SPDLOG_INFO_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Using UE 5.5 sret FSlateDrawWindowPassInputs layout");
    }

    if (!a4_is_ue_5_5_variant &&
        everwind_is_current_game() &&
        try_read_ue55_slate_draw_windows_first_input(a3, renderer, ue55_inputs, ue55_inputs_full, a4_has_ue_5_5_full_inputs))
    {
        // Everwind/UE5.5.2's SlateOutputTexture string scan lands on
        // DrawWindows_RenderThread, whose R8 is a TConstArrayView of inputs.
        a4_is_ue_5_5_variant = true;
        ue55_inputs_are_from_windows_array = true;
        ue55_draw_window_outputs_ptr = nullptr;
        SPDLOG_INFO_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Using UE 5.5 DrawWindows array-view layout");
    }

    if (a4_is_ue_5_5_variant) {
        SPDLOG_INFO_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Using UE 5.5 FSlateDrawWindowPassInputs layout");
        viewport_info = ue55_inputs.viewport_info;
    }

    if (!a4_is_ue_5_5_variant) {
        // How are we going to fix this on UE5.5?
        g_hook->get_slate_thread_worker()->execute((FRHICommandListImmediate*)a2);
    } else if (!supports_ue55_dedicated_ui_target_for_current_game()) {
        const auto window = (uintptr_t)ue55_inputs.window;

        static std::optional<size_t> viewport_offset = [&]() -> std::optional<size_t> {
            std::optional<size_t> result{};
            const auto module_within = utility::get_module_within(g_hook->m_slate_thread_hook.target_address());

            // Temporarily unhook the DrawWindow_RenderThread hook because we need to emulate the function
            // We could use the trampoline but bdshemu is picky about whether RIP is
            // within the "shellcode" or not (e.g. within the module bounds)
            // and so, the hook must be temporarily unhooked
            if (!module_within) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to get module within for target address!");
                return result;
            }

            SPDLOG_DEBUG("[SlateRHIRenderer::DrawWindow_RenderThread] Module within: {:x}", (uintptr_t)*module_within);

            if (!g_hook->m_slate_thread_hook.disable().has_value()) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to disable slate thread hook!");
                return result;
            }

            utility::ScopeGuard guard{[&]() {
                SPDLOG_DEBUG("[SlateRHIRenderer::DrawWindow_RenderThread] Re-enabling slate thread hook");
                if (!g_hook->m_slate_thread_hook.enable().has_value()) {
                    SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to re-enable slate thread hook!");
                }
            }};

            utility::ShemuContext ctx{*module_within};
            ctx.ctx->Registers.RegRip = (ND_UINT64)g_hook->m_slate_thread_hook.target_address();
            ctx.ctx->Registers.RegRcx = (ND_UINT64)renderer;
            ctx.ctx->Registers.RegRdx = (ND_UINT64)a2;
            ctx.ctx->Registers.RegR8 = (ND_UINT64)a3;
            ctx.ctx->Registers.RegR9 = (ND_UINT64)a4;
            ctx.ctx->MemThreshold = 1000;

            uint32_t window_getter_callstack_level = 0;
            std::span<uint8_t> window_bounds{(uint8_t*)window, (uint8_t*)window + 0x1000};

            utility::emulate(*module_within, ctx.ctx->Registers.RegRip, 1000, ctx, [&](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                SPDLOG_DEBUG("[SlateRHIRenderer::DrawWindow_RenderThread] Emulating instruction: {:x} ({:X})", ctx.ctx->ctx->Registers.RegRip, ctx.ctx->ctx->Registers.RegRip - (uintptr_t)*module_within);

                auto is_within_stack = [&](uintptr_t addr) -> bool {
                    return addr >= ctx.ctx->ctx->StackBase && addr < ctx.ctx->ctx->StackBase + ctx.ctx->ctx->StackSize;
                };

                // Allow writes to go through if we are inside the window getter.
                // The downside is this might unintentionally increase the reference count of the window
                // but it's necessary for the window getter to not give us a nullptr.
                if (ctx.next.writes_to_memory && window_getter_callstack_level == 0) {
                    bool allow_write = false;

                    // However, if it writes to the stack, allow it through.
                    for (size_t i = 0; i < ctx.next.ix.OperandsCount; ++i) {
                        const auto& op = ctx.next.ix.Operands[i];
                        
                        if (op.Type == ND_OP_MEM && op.Access.Write) {
                            const auto base_reg = op.Info.Memory.HasBase ? ((uint64_t*)&ctx.ctx->ctx->Registers.RegRax)[op.Info.Memory.Base] : 0;
                            const auto index_reg = op.Info.Memory.HasIndex ? ((uint64_t*)&ctx.ctx->ctx->Registers.RegRax)[op.Info.Memory.Index] : 0;
                            const auto addr = base_reg + index_reg * op.Info.Memory.Scale + op.Info.Memory.Disp;

                            if (is_within_stack(addr)) {
                                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing write to stack at {:x}!", addr);
                                allow_write = true;
                                break;
                            }
                        }
                    }

                    if (!allow_write) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Instruction writes to memory but we're not inside the window getter, skipping! ({:x})", ctx.ctx->ctx->Registers.RegRip);
                        return utility::ExhaustionResult::STEP_OVER;
                    }
                }

                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    if (window_getter_callstack_level > 0) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call inside window getter function, continuing!");
                        ++window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }

                    const auto rcx_within_bounds = (uint8_t*)ctx.ctx->ctx->Registers.RegRcx >= window_bounds.data() && (uint8_t*)ctx.ctx->ctx->Registers.RegRcx < window_bounds.data() + window_bounds.size();
                    const auto rdx_within_bounds = (uint8_t*)ctx.ctx->ctx->Registers.RegRdx >= window_bounds.data() && (uint8_t*)ctx.ctx->ctx->Registers.RegRdx < window_bounds.data() + window_bounds.size();

                    // Check if RCX != window first. We don't want to skip over the call if it is set to it.
                    // There are inlined and non-inlined versions of this function which is why we need to check this.
                    if (!rcx_within_bounds && !rdx_within_bounds) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping call (not within window bounds)!");
                        return utility::ExhaustionResult::STEP_OVER;
                    }

                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call to 0x{:x}, RCX or RDX matches window {:x}!", ctx.next.ix.Operands[0].Info.Register.Reg, window);
                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RCX: {:x}, RDX: {:x}", ctx.ctx->ctx->Registers.RegRcx, ctx.ctx->ctx->Registers.RegRdx);
                    ++window_getter_callstack_level;
                    return utility::ExhaustionResult::CONTINUE;
                }

                // Check if we hit a ret and are inside the window getter function.
                if (ctx.next.ix.Instruction == ND_INS_RETN) {
                    if (window_getter_callstack_level > 0) { 
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Hit ret inside window getter function, continuing!");
                        --window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }
                }

                // We're looking for a mov reg, [reg+offset] instruction
                // where reg contains the pointer to the window
                // and offset is the offset to the viewport.
                const auto& cctx = ctx.ctx->ctx;
                const auto& ix = ctx.next.ix;

                // Debug stuff
#if 0
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Instruction: {:x} ({})", ctx.ctx->ctx->Registers.RegRip, ix.Mnemonic);

                for (uint32_t i = 0; i < ix.OperandsCount; ++i) {
                    const auto& op = ix.Operands[i];

                    if (op.Type == ND_OP_REG) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is register: {}", i, op.Info.Register.Reg);
                    } else if (op.Type == ND_OP_MEM) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is memory: [base: {}, index: {}, scale: {}, disp: {:x}]", i,
                            op.Info.Memory.HasBase ? op.Info.Memory.Base : 0,
                            op.Info.Memory.HasIndex ? op.Info.Memory.Index : 0,
                            op.Info.Memory.Scale,
                            op.Info.Memory.HasDisp ? op.Info.Memory.Disp : 0);
                    } else {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is of type {}", i, static_cast<uint32_t>(op.Type));
                    }
                }

                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RDI: {:x}", ctx.ctx->ctx->Registers.RegRdi);
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RDX: {:x}", ctx.ctx->ctx->Registers.RegRdx);
#endif

                if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_MEM &&
                    ix.Operands[1].Info.Memory.HasBase && ix.Operands[1].Info.Memory.HasDisp)
                {
                    uintptr_t* reg = (uintptr_t*)&((uint64_t*)&cctx->Registers.RegRax)[ix.Operands[1].Info.Memory.Base];
                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found memory operand with base register {:x} and displacement {:x}!", (uintptr_t)reg, ix.Operands[1].Info.Memory.Disp);

                    // Instead of checking the window, we check if the register is within the bounds of the window's memory.
                    // This should allow us to catch all sorts of compiler optimizations.
                    if ((uint8_t*)*reg >= window_bounds.data() && (uint8_t*)*reg < window_bounds.data() + window_bounds.size()) try {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Base register {:x} is within window bounds, checking offset...", (uintptr_t)reg);

                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found window pointer at {:x}!", (uintptr_t)reg);
                        auto offset = ix.Operands[1].Info.Memory.Disp;
                        const auto value = *(uintptr_t***)((uintptr_t)*reg + offset);

                        if (value == nullptr || IsBadReadPtr((void*)value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid offset at {:x}!", (uintptr_t)value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (*value == nullptr || IsBadReadPtr((void*)*value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid vtable at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (!utility::get_module_within(*value).has_value() || !utility::get_module_within((*value)[0]).has_value()) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid module at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        const auto behind_value = *(uintptr_t***)((uintptr_t)*reg + offset - sizeof(void*));

                        if (behind_value != nullptr && !IsBadReadPtr((void*)behind_value, sizeof(void*)) &&
                            *behind_value != nullptr && !IsBadReadPtr((void*)*behind_value, sizeof(void*)) &&
                            utility::get_module_within(*behind_value).has_value() && utility::get_module_within((*behind_value)[0]).has_value())
                        {
                            SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Adjusting offset by sizeof(void*)!");
                            offset -= sizeof(void*);
                        }

                        result = (*reg + offset) - (uintptr_t)window;

                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found viewport offset at {:x}!", *result);
                        return utility::ExhaustionResult::BREAK;
                    } catch (...) {
                        SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Exception while checking offset!");
                    }
                }

                return utility::ExhaustionResult::CONTINUE;
            });

            if (!result) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to find viewport offset!");
            }

            return result;
        }();

        if (viewport_offset) {
            sdk::ISlateViewport* candidate{};

            if (safe_read_value((uintptr_t)window + *viewport_offset, candidate) && looks_like_vtable_object(candidate)) {
                slate_viewport = candidate;
            } else {
                SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] UE 5.5 Slate viewport candidate was not a valid vtable object");
            }
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_pre_slate_draw_window(renderer, a2, viewport_info);
    }

    g_hook->m_inside_slate_draw_window = true;
    g_hook->m_slate_draw_window_thread_id = GetCurrentThreadId();

    auto call_orig = [&]() {
        auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

        for (auto& mod : mods) {
            mod->on_post_slate_draw_window(renderer, a2, viewport_info);
        }

        g_hook->m_inside_slate_draw_window = false;

        return ret;
    };


    auto vr = VR::get();

    if (!vr->is_hmd_active() || vr->is_stereo_emulation_enabled()) {
        return call_orig();
    }

    sdk::FSlateResource* slate_resource = nullptr;
    sdk::FSlateResource* provider_resource = nullptr;
    FRHITexture2D* provider_texture = nullptr;
    auto rtm = g_hook->get_render_target_manager();

    if (supports_ue55_dedicated_ui_target_for_current_game() && a4_is_ue_5_5_variant) {
        g_hook->note_stable_slate_draw();
        g_hook->attempt_hook_ue55_slate_output_texture_register();

        const auto expected_extent = a4_has_ue_5_5_full_inputs ? ue55_get_slate_expected_extent(ue55_inputs_full) : std::nullopt;

        if (expected_extent) {
            SPDLOG_INFO_EVERY_N_SEC(2,
                "[UE5.5][SlateUI] DrawWindow trusted Slate extent [{}x{}] scene_rect min=[{},{}] max=[{},{}] scale={:.3f}",
                expected_extent->width,
                expected_extent->height,
                ue55_inputs_full.scene_view_rect.min.x,
                ue55_inputs_full.scene_view_rect.min.y,
                ue55_inputs_full.scene_view_rect.max.x,
                ue55_inputs_full.scene_view_rect.max.y,
                ue55_inputs_full.viewport_scale_ui);

            if (everwind_is_current_game() || is_deadzone_ue56_executable()) {
                // Some UE5.5/5.6 titles already expose the correct UI texture
                // through the D3D12 path while Slate's viewport resource path is
                // unavailable. Reuse that target instead of clipping Slate through
                // the scene or creating an unstable UObject RT.
                const auto promoted_fallback =
                    ue55_try_promote_fallback_ui_target(
                        rtm,
                        expected_extent,
                        is_deadzone_ue56_executable() ? "Deadzone D3D12 UI fallback" : "Everwind D3D12 UI fallback");

                if (!promoted_fallback && rtm->get_dedicated_ui_target() == nullptr) {
                    SPDLOG_INFO_EVERY_N_SEC(2,
                        "[UE5.5][SlateUI] {} waiting for D3D12 UI fallback target before rerouting SlateOutputTexture",
                        is_deadzone_ue56_executable() ? "Deadzone" : "Everwind");
                }
            } else {
                rtm->request_dedicated_ui_target(expected_extent->width, expected_extent->height);
                rtm->ensure_dedicated_ui_target((uintptr_t)a2);
            }
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] No trusted Slate extent yet; dedicated UI creation is deferred");
        }

        if (slate_viewport != nullptr) {
            bool use_separate{};
            bool stereo{};
            const auto have_use_separate = try_call_slate_viewport_bool_slot(slate_viewport, 7, use_separate);
            const auto have_stereo = try_call_slate_viewport_bool_slot(slate_viewport, 6, stereo);

            SPDLOG_INFO_EVERY_N_SEC(2,
                "[UE5.5][SlateUI] ISlateViewport={:x} UseSeparate={}{} IsStereoscopic3D={}{}",
                (uintptr_t)slate_viewport,
                have_use_separate ? "" : "unreadable/",
                have_use_separate ? use_separate : false,
                have_stereo ? "" : "unreadable/",
                have_stereo ? stereo : false);

            if (have_use_separate && use_separate) {
                bool slate_viewport_faulted = false;
                auto* direct_resource = try_get_slate_viewport_render_target_texture(slate_viewport, slate_viewport_faulted);
                auto* direct_texture = direct_resource != nullptr ? direct_resource->get_mutable_resource() : nullptr;

                if (slate_viewport_faulted) {
                    SPDLOG_WARN_ONCE("[UE5.5][SlateUI] GetViewportRenderTargetTexture faulted; relying on DrawWindow outputs and dedicated target");
                } else if (ue55_is_valid_ui_texture_candidate(rtm, direct_texture, expected_extent, "ISlateViewport direct texture")) {
                    rtm->set_dedicated_ui_target(direct_texture, expected_extent->width, expected_extent->height);
                    rtm->get_fallback_ui_target_ref() = nullptr;
                    if (should_preserve_promoted_ue55_slate_target()) {
                        rtm->cancel_dedicated_ui_creation_preserving_target("UE5.5 promoted ISlateViewport direct texture");
                    }
                    SPDLOG_WARN_ONCE("[UE5.5][SlateUI] promoted ISlateViewport direct texture as dedicated UI target");
                }
            }
        }

        const auto ret = call_orig();
        if (!ue55_inputs_are_from_windows_array && ue55_draw_window_outputs_ptr != nullptr) {
            ue55_promote_slate_outputs(rtm, ue55_draw_window_outputs_ptr, expected_extent);
        }
        return ret;
    }

    if (slate_viewport != nullptr) {
        bool slate_viewport_faulted = false;
        slate_resource = try_get_slate_viewport_render_target_texture(slate_viewport, slate_viewport_faulted);

        if (slate_viewport_faulted) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] UE 5.5 Slate viewport resource call faulted; deferring to fallback path");
        }
    }

    bool skip_ue56_viewport_provider = false;

    if (viewport_info != nullptr && !a4_is_ue_5_5_variant && !is_ue_5_7_or_newer()) {
        const auto known_texture = rtm->get_render_target() != nullptr ? rtm->get_render_target() : (slate_resource != nullptr ? slate_resource->get_mutable_resource() : nullptr);
        const auto known_texture_is_safe =
            known_texture == nullptr ||
            !is_ue_5_6_dx12_backend() ||
            ue56_dx12_try_get_native_resource(known_texture, "FViewportInfo known texture");

        if (!known_texture_is_safe) {
            skip_ue56_viewport_provider = true;
            SPDLOG_WARNING_EVERY_N_SEC(2,
                "[UE5.6][RT] Skipping FViewportInfo::GetRenderTargetProvider probing because the known texture cannot expose a stable native resource; deferring to D3D12 hooks");
        }

        const auto viewport_rt_provider = known_texture != nullptr && known_texture_is_safe ? viewport_info->get_rt_provider(known_texture) : nullptr;

        if (viewport_rt_provider != nullptr) {
            provider_resource = viewport_rt_provider->get_viewport_render_target_texture();

            if (provider_resource != nullptr) {
                provider_texture = provider_resource->get_mutable_resource();
            }
        } else if (slate_viewport == nullptr && rtm->get_render_target() == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1, "No viewport RT provider, skipping!");
            return call_orig();
        }
    } else if (viewport_info != nullptr && a4_is_ue_5_5_variant) {
        SPDLOG_INFO_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping FViewportInfo RT provider probing on UE 5.5 direct Slate inputs");
    } else if (viewport_info != nullptr && is_ue_5_7_or_newer()) {
        SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping FViewportInfo RT provider probing on UE 5.7+ for stability");
    }

    if (slate_resource == nullptr) {
        slate_resource = provider_resource;
    }

    if (slate_resource == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "No slate resource, skipping!");
        return call_orig();
    }

    const auto engine_texture = slate_resource->get_mutable_resource();
    bool engine_texture_native_ok = true;
    bool provider_texture_native_ok = true;
    const auto vr_state = VR::get();
    const bool ue56_d3d12_targets_ready =
        is_ue_5_6_dx12_backend() &&
        vr_state != nullptr &&
        vr_state->has_d3d12_game_ui_textures() &&
        rtm->get_render_target() != nullptr &&
        rtm->get_ui_target() != nullptr;

    if (engine_texture != nullptr && !IsBadReadPtr(engine_texture, sizeof(void*))) {
        engine_texture_native_ok =
            !is_ue_5_6_dx12_backend() ||
            ue56_dx12_try_get_native_resource(engine_texture, "Slate viewport texture");

        if (engine_texture_native_ok) {
            FRHITexture2D::set_vtable(*(void**)engine_texture);
            g_hook->note_stable_slate_draw();
        } else if (!ue56_d3d12_targets_ready) {
            SPDLOG_WARNING_EVERY_N_SEC(2,
                "[UE5.6][RT] Not adopting Slate viewport texture because native-resource discovery failed; waiting for D3D12 texture/backbuffer hooks");
        }

        if (engine_texture_native_ok && rtm->get_render_target() == nullptr) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Adopting Slate viewport texture as render target fallback");
            rtm->set_render_target(engine_texture);
        }
    }

    if (provider_texture != nullptr && !IsBadReadPtr(provider_texture, sizeof(void*))) {
        provider_texture_native_ok =
            !is_ue_5_6_dx12_backend() ||
            ue56_dx12_try_get_native_resource(provider_texture, "Viewport RT provider texture");
    }

    if (is_ue_5_7_or_newer()) {
        rtm->ensure_dedicated_ui_target((uintptr_t)a2);

        if (!rtm->has_dedicated_ui_target() && !g_hook->m_hooked_ue57_slate_elements_pass) {
            const auto now = std::chrono::steady_clock::now();

            if (g_hook->m_ue57_dedicated_ui_missing_since.time_since_epoch().count() == 0) {
                g_hook->m_ue57_dedicated_ui_missing_since = now;
                g_hook->m_ue57_dedicated_ui_missing_frames = 0;
            }

            ++g_hook->m_ue57_dedicated_ui_missing_frames;

            if (g_hook->m_ue57_dedicated_ui_missing_frames > 180 &&
                now - g_hook->m_ue57_dedicated_ui_missing_since > std::chrono::seconds(3))
            {
                g_hook->attempt_hook_ue57_slate_elements_pass();
            }
        } else if (rtm->has_dedicated_ui_target()) {
            g_hook->m_ue57_dedicated_ui_missing_since = {};
            g_hook->m_ue57_dedicated_ui_missing_frames = 0;
        }
    }

    auto ui_target = rtm->get_ui_target();
    const auto render_target_fallback = rtm->get_render_target();
    const auto daysgone_dx11_no_scene_as_ui = daysgone_is_current_game() && g_framework->is_dx11();

    if (is_ue_5_7_or_newer()) {
        if (rtm->has_dedicated_ui_target()) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Using explicit dedicated UI target on UE 5.7+");
        }

        auto& fallback_ui_target = rtm->get_fallback_ui_target_ref();

        if (fallback_ui_target == render_target_fallback) {
            fallback_ui_target = nullptr;
        }

        ui_target = rtm->get_ui_target();
    } else {
        if (daysgone_dx11_no_scene_as_ui && ui_target == render_target_fallback) {
            SPDLOG_WARN_ONCE("[DaysGone] Clearing scene render target UI fallback before Slate DrawWindow");
            rtm->get_fallback_ui_target_ref() = nullptr;
            ui_target = nullptr;
        }

        if (ui_target == nullptr && provider_texture_native_ok && provider_texture != nullptr && !IsBadReadPtr(provider_texture, sizeof(void*)) && provider_texture != render_target_fallback) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Adopting viewport RT provider texture as dedicated UI target fallback");
            ui_target = provider_texture;
            rtm->get_fallback_ui_target_ref() = provider_texture;
        }

        if (ui_target == nullptr && engine_texture_native_ok && engine_texture != nullptr && !IsBadReadPtr(engine_texture, sizeof(void*)) && engine_texture != render_target_fallback) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Adopting Slate viewport texture as dedicated UI target fallback");
            ui_target = engine_texture;
            rtm->get_fallback_ui_target_ref() = engine_texture;
        }

        if (ui_target == nullptr && render_target_fallback != nullptr && !skip_ue56_viewport_provider && !daysgone_dx11_no_scene_as_ui) {
            SPDLOG_WARN_ONCE("[SlateRHIRenderer::DrawWindow_RenderThread] Falling back to render target because no dedicated UI target was recovered");
            ui_target = render_target_fallback;
            rtm->get_fallback_ui_target_ref() = render_target_fallback;
        } else if (ui_target == nullptr && render_target_fallback != nullptr && daysgone_dx11_no_scene_as_ui) {
            SPDLOG_WARN_ONCE("[DaysGone] Not replacing Slate viewport with the scene RT; using Bend's in-scene Slate composite");
        }
    }

    if (ui_target == nullptr) {
        if (is_ue_5_7_or_newer()) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[SlateRHIRenderer::DrawWindow_RenderThread] No dedicated UI target yet");
            return call_orig();
        }

        SPDLOG_INFO_EVERY_N_SEC(1, "No UI target, skipping!");
        return call_orig();
    }

    // Replace the texture with one we have control over.
    // This isolates the UI to render on our own texture separate from the scene.
    const auto old_texture = slate_resource->get_mutable_resource();
    slate_resource->get_mutable_resource() = ui_target;

    // To be seen if we need to resort to a MidHook on this function if the parameters
    // are wildly different between UE versions.
    const auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

    // Restore the old texture.
    slate_resource->get_mutable_resource() = old_texture;

    for (auto& mod : mods) {
        mod->on_post_slate_draw_window(renderer, a2, viewport_info);
    }
    
    // After this we copy over the texture and clear it in the present hook. doing it here just seems to crash sometimes.
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread finished!");

    return ret;
}

// INTERNAL USE ONLY!!!!
__declspec(noinline) void VRRenderTargetManager::CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::CalculateRenderTargetSize called!");

    m_last_calculate_render_size_return_address = (uintptr_t)_ReturnAddress();

    VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateDepthTexture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateDepthTexture called!");

    m_last_needs_reallocate_depth_texture_return_address = (uintptr_t)_ReturnAddress();

    if (this->depth_analysis_passed) {
        return VRRenderTargetManager_Base::need_reallocate_depth_texture(DepthTarget);
    }

    return false;
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateShadingRateTexture called!");

    const auto return_address = (uintptr_t)_ReturnAddress();
    const auto diff = return_address - m_last_calculate_render_size_return_address;

    if (diff <= 0x50) {
        // We need to switch the FFakeStereoRenderingHook's render target manager
        // to the old one NOW or we will crash. Reason being what was actually called
        // is the GetNumberOfBufferedFrames function, not NeedReAllocateShadingRateTexture.
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return true; // The return value should actually be 1, so just return true.
    }

    return false;
}

void VRRenderTargetManager_Base::update_viewport(bool use_separate_rt, const sdk::FViewport& vp, class SViewport* vp_widget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::update_viewport called! {} {:x} {:x}", use_separate_rt, (uintptr_t)&vp, (uintptr_t)vp_widget);

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    shf_force_scene_viewport_separate_rt(vp, "RenderTargetManager::UpdateViewport");

    //SPDLOG_INFO("Widget: {:x}", (uintptr_t)ViewportWidget);
}

void VRRenderTargetManager_Base::calculate_render_target_size(const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::calculate_render_target_size called!");

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate render target size called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    if (is_ue_5_7_or_newer() && x > 0 && y > 0) {
        // UE 5.7 still reports the pre-VR Slate/window size here before we overwrite it
        // with the stereo render target size. Keep it so the UI path can stay full-width.
        this->request_dedicated_ui_target(x, y);
    }

    x = VR::get()->get_hmd_width() * 2;
    y = VR::get()->get_hmd_height();

    SPDLOG_DEBUG("RenderTargetSize After: {}x{}", x, y);
}

bool VRRenderTargetManager_Base::need_reallocate_view_target(const sdk::FViewport& Viewport) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_view_target called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (!m_attempted_find_force_separate_rt) try {
        m_attempted_find_force_separate_rt = true;

        // Go up the stack until we find something that isn't in our module.
        const auto our_module = g_framework->get_framework_module();
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

        std::optional<uintptr_t> ret_addr{};
        std::optional<HMODULE> module_within{};

        for (auto i = 0; i < depth; ++i) {
            SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);

            module_within = utility::get_module_within(stack[i]);

            if (!module_within) {
                continue;
            }

            if (*module_within != our_module) {
                ret_addr = stack[i];
                break;
            }
        }

        // Emulate from the return address and find a memory write
        // this should contain the offset to the force separate rt bool.
        if (ret_addr) {
            SPDLOG_INFO("Found return address: {:x}", *ret_addr);

            utility::ShemuContext ctx{*module_within};
            ctx.ctx->Registers.RegRip = *ret_addr;
            ctx.ctx->Registers.RegRax = 1; // As if we're returning true from this function.

            utility::emulate(*module_within, *ret_addr, 100, ctx, [this](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                SPDLOG_INFO("Emulating instruction: {:x}", ctx.ctx->ctx->Registers.RegRip);

                if (ctx.next.writes_to_memory) {
                    const auto& ix = ctx.next.ix;
                    if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
                        // We're looking for a mov [reg1+N], reg2
                        const auto& op0 = ix.Operands[0];

                        // Needs a register
                        if (!op0.Info.Memory.HasBase || op0.Info.Memory.IsRipRel) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        // Needs a displacement
                        if (!op0.Info.Memory.HasDisp) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        // We don't want a stack based register
                        if (op0.Info.Memory.Base == NDR_RSP || op0.Info.Memory.Base == NDR_RBP) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        if (op0.Info.Memory.Disp > 0 && op0.Info.Memory.Disp < 0x2000) {
                            m_viewport_force_separate_rt_offset = op0.Info.Memory.Disp;
                            SPDLOG_INFO("Found force separate rt offset: {:x}", *m_viewport_force_separate_rt_offset);
                            return utility::ExhaustionResult::BREAK;
                        }
                    }

                    SPDLOG_INFO("Stepping over...");

                    return utility::ExhaustionResult::STEP_OVER;
                }

                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    // We need to break out of this, we should've found the offset before the call.
                    SPDLOG_ERROR("Failed to find force separate rt offset! Encountered call at {:x}", ctx.ctx->ctx->Registers.RegRip);
                    return utility::ExhaustionResult::BREAK;
                }

                return utility::ExhaustionResult::CONTINUE;
            });
        }
    } catch(...) { // if we dont find it, it's fine, not very many games require it.
        SPDLOG_ERROR("Failed to find force separate rt offset! (Exception)");
    }

    const auto w = VR::get()->get_hmd_width();
    const auto h = VR::get()->get_hmd_height();

    if (w != this->last_width || h != this->last_height || g_hook->should_recreate_textures()) {
        SPDLOG_INFO("Reallocating view target! {} {} -> {} {}", this->last_width, this->last_height, w, h);

        this->last_width = w;
        this->last_height = h;
        this->wants_depth_reallocate = true;
        this->destroy_scene_capture();
        g_hook->set_should_recreate_textures(false);
        return true;
    }

    return false;
}

bool VRRenderTargetManager_Base::need_reallocate_depth_texture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_depth_texture called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (this->wants_depth_reallocate) {
        SPDLOG_INFO("Reallocating depth texture!");

        this->wants_depth_reallocate = false;
        return true;
    }

    return false;
}

void VRRenderTargetManager_Base::pre_texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    if (g_framework->is_dx12() && shf_is_current_game()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] PreTextureHook summary last_desc={:x}", ctx.r8);
    } else if (is_ue_5_1_dx12_backend()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.1][RTChurn] PreTextureHook summary last_desc={:x}", ctx.r8);
    } else {
        SPDLOG_INFO("PreTextureHook called! {}", ctx.r8);
    }

    auto rtm = g_hook->get_render_target_manager();

    if (g_framework->is_dx12() && pitpanic_is_current_game() && is_ue_5_7_or_newer()) {
        // Pit Panic 5.7.2's resolved texture-desc helper copies an internal
        // TArray<EPixelFormat>. Replaying it into UEVR's scratch byte buffer can
        // trip UE's sized-allocation assert. Let the engine allocation run and
        // adopt the resulting texture refs in the post hook instead.
        SPDLOG_WARN_ONCE("[PitPanic] Skipping UE 5.7 DX12 pre-texture duplicate creation; using engine-created RT refs");
        return;
    }

    if (g_framework->is_dx12() && is_ue_5_7_or_newer()) {
        if (rtm->texture_desc_prepare_func == 0 || rtm->texture_create_wrapper_func == 0 || rtm->texture_finalize_func == 0) {
            SPDLOG_WARN_ONCE("Skipping pre-texture duplication on UE 5.7+ DX12 because the real texture wrapper/finalize sequence was not resolved");
            return;
        }

        if (!rtm->allocate_texture_called) {
            SPDLOG_ERROR("AllocateTexture not called yet! (UE 5.7 PreTextureHook)");
            return;
        }

        if (ctx.r8 == 0 || IsBadReadPtr((void*)ctx.r8, sizeof(void*))) {
            SPDLOG_WARN_ONCE("Skipping UE 5.7 pre-texture duplication because the texture desc pointer is invalid");
            return;
        }

        using PrepareDescFn = void(*)(void*, const void*);
        using CreateTextureWrapperFn = void*(*)(void*, void*, const void*);
        using FinalizeTextureFn = void(*)(void*, FTexture2DRHIRef*);

        alignas(16) std::array<uint8_t, 0x80> copied_desc{};
        alignas(16) std::array<uint8_t, 0x90> texture_initializer{};
        static FTexture2DRHIRef duplicated_ui_texture{};

        ((PrepareDescFn)rtm->texture_desc_prepare_func)(copied_desc.data(), (const void*)ctx.r8);

        const auto scan_x = VR::get()->get_hmd_width() * 2;
        const auto scan_y = VR::get()->get_hmd_height();
        const auto requested_width = rtm->get_dedicated_ui_width() != 0 ? rtm->get_dedicated_ui_width() : (uint32_t)g_framework->get_d3d12_rt_size().x;
        const auto requested_height = rtm->get_dedicated_ui_height() != 0 ? rtm->get_dedicated_ui_height() : (uint32_t)g_framework->get_d3d12_rt_size().y;

        bool patched_desc = false;

        for (auto i = 0; i < 0x60; ++i) {
            auto& x = *(int32_t*)(copied_desc.data() + i);
            auto& y = *(int32_t*)(copied_desc.data() + i + 4);

            if (x == (int32_t)scan_x && y == (int32_t)scan_y) {
                SPDLOG_INFO("UE 5.7: Found scene render target extent at desc offset 0x{:x}; duplicating UI extent as [{}x{}]", i, requested_width, requested_height);
                x = (int32_t)requested_width;
                y = (int32_t)requested_height;

                auto* format = copied_desc.data() + i + 15;
                if (*format == 18) {
                    *format = 2;
                }

                patched_desc = true;
                break;
            }
        }

        if (!patched_desc) {
            SPDLOG_WARN_ONCE("Skipping UE 5.7 pre-texture duplication because the texture desc width/height pair could not be found");
            return;
        }

        duplicated_ui_texture.texture = nullptr;

        ((CreateTextureWrapperFn)rtm->texture_create_wrapper_func)((void*)ctx.rcx, texture_initializer.data(), copied_desc.data());
        ((FinalizeTextureFn)rtm->texture_finalize_func)(texture_initializer.data(), &duplicated_ui_texture);

        if (duplicated_ui_texture.texture == nullptr || IsBadReadPtr(duplicated_ui_texture.texture, sizeof(void*))) {
            SPDLOG_WARN_ONCE("UE 5.7 UI duplication ran but did not produce a texture");
            return;
        }

        FRHITexture2D::set_vtable(*(void**)duplicated_ui_texture.texture);
        rtm->set_dedicated_ui_target(duplicated_ui_texture.texture, requested_width, requested_height);
        rtm->get_fallback_ui_target_ref() = nullptr;

        SPDLOG_WARN_ONCE("UE 5.7 created a dedicated UI texture through the real texture wrapper path");
        SPDLOG_INFO("UE 5.7 dedicated UI texture: {:x} [{}x{}]", (uintptr_t)duplicated_ui_texture.texture, requested_width, requested_height);

        VR::get()->reinitialize_renderer();
        return;
    }

    if (is_ue_5_1_dx12_backend() && rtm->allocate_texture_called) {
        const auto size = g_framework->get_d3d12_rt_size();

        if (ue51_can_reuse_current_ui_target(rtm, (uint32_t)size.x, (uint32_t)size.y)) {
            // The engine allocation still continues after this pre-hook. Reusing the
            // existing duplicate UI target avoids rebuilding UEVR's UI texture every
            // frame when UE 5.1 repeatedly hits the same RT allocation path.
            if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
                if (!rtm->is_pre_texture_call_e8 &&
                    ctx.rdx != 0 && !IsBadReadPtr((void*)ctx.rdx, sizeof(FTexture2DRHIRef)))
                {
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
                } else if (rtm->is_pre_texture_call_e8 &&
                    ctx.rcx != 0 && !IsBadReadPtr((void*)ctx.rcx, sizeof(FTexture2DRHIRef)))
                {
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rcx;
                }
            }

            SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.1][RTChurn] Reusing stable UI texture; skipping duplicate UI texture creation");
            return;
        }
    }

    if (g_framework->is_dx12() && shf_is_current_game() && rtm->allocate_texture_called) {
        const auto size = g_framework->get_d3d12_rt_size();

        if (shf_can_reuse_current_ui_target(rtm, (uint32_t)size.x, (uint32_t)size.y)) {
            if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1 && !rtm->is_pre_texture_call_e8 &&
                ctx.rdx != 0 && !IsBadReadPtr((void*)ctx.rdx, sizeof(FTexture2DRHIRef)))
            {
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
                SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] Reusing stable UI texture; tracking current UE5 texture-desc output ref {:x}", ctx.rdx);
            }

            return;
        }
    }

    if (g_framework->is_dx11() && daysgone_is_current_game()) {
        // Days Gone is UE4.11/D3D11 and its Slate/RT path can fatal if UEVR
        // replays the texture-create call with a forced UI format while keeping
        // UAV-capable flags. Let the original engine allocation run and adopt
        // the resulting refs in the post hook instead.
        SPDLOG_WARN_ONCE("[DaysGone] Skipping D3D11 pre-texture duplicate creation; using engine-created RT refs");
        return;
    }

    // maybe do some work later to bruteforce the registers/offsets for these
    // a la emulation or something more rudimentary
    // since it always seems to access a global right before, which
    // refers to the current pixel format, which we can overwrite (which may not be safe)
    // so we could just follow how the global is being written to registers or the stack
    // and then just overwrite the registers/stack with our own values
    if (!rtm->allocate_texture_called) {
        SPDLOG_ERROR("AllocateTexture not called yet! (PreTextureHook)");
        return;
    }

    if (!g_hook->has_pixel_format_cvar()) {
        if (g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            //ctx.r8 = 2; // PF_B8G8R8A8 // decided not to actually set it here, we need to double check when it's actually called
        } else if (!rtm->is_using_texture_desc) {
            *((uint8_t*)ctx.rsp + 0x28) = 2; // PF_B8G8R8A8
        }
    }

    // Now we are going to attempt to JIT a function that will call the original function
    // using the context we have. This will call it twice, but allow us to
    // have control over one of the textures it generates. We need
    // the other generated texture as a UI render target to be used in FFakeStereoRenderingHook::slate_draw_window_render_thread.
    // This will allow the original game UI to be rendered in world space without resorting to WidgetComponent.
    // One can argue that this may be an overengineered alternative to "just" calling FDynamicRHI::CreateTexture2D
    // but that function is very hard to pattern scan for, and we already have it here, so why not use it?
    using namespace asmjit;
    using namespace asmjit::x86;

    SPDLOG_INFO("Attempting to JIT a function to call the original function!");

    auto& insn_bytes = !from_second ? rtm->texture_create_insn_bytes : rtm->texture_create_insn_bytes2;

    const auto ix = utility::decode_one(insn_bytes.data(), insn_bytes.size());

    if (!ix) {
        SPDLOG_ERROR("Failed to decode instruction!");
        return;
    }
    
    // We can't do it to the normal E8 call because the code is not in the same area
    // so RIP relative calls are not possible through the emulator. will just have to
    // resolve those manually through disassembly.
    uintptr_t func_ptr = 0;

    if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
        // Set up the emulator. We will use it to emulate the function call.
        // All we need from it is where the function call lands, so we can call it for real.
        auto emu_ctx = utility::ShemuContext(
            (uintptr_t)insn_bytes.data(),
            insn_bytes.size());

        SPDLOG_INFO("Insn bytes size: {}", insn_bytes.size());
        for (size_t i = 0; i < insn_bytes.size(); ++i) {
            SPDLOG_INFO("Byte[{}]: {:x}", i, insn_bytes[i]);
        }

        emu_ctx.ctx->Registers.RegRcx = ctx.rcx;
        emu_ctx.ctx->Registers.RegRdx = ctx.rdx;
        emu_ctx.ctx->Registers.RegR8 = ctx.r8;
        emu_ctx.ctx->Registers.RegR9 = ctx.r9;
        emu_ctx.ctx->Registers.RegRbx = ctx.rbx;
        emu_ctx.ctx->Registers.RegRax = ctx.rax;
        emu_ctx.ctx->Registers.RegRdi = ctx.rdi;
        emu_ctx.ctx->Registers.RegRsi = ctx.rsi;
        emu_ctx.ctx->Registers.RegR10 = ctx.r10;
        emu_ctx.ctx->Registers.RegR11 = ctx.r11;
        emu_ctx.ctx->Registers.RegR12 = ctx.r12;
        emu_ctx.ctx->Registers.RegR13 = ctx.r13;
        emu_ctx.ctx->Registers.RegR14 = ctx.r14;
        emu_ctx.ctx->Registers.RegR15 = ctx.r15;

        // if disasm is call [rsp+N] we need to set RSP to the actual stack
        // otherwise emulation will fail.
        // conversely, if we set RSP when it's NOT using RSP in the register
        // it will also fail.
        if (ix->Operands[0].Type == ND_OP_MEM && ix->Operands[0].Info.Memory.HasBase &&
            ix->Operands[0].Info.Memory.Base == NDR_RSP)
        {
            emu_ctx.ctx->Registers.RegRsp = ctx.rsp;
            emu_ctx.ctx->Stack = (ND_UINT8*)ctx.rsp;
            emu_ctx.ctx->StackBase = ctx.rsp;
            SPDLOG_INFO("Setting RSP to {:x} for emulation!", ctx.rsp);
        } else {
            SPDLOG_INFO("Not setting RSP for emulation!");
        }

        emu_ctx.ctx->MemThreshold = 1;

        if (emu_ctx.emulate((uintptr_t)insn_bytes.data(), 1) != SHEMU_SUCCESS) {
            SPDLOG_ERROR("Failed to emulate instruction!: {} RIP: {:x}", emu_ctx.status, emu_ctx.ctx->Registers.RegRip);
            return;
        }
    
        SPDLOG_INFO("Emu landed at {:x}", emu_ctx.ctx->Registers.RegRip);
        func_ptr = emu_ctx.ctx->Registers.RegRip;

        if (func_ptr == 0) {
            SPDLOG_ERROR("Function pointer is null after emulation!");
            return;
        }
    } else {
        const auto target = g_hook->get_render_target_manager()->pre_texture_hook.target_address();
        func_ptr = target + 5 + *(int32_t*)&insn_bytes.data()[1];
    }

    SPDLOG_INFO("Function pointer: {:x}", func_ptr);

    /*CodeHolder code{};
    JitRuntime runtime{};
    code.init(runtime.environment());

    Assembler a{&code};
    
    static auto cloned_stack = std::make_unique<std::array<uint8_t, 0x3000>>();
    static auto cloned_registers = std::make_unique<std::array<uint8_t, 0x1000>>();

    auto aligned_stack = ((uintptr_t)&(*cloned_stack)[0x2000]);
    aligned_stack += (-(intptr_t)aligned_stack) & (40 - 1);

    memcpy((void*)aligned_stack, (void*)(ctx.rsp), 0x1000);

    static auto stack_ptr = std::make_unique<uintptr_t>();
    static auto post_register_storage = std::make_unique<uintptr_t>();

    // Store the original stack pointer.
    a.movabs(rax, (void*)stack_ptr.get());
    a.mov(ptr(rax), rsp);

    // Push all of the original registers onto the stack.
    a.movabs(rsp, (void*)&(*cloned_registers)[0x500]);
    //a.mov(rsp, rax);

    a.push(rcx);
    a.push(rdx);
    a.push(r8);
    a.push(r9);
    a.push(r10);
    a.push(r11);
    a.push(r12);
    a.push(r13);
    a.push(r14);
    a.push(r15);
    a.push(rbx);
    a.push(rbp);
    a.push(rsi);
    a.push(rdi);
    a.pushfq();

    a.mov(rax, (void*)post_register_storage.get());
    a.mov(ptr(rax), rsp);

    a.movabs(rsp, aligned_stack);


    a.mov(rdx, rcx); // func param
    a.movabs(rcx, ctx.rcx);
    //a.movabs(rdx, ctx.rdx);
    a.movabs(r8, ctx.r8);
    //a.movabs(r9, ctx.r9);
    const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    a.mov(r9, (uint32_t)size.x);
    // move w into first stack argument
    a.mov(dword_ptr(rsp, 0x20), (uint32_t)size.y);
    a.movabs(r10, ctx.r10);
    a.movabs(r11, ctx.r11);
    a.movabs(r12, ctx.r12);
    a.movabs(r13, ctx.r13);
    a.movabs(r14, ctx.r14);
    a.movabs(r15, ctx.r15);
    a.movabs(rax, ctx.rax);
    a.movabs(rbx, ctx.rbx);
    a.movabs(rbp, ctx.rbp);
    a.movabs(rsi, ctx.rsi);
    a.movabs(rdi, ctx.rdi);

    // Correct the stack pointers inside the stack we cloned
    // to point to areas within the cloned stack if they were
    // pointing to the original stack.
    for (auto stack_var = 0; stack_var < 0x1000; stack_var += sizeof(void*)) {
        auto stack_var_ptr = (uintptr_t*)(aligned_stack + stack_var);

        if (*stack_var_ptr >= ctx.rsp && *stack_var_ptr < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting stack var at 0x{:x}", stack_var);
            *stack_var_ptr = aligned_stack + (*stack_var_ptr - ctx.rsp);
        }
    }

    auto correct_register = [&](auto& reg) {
        if (reg >= ctx.rsp && reg < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting Register");
            reg = aligned_stack + (reg - ctx.rsp);
        }

    };
    for (auto insn_byte : g_hook->get_render_target_manager()->texture_create_insn_bytes) {
        a.db(insn_byte);
    }

    a.mov(rsp, post_register_storage.get());
    a.mov(rsp, ptr(rsp));
    //a.mov(rsp, rcx);

    // Pop all of the original registers off of the stack.
    a.popfq();
    a.pop(rdi);
    a.pop(rsi);
    a.pop(rbp);
    a.pop(rbx);
    a.pop(r15);
    a.pop(r14);
    a.pop(r13);
    a.pop(r12);
    a.pop(r11);
    a.pop(r10);
    a.pop(r9);
    a.pop(r8);
    a.pop(rdx);
    a.pop(rcx);

    //a.pop(rsp); // Restore the original stack pointer.
    a.movabs(rsp, (void*)stack_ptr.get());
    a.mov(rsp, ptr(rsp));

    a.ret();

    uintptr_t code_addr{};
    runtime.add(&code_addr, &code);

    SPDLOG_INFO("JITed address: {:x}", code_addr);

    //MessageBox(0, "debug now", "debug", 0);

    void (*func)(void* rdx) = (decltype(func))code_addr;

    static FTexture2DRHIRef out{};
    out.texture = nullptr;
    func(&out);*/

    auto call_with_context = [&](uintptr_t func, FTexture2DRHIRef& out) {
        CodeHolder code{};
        JitRuntime runtime{};
        code.init(runtime.environment());

        Assembler a{&code};

        auto post_align_label = a.newLabel();

        a.push(rbx);

        a.mov(rcx, ctx.rcx);
        
        if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            a.movabs(rdx, (uintptr_t)&out);
        } else {
            a.mov(rdx, ctx.rdx);
        }

        a.mov(r8, ctx.r8);

        const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
        a.mov(r9, (uint32_t)size.x);

        a.sub(rsp, 0x100);
        a.mov(rbx, 0x100);
        a.test(rsp, sizeof(void*));
        a.jz(post_align_label);

        a.sub(rsp, 8);
        a.mov(rbx, 0x108);
        a.bind(post_align_label);

        a.mov(ptr(rsp, 0x20), (uint32_t)size.y);

        for (auto i = 0x28; i < 0x90; i += sizeof(void*)) {
            a.mov(rax, *(uintptr_t*)(ctx.rsp + i));
            a.mov(ptr(rsp, i), rax);
        }

        a.mov(rax, (void*)func);
        a.call(rax);

        a.add(rsp, rbx);
        a.pop(rbx);

        a.ret();

        uintptr_t code_addr{};
        runtime.add(&code_addr, &code);
        void (*jitted_func)() = (decltype(jitted_func))code_addr;

        jitted_func();
    };

    static FTexture2DRHIRef out{};
    static FTexture2DRHIRef shader_out{};

    const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    const auto stack_args = (uintptr_t*)(ctx.rsp + 0x20);

    SPDLOG_INFO("About to call the original!");
    
    if (!rtm->is_pre_texture_call_e8) {
        SPDLOG_INFO("Calling register version of texture create");

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            if (ctx.r9 == 0 || IsBadReadPtr((void*)ctx.r9, sizeof(void*))) {
                SPDLOG_INFO("Possible UE 5.0.3 detected, not 5.1 or above");
                rtm->is_using_texture_desc = false;
                rtm->is_version_5_0_3 = true;
                rtm->is_version_greq_5_1;
            }
        }

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            SPDLOG_INFO("Calling UE5 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t desc,
                uintptr_t stack_0, // Stack dummies in-case this is the wrong function
                uintptr_t stack_1,
                uintptr_t stack_2,
                uintptr_t stack_3,
                uintptr_t stack_4,
                uintptr_t stack_5,
                uintptr_t stack_6,
                uintptr_t stack_7,
                uintptr_t stack_8) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.r9 + i);
                auto& y = *(int32_t*)(ctx.r9 + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;

                    uint8_t* format = (uint8_t*)(ctx.r9 + width_offset.value() + 15);

                    // some games have 10 bit format
                    if (*format == 18) {
                        *format = 2; // PF_B8G8R8A8
                    }

                    break;
                }
            }

            func(ctx.rcx, &out, ctx.r8, ctx.r9,
                stack_args[0], stack_args[1], 
                stack_args[2], stack_args[3],
                stack_args[4],
                stack_args[5], stack_args[6],
                stack_args[7], stack_args[8]
            );

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.r9 + *width_offset);
                auto& y = *(int32_t*)(ctx.r9 + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        } else if (rtm->is_using_texture_desc) { // extremely rare.
            SPDLOG_INFO("Calling UE4 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                uintptr_t desc,
                TRefCountPtr<IPooledRenderTarget>* out,
                uintptr_t name // wchar_t*
            ) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.rdx + i);
                auto& y = *(int32_t*)(ctx.rdx + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE4: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;
                    break;
                }
            }

            static TRefCountPtr<IPooledRenderTarget> real_out{};

            func(ctx.rcx, ctx.rdx, &real_out, ctx.r9);

            if (real_out.reference != nullptr) {
                const auto& tex = real_out.reference->item.texture;
                const auto& shader = real_out.reference->item.srt;
                out.texture = tex.texture;
                shader_out.texture = shader.texture;
            }

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.r8;
            }
        } else { // most common version.
            SPDLOG_INFO("Calling common version of texture create (several arguments)");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t w,
                uintptr_t h,
                uintptr_t format,
                uintptr_t mips,
                uintptr_t samples,
                uintptr_t flags,
                uintptr_t create_info,
                uintptr_t additional,
                uintptr_t additional2) = (decltype(func))func_ptr;

            func(ctx.rcx, &out, ctx.r8, size.x, size.y, 2, 
                stack_args[2], stack_args[3], stack_args[4], 
                stack_args[5], stack_args[6], stack_args[7]);

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        }

        rtm->ui_target = out.texture;
    } else {
        SPDLOG_INFO("Calling E8 version of texture create");

        if (is_ue57_dx11_backend() && rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            if (is_probable_ue57_dx11_texture_desc_prepare_function(func_ptr)) {
                SPDLOG_WARN_ONCE("Skipping UE 5.7 D3D11 texture-desc prepare helper");
                return;
            }

            SPDLOG_WARN_ONCE("Skipping UE 5.7 D3D11 texture-create replay; RHICmdList texture initializers are not safe to duplicate here");
            return;
        }

        // check if RCX is near the stack pointer
        // if it is then it's a different form of E8 call that takes the texture in the first parameter.
        if (ctx.rcx != 0 && std::abs((int64_t)ctx.rcx - (int64_t)ctx.rsp) <= 0x300) {
            SPDLOG_INFO("Weird form of E8 call detected...");

            // RDX check is to make sure RDX is a pointer and not something like the width which would be a relatively small integer
            if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1 && ctx.rdx >= 65535) {
                SPDLOG_INFO("Calling UE5 texture desc version of texture create");

                void (*func)(
                    FTexture2DRHIRef* out,
                    uintptr_t desc,
                    uintptr_t r8,
                    uintptr_t r9
                ) = (decltype(func))func_ptr;

                // Scan for the render target width and height in the desc
                // and replace it with the desktop resolution (This is for the UI texture)
                const auto scan_x = VR::get()->get_hmd_width() * 2;
                const auto scan_y = VR::get()->get_hmd_height();

                std::optional<int32_t> width_offset{};
                std::optional<int32_t> height_offset{};

                int32_t old_width{};
                int32_t old_height{};

                for (auto i = 0; i < 0x100; ++i) {
                    auto& x = *(int32_t*)(ctx.rdx + i);
                    auto& y = *(int32_t*)(ctx.rdx + i + 4);

                    if (x == scan_x && y == scan_y) {
                        SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                        width_offset = i;
                        height_offset = i + 4;

                        old_width = x;
                        old_height = y;

                        x = size.x;
                        y = size.y;
                        break;
                    }
                }

                func(&out, ctx.rdx, ctx.r8, ctx.r9);

                if (width_offset && height_offset) {
                    auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                    auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                    x = old_width;
                    y = old_height;
                }

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rcx;
                }
            } else {
                // Format
                ctx.r9 = 2; // PF_B8G8R8A8

                void (*func)(
                    FTexture2DRHIRef* out,
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func(&out, (uint32_t)size.x, (uint32_t)size.y, 2,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    stack_args[7], stack_args[8], stack_args[9]);
            }
        } else {
            ctx.r8 = 2; // PF_B8G8R8A8

            std::optional<int> previous_stack_found_index{};
            std::optional<int> previous_stack_repeating_index{};

            std::optional<int> texture_argument_index{};
            std::optional<int> shader_argument_index{};

            for (auto i = 0; i < 10; ++i) {
                const auto stack_ptr = stack_args[i];

                if (std::abs((int64_t)stack_ptr - (int64_t)ctx.rsp) <= 0x300) {
                    if (previous_stack_found_index && *previous_stack_found_index == i - 1) {
                        previous_stack_repeating_index = i;
                    }

                    previous_stack_found_index = i;
                    SPDLOG_INFO("Stack pointer found at arg index {} ({} stack)", i + 4, i);
                } else if (previous_stack_repeating_index && *previous_stack_repeating_index == i - 1) {
                    texture_argument_index = i - 2;
                    shader_argument_index = i - 1;
                    SPDLOG_INFO("Texture argument may be at index {} ({} stack)", *texture_argument_index + 4, *texture_argument_index);
                    SPDLOG_INFO("Shader argument may be at index {} ({} stack)", *shader_argument_index + 4, *shader_argument_index);
                    break;
                }
            }

            if (!texture_argument_index && !shader_argument_index) {
                // operate on a wild guess (hardcoded function signature)
                SPDLOG_INFO("Calling E8 version of texture create with hardcoded function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    FTexture2DRHIRef* out,
                    FTexture2DRHIRef* shader_out,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    &out, &shader_out,
                    stack_args[7], stack_args[8]);
            } else {
                // dynamically generate the function call
                SPDLOG_INFO("Calling E8 version of texture create with dynamically generated function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t stack_0,
                    uintptr_t stack_1,
                    uintptr_t stack_2,
                    uintptr_t stack_3,
                    uintptr_t stack_4,
                    uintptr_t stack_5,
                    uintptr_t stack_6,
                    uintptr_t stack_7,
                    uintptr_t stack_8) = (decltype(func))func_ptr;

                std::array<uintptr_t, 9> cloned_stack{};
                for (auto i = 0; i < 9; ++i) {
                    cloned_stack[i] = stack_args[i];
                }

                cloned_stack[*texture_argument_index] = (uintptr_t)&out;
                cloned_stack[*shader_argument_index] = (uintptr_t)&shader_out;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    cloned_stack[0], cloned_stack[1], 
                    cloned_stack[2], cloned_stack[3],
                    cloned_stack[4],
                    cloned_stack[5], cloned_stack[6],
                    cloned_stack[7], cloned_stack[8]);

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)stack_args[*texture_argument_index];
                }
            }
        }

        rtm->ui_target = out.texture;
    }

    if (out.texture == nullptr) {
        SPDLOG_ERROR("Failed to create UI texture!");
    } else {
        SPDLOG_INFO("Created UI texture at {:x}", (uintptr_t)out.texture);
        ue51_note_ui_created(out.texture, (uint32_t)size.x, (uint32_t)size.y);
    }

    //call_with_context((uintptr_t)func, out);

    SPDLOG_INFO("Called the original function!");

    // Cause stuff like the VR ui texture to get recreated.
    VR::get()->reinitialize_renderer();
}

void VRRenderTargetManager_Base::texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    auto rtm = g_hook->get_render_target_manager();

    if (g_framework->is_dx12() && shf_is_current_game()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] PostTextureHook summary last_ref={:x}", (uintptr_t)rtm->texture_hook_ref);
    } else if (is_ue_5_1_dx12_backend()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.1][RTChurn] PostTextureHook summary last_ref={:x}", (uintptr_t)rtm->texture_hook_ref);
    } else {
        SPDLOG_INFO("Post texture hook called!");
        SPDLOG_INFO(" Ref: {:x}", (uintptr_t)rtm->texture_hook_ref);
    }

    if (!rtm->allocate_texture_called) {
        g_hook->set_should_recreate_textures(true);
        rtm->texture_hook_ref = nullptr;
        rtm->shader_resource_hook_ref = nullptr;

        SPDLOG_INFO("[Post texture hook] Allocate texture was not called, skipping...");
        return;
    }

    rtm->allocate_texture_called = false;

    // very rare...
    if (rtm->is_using_texture_desc && !rtm->is_version_greq_5_1) {
        const auto pooled_rt_container = (TRefCountPtr<IPooledRenderTarget>*)rtm->texture_hook_ref;

        if (pooled_rt_container != nullptr && pooled_rt_container->reference != nullptr) {
            rtm->texture_hook_ref = &pooled_rt_container->reference->item.texture;
        }
    }

    auto is_valid_texture_candidate = [&](FRHITexture2D* candidate, const char* source) -> bool {
        if (candidate == nullptr || IsBadReadPtr(candidate, sizeof(void*))) {
            return false;
        }

        void* vtable{};

        try {
            vtable = *(void**)candidate;
        } catch (...) {
            return false;
        }

        if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
            SPDLOG_INFO_EVERY_N_SEC(1, " Rejected texture candidate from {} because the vtable is invalid", source);
            return false;
        }

        if (!utility::get_module_within(vtable).has_value()) {
            SPDLOG_INFO_EVERY_N_SEC(1, " Rejected texture candidate from {} because its vtable {:x} is not inside a module", source, (uintptr_t)vtable);
            return false;
        }

        FRHITexture2D::set_vtable(vtable);
        return true;
    };

    auto recover_texture_from_ref = [&](uintptr_t ref_ptr, const char* source) -> FRHITexture2D* {
        if (ref_ptr == 0 || IsBadReadPtr((void*)ref_ptr, sizeof(FTexture2DRHIRef))) {
            return nullptr;
        }

        const auto ref = (FTexture2DRHIRef*)ref_ptr;

        if (!is_valid_texture_candidate(ref->texture, source)) {
            return nullptr;
        }

        SPDLOG_INFO(" Recovered texture from {}: {:x}", source, (uintptr_t)ref->texture);
        return ref->texture;
    };

    auto try_promote_dedicated_ui_candidate = [&](FRHITexture2D* candidate, const char* source) -> bool {
        if (!is_ue_5_7_or_newer() || !g_framework->is_dx12()) {
            return false;
        }

        if (!is_valid_texture_candidate(candidate, source) || candidate == rtm->get_render_target()) {
            return false;
        }

        const auto native = (ID3D12Resource*)candidate->get_native_resource();

        if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
            return false;
        }

        const auto desc = native->GetDesc();

        if (desc.Width == 0 || desc.Height == 0) {
            return false;
        }

        const auto requested_width = rtm->get_dedicated_ui_width();
        const auto requested_height = rtm->get_dedicated_ui_height();

        if (requested_width == 0 || requested_height == 0) {
            return false;
        }

        if (desc.Width != requested_width || desc.Height != requested_height) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VRRenderTargetManager] Rejected UE 5.7 UI candidate from {} because size [{}x{}] != requested [{}x{}]",
                source, desc.Width, desc.Height, requested_width, requested_height);
            return false;
        }

        rtm->set_dedicated_ui_target(candidate, desc.Width, desc.Height);
        rtm->get_fallback_ui_target_ref() = nullptr;

        SPDLOG_WARN_ONCE("[VRRenderTargetManager] Promoted an engine-created texture to the dedicated UI target");
        SPDLOG_INFO("[VRRenderTargetManager] dedicated UI target from {} [{:x}] [{}x{}]",
            source, (uintptr_t)candidate, desc.Width, desc.Height);

        return true;
    };

    FRHITexture2D* texture = nullptr;
    const char* texture_source = "unknown";

    if (rtm->texture_hook_ref != nullptr) {
        texture = rtm->texture_hook_ref->texture;
        texture_source = "texture_hook_ref";

        // happens?
        if (texture == nullptr) {
            if (g_framework->is_dx12() && is_ue_5_7_or_newer() && rtm->texture_finalize_func != 0) {
                SPDLOG_INFO_EVERY_N_SEC(1, " Texture is null after UE 5.7 finalize hook; skipping unreliable RAX fallback");
            } else {
                SPDLOG_INFO(" Texture is null, trying to get it from RAX...");

                const auto ref = (FTexture2DRHIRef*)ctx.rax;

                if (!IsBadReadPtr(ref, sizeof(void*)) && is_valid_texture_candidate(ref->texture, "RAX")) {
                    texture = ref->texture;
                    texture_source = "RAX";
                } else {
                    SPDLOG_ERROR(" RAX is bad! Can't get texture!");
                }
            }
        }

        if (is_valid_texture_candidate(texture, "texture_hook_ref")) {
            if (shf_is_current_game() && g_framework->is_dx12()) {
                shf_log_rtm_candidate(rtm, texture, texture_source);
            } else if (is_ue_5_1_dx12_backend()) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.1][RTChurn] Resulting texture summary source={} tex={:x}",
                    texture_source, (uintptr_t)texture);
            } else {
                SPDLOG_INFO(" Resulting texture: {:x}", (uintptr_t)texture);
                SPDLOG_INFO(" Real resource: {:x}", (uintptr_t)texture->get_native_resource());
            }
        } else {
            texture = nullptr;
            SPDLOG_INFO(" Texture is still null!");
        }
    }

    if ((!shf_is_current_game() || !g_framework->is_dx12()) && !is_ue_5_1_dx12_backend()) {
        SPDLOG_INFO(" last texture index: {}", rtm->last_texture_index);
    }

    if (texture != nullptr) {
        rtm->render_target = texture;
    }

    bool dedicated_ui_promoted = false;

    if (rtm->shader_resource_hook_ref != nullptr && rtm->shader_resource_hook_ref->texture != nullptr) {
        dedicated_ui_promoted = try_promote_dedicated_ui_candidate(rtm->shader_resource_hook_ref->texture, "shader_resource_hook_ref");
    }

    const auto daysgone_dx11_no_scene_as_ui = daysgone_is_current_game() && g_framework->is_dx11();

    if (!dedicated_ui_promoted && !is_ue_5_7_or_newer() && rtm->get_fallback_ui_target_ref() == nullptr && rtm->shader_resource_hook_ref != nullptr) {
        const auto shader_texture = rtm->shader_resource_hook_ref->texture;

        if (shader_texture != nullptr && !(daysgone_dx11_no_scene_as_ui && shader_texture == rtm->get_render_target())) {
            SPDLOG_INFO(" Falling back to original shader resource texture as UI target: {:x}", (uintptr_t)shader_texture);
            FRHITexture2D::set_vtable(*(void**)shader_texture);
            rtm->get_fallback_ui_target_ref() = shader_texture;
        } else if (shader_texture != nullptr) {
            SPDLOG_WARN_ONCE("[DaysGone] Refusing to use scene render target shader resource as a VR UI layer");
        }
    }

    if (!dedicated_ui_promoted && rtm->get_fallback_ui_target_ref() == nullptr) {
        for (const auto& candidate : {
                std::pair{(uintptr_t)rtm->shader_resource_hook_ref, "shader_resource_hook_ref"},
                std::pair{ctx.rdx, "RDX"},
                std::pair{ctx.rcx, "RCX"},
                std::pair{ctx.r8, "R8"},
                std::pair{ctx.r9, "R9"},
                std::pair{ctx.rax, "RAX"},
            })
        {
            const auto recovered = recover_texture_from_ref(candidate.first, candidate.second);

            if (recovered != nullptr) {
                if (try_promote_dedicated_ui_candidate(recovered, candidate.second)) {
                    dedicated_ui_promoted = true;
                    break;
                }

                if (!is_ue_5_7_or_newer()) {
                    if (daysgone_dx11_no_scene_as_ui && recovered == rtm->get_render_target()) {
                        SPDLOG_WARN_ONCE("[DaysGone] Refusing recovered scene render target as a VR UI layer");
                        continue;
                    }

                    rtm->get_fallback_ui_target_ref() = recovered;
                }
                break;
            }
        }
    }

    if (!is_ue_5_7_or_newer() && rtm->get_fallback_ui_target_ref() == nullptr && texture != nullptr) {
        if (daysgone_dx11_no_scene_as_ui) {
            // Days Gone composites Slate through BendTemporalAA into the scene RT.
            // Treating that scene RT as a separate VR UI layer duplicates/crops the
            // full scene in the overlay and pushes the menu/HUD to the wrong place.
            SPDLOG_WARN_ONCE("[DaysGone] Skipping render-target-as-UI fallback; leaving Bend Slate composite in the scene");
        } else {
            SPDLOG_WARN(" Falling back to render target texture as UI target: {:x}", (uintptr_t)texture);
            rtm->get_fallback_ui_target_ref() = texture;
        }
    } else if (is_ue_5_7_or_newer() && rtm->get_fallback_ui_target_ref() == nullptr && texture != nullptr) {
        SPDLOG_WARN_ONCE("Skipping render-target-as-UI fallback on UE 5.7+; waiting for a dedicated UI target");
    }

    rtm->texture_hook_ref = nullptr;
    rtm->shader_resource_hook_ref = nullptr;
    ++rtm->last_texture_index;
}

void VRRenderTargetManager_Base::destroy_scene_capture() try {
    if (this->scene_capture_actor != nullptr && this->in_flight_target == nullptr) {
        SPDLOG_INFO("Destroying scene capture!");

        if (this->scene_capture_actor.valid()) {
            this->scene_capture_actor->destroy_actor();
        }
    }

    if (this->in_flight_target == nullptr) {
        this->scene_capture_actor = nullptr;
        this->scene_capture_component = nullptr;
        this->scene_capture_target = nullptr;

        RHIThreadWorker::get().enqueue([this]() -> void {
            this->scene_capture_target_rhi_thread = nullptr;
        });
    }
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in destroy_scene_capture: {}", e.what());
    this->scene_capture_target = nullptr;
    this->scene_capture_actor = nullptr;
    this->scene_capture_component = nullptr;
    
    RHIThreadWorker::get().enqueue([this]() -> void {
        this->scene_capture_target_rhi_thread = nullptr;
    });
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in destroy_scene_capture!");
}

void VRRenderTargetManager_Base::destroy_dedicated_ui_target() {
    if (dedicated_ui_texture != nullptr && dedicated_ui_texture.valid()) {
        auto rooted_texture = dedicated_ui_texture;

        GameThreadWorker::get().enqueue([rooted_texture]() mutable {
            if (rooted_texture != nullptr && rooted_texture.valid()) {
                rooted_texture->remove_from_root();
            }
        });
    }

    if (in_flight_dedicated_ui_texture != nullptr) {
        auto* rooted_texture = in_flight_dedicated_ui_texture;

        GameThreadWorker::get().enqueue([rooted_texture]() {
            if (rooted_texture != nullptr && !IsBadReadPtr(rooted_texture, sizeof(void*))) {
                try {
                    rooted_texture->remove_from_root();
                } catch (...) {
                }
            }
        });
    }

    owned_dedicated_ui_target.reset();
    dedicated_ui_target = nullptr;
    dedicated_ui_texture = nullptr;
    in_flight_dedicated_ui_texture = nullptr;
    reset_dedicated_ui_creation_state();
}

void VRRenderTargetManager_Base::cancel_dedicated_ui_creation_preserving_target(const char* reason) {
    const bool had_pending_creation =
        dedicated_ui_creation_pending ||
        dedicated_ui_object_created ||
        in_flight_dedicated_ui_generation != 0 ||
        in_flight_dedicated_ui_texture != nullptr ||
        dedicated_ui_texture != nullptr;

    if (!had_pending_creation) {
        return;
    }

    auto rooted_texture = dedicated_ui_texture;
    auto* in_flight_texture = in_flight_dedicated_ui_texture;
    auto* rooted_raw = rooted_texture.get();

    if (rooted_texture != nullptr && rooted_texture.valid()) {
        GameThreadWorker::get().enqueue([rooted_texture]() mutable {
            if (rooted_texture != nullptr && rooted_texture.valid()) {
                rooted_texture->remove_from_root();
            }
        });
    }

    if (in_flight_texture != nullptr && in_flight_texture != rooted_raw) {
        GameThreadWorker::get().enqueue([in_flight_texture]() {
            if (in_flight_texture != nullptr && !IsBadReadPtr(in_flight_texture, sizeof(void*))) {
                try {
                    in_flight_texture->remove_from_root();
                } catch (...) {
                }
            }
        });
    }

    dedicated_ui_texture = nullptr;
    in_flight_dedicated_ui_texture = nullptr;
    reset_dedicated_ui_creation_state();

    SPDLOG_INFO(
        "[VRRenderTargetManager] Cancelled in-flight dedicated UI UObject creation after {} promotion; preserving current FRHITexture target",
        reason != nullptr ? reason : "external UI target");
}

void VRRenderTargetManager_Base::invalidate_resolution_dependent_targets() {
    texture_hook_ref = nullptr;
    shader_resource_hook_ref = nullptr;
    allocate_texture_called = false;
    last_texture_index = 0;
    last_width = 0;
    last_height = 0;
    wants_depth_reallocate = true;

    ui_target = nullptr;
    destroy_dedicated_ui_target();
    dedicated_ui_width = 0;
    dedicated_ui_height = 0;
    dedicated_ui_last_attempt = {};
}

void VRRenderTargetManager_Base::reset_dedicated_ui_creation_state() {
    dedicated_ui_creation_pending = false;
    dedicated_ui_object_created = false;
    in_flight_dedicated_ui_generation = 0;
    dedicated_ui_pending_since = {};
    dedicated_ui_resource_pending_since = {};
}

void VRRenderTargetManager_Base::set_dedicated_ui_target(FRHITexture2D* rt, uint32_t width, uint32_t height) {
    if (rt != nullptr) {
        FRHITexture2D::set_vtable(*(void**)rt);
        owned_dedicated_ui_target = std::make_unique<FTexture2DRHIRef>(*rt);
    } else {
        owned_dedicated_ui_target.reset();
    }

    dedicated_ui_target = rt;
    dedicated_ui_width = width;
    dedicated_ui_height = height;
    dedicated_ui_creation_pending = false;
}

void VRRenderTargetManager_Base::request_dedicated_ui_target(uint32_t width, uint32_t height) {
    if (!supports_dedicated_ui_target_for_current_game() || width == 0 || height == 0) {
        return;
    }

    const bool extent_changed = dedicated_ui_width != width || dedicated_ui_height != height;
    dedicated_ui_width = width;
    dedicated_ui_height = height;

    if (extent_changed) {
        if (get_dedicated_ui_target() != nullptr || dedicated_ui_texture != nullptr || in_flight_dedicated_ui_texture != nullptr) {
            SPDLOG_INFO("[VRRenderTargetManager] Dedicated UI extent changed to [{}x{}], recreating", width, height);
            destroy_dedicated_ui_target();
        }

        dedicated_ui_last_attempt = {};
        reset_dedicated_ui_creation_state();
    }

    try_schedule_dedicated_ui_creation();
}

bool VRRenderTargetManager_Base::can_attempt_dedicated_ui_creation() const {
    if (!supports_dedicated_ui_target_for_current_game() || dedicated_ui_width == 0 || dedicated_ui_height == 0) {
        return false;
    }

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    auto* engine = sdk::UGameEngine::get();

    if (engine == nullptr) {
        return false;
    }

    if (g_hook == nullptr || !g_hook->has_slate_hook() || !g_hook->has_seen_stable_slate_draw()) {
        return false;
    }

    if (supports_ue55_dedicated_ui_target_for_current_game()) {
        return true;
    }

    if (!g_hook->prefers_slate_thread_for_session()) {
        return false;
    }

    if (!g_hook->has_seen_prerender_viewfamily()) {
        return false;
    }

    if (!g_hook->has_scene_view_family_offsets_ready()) {
        return false;
    }

    return true;
}

bool VRRenderTargetManager_Base::try_schedule_dedicated_ui_creation() {
    if (!can_attempt_dedicated_ui_creation()) {
        return false;
    }

    if (get_dedicated_ui_target() != nullptr || dedicated_ui_texture != nullptr || in_flight_dedicated_ui_texture != nullptr ||
        dedicated_ui_creation_pending || in_flight_dedicated_ui_generation != 0)
    {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (dedicated_ui_last_attempt.time_since_epoch().count() != 0 &&
        now - dedicated_ui_last_attempt < std::chrono::seconds(2))
    {
        return false;
    }

    return create_dedicated_ui_texture();
}

bool VRRenderTargetManager_Base::create_dedicated_ui_texture() {
    if (!supports_dedicated_ui_target_for_current_game()) {
        return false;
    }

    if (dedicated_ui_width == 0 || dedicated_ui_height == 0) {
        return false;
    }

    const auto width = dedicated_ui_width;
    const auto height = dedicated_ui_height;
    const auto generation = ++dedicated_ui_generation;
    dedicated_ui_last_attempt = std::chrono::steady_clock::now();
    dedicated_ui_creation_pending = true;
    dedicated_ui_object_created = false;
    in_flight_dedicated_ui_generation = generation;
    dedicated_ui_pending_since = {};
    dedicated_ui_resource_pending_since = {};

    SPDLOG_INFO("[VRRenderTargetManager] Scheduling dedicated UI target creation for generation {} [{}x{}]", generation, width, height);

    GameThreadWorker::get().enqueue([this, width, height, generation]() -> void {
        try {
            if (!this->is_dedicated_ui_generation_current(generation)) {
                return;
            }

            if (this->in_flight_dedicated_ui_texture != nullptr || this->get_dedicated_ui_target() != nullptr || this->dedicated_ui_texture != nullptr) {
                this->reset_dedicated_ui_creation_state();
                return;
            }

            auto* engine = sdk::UGameEngine::get();

            if (engine == nullptr) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] Delaying dedicated UI texture creation because UGameEngine is not ready");
                this->reset_dedicated_ui_creation_state();
                return;
            }

            auto* world = engine->get_world();

            if (world == nullptr) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] Delaying dedicated UI texture creation because the world is not ready");
                this->reset_dedicated_ui_creation_state();
                return;
            }

            auto* kismet_rendering = sdk::UKismetRenderingLibrary::get();

            if (kismet_rendering == nullptr) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] Delaying dedicated UI texture creation because KismetRenderingLibrary is not ready");
                this->reset_dedicated_ui_creation_state();
                return;
            }

            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
            auto* tgt_raw = kismet_rendering->create_render_target_2d(world, width, height, 2, clear_color, false);

            if (tgt_raw == nullptr) {
                SPDLOG_WARNING_EVERY_N_SEC(2, "[VRRenderTargetManager] Failed to create dedicated UI texture [{}x{}] on the game thread", width, height);
                this->reset_dedicated_ui_creation_state();
                return;
            }

            tgt_raw->add_to_root();

            if (!this->is_dedicated_ui_generation_current(generation)) {
                try {
                    tgt_raw->remove_from_root();
                } catch (...) {
                }
                return;
            }

            sdk::UObjectReference<sdk::UTexture> tgt{tgt_raw};
            this->in_flight_dedicated_ui_texture = tgt_raw;
            this->dedicated_ui_object_created = true;
            this->dedicated_ui_pending_since = std::chrono::steady_clock::now();
            this->dedicated_ui_resource_pending_since = this->dedicated_ui_pending_since;

            SPDLOG_INFO("[VRRenderTargetManager] Created dedicated UI UObject for generation {}", generation);

            RenderThreadWorker::get().enqueue_conditional(
                [this, tgt, width, height, generation]() -> bool {
                    try {
                        if (!this->is_dedicated_ui_generation_current(generation)) {
                            return true;
                        }

                        if (!tgt.valid()) {
                            SPDLOG_ERROR("[VRRenderTargetManager] dedicated UI texture was destroyed before its render resource became valid");
                            GameThreadWorker::get().enqueue([this, generation]() -> void {
                                if (!this->is_dedicated_ui_generation_current(generation)) {
                                    return;
                                }

                                this->in_flight_dedicated_ui_texture = nullptr;
                                this->destroy_dedicated_ui_target();
                            });
                            return true;
                        }

                        if (!sdk::UTexture::update_render_resource_offset_texture2d(tgt)) {
                            return false;
                        }

                        auto* rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource();
                        if (rsrc == nullptr) {
                            return false;
                        }

                        const bool updated_vtable_offset = sdk::FTextureRenderTargetResource::update_render_target_vtable_offset(rsrc);
                        auto* frt = updated_vtable_offset ? rsrc->as_render_target() : nullptr;

                        if (frt == nullptr) {
                            return false;
                        }

                        sdk::FRenderTarget::update_offsets(frt);
                        auto** frt_texture = frt->get_render_target_texture();

                        if (frt_texture == nullptr || *frt_texture == nullptr || IsBadReadPtr(*frt_texture, sizeof(void*))) {
                            return false;
                        }

                        if (!this->is_dedicated_ui_generation_current(generation)) {
                            return true;
                        }

                        FRHITexture2D::set_vtable(*(void**)*frt_texture);
                        this->set_dedicated_ui_target(*frt_texture, width, height);
                        this->get_fallback_ui_target_ref() = nullptr;

                        GameThreadWorker::get().enqueue([this, tgt, generation]() -> void {
                            if (!this->is_dedicated_ui_generation_current(generation)) {
                                return;
                            }

                            if (!tgt.valid()) {
                                this->dedicated_ui_texture = nullptr;
                                this->in_flight_dedicated_ui_texture = nullptr;
                                this->destroy_dedicated_ui_target();
                                return;
                            }

                            this->dedicated_ui_texture = tgt;
                            this->in_flight_dedicated_ui_texture = nullptr;
                            this->reset_dedicated_ui_creation_state();
                        });

                        SPDLOG_INFO("[VRRenderTargetManager] dedicated UI target ready for generation {} [{}x{}]", generation, width, height);
                        return true;
                    } catch (...) {
                        SPDLOG_ERROR("[VRRenderTargetManager] Exception while waiting for the dedicated UI texture render resource");
                        GameThreadWorker::get().enqueue([this, generation]() -> void {
                            if (!this->is_dedicated_ui_generation_current(generation)) {
                                return;
                            }

                            this->dedicated_ui_texture = nullptr;
                            this->in_flight_dedicated_ui_texture = nullptr;
                            this->destroy_dedicated_ui_target();
                        });
                        return true;
                    }
                },
                [this, generation]() {
                    if (!this->is_dedicated_ui_generation_current(generation)) {
                        return;
                    }

                    SPDLOG_ERROR("[VRRenderTargetManager] dedicated UI target generation {} timed out waiting for the render resource", generation);
                    GameThreadWorker::get().enqueue([this, generation]() -> void {
                        if (!this->is_dedicated_ui_generation_current(generation)) {
                            return;
                        }

                        this->dedicated_ui_texture = nullptr;
                        this->in_flight_dedicated_ui_texture = nullptr;
                        this->destroy_dedicated_ui_target();
                    });
                },
                std::chrono::seconds(2));
        } catch (...) {
            SPDLOG_ERROR("[VRRenderTargetManager] Exception while scheduling dedicated UI texture creation");
            this->in_flight_dedicated_ui_texture = nullptr;
            this->reset_dedicated_ui_creation_state();
        }
    });

    return true;
}

void VRRenderTargetManager_Base::ensure_dedicated_ui_target(uintptr_t command_list) {
    (void)command_list;

    if (!supports_dedicated_ui_target_for_current_game()) {
        return;
    }

    if (dedicated_ui_width == 0 || dedicated_ui_height == 0) {
        return;
    }

    auto existing_target = get_dedicated_ui_target();

    if (existing_target != nullptr && !IsBadReadPtr(existing_target, sizeof(void*))) {
        if (g_framework->is_dx11()) {
            auto* native_resource = (ID3D11Texture2D*)existing_target->get_native_resource();

            if (native_resource != nullptr && !IsBadReadPtr(native_resource, sizeof(void*))) {
                D3D11_TEXTURE2D_DESC desc{};
                native_resource->GetDesc(&desc);

                if (desc.Width == dedicated_ui_width && desc.Height == dedicated_ui_height) {
                    return;
                }
            }
        } else {
            auto* native_resource = (ID3D12Resource*)existing_target->get_native_resource();

            if (native_resource != nullptr) {
                const auto desc = native_resource->GetDesc();

                if (desc.Width == dedicated_ui_width && desc.Height == dedicated_ui_height) {
                    return;
                }
            }
        }

        SPDLOG_INFO("[VRRenderTargetManager] Recreating dedicated UI target [{}x{}]", dedicated_ui_width, dedicated_ui_height);
        destroy_dedicated_ui_target();
    }

    if (existing_target == nullptr && owned_dedicated_ui_target != nullptr && owned_dedicated_ui_target->texture != nullptr &&
        !IsBadReadPtr(owned_dedicated_ui_target->texture, sizeof(void*)))
    {
        FRHITexture2D::set_vtable(*(void**)owned_dedicated_ui_target->texture);
        dedicated_ui_target = owned_dedicated_ui_target->texture;
        existing_target = get_dedicated_ui_target();
    }

    if (dedicated_ui_texture != nullptr && dedicated_ui_texture.valid()) {
        if (sdk::UTexture::update_render_resource_offset_texture2d(dedicated_ui_texture)) {
            if (auto* rsrc = (sdk::FTextureRenderTargetResource*)dedicated_ui_texture->get_resource(); rsrc != nullptr) {
                const bool updated_vtable_offset = sdk::FTextureRenderTargetResource::update_render_target_vtable_offset(rsrc);
                auto* frt = updated_vtable_offset ? rsrc->as_render_target() : nullptr;

                if (frt != nullptr) {
                    sdk::FRenderTarget::update_offsets(frt);
                    auto** frt_texture = frt->get_render_target_texture();

                    if (frt_texture != nullptr && *frt_texture != nullptr && !IsBadReadPtr(*frt_texture, sizeof(void*))) {
                        FRHITexture2D::set_vtable(*(void**)*frt_texture);
                        set_dedicated_ui_target(*frt_texture, dedicated_ui_width, dedicated_ui_height);
                        get_fallback_ui_target_ref() = nullptr;
                        return;
                    }
                }
            }
        }

        SPDLOG_INFO_EVERY_N_SEC(5, "[VRRenderTargetManager] dedicated UI UObject is alive but its render resource is not ready");
    }

    if (dedicated_ui_object_created &&
        in_flight_dedicated_ui_texture != nullptr &&
        in_flight_dedicated_ui_generation != 0 &&
        dedicated_ui_resource_pending_since.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - dedicated_ui_resource_pending_since > std::chrono::seconds(5))
    {
        SPDLOG_WARN("[VRRenderTargetManager] dedicated UI target creation appears stuck; resetting and retrying");
        destroy_dedicated_ui_target();
    }

    if (in_flight_dedicated_ui_texture != nullptr || dedicated_ui_creation_pending || in_flight_dedicated_ui_generation != 0) {
        return;
    }

    try_schedule_dedicated_ui_creation();
}

FRHITexture2D* VRRenderTargetManager_Base::get_scene_capture_render_target() {
    if (this->in_flight_target != nullptr) {
        return nullptr;
    }

    const auto is_same_as_rhi_thread = RHIThreadWorker::get().is_same_thread();
    const auto& sct = is_same_as_rhi_thread ? this->scene_capture_target_rhi_thread : this->scene_capture_target;

    if (sct != nullptr) try {
        // I REALLY don't want to lock a mutex in a hot path so let's hope that our exception handler catches everything.
        if (!sct.valid()) {
            SPDLOG_WARN("[VRRenderTargetManager] Scene capture target is not a UTexture! Texture probably deleted on level change!");
            
            if (is_same_as_rhi_thread) {
                this->scene_capture_target_rhi_thread = nullptr;
            }

            return nullptr;
        }

        auto rsrc = (sdk::FTextureRenderTargetResource*)sct->get_resource();
        auto rsrc_frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;

        if (rsrc_frt != nullptr) {  
            auto tex_ref = rsrc_frt->get_render_target_texture();
            if (tex_ref != nullptr) {
                return *tex_ref;
            }
        }
    } catch (...) {
        SPDLOG_ERROR("[VRRenderTargetManager] Exception in get_scene_capture_render_target! Texture probably deleted on level change!");

        if (is_same_as_rhi_thread) {
            this->scene_capture_target_rhi_thread = nullptr;
        }
    }

    return nullptr;
}

sdk::UTexture* VRRenderTargetManager_Base::get_scene_capture_utexture() {
    if (this->in_flight_target != nullptr) {
        return nullptr;
    }

    const auto& utex = this->scene_capture_target;

    if (utex != nullptr) try {
        if (utex.valid()) {
            return (sdk::UTexture*)utex;
        }

        SPDLOG_WARN("[VRRenderTargetManager] Scene capture target is not a UTexture! Texture probably deleted on level change!");

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    } catch (...) {
        SPDLOG_ERROR("[VRRenderTargetManager] Exception in get_scene_capture_utexture! Texture probably deleted on level change!");

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    }

    return nullptr;
}

bool VRRenderTargetManager_Base::create_scene_capture() try {
    if (this->in_flight_target != nullptr) {
        return false;
    }

    // This is necessary for offset calculations to succeed.
    if (FRHITexture2D::get_vtable() == nullptr) {
        SPDLOG_WARN("[VRRenderTargetManager] FRHITexture2D vtable is null, waiting for it to be set!");
        return false;
    }

    destroy_scene_capture();

    SPDLOG_INFO("Creating scene capture!");

    auto kismet_rendering = sdk::UKismetRenderingLibrary::get();

    if (kismet_rendering == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UKismetRenderingLibrary!");
        return false;
    }

    static auto scene_capture_c = sdk::USceneCaptureComponent2D::static_class();

    if (scene_capture_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get USceneCaptureComponent2D class!");
        return false;
    }

    auto ugs = sdk::UGameplayStatics::get();

    if (ugs == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameplayStatics!");
        return false;
    }

    auto engine = sdk::UGameEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameEngine!");
        return false;
    }

    auto world = engine->get_world();

    if (world == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UWorld!");
        return false;
    }

    static auto actor_c = sdk::AActor::static_class();

    if (actor_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get AActor class!");
        return false;
    }

    this->scene_capture_actor = ugs->spawn_actor(world, actor_c, glm::vec3{0, 0, 0});

    if (this->scene_capture_actor == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to spawn actor!");
        return false;
    }

    this->scene_capture_component = (sdk::USceneCaptureComponent2D*)this->scene_capture_actor->add_component_by_class(scene_capture_c, false);

    if (this->scene_capture_component == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to add scene capture component!");
        return false;
    }

    const float clear_color[4] {0.0f, 0.0f, 0.0f, 1.0f};
    auto tgt_raw = kismet_rendering->create_render_target_2d(world, VR::get()->get_hmd_width(), VR::get()->get_hmd_height(), 2, clear_color, false);

    if (tgt_raw == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to create texture!");
        return false;
    }

    sdk::UObjectReference tgt{tgt_raw};

    SPDLOG_INFO("[VRRenderTargetManager] Created texture target: {:x}", (uintptr_t)tgt.get());
    this->scene_capture_actor->finish_add_component(this->scene_capture_component);

    this->scene_capture_component->set_texture_target(tgt);

    // We don't actually want this to tick.
    // We are just using the property as a convenient way to keep the texture alive without crashing.
    this->scene_capture_component->set_visibility(false);
    if (auto capture_every_frame = scene_capture_c->find_property(L"bCaptureEveryFrame"); capture_every_frame != nullptr) {
        *capture_every_frame->get_data<bool>(this->scene_capture_component) = false;
    }

    static bool already_updated{false};
    static std::array<uintptr_t, 100> original_frender_target_vtable{};
    static auto gamma_increase_fn = +[](const sdk::FRenderTarget* frt) -> float {
        auto rtm = g_hook->get_render_target_manager();
        auto viewport = rtm != nullptr ? rtm->get_viewport() : nullptr;

        if (viewport != nullptr) {
            return viewport->get_display_gamma();
        }

        return 2.2f;
    };

    static auto hook_frt = [](sdk::FRenderTarget* frt) {
        if (frt == nullptr) {
            SPDLOG_WARN("[FRenderTarget] FRenderTarget is null! Can't hook!");
            return;
        }

        SPDLOG_INFO("[FRenderTarget] Hooking FRenderTarget!");

        auto& vtable = *(void**)frt;
        memcpy(original_frender_target_vtable.data(), vtable, original_frender_target_vtable.size() * sizeof(uintptr_t));

        if (auto display_gamma_index = sdk::FRenderTarget::get_display_gamma_index(); display_gamma_index != 0) {
            original_frender_target_vtable[*display_gamma_index] = (uintptr_t)gamma_increase_fn;
            vtable = original_frender_target_vtable.data();
            SPDLOG_INFO("[FRenderTarget] Hooked FRenderTarget!");
        } else {
            SPDLOG_WARN("[FRenderTarget] Gamma index not found, can't hook!");
        }
    };

    static const auto utex_c = sdk::UTexture::static_class();

    // Enqueue offset lookup on the render thread because that's when the resource is actually created.
    if (!already_updated) {
        this->in_flight_target = tgt;

        // Repeats every render loop for 5 seconds, times out if the texture is not created.
        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
                    return true;
                }
    
                if (sdk::UTexture::update_render_resource_offset_texture2d(tgt)) {
                    SPDLOG_INFO("Successfully updated render resource offset for scene capture target!");
    
                    if (auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource(); rsrc != nullptr) {
                        const bool success = sdk::FTextureRenderTargetResource::update_render_target_vtable_offset(rsrc);
                        const auto frt = success ? rsrc->as_render_target() : nullptr;
    
                        if (frt != nullptr) {
                            sdk::FRenderTarget::update_offsets(frt);

                            if (frt->get_render_target_texture() == nullptr || *frt->get_render_target_texture() == nullptr) {
                                SPDLOG_WARN("Waiting for render target texture to be valid...");
                                return false;
                            }
    
                            hook_frt(frt);
    
                            RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->scene_capture_target_rhi_thread = nullptr;
                                    return;
                                }

                                this->scene_capture_target_rhi_thread = tgt;
                            });
                            
                            GameThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->in_flight_target = nullptr;
                                    destroy_scene_capture();
                                    return;
                                }
                                
                                this->scene_capture_target = tgt;
                                this->in_flight_target = nullptr;
    
                                SPDLOG_INFO("Scene capture texture created!");
                            });
    
                            already_updated = true;
        
                            return true;
                        }
    
                        SPDLOG_WARN("Waiting for render target to be valid...");
    
                        return false; // Keep waiting until it works.
                    }
                } else {
                    SPDLOG_ERROR("Failed to update render resource offset for scene capture target!");
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture (offset lookup): {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture (offset lookup)!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }

            return false;
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));
    
        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    } else {
        this->in_flight_target = tgt;

        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
    
                    return true;
                }
    
                auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource();
                auto frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;
                auto frttex = frt != nullptr ? frt->get_render_target_texture() : nullptr;
    
                // Wait until FRenderTarget is not null.
                if (frt == nullptr || frttex == nullptr || *frttex == nullptr) {
                    SPDLOG_WARN("Waiting for render target to be valid...");
                    return false;
                }
    
                hook_frt(frt);
    
                RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->scene_capture_target_rhi_thread = nullptr;
                        return;
                    }

                    this->scene_capture_target_rhi_thread = tgt;
                });
    
                GameThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                        return;
                    }
    
                    this->in_flight_target = nullptr;
                    this->scene_capture_target = tgt;
    
                    SPDLOG_INFO("Scene capture texture fully created!");
                });
    
                return true;
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));

        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    }

    return true;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
    return false;
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
    return false;
}

// This is a very special fix for cases where engine modifications
// can add a second call to UpdateViewportRHI right before the place we expect it to get called
// The fact that they get called back-to-back over and over causes huge performance problems
// because the viewport texture keeps getting recreated over and over.
// This hook attempts to only allow the last call to UpdateViewportRHI inside of EnqueueBeginRenderFrame to do anything
// Usually there's only one call to UpdateViewportRHI inside of EnqueueBeginRenderFrame, but (very rarely) there can be two.
__declspec(noinline) void FFakeStereoRenderingHook::update_viewport_rhi_hook(void* viewport, size_t destroyed, size_t new_size_x, size_t new_size_y, size_t new_window_mode, size_t preferred_pixel_format) {
    auto call_orig = [&]() {
        g_hook->m_update_viewport_rhi_hook->get_original<void(*)(void*, size_t, size_t, size_t, size_t, size_t)>()(viewport, destroyed, new_size_x, new_size_y, new_window_mode, preferred_pixel_format);
    };

    SPDLOG_INFO_ONCE("UpdateViewportRHI (embedded): {:x}", (uintptr_t)_ReturnAddress());

    const auto hmd_active = VR::get()->is_hmd_active();
    static bool modified_use_separate_rt = false;

    if (!hmd_active) {
        if (modified_use_separate_rt) {
            modified_use_separate_rt = false;

            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    SPDLOG_INFO_ONCE("Resetting bUseSeparateRenderTarget to false!");
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));
                    use_separate_rt = false;
                }
            }
        }

        call_orig();
        return;
    }

    struct FunctionInfo {
        std::vector<uintptr_t> return_addrs{}; // in order of call
        size_t count{0};
    };

    static std::mutex mtx{};
    static std::unordered_map<uintptr_t, uintptr_t> functions_within{};
    static std::unordered_map<uintptr_t, FunctionInfo> function_infos{};

    {
        std::scoped_lock _{mtx};

        const auto return_addr = (uintptr_t)_ReturnAddress();
        auto function_within = functions_within.find(return_addr);

        if (function_within == functions_within.end()) {
            const auto result = utility::find_virtual_function_start(return_addr);

            if (result) {
                functions_within[return_addr] = *result;
            } else {
                functions_within[return_addr] = 0;
            }

            function_within = functions_within.find(return_addr);

            if (function_within->second != 0) {
                ++function_infos[function_within->second].count;
            }

            function_infos[function_within->second].return_addrs.push_back(return_addr);

            SPDLOG_INFO("Added new call of UpdateViewportRHI to function {:x} (count: {})", function_within->second, function_infos[function_within->second].count);
        }

        if (function_within->second == 0) {
            SPDLOG_INFO_ONCE("Could not find vfunc start for call of UpdateViewportRHI, calling original.");
            call_orig();
            return;
        }

        const auto& function_info = function_infos[function_within->second]; 

        // We only care about corrections where UpdateViewportRHI is called more than once in the same function.
        if (function_info.count <= 1 || function_info.return_addrs.empty()) {
            call_orig();
            return;
        }

        if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    auto& should_force_separate_rt = *(bool*)((uintptr_t)viewport + *offset);
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));

                    if (!should_force_separate_rt) {
                        SPDLOG_INFO_ONCE("UpdateViewportRHI was called without should_force_separate_rt being set to true, skipping.");
                        should_force_separate_rt = true;
                        use_separate_rt = true;
                        modified_use_separate_rt = true;
                        return; // NO!!!!!!!!!!!!!!!!!!!
                    }
                }
            }
        } else {      
            // We only want the last function to be called.
            // We don't need to call the original here because it will get called by the last function.
            // if we call the original here, it will cause performance issues.
            if (function_info.return_addrs.back() != return_addr) {
                return;
            }
        }
    }

    
    if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
        call_orig();
        return;
    }

    auto& rtm = g_hook->get_embedded_rtm();

    static std::chrono::steady_clock::time_point last_time_hmd_active{};
    static bool hmd_was_active = false;
    bool should_call_orig = false;

    if (hmd_active && !hmd_was_active) {
        last_time_hmd_active = std::chrono::steady_clock::now();
        hmd_was_active = true;
    } else if (!hmd_active) {
        hmd_was_active = false;
        should_call_orig = true;
    }

    if (hmd_active) {
        should_call_orig = std::chrono::steady_clock::now() - last_time_hmd_active <= std::chrono::milliseconds(2000);
        //should_call_orig = should_call_orig || (std::chrono::steady_clock::now() - rtm.last_time_needed_hmd_reallocate <= std::chrono::milliseconds(2000));
    }

    if (should_call_orig) {
        rtm.should_use_separate_rt_called = false;
        rtm.need_reallocate_viewport_render_target_called = false;
        call_orig();
        return;
    }

    if (!rtm.should_use_separate_rt_called) {
        SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because ShouldUseSeparateRenderTarget() was not called!");
        return; // Do not call at all.
    }

    if (!rtm.need_reallocate_viewport_render_target_called) {
        const auto need_reallocate = g_hook->get_render_target_manager()->need_reallocate_view_target(*(sdk::FViewport*)viewport);

        if (!need_reallocate) {
            SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because NeedReallocateViewportRenderTarget() was not called and we don't need to reallocate anyway!");
            rtm.should_use_separate_rt_called = false;
            return; // Do not call at all.
        }

        SPDLOG_INFO_ONCE("We need to reallocate the viewport render target even though NeedReallocateViewportRenderTarget() was not called!");
        //rtm.last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
    }

    call_orig();
    rtm.should_use_separate_rt_called = false;
    rtm.need_reallocate_viewport_render_target_called = false;
}

void FFakeStereoRenderingHook::attempt_hook_update_viewport_rhi(uintptr_t return_address) {
    if (/*!m_rendertarget_manager_embedded_in_stereo_device ||*/ m_special_detected || m_attempted_hook_update_viewport_rhi) {
        return;
    }

    m_attempted_hook_update_viewport_rhi = true;

    if (m_update_viewport_rhi_hook == nullptr) {
        SPDLOG_INFO("Attempting to hook UpdateViewportRHI...");

        const auto init_dynamic_rhi = utility::find_virtual_function_start(return_address);

        if (init_dynamic_rhi) {
            SPDLOG_INFO("Found InitDynamicRHI: {:x}", *init_dynamic_rhi);

            const auto init_dynamic_rhi_ptr = utility::scan_ptr(*utility::get_module_within(*init_dynamic_rhi), *init_dynamic_rhi);
            if (!init_dynamic_rhi_ptr) {
                SPDLOG_ERROR("Failed to find InitDynamicRHI pointer!");
                return;
            }

            const auto update_viewport_rhi_ptr = *init_dynamic_rhi_ptr - (sizeof(void*) * 2);

            if (*(void**)update_viewport_rhi_ptr == nullptr || IsBadReadPtr(*(void**)update_viewport_rhi_ptr, sizeof(void*))) {
                SPDLOG_ERROR("Failed to find UpdateViewportRHI!");
                return;
            }

            // Make sure this is no displacement reference to this. This can mean we accidentally found the vtable for IViewportRenderTargetProvider
            // The vfunc pointer should be in the middle of the vtable, not the start.
            if (utility::scan_displacement_reference(*utility::get_module_within(*init_dynamic_rhi), update_viewport_rhi_ptr)) {
                SPDLOG_ERROR("Found displacement reference to UpdateViewportRHI, this is probably the vtable for IViewportRenderTargetProvider, aborting!");
                return;
            }

            m_update_viewport_rhi_hook = std::make_unique<PointerHook>((void**)update_viewport_rhi_ptr, &update_viewport_rhi_hook);
        } else {
            SPDLOG_ERROR("Failed to find InitDynamicRHI, cannot hook UpdateViewportRHI!");
        }
    }
}

bool VRRenderTargetManager_Base::allocate_render_target_texture(uintptr_t return_address, FTexture2DRHIRef* tex, FTexture2DRHIRef* shader_resource) {
    this->texture_hook_ref = tex;
    this->shader_resource_hook_ref = shader_resource;
    this->allocate_texture_called = true;

    if (!this->set_up_texture_hook) {
        ZoneScopedN("VRRenderTargetManager_Base::allocate_render_target_texture initialization");
        SPDLOG_INFO("AllocateRenderTargetTexture retaddr: {:x}", return_address);

        g_hook->attempt_hook_update_viewport_rhi(return_address);

        SPDLOG_INFO("Scanning for call instr...");

        bool next_call_is_not_the_right_one = false;

        auto is_string_nearby = [](uintptr_t addr, std::wstring_view str) {
            const auto addr_module = utility::get_module_within(addr);
            if (!addr_module) {
                return false;
            }

            const auto module_size = utility::get_module_size(*addr_module);
            const auto module_end = (uintptr_t)*addr_module + *module_size - 0x1000;

            // Find all possible strings, not just the first one
            for (auto str_addr = utility::scan_string(*addr_module, str.data(), true); 
                str_addr.has_value(); 
                str_addr = utility::scan_string(*str_addr + 1, (module_end - (*str_addr + 1)), str.data(), true)) 
            {
                // Scan for ALL references to this string
                for (auto string_ref = utility::scan_displacement_reference(*addr_module, (uintptr_t)*str_addr);
                    string_ref.has_value();
                    string_ref = utility::scan_displacement_reference(*string_ref + 1, (module_end - (*string_ref + 1)), (uintptr_t)*str_addr))
                {
                    const auto string_ref_func_start = utility::find_function_start((uintptr_t)*string_ref);
                    const auto return_addr_func_start = utility::find_function_start(addr);

                    SPDLOG_INFO("String ref func start: {:x}", (uintptr_t)*string_ref_func_start);
                    SPDLOG_INFO("Return addr func start: {:x}", (uintptr_t)*return_addr_func_start);

                    if (string_ref_func_start && return_addr_func_start && *string_ref_func_start == *return_addr_func_start) {
                        return true;
                    }
                }
            }

            return false;
        };

        // This string is present in UE5 (>= 5.1) and used when using texture descriptors to create textures.
        // that means this is UE5 and the function will take a texture descriptor instead of a bunch of arguments.
        if (is_string_nearby(return_address, L"BufferedRT")) {
            SPDLOG_INFO("Found string ref for BufferedRT, this is UE5!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = true;

            if (is_ue57_dx11_backend()) {
                SPDLOG_WARN_ONCE("Skipping UE 5.7 D3D11 BufferedRT texture-create replay hook; using the engine allocation path");
                this->set_up_texture_hook = true;
                return false;
            }
        }

        // Present in a specific game or game(s), somewhere around 4.8-4.12 (?)
        // indicates that texture descriptors are being used.
        if (is_string_nearby(return_address, L"SceneViewBuffer")) {
            SPDLOG_INFO("Found string ref for SceneViewBuffer, texture descriptors are being used!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = false;

            next_call_is_not_the_right_one = true; // not seen a case where this isn't true (yet)
        }

        // Now, we need to emulate from where AllocateRenderTargetTexture returns from
        // we will set RAX to false, to get the control flow correct
        // and then keep emulating until we hit the call we want
        // Previously, we were using just straight linear disassembly to do this, and it mostly worked
        // but in one game, there was an unconditional branch after the call instead of flowing
        // directly into the next instruction.
        auto emu = utility::ShemuContext{*utility::get_module_within(return_address)};

        emu.ctx->Registers.RegRax = 0;
        emu.ctx->Registers.RegRip = (ND_UINT64)return_address;
        emu.ctx->MemThreshold = 100;

        const std::vector<std::string> bad_patterns_before_call = {
            "B2 32", // mov dl, 32h, (seen in UE5 debug/dev builds)
            "B2 2A", // mov dl, 2Ah, (seen in UE4.23 debug/dev builds)
            "B2 2B", // mov dl, 2Bh, (seen in UE4.25 debug/dev builds)
            "BA 2F 00 00 00", // mov edx, 2Fh (seen in UE5 debug/dev builds)
            "F6 85 ? ? ? ? 05", // test byte ptr [rbp+?], 5 (seen in UE5 debug/dev builds)
        };

        auto is_probable_ue57_texture_desc_prepare = [](uintptr_t fn) {
            return utility::scan(fn, 0x80, "0F B6 42 32").has_value()
                && utility::scan(fn, 0x80, "48 8B 42 24").has_value()
                && utility::scan(fn, 0x80, "48 83 C2 38").has_value();
        };

        struct DirectCallInfo {
            uintptr_t callsite{};
            uintptr_t target{};
            uint8_t length{};
        };

        auto find_next_direct_calls = [](uintptr_t start, size_t span, size_t max_calls) {
            std::vector<DirectCallInfo> results{};

            utility::exhaustive_decode((uint8_t*)start, span, [&](const utility::ExhaustionContext& decode_ctx) -> utility::ExhaustionResult {
                if (std::string_view{decode_ctx.instrux.Mnemonic}.starts_with("CALL") && *(uint8_t*)decode_ctx.addr == 0xE8) {
                    results.emplace_back(DirectCallInfo{
                        .callsite = decode_ctx.addr,
                        .target = utility::calculate_absolute(decode_ctx.addr + 1),
                        .length = (uint8_t)decode_ctx.instrux.Length
                    });

                    if (results.size() >= max_calls) {
                        return utility::ExhaustionResult::BREAK;
                    }
                }

                return utility::ExhaustionResult::CONTINUE;
            });

            return results;
        };

        while(true) {
            if (emu.ctx->InstructionsCount > 200) {
                SPDLOG_WARN("Emulated too many instructions without finding the call, aborting!");
                break;
            }

            const auto ip = emu.ctx->Registers.RegRip;
            const auto bytes = (uint8_t*)ip;
            const auto decoded = utility::decode_one((uint8_t*)ip);

            if (ip != 0) {
                for (const auto& pattern : bad_patterns_before_call) {
                    if (utility::scan(ip, 100, pattern).value_or(0) == ip) {
                        SPDLOG_INFO("Found bad pattern before call, skipping next call: {:x} ({})", ip, pattern);
                        next_call_is_not_the_right_one = true;
                        break;
                    }
                }
            }
            
            if (!next_call_is_not_the_right_one) try {
                const auto addr = utility::resolve_displacement(ip);

                if (addr && !IsBadReadPtr((void*)*addr, 12)) {
                    if (std::wstring_view{(const wchar_t*)*addr}.starts_with(L"BufferedRT")) {
                        this->is_using_texture_desc = true;
                        this->is_version_greq_5_1 = true;

                        SPDLOG_INFO("Found usage of string \"BufferedRT\" while analyzing AllocateRenderTargetTexture!");
                    } else if (std::string_view{(const char*)*addr}.starts_with("IsInRenderingThread") && std::string_view{decoded->Mnemonic}.starts_with("LEA") && decoded->Operands[0].Type == ND_OP_REG && decoded->Operands[0].Info.Register.Reg == NDR_RCX) {
                        SPDLOG_INFO("Found usage of string \"IsInRenderingThread\" while analyzing AllocateRenderTargetTexture, skipping next call!");
                        next_call_is_not_the_right_one = true;
                    }
                }
            } catch(...) {

            }

            // make sure we are not emulating any instructions that write to memory
            // so we can just set the IP to the next instruction
            if (decoded) {
                const auto is_call = std::string_view{decoded->Mnemonic}.starts_with("CALL");

                if (decoded->MemoryAccess & ND_ACCESS_ANY_WRITE || is_call) {
                    // We are looking for the call instruction
                    // This instruction calls RHICreateTargetableShaderResource2D(TexSizeX, TexSizeY, SceneTargetFormat, 1, TexCreate_None,
                    // TexCreate_RenderTargetable, false, CreateInfo, BufferedRTRHI, BufferedSRVRHI); Which sets up the BufferedRTRHI and
                    // BufferedSRVRHI variables.
                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xE8) try {
                        // Analyze some of the instructions inside the call first
                        // If it has a mov eax, 0x800, then returns, we can skip this function
                        const auto fn = utility::calculate_absolute(ip + 1);
                        SPDLOG_INFO("Analyzing call at {:x} to {:x}", ip, fn);

                        if (is_ue57_dx11_backend() && this->is_version_greq_5_1 &&
                            is_probable_ue57_dx11_texture_desc_prepare_function(fn))
                        {
                            SPDLOG_INFO("Skipping UE 5.7 D3D11 texture-desc prepare helper at {:x}; continuing to the real texture-create wrapper", fn);
                            next_call_is_not_the_right_one = true;
                        } else if (g_framework->is_dx12() && is_ue_5_7_or_newer()) {
                            const auto next_calls = find_next_direct_calls((uintptr_t)ip + decoded->Length, 0x80, 2);

                            if (next_calls.size() >= 2) {
                                this->texture_desc_prepare_func = fn;
                                this->texture_create_wrapper_func = next_calls[0].target;
                                this->texture_finalize_func = next_calls[1].target;
                                this->texture_release_func = 0;
                                this->texture_extract_func = 0;
                                this->texture_finalize_callsite = 0;
                                this->texture_extract_callsite = 0;

                                SPDLOG_INFO("Resolved UE 5.7 texture-desc helper {:x}, wrapper {:x}, and finalize helper {:x}",
                                    this->texture_desc_prepare_func,
                                    this->texture_create_wrapper_func,
                                    this->texture_finalize_func);

                                const auto wrapper_call_ip = next_calls[0].callsite;
                                const auto finalize_post_call = next_calls[1].callsite + next_calls[1].length;

                                this->texture_create_insn_bytes.resize(next_calls[0].length);
                                memcpy(this->texture_create_insn_bytes.data(), (void*)wrapper_call_ip, next_calls[0].length);

                                auto texture_hook_result = safetyhook::MidHook::create((void*)finalize_post_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::texture_hook_callback(ctx, false);
                                });

                                if (!texture_hook_result.has_value()) {
                                    const auto e = texture_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create UE 5.7 post texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create UE 5.7 post texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->texture_hook = std::move(texture_hook_result.value());
                                }

                                auto pre_texture_hook_result = safetyhook::MidHook::create((void*)wrapper_call_ip, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::pre_texture_hook_callback(ctx, false);
                                });

                                if (!pre_texture_hook_result.has_value()) {
                                    const auto e = pre_texture_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create UE 5.7 pre texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create UE 5.7 pre texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->pre_texture_hook = std::move(pre_texture_hook_result.value());
                                }

                                this->is_pre_texture_call_e8 = true;
                                this->set_up_texture_hook = true;
                                return false;
                            }

                            if (is_probable_ue57_texture_desc_prepare(fn)) {
                                SPDLOG_INFO("Detected UE 5.7 texture-desc prepare helper at {:x}, but failed to resolve the wrapper/finalize sequence", fn);
                            }
                        } else if (auto result = utility::scan(fn, 10, "41 B8 30 00 00 00"); result.has_value() && *result == fn) {
                            SPDLOG_INFO("First instruction is a mov r8d, 30h, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (auto result = utility::scan(fn, 50, "B8 00 08 00 00 C3"); result.has_value()) {
                            SPDLOG_INFO("First few instructions are a mov eax, 800h, ret, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (this->is_version_greq_5_1) { // Limiting the scope of this to newer UE5 versions so we don't potentially break older versions
                            const auto module_fn_within = utility::get_module_within(fn);
                            const auto next_insn = (uint8_t*)(ip + decoded->Length);

                            // Seen on UE5.3.2 development builds
                            if (auto result = utility::scan_disasm(fn, 15, "BD 01 00 00 00"); result.has_value()) {
                                // This string is not unicode
                                if (utility::find_string_reference_in_path(fn, "InGPUMask != 0", false).has_value()) {
                                    SPDLOG_INFO("Found InGPUMask != 0 string within the function and mov ebp, 1, skipping this call!");
                                    next_call_is_not_the_right_one = true;
                                }
                            } else if (next_insn[0] == 0x84 && next_insn[1] == 0xC0) { // test al, al
                                if (auto ref = utility::find_string_reference_in_path((uintptr_t)next_insn, "IsInRenderingThread()", false); ref.has_value()) {
                                    if (ref->addr > (uintptr_t)next_insn && ref->addr - (uintptr_t)next_insn < 30) {
                                        SPDLOG_INFO("Found IsInRenderingThread() instead of the function we want, skipping this call!");
                                        next_call_is_not_the_right_one = true;
                                    }
                                }
                            } else if (utility::find_pattern_in_path((uint8_t*)fn, 30, true, "66 41 C7 40 34 00 FF")) {
                                SPDLOG_INFO("Found 66 41 C7 40 34 00 FF pattern within the function, skipping this call!");
                                next_call_is_not_the_right_one = true;
                            } else {
                                // Check how many instructions are in the call. If there's <= 30 AND there's no call/jmp in it, this is not the right one
                                size_t insn_count = 0;
                                bool encountered_branch = false;
                                utility::exhaustive_decode((uint8_t*)fn, 200, [&](const utility::ExhaustionContext& ctx) -> utility::ExhaustionResult {
                                    if (std::string_view{ctx.instrux.Mnemonic}.starts_with("CALL") || std::string_view{ctx.instrux.Mnemonic}.starts_with("JMP")) {
                                        encountered_branch = true;
                                        return utility::ExhaustionResult::BREAK;
                                    }

                                    return utility::ExhaustionResult::CONTINUE;
                                });

                                if (insn_count <= 30 && !encountered_branch) {
                                    SPDLOG_INFO("Function at {:x} only has {} instructions and no calls/branches, skipping this call!", fn, insn_count);
                                    next_call_is_not_the_right_one = true;
                                }
                            }
                        }
                    } catch(...) {
                        SPDLOG_INFO("Failed to analyze call at {:x}", ip);
                    }

                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xFF && bytes[1] == 0x15) {
                        // well this definitely is not the right one, indirect calls have never called the function we wanted (I think)
                        SPDLOG_INFO("Found indirect call @ {:x}, skipping", ip);
                        next_call_is_not_the_right_one = true;
                    }

                    if (is_call && !next_call_is_not_the_right_one) {
                        const auto post_call = (uintptr_t)ip + decoded->Length;
                        SPDLOG_INFO("AllocateRenderTargetTexture post_call: {:x}, rel {:x}", post_call, post_call - (uintptr_t)*utility::get_module_within((void*)post_call));

                        if (*(uint8_t*)ip == 0xE8) {
                            SPDLOG_INFO("E8 call found!");
                            this->is_pre_texture_call_e8 = true;
                        } else {
                            SPDLOG_INFO("E8 call not found, assuming register call!");
                        }

                        // So we can call the original texture create function again.
                        this->texture_create_insn_bytes.resize(decoded->Length);
                        memcpy(this->texture_create_insn_bytes.data(), (void*)ip, decoded->Length);

                        if (this->is_version_greq_5_1 && !this->is_pre_texture_call_e8 && bytes[-7] == 0x48 && bytes[-6] == 0x8B && bytes[-5] == 0x0D && bytes[0] == 0xFF && bytes[1] == 0x94) {
                            // Scan forward for a similar one and also hook that
                            auto second_call = utility::scan((uintptr_t)ip + decoded->Length, 0x60, "48 8B 0D ? ? ? ? FF 94 ? ? ? ? ?");

                            if (second_call) {
                                // So we can call the original texture create function again.
                                this->texture_create_insn_bytes2.resize(decoded->Length);
                                memcpy(this->texture_create_insn_bytes2.data(), (void*)(*second_call + 7), decoded->Length);

                                SPDLOG_INFO("Found second call at {:x}", *second_call);
                                auto post_second_call = *second_call + 7 + decoded->Length;
                                //auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, &VRRenderTargetManager::texture_hook_callback);
                                auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::texture_hook_callback(ctx, true);
                                });

                                if (!texture_hook_result.has_value()) {
                                    const auto e = texture_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->texture_hook2 = std::move(texture_hook_result.value());
                                    SPDLOG_INFO("Successfully created second texture hook!");
                                }

                                auto pre_second_call = *second_call + 7;
                                //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, &VRRenderTargetManager::pre_texture_hook_callback);
                                auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::pre_texture_hook_callback(ctx, true);
                                });

                                if (!pre_texure_hook_result.has_value()) {
                                    const auto e = pre_texure_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->pre_texture_hook2 = std::move(pre_texure_hook_result.value());
                                    SPDLOG_INFO("Successfully created second pre texture hook!");
                                }
                            } else {
                                SPDLOG_INFO("Second call not detected! Continuing...");
                            }
                        }

                        //auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, &VRRenderTargetManager::texture_hook_callback);
                        auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::texture_hook_callback(ctx, false);
                        });

                        if (!texture_hook_result.has_value()) {
                            const auto e = texture_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->texture_hook = std::move(texture_hook_result.value());
                        }

                        //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, &VRRenderTargetManager::pre_texture_hook_callback);
                        auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::pre_texture_hook_callback(ctx, false);
                        });

                        if (!pre_texure_hook_result.has_value()) {
                            const auto e = pre_texure_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->pre_texture_hook = std::move(pre_texure_hook_result.value());
                        }
                        this->set_up_texture_hook = true;

                        return false;
                    }

                    SPDLOG_INFO("Skipping write to memory instruction at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    emu.ctx->Registers.RegRip += decoded->Length;
                    emu.ctx->Instruction = *decoded; // pseudo-emulate the instruction
                    ++emu.ctx->InstructionsCount;

                    if (is_call) {
                        next_call_is_not_the_right_one = false;
                    }
                } else if (emu.emulate() != SHEMU_SUCCESS) { // only emulate the non-memory write instructions
                    SPDLOG_INFO("Emulation failed at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    // instead of just adding it onto the RegRip, we need to use the ip we had previously from the decode
                    // because the emulator can move the instruction pointer after emulate() is called
                    emu.ctx->Registers.RegRip = ip + decoded->Length;
                    continue;
                }
            } else {
                break;
            }
        }

        SPDLOG_ERROR("Failed to find call instruction!");
    }

    return false;
}

bool VRRenderTargetManager::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) {
    // So, what's happening here is instead of using this method
    // to actually create our textures, we are going to
    // get the return address, scan forward for the next call instruction
    // and insert a midhook after the next call instruction.
    // The purpose of this is to get the texture that is being created
    // by the engine itself after we return false from this function.
    // When we return false from this function, it indicates
    // to the engine that we are letting the engine itself
    // create the texture, rather than us creating it ourselves.
    // This should allow maximum compatibility across engine versions.
    /*const auto dynamic_rhi = *(uintptr_t*)((uintptr_t)sdk::get_ue_module(L"Engine") + 0x3309C50);
    const auto command_list = (uintptr_t)sdk::get_ue_module(L"Engine") + 0x330AE70;
    struct {
        void* bulk_data{nullptr};
        void* rsrc_array{nullptr};

        struct {
            uint32_t color_binding{1};
            float color[4]{};
        } clear_value_binding;

        uint32_t gpu_mask{1};
        bool without_native_rsrc{false};
        const TCHAR* debug_name{"BufferedRT"};
        uint32_t extended_data{};
    } create_info;

    const void (*RHICreateTexture2D_RenderThread)(
        uintptr_t rhi,
        FTexture2DRHIRef* out,
        uintptr_t command_list,
        uint32_t w,
        uint32_t h,
        uint8_t format,
        uint32_t mips,
        uint32_t samples,
        ETextureCreateFlags flags,
        void* create_info) = (*(decltype(RHICreateTexture2D_RenderThread)**)dynamic_rhi)[178];

    *(uint64_t*)&TargetableTextureFlags |= (uint64_t)ETextureCreateFlags::ShaderResource | (uint64_t)Flags;
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutTargetableTexture, command_list, SizeX, SizeY, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutShaderResourceTexture, command_list, (uint32_t)size.x, (uint32_t)size.y, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    this->render_target = OutTargetableTexture.texture;
    this->ui_target = OutShaderResourceTexture.texture;

    OutShaderResourceTexture.texture = OutTargetableTexture.texture;*/

    m_last_allocate_render_target_return_address = (uintptr_t)_ReturnAddress();
    const auto relative_allocate_render_target_return_address =
        m_last_allocate_render_target_return_address - (uintptr_t)*utility::get_module_within((void*)m_last_allocate_render_target_return_address);

    if (g_framework->is_dx12() && shf_is_current_game()) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf] AllocateRenderTargetTexture summary last_caller={:x}", relative_allocate_render_target_return_address);
    } else if (is_ue_5_1_dx12_backend()) {
        ue51_note_rt_allocation(relative_allocate_render_target_return_address);
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.1][RTChurn] AllocateRenderTargetTexture summary last_caller={:x}", relative_allocate_render_target_return_address);
    } else {
        SPDLOG_INFO("AllocateRenderTargetTexture called from: {:x}", relative_allocate_render_target_return_address);
    }

    // So, if CalculateRenderTargetSize was *never* called before this function
    // that means we have the virtual index of this function wrong, and we must swap the vtable out.
    // also, if this function was called very close to NeedReallocateDepthTexture, that also means
    // the virtual index is wrong, and we must swap the vtable out.
    const auto is_incorrect_vtable = 
        m_last_calculate_render_size_return_address == 0 ||
        m_last_allocate_render_target_return_address - m_last_needs_reallocate_depth_texture_return_address <= 0x200;

    if (is_incorrect_vtable) {
        // oh no this is the wrong vtable!!!! we need to fix it  nOW!!!
        SPDLOG_INFO("AllocateRenderTargetTexture called instead of AllocateDepthTexture! Fixing...");
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return false;
    }

    this->depth_analysis_passed = true;

    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);

    //return true;
}

bool VRRenderTargetManager::AllocateRenderTargetTextures(uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumLayers,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTexture2DRHIRef>& OutTargetableTextures,
    TArray<FTexture2DRHIRef>& OutShaderResourceTextures, uint32_t NumSamples)
{
    SPDLOG_INFO_ONCE("VRRenderTargetManager::AllocateRenderTargetTextures called!");

    // Keep the engine on the deprecated single-texture allocation path for now.
    // UEVR's 5.7 UI separation still depends on analyzing and midhooking the real
    // texture creation sequence that happens after this returns false.
    return false;
}

bool VRRenderTargetManager_418::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint32_t Flags,
        uint32_t TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture, FTexture2DRHIRef& OutShaderResourceTexture,
        uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}

bool VRRenderTargetManager_Special::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}
