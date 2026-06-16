#include <Windows.h>
#include <TlHelp32.h>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include <imgui.h>

#include <sdk/CVar.hpp>
#include <sdk/Globals.hpp>
#include <sdk/Utility.hpp>

#include "Framework.hpp"

#include "../../GameSpecific.hpp"
#include "../../VR.hpp"
#include "../../../utility/Logging.hpp"
#include "OpenXR.hpp"

using namespace nlohmann;

namespace runtimes {
namespace {
constexpr auto FRAME_BEGIN_STUCK_RECOVERY_THRESHOLD = 180u;
constexpr auto FRAME_BEGIN_SKIP_SUMMARY_INTERVAL = std::chrono::minutes(1);
constexpr auto FRAME_SYNCED_SKIP_LOG_INTERVAL = std::chrono::minutes(1);
constexpr auto FOCUSED_FRAME_LOOP_STALE_RECOVERY_AGE = std::chrono::milliseconds(1000);
constexpr auto FOCUSED_SYNCED_NO_BEGIN_RECOVERY_AGE = std::chrono::milliseconds(300);
constexpr auto FOCUSED_FRAME_LOOP_RECOVERY_COOLDOWN = std::chrono::seconds(1);
constexpr auto READY_STATE_STUCK_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto VALID_POSE_PROBE_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto RELAXED_STARTUP_POSE_POST_SUBMIT_WINDOW = std::chrono::seconds(30);
constexpr auto FRAME_TIMING_LOG_INTERVAL = std::chrono::seconds(5);
constexpr auto LONG_WAIT_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto STALE_POSE_SUBMIT_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto STALE_POSE_SKIP_SUMMARY_INTERVAL = std::chrono::minutes(1);
constexpr auto SLOW_POSE_UPDATE_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto SLOW_POSE_UPDATE_LOG_THRESHOLD_MS = 10.0;
constexpr auto STALE_POSE_REFRESH_MIN_AGE_MS = 50LL;
constexpr auto EVERSPACE2_COHERENT_SUBMIT_LOG_INTERVAL = std::chrono::seconds(2);
constexpr auto EVERSPACE2_COHERENT_SUBMIT_SUMMARY_INTERVAL = std::chrono::seconds(5);

struct ScopedOpenXRTiming {
    OpenXR::FrameTimingStats& stats;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

    ~ScopedOpenXRTiming() {
        stats.add(std::chrono::steady_clock::now() - start);
    }
};

int64_t elapsed_ms_since(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point then) {
    if (then.time_since_epoch().count() == 0) {
        return -1;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
}

bool is_older_frame_count(uint32_t frame_count, uint32_t current_frame_count) {
    if (frame_count == 0 || current_frame_count == 0) {
        return false;
    }

    return static_cast<int32_t>(frame_count - current_frame_count) < 0;
}

const char* sync_frame_callsite_name(VRRuntime::SyncFrameCallsite callsite) {
    switch (callsite) {
    case VRRuntime::SyncFrameCallsite::RuntimeFixFrame:
        return "runtime_fix_frame";
    case VRRuntime::SyncFrameCallsite::VRLateOnPresent:
        return "vr_late_on_present";
    case VRRuntime::SyncFrameCallsite::VREarlyRHICommand:
        return "vr_early_rhi_command";
    case VRRuntime::SyncFrameCallsite::VRD3D11InitialSync:
        return "vr_d3d11_initial_sync";
    case VRRuntime::SyncFrameCallsite::VRPostPresentInitialSync:
        return "vr_post_present_initial_sync";
    case VRRuntime::SyncFrameCallsite::VRVeryLatePostPresent:
        return "vr_very_late_post_present";
    case VRRuntime::SyncFrameCallsite::OpenXRSessionReady:
        return "openxr_session_ready";
    case VRRuntime::SyncFrameCallsite::OpenXRBeginFrameRecovery:
        return "openxr_begin_frame_recovery";
    default:
        return "unknown";
    }
}

bool is_projection_component_valid(float value) {
    return std::isfinite(value);
}

bool is_eye_projection_valid(const Vector4f& projection) {
    constexpr auto epsilon = 1.0e-5f;

    return is_projection_component_valid(projection[0]) &&
           is_projection_component_valid(projection[1]) &&
           is_projection_component_valid(projection[2]) &&
           is_projection_component_valid(projection[3]) &&
           projection[0] < -epsilon &&
           projection[1] > epsilon &&
           projection[2] > epsilon &&
           projection[3] < -epsilon;
}

bool is_stalker2_openxr_frame_loop_guarded() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_stalker2_executable_path(*exe_path);
    }();

    return result;
}

bool is_everspace2_executable() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_everspace2_executable_path(*exe_path);
    }();

    return result;
}

std::string format_space_location_flags(XrSpaceLocationFlags flags) {
    std::ostringstream ss;
    ss << "0x" << std::hex << static_cast<uint64_t>(flags) << std::dec << " [";

    bool first = true;
    const auto append = [&](const char* label, XrSpaceLocationFlags bit) {
        if ((flags & bit) == 0) {
            return;
        }

        if (!first) {
            ss << ',';
        }

        first = false;
        ss << label;
    };

    append("pos_valid", XR_SPACE_LOCATION_POSITION_VALID_BIT);
    append("ori_valid", XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    append("pos_tracked", XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
    append("ori_tracked", XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
    ss << ']';
    return ss.str();
}

std::string format_view_state_flags(XrViewStateFlags flags) {
    std::ostringstream ss;
    ss << "0x" << std::hex << static_cast<uint64_t>(flags) << std::dec << " [";

    bool first = true;
    const auto append = [&](const char* label, XrViewStateFlags bit) {
        if ((flags & bit) == 0) {
            return;
        }

        if (!first) {
            ss << ',';
        }

        first = false;
        ss << label;
    };

    append("pos_valid", XR_VIEW_STATE_POSITION_VALID_BIT);
    append("ori_valid", XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    append("pos_tracked", XR_VIEW_STATE_POSITION_TRACKED_BIT);
    append("ori_tracked", XR_VIEW_STATE_ORIENTATION_TRACKED_BIT);
    ss << ']';
    return ss.str();
}

bool has_required_location_validity(XrSpaceLocationFlags flags) {
    constexpr auto required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    return (flags & required) == required;
}

bool has_any_location_tracking(XrSpaceLocationFlags flags) {
    constexpr auto tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return (flags & tracked) != 0;
}

bool has_required_view_validity(XrViewStateFlags flags) {
    constexpr auto required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    return (flags & required) == required;
}

bool has_any_view_tracking(XrViewStateFlags flags) {
    constexpr auto tracked = XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
    return (flags & tracked) != 0;
}

bool should_accept_startup_poses(OpenXR* openxr) {
    const auto pose_flags_acceptable =
        has_required_location_validity(openxr->view_space_location.locationFlags) &&
        has_any_location_tracking(openxr->view_space_location.locationFlags) &&
        has_required_view_validity(openxr->view_state.viewStateFlags) &&
        has_any_view_tracking(openxr->view_state.viewStateFlags) &&
        has_required_view_validity(openxr->stage_view_state.viewStateFlags) &&
        has_any_view_tracking(openxr->stage_view_state.viewStateFlags);

    if (!pose_flags_acceptable) {
        return false;
    }

    if (!openxr->ever_submitted) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto post_submit_age = elapsed_ms_since(now, openxr->last_successful_end_frame);
    const auto in_startup_recovery_window =
        !openxr->got_first_valid_poses &&
        (openxr->session_state == XR_SESSION_STATE_VISIBLE || openxr->session_state == XR_SESSION_STATE_FOCUSED) &&
        post_submit_age >= 0 &&
        std::chrono::milliseconds{post_submit_age} <= RELAXED_STARTUP_POSE_POST_SUBMIT_WINDOW;

    return in_startup_recovery_window;
}

void log_pose_validation_failure(OpenXR* openxr, const char* reason, uint32_t frame_count, XrTime display_time) {
    const auto now = std::chrono::steady_clock::now();

    if (openxr->last_pose_validation_failure_log.time_since_epoch().count() != 0 &&
        now - openxr->last_pose_validation_failure_log < std::chrono::seconds(1))
    {
        return;
    }

    openxr->last_pose_validation_failure_log = now;

    spdlog::warn(
        "[OpenXR] Waiting for first valid poses: {} frame_count={} display_time={} session_state={} frame_synced={} frame_began={} got_first_poses={} view_flags={} stage_view_flags={} view_space_flags={}",
        reason,
        frame_count,
        display_time,
        openxr->get_session_state_string(openxr->session_state),
        openxr->frame_synced,
        openxr->frame_began,
        openxr->got_first_poses,
        format_view_state_flags(openxr->view_state.viewStateFlags),
        format_view_state_flags(openxr->stage_view_state.viewStateFlags),
        format_space_location_flags(openxr->view_space_location.locationFlags)
    );
}

void emit_openxr_state_probes(OpenXR* openxr, const char* context) {
    const auto now = std::chrono::steady_clock::now();

    if (openxr->session_state == XR_SESSION_STATE_READY && openxr->session_ready_since.time_since_epoch().count() != 0 &&
        now - openxr->session_ready_since >= READY_STATE_STUCK_LOG_INTERVAL &&
        (openxr->last_ready_state_probe_log.time_since_epoch().count() == 0 ||
         now - openxr->last_ready_state_probe_log >= READY_STATE_STUCK_LOG_INTERVAL))
    {
        openxr->last_ready_state_probe_log = now;

        spdlog::warn(
            "[OpenXR] Session has remained in READY for {} ms during {}. session_ready={} frame_synced={} frame_began={} got_first_poses={} got_first_valid_poses={}",
            std::chrono::duration_cast<std::chrono::milliseconds>(now - openxr->session_ready_since).count(),
            context,
            openxr->session_ready,
            openxr->frame_synced,
            openxr->frame_began,
            openxr->got_first_poses,
            openxr->got_first_valid_poses
        );
    }

    if (!openxr->got_first_valid_poses &&
        (openxr->last_valid_pose_probe_log.time_since_epoch().count() == 0 ||
         now - openxr->last_valid_pose_probe_log >= VALID_POSE_PROBE_LOG_INTERVAL))
    {
        openxr->last_valid_pose_probe_log = now;

        spdlog::warn(
            "[OpenXR] got_first_valid_poses is still false during {}. session_state={} session_ready={} frame_synced={} frame_began={} got_first_poses={} predictedDisplayTime={} predictedDisplayPeriod={}",
            context,
            openxr->get_session_state_string(openxr->session_state),
            openxr->session_ready,
            openxr->frame_synced,
            openxr->frame_began,
            openxr->got_first_poses,
            openxr->frame_state.predictedDisplayTime,
            openxr->frame_state.predictedDisplayPeriod
        );
    }
}
}

void OpenXR::on_draw_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("OpenXR Options")) {
        if (this->last_applied_resolution_width == 0 || this->last_applied_resolution_height == 0) {
            this->last_applied_resolution_scale = this->resolution_scale->value();
            this->last_applied_resolution_width = this->get_width();
            this->last_applied_resolution_height = this->get_height();
        }

        if (this->resolution_scale->draw("Resolution Scale")) {
            this->resolution_scale_reconfigure_pending = true;
        }

        if (this->resolution_scale_reconfigure_pending && !ImGui::IsItemActive()) {
            this->resolution_scale_reconfigure_pending = false;

            if (auto vr = VR::get(); vr != nullptr) {
                const auto old_scale = this->last_applied_resolution_scale;
                const auto new_scale = this->resolution_scale->value();
                const auto old_width = this->last_applied_resolution_width != 0 ? this->last_applied_resolution_width : this->get_width_for_scale(old_scale);
                const auto old_height = this->last_applied_resolution_height != 0 ? this->last_applied_resolution_height : this->get_height_for_scale(old_scale);
                const auto new_width = this->get_width_for_scale(new_scale);
                const auto new_height = this->get_height_for_scale(new_scale);

                spdlog::info(
                    "[OpenXR] Resolution scale changed {:.3f}->{:.3f} [{}x{}]->[{}x{}]; recreating renderer textures and swapchains",
                    old_scale,
                    new_scale,
                    old_width,
                    old_height,
                    new_width,
                    new_height
                );

                const auto applied_live = vr->on_openxr_resolution_scale_changed(old_width, old_height, new_width, new_height);

                if (applied_live) {
                    this->resolution_scale_live_apply_deferred = false;
                    this->last_applied_resolution_scale = new_scale;
                    this->last_applied_resolution_width = new_width;
                    this->last_applied_resolution_height = new_height;
                } else {
                    this->resolution_scale_live_apply_deferred = true;
                }
            }
        }

        if (this->resolution_scale_live_apply_deferred) {
            ImGui::TextWrapped(
                "Resolution scale is saved, but this engine path cannot safely rebuild OpenXR swapchains live. Reinject/restart to apply it.");
        }

        //this->push_dummy_projection->draw("Virtual Desktop Fix");

        ImGui::Checkbox("Virtual Desktop Fix", &this->push_dummy_projection);

        ImGui::SameLine();

        this->ignore_vd_checks->draw("Ignore Virtual Desktop Checks");

        if (ImGui::TreeNode("Bindings")) {
            display_bindings_editor();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Diagnostics")) {
            ImGui::TextWrapped("Debug perf toggles. Use only to isolate cost; most intentionally degrade output.");
            this->debug_submit_empty_frame->draw("Submit Empty Frame");
            this->debug_skip_scene_copy->draw("Skip Scene Copy");
            this->debug_skip_ui_copy->draw("Skip UI Copy");
            this->debug_disable_depth_submit->draw("Disable Depth Submit");
            this->refresh_stale_pose_before_submit_enabled->draw("Refresh Stale Pose Before Submit");
            ImGui::TreePop();
        }
        
        ImGui::TreePop();
    }
}

void OpenXR::on_system_properties_acquired(const XrSystemProperties& system_properties) {
    spdlog::info("[OpenXR] OpenXR system Name: {}", system_properties.systemName);
    spdlog::info("[OpenXR] OpenXR system Vendor: {}", system_properties.vendorId);
    spdlog::info("[OpenXR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth);
    spdlog::info("[OpenXR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight);
    spdlog::info("[OpenXR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount);
    spdlog::info("[OpenXR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking);
    spdlog::info("[OpenXR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking);

    const auto should_check_vd = !this->ignore_vd_checks->value();

    if (should_check_vd && std::string_view{system_properties.systemName}.find("SteamVR/OpenXR") != std::string_view::npos) try {
        spdlog::info("[OpenXR] Detected SteamVR/OpenXR, checking for Virtual Desktop Streamer...");

        // Now double check that the Virtual Desktop streamer executable is running
        HANDLE hProcessSnap{};
        PROCESSENTRY32 pe32{};
        pe32.dwSize = sizeof(PROCESSENTRY32);

        // Take a snapshot of all processes in the system.
        hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (hProcessSnap == INVALID_HANDLE_VALUE) {
            spdlog::error("[OpenXR] Failed to take a snapshot of all processes in the system.");
            return;
        }

        // Retrieve information about the first process,
        // and exit if unsuccessful
        if (!Process32First(hProcessSnap, &pe32)) {
            spdlog::error("[OpenXR] Failed to retrieve information about the first process.");
            CloseHandle(hProcessSnap); // clean the snapshot object
            return;
        }

        // Now walk the snapshot of processes, and
        // display information about each process in turn
        bool found = false;
        do {
            if (std::string_view{pe32.szExeFile}.find("VirtualDesktop.Streamer.exe") != std::string_view::npos) {
                spdlog::info("[OpenXR] Detected Virtual Desktop, enabling Virtual Desktop fix.");
                this->push_dummy_projection = true;
                found = true;
                break;
            }
        } while (Process32Next(hProcessSnap, &pe32));

        if (!found) {
            spdlog::info("[OpenXR] Did not detect Virtual Desktop, disabling Virtual Desktop fix.");
            this->push_dummy_projection = false;
        }

        CloseHandle(hProcessSnap);
    } catch(...) {
        spdlog::error("[OpenXR] Failed to check if Virtual Desktop is running.");
    }
}

void OpenXR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : this->options) {
        option.config_load(cfg, set_defaults);
    }
}

void OpenXR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : this->options) {
        option.config_save(cfg);
    }
}

void OpenXR::on_pre_render_game_thread(uint32_t frame_count) {
    if (this->is_everspace2_coherent_submit_active()) {
        std::scoped_lock _{this->sync_assignment_mtx};
        auto& pipeline_state = this->pipeline_states[frame_count % OpenXR::QUEUE_SIZE];

        if (pipeline_state.frame_count != frame_count) {
            pipeline_state = {};
            pipeline_state.frame_count = frame_count;
        }

        return;
    }

    this->pipeline_states[frame_count % OpenXR::QUEUE_SIZE].frame_count = frame_count;
}

bool OpenXR::is_everspace2_coherent_submit_active() const {
    return is_everspace2_executable() &&
        g_framework != nullptr &&
        g_framework->get_renderer_type() == Framework::RendererType::D3D12;
}

void OpenXR::set_everspace2_d3d12_submit_active(bool active) {
    if (!this->is_everspace2_coherent_submit_active()) {
        return;
    }

    this->everspace2_d3d12_submit_active.store(active, std::memory_order_release);
}

void OpenXR::log_frame_lifecycle_state(const char* prefix) const {
    spdlog::warn(
        "[OpenXR] {} session_ready={} session_state={} frame_synced={} frame_began={} got_first_sync={} got_first_poses={} got_first_valid_poses={} predictedDisplayTime={} predictedDisplayPeriod={} recoveries={}",
        prefix,
        this->session_ready,
        this->get_session_state_string(this->session_state),
        this->frame_synced,
        this->frame_began,
        this->got_first_sync,
        this->got_first_poses,
        this->got_first_valid_poses,
        this->frame_state.predictedDisplayTime,
        this->frame_state.predictedDisplayPeriod,
        this->forced_frame_recovery_count
    );
}

bool OpenXR::should_trace_frame_flow() const {
    return this->debug_frame_trace->value();
}

int64_t OpenXR::get_pose_update_age_ms(std::chrono::steady_clock::time_point now) const {
    return elapsed_ms_since(now, this->last_successful_pose_update);
}

void OpenXR::trace_wait_frame_success(std::optional<uint32_t> frame_count, SyncFrameCallsite callsite) {
    ++this->last_wait_trace_sequence;
    this->last_wait_trace_frame_count = frame_count.value_or(this->internal_frame_count);
    this->last_wait_trace_callsite = callsite;

    if (!this->should_trace_frame_flow()) {
        return;
    }

    spdlog::info(
        "[OpenXR][trace] wait_success seq={} callsite={} frame_count={} frame_synced={} frame_began={} session={} shouldRender={} displayTime={} displayPeriod={} last_clear_reason={} last_begin_caller={}",
        this->last_wait_trace_sequence,
        sync_frame_callsite_name(callsite),
        this->last_wait_trace_frame_count,
        this->frame_synced,
        this->frame_began,
        this->get_session_state_string(this->session_state),
        this->frame_state.shouldRender,
        this->frame_state.predictedDisplayTime,
        this->frame_state.predictedDisplayPeriod,
        this->last_frame_synced_clear_reason,
        this->last_begin_frame_caller
    );
}

void OpenXR::trace_begin_frame_request(const char* caller) {
    this->last_begin_frame_caller = caller != nullptr ? caller : "unknown";

    if (!this->should_trace_frame_flow()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto wait_age_ms = this->last_successful_wait_frame.time_since_epoch().count() == 0
        ? -1LL
        : std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_successful_wait_frame).count();
    const auto clear_age_ms = this->last_frame_synced_clear_time.time_since_epoch().count() == 0
        ? -1LL
        : std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_frame_synced_clear_time).count();

    spdlog::info(
        "[OpenXR][trace] begin_frame_request caller={} frame_synced={} frame_began={} session={} got_first_poses={} got_first_valid_poses={} last_wait_seq={} last_wait_callsite={} last_wait_frame_count={} wait_age_ms={} last_clear_reason={} clear_age_ms={} displayTime={} displayPeriod={}",
        this->last_begin_frame_caller,
        this->frame_synced,
        this->frame_began,
        this->get_session_state_string(this->session_state),
        this->got_first_poses,
        this->got_first_valid_poses,
        this->last_wait_trace_sequence,
        sync_frame_callsite_name(this->last_wait_trace_callsite),
        this->last_wait_trace_frame_count,
        wait_age_ms,
        this->last_frame_synced_clear_reason,
        clear_age_ms,
        this->frame_state.predictedDisplayTime,
        this->frame_state.predictedDisplayPeriod
    );
}

void OpenXR::clear_frame_synced(const char* reason) {
    this->frame_synced = false;
    this->last_frame_synced_clear_reason = reason != nullptr ? reason : "unknown";
    this->last_frame_synced_clear_time = std::chrono::steady_clock::now();

    if (!this->should_trace_frame_flow()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto wait_age_ms = this->last_successful_wait_frame.time_since_epoch().count() == 0
        ? -1LL
        : std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_successful_wait_frame).count();

    spdlog::info(
        "[OpenXR][trace] clear_frame_synced reason={} was_synced={} frame_began={} session={} last_wait_seq={} last_wait_callsite={} last_wait_frame_count={} wait_age_ms={} last_begin_caller={} displayTime={} displayPeriod={}",
        reason != nullptr ? reason : "unknown",
        this->frame_synced,
        this->frame_began,
        this->get_session_state_string(this->session_state),
        this->last_wait_trace_sequence,
        sync_frame_callsite_name(this->last_wait_trace_callsite),
        this->last_wait_trace_frame_count,
        wait_age_ms,
        this->last_begin_frame_caller,
        this->frame_state.predictedDisplayTime,
        this->frame_state.predictedDisplayPeriod
    );
}

VRRuntime::Error OpenXR::refresh_stale_pose_before_submit(uint32_t frame_count, const char* caller) {
    if (this->is_everspace2_coherent_submit_active()) {
        SPDLOG_INFO_ONCE(
            "[Everspace2][OpenXR][coherent-submit] Legacy stale-pose refresh is bypassed; "
            "submit-time coherent capture owns refresh without regressing internal frame state");
        return VRRuntime::Error::SUCCESS;
    }

    if (!this->refresh_stale_pose_before_submit_enabled->value()) {
        return VRRuntime::Error::SUCCESS;
    }

    if (!this->can_run_frame_loop() || this->session_state != XR_SESSION_STATE_FOCUSED ||
        !this->frame_synced || !this->frame_began || !this->got_first_poses)
    {
        return VRRuntime::Error::SUCCESS;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto pose_age_ms = this->get_pose_update_age_ms(now);
    const auto predicted_period_ms = this->frame_state.predictedDisplayPeriod > 0
        ? static_cast<int64_t>(this->frame_state.predictedDisplayPeriod / 1000000)
        : 0LL;
    const auto stale_threshold_ms = std::max<int64_t>(STALE_POSE_REFRESH_MIN_AGE_MS, predicted_period_ms * 3);

    if (pose_age_ms >= 0 && pose_age_ms < stale_threshold_ms) {
        return VRRuntime::Error::SUCCESS;
    }

    uint32_t current_internal_frame_count{};
    {
        std::scoped_lock _{this->sync_assignment_mtx};
        current_internal_frame_count = this->internal_frame_count;
    }

    if (is_older_frame_count(frame_count, current_internal_frame_count)) {
        const auto first_log = this->last_stale_pose_skip_log.time_since_epoch().count() == 0;

        if (first_log) {
            this->last_stale_pose_skip_log = now;
            spdlog::warn(
                "[OpenXR][stale-pose] skipped refresh for older submit frame to avoid internal frame regression caller={} frame_count={} internal_frame_count={} pose_age_ms={} threshold_ms={} session={} shouldRender={}",
                caller != nullptr ? caller : "unknown",
                frame_count,
                current_internal_frame_count,
                pose_age_ms,
                stale_threshold_ms,
                this->get_session_state_string(this->session_state),
                this->frame_state.shouldRender
            );
        } else {
            ++this->stale_pose_skip_suppressed_count;

            if (now - this->last_stale_pose_skip_log >= STALE_POSE_SKIP_SUMMARY_INTERVAL) {
                spdlog::warn(
                    "[OpenXR][stale-pose] suppressed repeated older-frame refresh skips count={} caller={} frame_count={} internal_frame_count={} pose_age_ms={} threshold_ms={} session={} shouldRender={}",
                    this->stale_pose_skip_suppressed_count,
                    caller != nullptr ? caller : "unknown",
                    frame_count,
                    current_internal_frame_count,
                    pose_age_ms,
                    stale_threshold_ms,
                    this->get_session_state_string(this->session_state),
                    this->frame_state.shouldRender
                );

                this->last_stale_pose_skip_log = now;
                this->stale_pose_skip_suppressed_count = 0;
            }
        }

        return VRRuntime::Error::SUCCESS;
    }

    ++this->stale_pose_refresh_attempt_count;

    const auto wait_age_ms = elapsed_ms_since(now, this->last_successful_wait_frame);
    const auto begin_age_ms = elapsed_ms_since(now, this->last_successful_begin_frame);
    const auto end_age_ms = elapsed_ms_since(now, this->last_successful_end_frame);
    const auto result = this->update_poses(false, frame_count);
    const auto after = std::chrono::steady_clock::now();
    const auto pose_age_after_ms = this->get_pose_update_age_ms(after);

    if (result == VRRuntime::Error::SUCCESS) {
        ++this->stale_pose_refresh_success_count;
    } else {
        ++this->stale_pose_refresh_failed_count;
    }

    if (result != VRRuntime::Error::SUCCESS ||
        this->last_stale_pose_submit_log.time_since_epoch().count() == 0 ||
        after - this->last_stale_pose_submit_log >= STALE_POSE_SUBMIT_LOG_INTERVAL)
    {
        this->last_stale_pose_submit_log = after;
        spdlog::warn(
            "[OpenXR][stale-pose] submit refresh caller={} frame_count={} result={} pose_age_before_ms={} pose_age_after_ms={} threshold_ms={} session={} shouldRender={} wait_age_ms={} begin_age_ms={} end_age_ms={} attempts={} success={} failed={}",
            caller != nullptr ? caller : "unknown",
            frame_count,
            static_cast<int64_t>(result),
            pose_age_ms,
            pose_age_after_ms,
            stale_threshold_ms,
            this->get_session_state_string(this->session_state),
            this->frame_state.shouldRender,
            wait_age_ms,
            begin_age_ms,
            end_age_ms,
            this->stale_pose_refresh_attempt_count,
            this->stale_pose_refresh_success_count,
            this->stale_pose_refresh_failed_count
        );
    }

    return result;
}

bool OpenXR::recover_focused_stale_frame_loop(const char* caller) {
    if (this->session == XR_NULL_HANDLE ||
        this->session_state != XR_SESSION_STATE_FOCUSED ||
        !this->session_ready ||
        !this->ever_submitted ||
        !this->got_first_poses ||
        !this->got_first_valid_poses ||
        !this->frame_synced)
    {
        return false;
    }

    if (this->is_everspace2_coherent_submit_active()) {
        if (!this->everspace2_has_real_projection_submit.load(std::memory_order_acquire)) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Everspace2][OpenXR][coherent-submit] Preserving startup synchronized frame; "
                "no successful projection-layer submit has occurred yet caller={}",
                caller != nullptr ? caller : "unknown");
            return false;
        }

        if (this->everspace2_d3d12_submit_active.load(std::memory_order_acquire)) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Everspace2][OpenXR][coherent-submit] Suppressing stale-frame recovery while "
                "the D3D12 submit path is active caller={}",
                caller != nullptr ? caller : "unknown");
            return false;
        }
    }

    const auto now = std::chrono::steady_clock::now();

    if (this->last_focused_frame_loop_recovery.time_since_epoch().count() != 0 &&
        now - this->last_focused_frame_loop_recovery < FOCUSED_FRAME_LOOP_RECOVERY_COOLDOWN)
    {
        return false;
    }

    const auto wait_age_ms = elapsed_ms_since(now, this->last_successful_wait_frame);
    const auto begin_age_ms = elapsed_ms_since(now, this->last_successful_begin_frame);
    const auto end_age_ms = elapsed_ms_since(now, this->last_successful_end_frame);

    if (wait_age_ms < 0 || begin_age_ms < 0 || end_age_ms < 0) {
        return false;
    }

    const auto recovery_age = this->frame_began
        ? FOCUSED_FRAME_LOOP_STALE_RECOVERY_AGE
        : FOCUSED_SYNCED_NO_BEGIN_RECOVERY_AGE;

    if (std::chrono::milliseconds{wait_age_ms} < recovery_age ||
        std::chrono::milliseconds{begin_age_ms} < recovery_age ||
        std::chrono::milliseconds{end_age_ms} < recovery_age)
    {
        return false;
    }

    if (!this->frame_began &&
        is_stalker2_openxr_frame_loop_guarded() &&
        caller != nullptr &&
        std::string_view{caller} == "runtime_fix_frame")
    {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Stalker2][OpenXR] Preserving stale synchronized frame for imminent D3D12 submit wait_age={}ms begin_age={}ms end_age={}ms recoveries={}",
            wait_age_ms,
            begin_age_ms,
            end_age_ms,
            this->focused_frame_loop_recovery_count);
        return false;
    }

    spdlog::warn(
        "[OpenXR] Recovering focused stale frame loop caller={} wait_age={}ms begin_age={}ms end_age={}ms frame_synced={} frame_began={} recoveries={}",
        caller != nullptr ? caller : "unknown",
        wait_age_ms,
        begin_age_ms,
        end_age_ms,
        this->frame_synced,
        this->frame_began,
        this->focused_frame_loop_recovery_count
    );

    if (this->frame_began) {
        this->recover_wedged_frame("focused stale frame loop");
    } else {
        // xrWaitFrame succeeded but no render submit reached xrBeginFrame for too long.
        // Preserve OpenXR call order by opening and ending an empty frame instead of
        // locally clearing frame_synced and risking the next xrWaitFrame being invalid.
        XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        this->last_begin_frame_caller = "focused_stale_synced_no_begin";

        this->begin_profile();
        const auto begin_frame_start = std::chrono::steady_clock::now();
        const auto begin_result = xrBeginFrame(this->session, &frame_begin_info);
        this->begin_frame_timing.add(std::chrono::steady_clock::now() - begin_frame_start);
        this->end_profile("xrBeginFrame");

        if (begin_result != XR_SUCCESS && begin_result != XR_FRAME_DISCARDED) {
            spdlog::warn(
                "[OpenXR] Failed to open focused stale empty recovery frame caller={} result={}",
                caller != nullptr ? caller : "unknown",
                this->get_result_string(begin_result));
            return false;
        }

        this->frame_began = true;
        this->last_successful_begin_frame = std::chrono::steady_clock::now();

        XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
        frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
        frame_end_info.environmentBlendMode = this->blend_mode;
        frame_end_info.layerCount = 0;
        frame_end_info.layers = nullptr;

        const auto end_frame_start = std::chrono::steady_clock::now();
        const auto end_result = xrEndFrame(this->session, &frame_end_info);
        this->end_frame_timing.add(std::chrono::steady_clock::now() - end_frame_start);

        if (end_result != XR_SUCCESS) {
            spdlog::warn(
                "[OpenXR] Focused stale empty recovery xrEndFrame failed caller={} result={}",
                caller != nullptr ? caller : "unknown",
                this->get_result_string(end_result));
        } else {
            this->ever_submitted = true;
            this->last_successful_end_frame = std::chrono::steady_clock::now();
            spdlog::warn("[OpenXR] Closed focused stale synchronized frame without layers");
        }

        this->frame_began = false;
        this->clear_frame_synced("focused_stale_synced_no_begin");
        ++this->forced_frame_recovery_count;
    }

    this->last_focused_frame_loop_recovery = now;
    ++this->focused_frame_loop_recovery_count;
    this->frame_began_skip_streak = 0;
    this->frame_synced_skip_streak = 0;

    return true;
}

XrResult OpenXR::recover_wedged_frame(const char* reason) {
    if (!this->frame_began || this->session == XR_NULL_HANDLE) {
        return XR_SUCCESS;
    }

    this->log_frame_lifecycle_state(reason);

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = 0;
    frame_end_info.layers = nullptr;

    const auto result = xrEndFrame(this->session, &frame_end_info);

    if (result != XR_SUCCESS) {
        spdlog::error("[OpenXR] Recovery xrEndFrame failed ({}): {}", reason, this->get_result_string(result));
    } else {
        spdlog::warn("[OpenXR] Recovered wedged frame with empty xrEndFrame ({})", reason);
    }

    this->frame_began = false;
    this->clear_frame_synced("recover_wedged_frame");
    ++this->forced_frame_recovery_count;

    return result;
}

bool OpenXR::close_synced_frame_without_layers(const char* reason) {
    std::scoped_lock _{sync_mtx};

    if (this->session == XR_NULL_HANDLE || !this->session_ready || !this->frame_synced) {
        return false;
    }

    const auto safe_reason = reason != nullptr ? reason : "unknown";

    // If xrWaitFrame succeeded, keep OpenXR call order intact: open an empty
    // frame and end it instead of locally clearing frame_synced.
    if (!this->frame_began) {
        this->last_begin_frame_caller = safe_reason;
        XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};

        this->begin_profile();
        const auto begin_frame_start = std::chrono::steady_clock::now();
        const auto begin_result = xrBeginFrame(this->session, &frame_begin_info);
        this->begin_frame_timing.add(std::chrono::steady_clock::now() - begin_frame_start);
        this->end_profile("xrBeginFrame");

        if (begin_result != XR_SUCCESS && begin_result != XR_FRAME_DISCARDED) {
            spdlog::warn(
                "[OpenXR] Failed to open empty recovery frame ({}): {}",
                safe_reason,
                this->get_result_string(begin_result));
            return false;
        }

        this->frame_began = true;
        this->last_successful_begin_frame = std::chrono::steady_clock::now();
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = 0;
    frame_end_info.layers = nullptr;

    const auto end_frame_start = std::chrono::steady_clock::now();
    const auto result = xrEndFrame(this->session, &frame_end_info);
    this->end_frame_timing.add(std::chrono::steady_clock::now() - end_frame_start);

    if (result != XR_SUCCESS) {
        spdlog::warn(
            "[OpenXR] Empty recovery xrEndFrame failed ({}): {}",
            safe_reason,
            this->get_result_string(result));
    } else {
        this->ever_submitted = true;
        this->last_successful_end_frame = std::chrono::steady_clock::now();
        SPDLOG_WARNING_EVERY_N_SEC(
            1,
            "[OpenXR] Closed synchronized frame without layers ({})",
            safe_reason);
    }

    this->frame_began = false;
    this->clear_frame_synced(safe_reason);
    ++this->forced_frame_recovery_count;

    return true;
}

void OpenXR::prepare_resolution_scale_reconfigure(const char* reason) {
    const auto safe_reason = reason != nullptr ? reason : "resolution_scale_reconfigure";
    const auto had_synced = this->frame_synced;
    const auto had_began = this->frame_began;
    bool closed_pending_frame = false;

    // A resolution/swapchain rebuild while xrWaitFrame has already returned can
    // leave the runtime pacing the next frames badly. Preserve OpenXR call order
    // by closing that pending frame before the renderer destroys/recreates targets.
    if (had_began) {
        closed_pending_frame = this->recover_wedged_frame(safe_reason) == XR_SUCCESS;
    } else if (had_synced) {
        closed_pending_frame = this->close_synced_frame_without_layers(safe_reason);
    }

    {
        std::scoped_lock _{sync_mtx};
        this->frame_synced_skip_streak = 0;
        this->frame_began_skip_streak = 0;
        this->frame_began_skip_suppressed_count = 0;
        this->long_wait_suppressed_count = 0;
        this->long_wait_max_suppressed_ms = 0.0;
        this->last_long_wait_log = {};
        this->last_frame_timing_log = {};
        this->wait_frame_timing.reset();
        this->begin_frame_timing.reset();
        this->end_frame_timing.reset();
        for (auto& timing : this->wait_frame_callsite_timing) {
            timing.reset();
        }
    }

    spdlog::info(
        "[OpenXR] Prepared frame loop for resolution-scale reconfigure reason={} had_synced={} had_began={} closed_pending={} session={} ready={}",
        safe_reason,
        had_synced,
        had_began,
        closed_pending_frame,
        this->get_session_state_string(this->session_state),
        this->session_ready);
}

VRRuntime::Error OpenXR::synchronize_frame(std::optional<uint32_t> frame_count, SyncFrameCallsite callsite) {
    std::scoped_lock _{sync_mtx};

    if (this->recover_focused_stale_frame_loop(sync_frame_callsite_name(callsite))) {
        return VRRuntime::Error::UNSPECIFIED;
    }

    if (!this->session_ready || this->frame_began) {
        if (this->frame_began) {
            ++this->frame_began_skip_streak;

            const auto now = std::chrono::steady_clock::now();

            const auto first_log = this->last_frame_began_log.time_since_epoch().count() == 0;

            if (first_log) {
                this->last_frame_began_log = now;
                this->frame_began_skip_suppressed_count = 0;
                this->log_frame_lifecycle_state("Frame already began, skipping xrWaitFrame call");
            } else {
                ++this->frame_began_skip_suppressed_count;

                if (now - this->last_frame_began_log >= FRAME_BEGIN_SKIP_SUMMARY_INTERVAL) {
                    spdlog::warn(
                        "[OpenXR] suppressed repeated frame-began xrWaitFrame skip logs count={} streak={} session={} frame_synced={} last_begin_caller={} recoveries={}",
                        this->frame_began_skip_suppressed_count,
                        this->frame_began_skip_streak,
                        this->get_session_state_string(this->session_state),
                        this->frame_synced,
                        this->last_begin_frame_caller,
                        this->forced_frame_recovery_count
                    );

                    this->last_frame_began_log = now;
                    this->frame_began_skip_suppressed_count = 0;
                }
            }

            if (this->frame_began_skip_streak >= FRAME_BEGIN_STUCK_RECOVERY_THRESHOLD) {
                this->recover_wedged_frame("synchronize_frame stuck with frame_began");
                this->frame_began_skip_streak = 0;
            }
        }
        
        return VRRuntime::Error::UNSPECIFIED;
    }

    this->frame_began_skip_streak = 0;

    if (this->frame_synced) {
        ++this->frame_synced_skip_streak;

        const auto now = std::chrono::steady_clock::now();

        if (this->last_frame_synced_skip_log.time_since_epoch().count() == 0 || now - this->last_frame_synced_skip_log >= FRAME_SYNCED_SKIP_LOG_INTERVAL) {
            this->last_frame_synced_skip_log = now;
            spdlog::info(
                "[OpenXR] Frame already synchronized, skipping xrWaitFrame call. repeat_count={} session={} last_clear_reason={} last_begin_caller={}",
                this->frame_synced_skip_streak,
                this->get_session_state_string(this->session_state),
                this->last_frame_synced_clear_reason,
                this->last_begin_frame_caller
            );
            this->frame_synced_skip_streak = 0;
        }

        return VRRuntime::Error::SUCCESS;
    }
    this->begin_profile();

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState local_frame_state{XR_TYPE_FRAME_STATE};
    const auto wait_frame_start = std::chrono::steady_clock::now();
    auto result = xrWaitFrame(this->session, &frame_wait_info, &local_frame_state);
    const auto wait_frame_end = std::chrono::steady_clock::now();
    const auto wait_frame_duration = wait_frame_end - wait_frame_start;
    this->wait_frame_timing.add(wait_frame_duration);

    const auto callsite_index = (size_t)callsite;
    if (callsite_index < this->wait_frame_callsite_timing.size()) {
        this->wait_frame_callsite_timing[callsite_index].add(wait_frame_duration);
    }

    const auto wait_frame_ms = std::chrono::duration<double, std::milli>{wait_frame_duration}.count();
    if (wait_frame_ms >= 100.0) {
        const auto should_log =
            this->last_long_wait_log.time_since_epoch().count() == 0 ||
            wait_frame_end - this->last_long_wait_log >= LONG_WAIT_LOG_INTERVAL;

        if (should_log) {
            const auto suppressed = this->long_wait_suppressed_count;
            const auto suppressed_max = this->long_wait_max_suppressed_ms;
            this->last_long_wait_log = wait_frame_end;
            this->long_wait_suppressed_count = 0;
            this->long_wait_max_suppressed_ms = 0.0;

            spdlog::warn(
                "[OpenXR][wait-profiler] xrWaitFrame took {:.2f}ms callsite={} frame_count={} session={} synced={} began={} first_poses={} valid_poses={} predictedDisplayTime={} predictedDisplayPeriod={} suppressed={} suppressed_max_ms={:.2f}",
                wait_frame_ms,
                sync_frame_callsite_name(callsite),
                frame_count.value_or(this->internal_frame_count),
                this->get_session_state_string(this->session_state),
                this->frame_synced,
                this->frame_began,
                this->got_first_poses,
                this->got_first_valid_poses,
                local_frame_state.predictedDisplayTime,
                local_frame_state.predictedDisplayPeriod,
                suppressed,
                suppressed_max
            );
        } else {
            ++this->long_wait_suppressed_count;
            if (wait_frame_ms > this->long_wait_max_suppressed_ms) {
                this->long_wait_max_suppressed_ms = wait_frame_ms;
            }
        }
    }

    this->end_profile("xrWaitFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrWaitFrame failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    } else {
        this->last_successful_wait_frame = std::chrono::steady_clock::now();
        std::scoped_lock __{this->sync_assignment_mtx};

        // Correct for invalid predictedDisplayPeriod values. Seen on SteamVR OpenXR
        if (local_frame_state.predictedDisplayPeriod >= 1000000000) {
            spdlog::error("[VR] xrWaitFrame returned invalid predictedDisplayPeriod: {}", local_frame_state.predictedDisplayPeriod);
            local_frame_state.predictedDisplayPeriod = 0;
        }

        this->frame_state = local_frame_state;

        // Initialize all the existing frame states if they aren't already so we don't get some random error when calling xrEndFrame
        for (auto& pipeline_state : this->pipeline_states) {
            if (pipeline_state.frame_state.predictedDisplayTime <= 0) {
                pipeline_state.frame_state = this->frame_state;
            }
        }

        if (!frame_count && this->last_submit_state.frame_count > 0) {
            const auto next_frame = (this->last_submit_state.frame_count + 1) % OpenXR::QUEUE_SIZE;
            this->pipeline_states[next_frame].frame_state = this->frame_state;
            this->pipeline_states[next_frame].prev_frame_count = this->last_submit_state.frame_count;

            if (VR::get()->is_using_afr()) {
                const auto afr_frame = (next_frame + 1) % OpenXR::QUEUE_SIZE;
                this->pipeline_states[afr_frame].frame_state = this->frame_state;
                this->pipeline_states[afr_frame].prev_frame_count = next_frame;
            }
        } else if (frame_count) {
            this->pipeline_states[*frame_count % OpenXR::QUEUE_SIZE].frame_state = this->frame_state;
            this->pipeline_states[*frame_count % OpenXR::QUEUE_SIZE].prev_frame_count = *frame_count;

            if (VR::get()->is_using_afr()) {
                this->pipeline_states[(*frame_count + 1) % OpenXR::QUEUE_SIZE].frame_state = this->frame_state;
                this->pipeline_states[(*frame_count + 1) % OpenXR::QUEUE_SIZE].prev_frame_count = *frame_count;
            }
        }

        this->got_first_sync = true;
        this->frame_synced = true;
        this->should_update_eye_matrices = true;
        this->trace_wait_frame_success(frame_count, callsite);
    }
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_poses(bool from_view_extensions, uint32_t frame_count) {
    //std::scoped_lock ___{sync_mtx};
    std::unique_lock<std::mutex> everspace2_pose_lock{this->everspace2_pose_capture_mtx, std::defer_lock};
    if (this->is_everspace2_coherent_submit_active()) {
        everspace2_pose_lock.lock();
    }

    std::scoped_lock _{ this->sync_assignment_mtx };

    if (!this->session_ready) {
        return VRRuntime::Error::SUCCESS;
    }

    ScopedOpenXRTiming pose_timing{this->pose_update_timing};

    const auto& vr = VR::get();

    /*if (!this->needs_pose_update) {
        return VRRuntime::Error::SUCCESS;
    }*/

    this->view_state = {XR_TYPE_VIEW_STATE};
    this->stage_view_state = {XR_TYPE_VIEW_STATE};

    uint32_t view_count{};

    bool should_enqueue = false;

    if (frame_count == 0) {
        frame_count = ++this->internal_frame_count;
        should_enqueue = true;
    } else {
        this->internal_frame_count = frame_count;
    }

    const auto pose_update_start = std::chrono::steady_clock::now();
    const auto pose_age_before_ms = elapsed_ms_since(pose_update_start, this->last_successful_pose_update);
    double view_locate_ms = 0.0;
    double stage_locate_ms = 0.0;
    double space_locate_ms = 0.0;

    ++this->pose_update_call_count;
    if (from_view_extensions) {
        ++this->pose_update_view_extension_count;
    } else {
        ++this->pose_update_non_view_extension_count;
    }

    this->last_pose_update_frame_count = frame_count;
    this->last_pose_update_from_view_extensions = from_view_extensions;

    const auto finish_pose_update = [&](VRRuntime::Error result) -> VRRuntime::Error {
        const auto now = std::chrono::steady_clock::now();
        const auto pose_update_ms = std::chrono::duration<double, std::milli>{now - pose_update_start}.count();
        this->last_pose_update_result = static_cast<int64_t>(result);
        this->last_pose_update_ms = pose_update_ms;
        this->last_pose_view_locate_ms = view_locate_ms;
        this->last_pose_stage_locate_ms = stage_locate_ms;
        this->last_pose_space_locate_ms = space_locate_ms;

        if (pose_update_ms >= SLOW_POSE_UPDATE_LOG_THRESHOLD_MS &&
            (this->last_slow_pose_update_log.time_since_epoch().count() == 0 ||
             now - this->last_slow_pose_update_log >= SLOW_POSE_UPDATE_LOG_INTERVAL))
        {
            this->last_slow_pose_update_log = now;
            spdlog::warn(
                "[OpenXR][pose-profiler] update_poses took {:.2f}ms source={} frame_count={} result={} view_locate_ms={:.2f} stage_locate_ms={:.2f} space_locate_ms={:.2f} session={} shouldRender={} displayTime={} displayPeriod={} pose_age_before_ms={}",
                pose_update_ms,
                from_view_extensions ? "view_extension" : "runtime",
                frame_count,
                static_cast<int64_t>(result),
                view_locate_ms,
                stage_locate_ms,
                space_locate_ms,
                this->get_session_state_string(this->session_state),
                this->frame_state.shouldRender,
                this->frame_state.predictedDisplayTime,
                this->frame_state.predictedDisplayPeriod,
                pose_age_before_ms
            );
        }

        return result;
    };

    auto& pipeline_state = this->pipeline_states[frame_count % OpenXR::QUEUE_SIZE];

    if (this->is_everspace2_coherent_submit_active()) {
        if (pipeline_state.frame_count != frame_count) {
            pipeline_state = {};
            pipeline_state.frame_count = frame_count;
        }

        pipeline_state.coherent_complete = false;
        pipeline_state.coherent_matches_render = false;
    }

    if (pipeline_state.frame_state.predictedDisplayTime <= 1000) {
        pipeline_state.frame_state = this->frame_state;
        pipeline_state.prev_frame_count = frame_count;
    }

    // Only signal that we got the first valid pose if the display time becomes a sane value
    if (!this->got_first_valid_poses) {
        // Seen on VDXR
        if (pipeline_state.frame_state.predictedDisplayTime <= pipeline_state.frame_state.predictedDisplayPeriod) {
            log_pose_validation_failure(this, "predicted display time <= predicted display period", frame_count, pipeline_state.frame_state.predictedDisplayTime);
            return finish_pose_update(VRRuntime::Error::SUCCESS);
        }

        // Seen on VDXR. If for some reason the above if statement doesn't work, this will catch it.
        if (pipeline_state.frame_state.predictedDisplayTime == 11111111) {
            log_pose_validation_failure(this, "predicted display time sentinel 11111111", frame_count, pipeline_state.frame_state.predictedDisplayTime);
            return finish_pose_update(VRRuntime::Error::SUCCESS);
        }
    }
    
    // Not a sane value
    if (pipeline_state.frame_state.predictedDisplayTime <= 1000) {
        if (!this->got_first_valid_poses) {
            log_pose_validation_failure(this, "predicted display time <= 1000", frame_count, pipeline_state.frame_state.predictedDisplayTime);
        }
        return finish_pose_update(VRRuntime::Error::SUCCESS);
    }

    // Pre-emptively update the frame state
    // this will allow us to get near-correct estimated poses for this frame
    // before xrWaitFrame has finished on the previous frame
    // This is usually the case *most* of the time, but sometimes the game thread will
    // actually execute after xrWaitFrame has finished, so we don't need to adjust the frame state
    if (pipeline_state.prev_frame_count + 1 < frame_count) {
        // Update the main frame state's predicted display time because it's possible that
        // the game thread can run multiple times before xrWaitFrame has finished
        // So maybe we will end up predicting more than one period ahead at times
        this->frame_state.predictedDisplayTime += this->frame_state.predictedDisplayPeriod;
        pipeline_state.frame_state = this->frame_state;
    }

    const auto display_time = pipeline_state.frame_state.predictedDisplayTime + (XrDuration)(pipeline_state.frame_state.predictedDisplayPeriod * this->prediction_scale);

    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->view_space;

    const auto view_locate_start = std::chrono::steady_clock::now();
    auto result = xrLocateViews(this->session, &view_locate_info, &this->view_state, (uint32_t)this->views.size(), &view_count, this->views.data());
    view_locate_ms = std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - view_locate_start}.count();

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for view space failed: {}", this->get_result_string(result));
        return finish_pose_update((VRRuntime::Error)result);
    }

    view_locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    const auto stage_locate_start = std::chrono::steady_clock::now();
    result = xrLocateViews(this->session, &view_locate_info, &this->stage_view_state, (uint32_t)this->stage_views.size(), &view_count, this->stage_views.data());
    stage_locate_ms = std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - stage_locate_start}.count();

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for stage space failed: {}", this->get_result_string(result));
        return finish_pose_update((VRRuntime::Error)result);
    }

    if (!this->is_everspace2_coherent_submit_active()) {
        pipeline_state.stage_views = this->stage_views;
    }
    //this->frame_state_queue[frame_count % this->frame_state_queue.size()] = this->frame_state;
    
    if (should_enqueue) {
        enqueue_render_poses_unsafe(frame_count); // because we've already locked the mutexes.
    }

    const auto space_locate_start = std::chrono::steady_clock::now();
    result = xrLocateSpace(this->view_space, this->stage_space, display_time, &this->view_space_location);
    space_locate_ms = std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - space_locate_start}.count();

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateSpace for view space failed: {}", this->get_result_string(result));

        if (result == XR_ERROR_TIME_INVALID) {
            spdlog::info("[VR] Time: {}", display_time);
        }

        return finish_pose_update((VRRuntime::Error)result);
    }

    pipeline_state.view_space_location = this->view_space_location;

    if (this->is_everspace2_coherent_submit_active()) {
        pipeline_state.stage_views = this->stage_views;
        pipeline_state.coherent_source_frame_count = frame_count;
        pipeline_state.coherent_sequence = ++this->everspace2_snapshot_sequence;
        pipeline_state.coherent_display_time = display_time;
        pipeline_state.coherent_capture_time = std::chrono::steady_clock::now();
        pipeline_state.coherent_complete = !pipeline_state.stage_views.empty();
        pipeline_state.coherent_matches_render = pipeline_state.coherent_complete;
    }

    for (auto i = 0; i < this->hands.size(); ++i) {
        auto& hand = this->hands[i];
        hand.aim_location.next = &hand.aim_velocity;
        result = xrLocateSpace(hand.aim_space, this->stage_space, display_time, &hand.aim_location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return finish_pose_update((VRRuntime::Error)result);
        }

        auto orientation_aim = runtimes::OpenXR::to_glm(hand.aim_location.pose.orientation);

        if (const auto pitch = vr->get_controller_pitch_offset(); pitch != 0.0f) {
            orientation_aim = glm::rotate(orientation_aim, glm::radians(pitch), Vector3f{1.0f, 0.0f, 0.0f});
        }

        this->aim_matrices[i] = Matrix4x4f{orientation_aim};
        this->aim_matrices[i][3] = Vector4f{*(Vector3f*)&hand.aim_location.pose.position, 1.0f};

        hand.grip_location.next = &hand.grip_velocity;
        result = xrLocateSpace(hand.grip_space, this->stage_space, display_time, &hand.grip_location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return finish_pose_update((VRRuntime::Error)result);
        }

        auto orientation_grip = runtimes::OpenXR::to_glm(hand.grip_location.pose.orientation);

        if (const auto pitch = vr->get_controller_pitch_offset(); pitch != 0.0f) {
            orientation_grip = glm::rotate(orientation_grip, glm::radians(pitch), Vector3f{1.0f, 0.0f, 0.0f});
        }

        this->grip_matrices[i] = Matrix4x4f{orientation_grip};
        this->grip_matrices[i][3] = Vector4f{*(Vector3f*)&hand.grip_location.pose.position, 1.0f};
    }

    if (!this->got_first_valid_poses) {
        constexpr auto wanted_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const auto fully_tracked = (this->view_space_location.locationFlags & wanted_flags) == wanted_flags;
        const auto startup_acceptable = !fully_tracked && should_accept_startup_poses(this);

        this->got_first_valid_poses = fully_tracked || startup_acceptable;

        if (this->got_first_valid_poses) {
            this->accepted_relaxed_startup_poses = startup_acceptable;
            this->last_valid_pose_probe_log = {};
            this->last_pose_validation_failure_log = {};
            if (startup_acceptable) {
                spdlog::warn(
                    "[OpenXR] Accepting startup poses without full tracked bits: display_time={} frame_count={} view_flags={} stage_view_flags={} view_space_flags={}",
                    display_time,
                    frame_count,
                    format_view_state_flags(this->view_state.viewStateFlags),
                    format_view_state_flags(this->stage_view_state.viewStateFlags),
                    format_space_location_flags(this->view_space_location.locationFlags)
                );
            }

            spdlog::info("[OpenXR] Got first valid poses at time: {} {} {}", display_time, pipeline_state.frame_state.predictedDisplayTime, pipeline_state.frame_state.predictedDisplayPeriod);
        } else {
            log_pose_validation_failure(this, "view space location missing required valid/tracked bits", frame_count, display_time);
        }
    }

    if (!this->got_first_poses) {
        spdlog::info(
            "[OpenXR] Got first poses at time: {} frame_count={} view_flags={} stage_view_flags={}",
            display_time,
            frame_count,
            format_view_state_flags(this->view_state.viewStateFlags),
            format_view_state_flags(this->stage_view_state.viewStateFlags)
        );
    }

    this->got_first_poses = true;
    this->needs_pose_update = false;
    this->last_successful_pose_update = std::chrono::steady_clock::now();
    return finish_pose_update(VRRuntime::Error::SUCCESS);
}

VRRuntime::Error OpenXR::update_render_target_size() {
    uint32_t view_count{};
    auto result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, 0, &view_count, nullptr); 
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    this->view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, view_count, &view_count, this->view_configs.data());
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenXR::get_width_for_scale(float scale) const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * scale * eye_width_adjustment);
}

uint32_t OpenXR::get_width() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    const auto use_last_applied =
        (this->resolution_scale_reconfigure_pending || this->resolution_scale_live_apply_deferred) &&
        this->last_applied_resolution_width != 0;
    const auto scale = use_last_applied
        ? this->last_applied_resolution_scale
        : this->resolution_scale->value();

    return this->get_width_for_scale(scale);
}

uint32_t OpenXR::get_height_for_scale(float scale) const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * scale * eye_height_adjustment);
}

uint32_t OpenXR::get_height() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    const auto use_last_applied =
        (this->resolution_scale_reconfigure_pending || this->resolution_scale_live_apply_deferred) &&
        this->last_applied_resolution_height != 0;
    const auto scale = use_last_applied
        ? this->last_applied_resolution_scale
        : this->resolution_scale->value();

    return this->get_height_for_scale(scale);
}

VRRuntime::Error OpenXR::consume_events(std::function<void(void*)> callback) {
    std::scoped_lock _{event_mtx};

    XrEventDataBuffer edb{XR_TYPE_EVENT_DATA_BUFFER};
    auto result = xrPollEvent(this->instance, &edb);

    const auto bh = (XrEventDataBaseHeader*)&edb;

    while (result == XR_SUCCESS) {
        spdlog::info("VR: xrEvent: {}", this->get_structure_string(bh->type));

        if (callback) {
            callback(&edb);
        }

        if (bh->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto ev = (XrEventDataSessionStateChanged*)&edb;
            this->session_state = ev->state;

            spdlog::info("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED {} ({})", (uint32_t)ev->state, this->get_session_state_string(ev->state));

            if (ev->state == XR_SESSION_STATE_READY) {
                spdlog::info("VR: XR_SESSION_STATE_READY");
                
                // Begin the session
                XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                session_begin_info.primaryViewConfigurationType = this->view_config;

                result = xrBeginSession(this->session, &session_begin_info);

                if (result != XR_SUCCESS) {
                    this->error = std::string{"xrBeginSessionFailed: "} + this->get_result_string(result);
                    spdlog::error("VR: xrBeginSession failed: {}", this->get_result_string(result));
                } else {
                    this->session_ready = true;
                    this->frame_began = false;
                    this->clear_frame_synced("session_ready");
                    this->last_successful_wait_frame = {};
                    this->last_successful_begin_frame = {};
                    this->last_successful_end_frame = {};
                    this->last_successful_pose_update = {};
                    this->accepted_relaxed_startup_poses = false;
                    this->session_ready_since = std::chrono::steady_clock::now();
                    this->last_ready_state_probe_log = {};
                    this->last_valid_pose_probe_log = {};
                    spdlog::info("[OpenXR] Session is READY; synchronizing first frame");
                    this->last_frame_timing_log = {};
                    this->wait_frame_timing.reset();
                    this->begin_frame_timing.reset();
                    this->end_frame_timing.reset();
                    this->pose_update_timing.reset();
                    for (auto& timing : this->wait_frame_callsite_timing) {
                        timing.reset();
                    }
                    synchronize_frame(std::nullopt, SyncFrameCallsite::OpenXRSessionReady);
                }
            } else if (ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                spdlog::info("VR: XR_SESSION_STATE_LOSS_PENDING");
                this->wants_reinitialize = true;
            } else if (ev->state == XR_SESSION_STATE_STOPPING) {
                spdlog::info("VR: XR_SESSION_STATE_STOPPING");

                if (this->ready()) {
                    xrEndSession(this->session);
                    this->session_ready = false;
                    this->clear_frame_synced("session_stopping");
                    this->frame_began = false;
                    this->last_successful_wait_frame = {};
                    this->last_successful_begin_frame = {};
                    this->last_successful_end_frame = {};
                    this->last_successful_pose_update = {};
                    this->accepted_relaxed_startup_poses = false;
                    this->session_ready_since = {};
                    this->last_frame_timing_log = {};
                    this->wait_frame_timing.reset();
                    this->begin_frame_timing.reset();
                    this->end_frame_timing.reset();
                    this->pose_update_timing.reset();
                    for (auto& timing : this->wait_frame_callsite_timing) {
                        timing.reset();
                    }

                    if (this->wants_reinitialize) {
                        //initialize_openxr();
                    }
                }
            }

            if (ev->state == XR_SESSION_STATE_VISIBLE || ev->state == XR_SESSION_STATE_FOCUSED || ev->state == XR_SESSION_STATE_SYNCHRONIZED) {
                spdlog::info(
                    "[OpenXR] Session transitioned to {}. session_ready={} frame_synced={} frame_began={} got_first_poses={} got_first_valid_poses={}",
                    this->get_session_state_string(ev->state),
                    this->session_ready,
                    this->frame_synced,
                    this->frame_began,
                    this->got_first_poses,
                    this->got_first_valid_poses
                );

                if (this->session_ready && !this->frame_synced && !this->frame_began) {
                    spdlog::info("[OpenXR] Session reached frame-loop state; synchronizing first frame");
                    synchronize_frame();
                }
            }

            if (this->session_state != XR_SESSION_STATE_READY) {
                this->session_ready_since = {};
            }
        } else if (bh->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            this->wants_reset_origin = true;
        }

        edb = {XR_TYPE_EVENT_DATA_BUFFER};
        result = xrPollEvent(this->instance, &edb);
    } 
    
    if (result != XR_EVENT_UNAVAILABLE) {
        spdlog::error("VR: xrPollEvent failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_matrices(float nearz, float farz) {
    // exit immediately if we've updated the eye matrices since the last frame sync, so we only do this
    // operation once per sync
    if (!this->should_update_eye_matrices) {
        return VRRuntime::Error::SUCCESS;
    }

    if (!this->session_ready || this->views.empty()) {
        return VRRuntime::Error::SUCCESS;
    }

    // always update the pose:
    std::unique_lock ___{ this->pose_mtx };
    const auto& left_pose = this->views[0].pose;
    const auto& right_pose = this->views[1].pose;
    this->eyes[0] = Matrix4x4f{OpenXR::to_glm(left_pose.orientation)};
    this->eyes[0][3] = Vector4f{*(Vector3f*)&left_pose.position, 1.0f};
    this->eyes[1] = Matrix4x4f{OpenXR::to_glm(right_pose.orientation)};
    this->eyes[1][3] = Vector4f{*(Vector3f*)&right_pose.position, 1.0f};

    auto get_mat = [&](int eye) {
        const auto& vr = VR::get();
        std::array<float, 4> tan_half_fov{};

        if (vr->get_horizontal_projection_override() == VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_SYMMETRIC) {
            tan_half_fov[0] = -std::max(std::max(-this->raw_projections[0][0], this->raw_projections[0][1]),
                                        std::max(-this->raw_projections[1][0], this->raw_projections[1][1]));
            tan_half_fov[1] = -tan_half_fov[0];
        } else if (vr->get_horizontal_projection_override() == VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_MIRROR) {
            float max_outer = std::max(-this->raw_projections[0][0], this->raw_projections[1][1]);
            float max_inner = std::max(this->raw_projections[0][1], -this->raw_projections[1][0]);
            tan_half_fov[0] = eye == 0 ? -max_outer : -max_inner;
            tan_half_fov[1] = eye == 0 ? max_inner : max_outer;
        } else {
            tan_half_fov[0] = this->raw_projections[eye][0];
            tan_half_fov[1] = this->raw_projections[eye][1];
        }

        if (vr->get_vertical_projection_override() == VR::VERTICAL_PROJECTION_OVERRIDE::VERTICAL_SYMMETRIC) {
            tan_half_fov[2] = std::max(std::max(this->raw_projections[0][2], -this->raw_projections[0][3]),
                                        std::max(this->raw_projections[1][2], -this->raw_projections[1][3]));
            tan_half_fov[3] = -tan_half_fov[2];
        } else if (vr->get_vertical_projection_override() == VR::VERTICAL_PROJECTION_OVERRIDE::VERTICAL_MATCHED) {
            float max_top = std::max(this->raw_projections[0][2], this->raw_projections[1][2]);
            float max_bottom = std::max(-this->raw_projections[0][3], -this->raw_projections[1][3]);
            tan_half_fov[2] = max_top;
            tan_half_fov[3] = -max_bottom;
        } else {
            tan_half_fov[2] = this->raw_projections[eye][2];
            tan_half_fov[3] = this->raw_projections[eye][3];
        }
        view_bounds[eye][0] = 0.5f - 0.5f * this->raw_projections[eye][0] / tan_half_fov[0];
        view_bounds[eye][1] = 0.5f + 0.5f * this->raw_projections[eye][1] / tan_half_fov[1];
        view_bounds[eye][2] = 0.5f - 0.5f * this->raw_projections[eye][2] / tan_half_fov[2];
        view_bounds[eye][3] = 0.5f + 0.5f * this->raw_projections[eye][3] / tan_half_fov[3];

        // if we've derived the right eye, we have up to date view bounds for both so adjust the render target if necessary
        if (eye == 1) {
            if (vr->should_grow_rectangle_for_projection_cropping()) {
                eye_width_adjustment = 1 / std::max(view_bounds[0][1] - view_bounds[0][0], view_bounds[1][1] - view_bounds[1][0]);
                eye_height_adjustment = 1 / std::max(view_bounds[0][3] - view_bounds[0][2], view_bounds[1][3] - view_bounds[1][2]);
            } else {
                eye_width_adjustment = 1;
                eye_height_adjustment = 1;
            }
            SPDLOG_INFO("Eye texture proportion scale: {} by {}", eye_width_adjustment, eye_height_adjustment);
        }

        const auto left =   tan_half_fov[0];
        const auto right =  tan_half_fov[1];
        const auto top =    tan_half_fov[2];
        const auto bottom = tan_half_fov[3];

        // signs: at this point we expect left[0] and bottom[3] to be negative
        SPDLOG_INFO("Original FOV for {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", this->raw_projections[eye][0], this->raw_projections[eye][1],
                                                                                            this->raw_projections[eye][2], this->raw_projections[eye][3]);
        SPDLOG_INFO("Derived FOV for {} eye:  {}, {}, {}, {}", eye == 0 ? "left" : "right", left, right, top, bottom);
        SPDLOG_INFO("Derived texture bounds {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", view_bounds[eye][0], view_bounds[eye][1], view_bounds[eye][2], view_bounds[eye][3]);
        float sum_rl = (right + left);
        float sum_tb = (top + bottom);
        float inv_rl = (1.0f / (right - left));
        float inv_tb = (1.0f / (top - bottom));

        return Matrix4x4f {
            (2.0f * inv_rl), 0.0f, 0.0f, 0.0f,
            0.0f, (2.0f * inv_tb), 0.0f, 0.0f,
            (sum_rl * -inv_rl), (sum_tb * -inv_tb), 0.0f, 1.0f,
            0.0f, 0.0f, nearz, 0.0f
        };
    };

    // if we've not yet derived an eye projection matrix, or we've changed the projection, derive it here
    // Hacky way to check for an uninitialised eye matrix - is there something better, is this necessary?
    if (this->should_recalculate_eye_projections || this->last_eye_matrix_nearz != nearz || this->projections[0][2][3] == 0) {
        // deriving the texture bounds when modifying projections requires left and right raw projections so get them all before we start:
        std::unique_lock __{this->eyes_mtx};
        const auto& left_fov = this->views[0].fov;
        this->raw_projections[0][0] = tan(left_fov.angleLeft);
        this->raw_projections[0][1] = tan(left_fov.angleRight);
        this->raw_projections[0][2] = tan(left_fov.angleUp);
        this->raw_projections[0][3] = tan(left_fov.angleDown);
        const auto& right_fov = this->views[1].fov;
        this->raw_projections[1][0] = tan(right_fov.angleLeft);
        this->raw_projections[1][1] = tan(right_fov.angleRight);
        this->raw_projections[1][2] = tan(right_fov.angleUp);
        this->raw_projections[1][3] = tan(right_fov.angleDown);

        if (!is_eye_projection_valid(this->raw_projections[0]) || !is_eye_projection_valid(this->raw_projections[1])) {
            spdlog::warn(
                "[OpenXR] Refusing to recalculate eye projections from invalid FOVs. Left=({}, {}, {}, {}) Right=({}, {}, {}, {})",
                this->raw_projections[0][0], this->raw_projections[0][1], this->raw_projections[0][2], this->raw_projections[0][3],
                this->raw_projections[1][0], this->raw_projections[1][1], this->raw_projections[1][2], this->raw_projections[1][3]
            );

            if (!this->has_valid_projection_data) {
                view_bounds[0][0] = 0.0f;
                view_bounds[0][1] = 1.0f;
                view_bounds[0][2] = 0.0f;
                view_bounds[0][3] = 1.0f;
                view_bounds[1][0] = 0.0f;
                view_bounds[1][1] = 1.0f;
                view_bounds[1][2] = 0.0f;
                view_bounds[1][3] = 1.0f;
            }

            this->should_update_eye_matrices = false;
            return VRRuntime::Error::SUCCESS;
        }

        this->projections[0] = get_mat(0);
        this->projections[1] = get_mat(1);
        this->has_valid_projection_data = true;
        this->should_recalculate_eye_projections = false;
        this->last_eye_matrix_nearz = nearz;
    }
    // don't allow the eye matrices to be derived again until after the next frame sync
    this->should_update_eye_matrices = false;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_input() {
    std::scoped_lock _{this->event_mtx};

    if (!this->ready() || this->session_state != XR_SESSION_STATE_FOCUSED) {
        return (VRRuntime::Error)XR_ERROR_SESSION_NOT_READY;
    }

    XrActiveActionSet active_action_set{this->action_set.handle, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;

    auto result = xrSyncActions(this->session, &sync_info);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to sync actions: {}", this->get_result_string(result));

        return (VRRuntime::Error)result;
    }

    const auto current_interaction_profile = this->get_current_interaction_profile();

    for (auto i = 0; i < 2; ++i) {
        auto& hand = this->hands[i];
        hand.forced_actions.clear();

        // Update controller pose state
        {
            XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
            get_info.subactionPath = hand.path;
            get_info.action = this->action_set.action_map["grippose"];

            XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

            result = xrGetActionStatePose(this->session, &get_info, &pose_state);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

                return (VRRuntime::Error)result;
            }

            hand.active = pose_state.isActive;
        }

        {
            XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
            get_info.subactionPath = hand.path;
            get_info.action = this->action_set.action_map["pose"];

            XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

            result = xrGetActionStatePose(this->session, &get_info, &pose_state);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

                return (VRRuntime::Error)result;
            }
        }

        // Handle vector activator stuff
        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            if (this->is_action_active(activator, (VRRuntime::Hand)i)) {
                const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)i);
                
                for (const auto& output : it.second) {
                    const auto distance = glm::length(output.value - axis);

                    if (distance < 0.7f) {
                        hand.forced_actions[output.action] = true;
                    }
                }
            }
        }
    }

    // TODO: Other non-hand specific inputs
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::destroy() {
    if (!this->loaded) {
        return;
    }

    std::scoped_lock _{sync_mtx};

    if (this->session != nullptr) {
        if (this->session_ready) {
            xrEndSession(this->session);
        }

        xrDestroySession(this->session);
    }

    if (this->instance != nullptr && this->ever_submitted) {
        xrDestroyInstance(this->instance);
        this->instance = nullptr;
    }

    this->session = nullptr;
    this->session_ready = false;
    this->system = XR_NULL_SYSTEM_ID;
    this->clear_frame_synced("destroy");
    this->frame_began = false;
}

bool OpenXR::is_everspace2_snapshot_fresh(
    const PipelineState& snapshot,
    std::chrono::steady_clock::time_point now,
    int64_t* age_ms) const
{
    if (!snapshot.coherent_complete ||
        snapshot.stage_views.empty() ||
        snapshot.frame_state.predictedDisplayTime <= 1000 ||
        snapshot.coherent_capture_time.time_since_epoch().count() == 0)
    {
        if (age_ms != nullptr) {
            *age_ms = -1;
        }
        return false;
    }

    const auto snapshot_age_ms = elapsed_ms_since(now, snapshot.coherent_capture_time);
    if (age_ms != nullptr) {
        *age_ms = snapshot_age_ms;
    }

    const auto period_ms = snapshot.frame_state.predictedDisplayPeriod > 0
        ? std::max<int64_t>(1, snapshot.frame_state.predictedDisplayPeriod / 1000000)
        : 11LL;
    return snapshot_age_ms >= 0 && snapshot_age_ms <= period_ms * 2;
}

bool OpenXR::capture_everspace2_submit_snapshot(uint32_t frame_count, PipelineState& snapshot) {
    if (!this->is_everspace2_coherent_submit_active() ||
        this->session == XR_NULL_HANDLE ||
        this->stage_space == XR_NULL_HANDLE ||
        this->view_space == XR_NULL_HANDLE)
    {
        return false;
    }

    std::scoped_lock pose_lock{this->everspace2_pose_capture_mtx};

    XrFrameState local_frame_state{XR_TYPE_FRAME_STATE};
    size_t stage_view_count{};
    {
        std::scoped_lock assignment_lock{this->sync_assignment_mtx};
        const auto& requested_state = this->pipeline_states[frame_count % QUEUE_SIZE];
        local_frame_state =
            requested_state.frame_count == frame_count &&
            requested_state.frame_state.predictedDisplayTime > 1000
            ? requested_state.frame_state
            : this->frame_state;
        stage_view_count = this->stage_views.size();
    }

    if (local_frame_state.predictedDisplayTime <= 1000 || stage_view_count == 0) {
        return false;
    }

    const auto display_time =
        local_frame_state.predictedDisplayTime +
        static_cast<XrDuration>(local_frame_state.predictedDisplayPeriod * this->prediction_scale);

    std::vector<XrView> local_stage_views(stage_view_count, {XR_TYPE_VIEW});
    XrViewState local_stage_view_state{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    uint32_t located_view_count{};
    const auto locate_views_result = xrLocateViews(
        this->session,
        &view_locate_info,
        &local_stage_view_state,
        static_cast<uint32_t>(local_stage_views.size()),
        &located_view_count,
        local_stage_views.data());

    if (locate_views_result != XR_SUCCESS || located_view_count == 0) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[Everspace2][OpenXR][coherent-submit] Fresh stage-view capture failed frame={} result={} views={}",
            frame_count,
            this->get_result_string(locate_views_result),
            located_view_count);
        return false;
    }

    local_stage_views.resize(std::min<size_t>(local_stage_views.size(), located_view_count));

    XrSpaceLocation local_view_space_location{XR_TYPE_SPACE_LOCATION};
    const auto locate_space_result = xrLocateSpace(
        this->view_space,
        this->stage_space,
        display_time,
        &local_view_space_location);

    constexpr auto required_location_flags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (locate_space_result != XR_SUCCESS ||
        (local_view_space_location.locationFlags & required_location_flags) != required_location_flags)
    {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[Everspace2][OpenXR][coherent-submit] Fresh view-space capture failed frame={} result={} flags={}",
            frame_count,
            this->get_result_string(locate_space_result),
            format_space_location_flags(local_view_space_location.locationFlags));
        return false;
    }

    PipelineState captured{};
    captured.frame_state = local_frame_state;
    captured.view_space_location = local_view_space_location;
    captured.stage_views = std::move(local_stage_views);
    captured.frame_count = frame_count;
    captured.prev_frame_count = frame_count;
    captured.coherent_source_frame_count = frame_count;
    captured.coherent_display_time = display_time;
    captured.coherent_capture_time = std::chrono::steady_clock::now();
    captured.coherent_complete = true;
    captured.coherent_matches_render = false;

    {
        std::scoped_lock assignment_lock{this->sync_assignment_mtx};
        captured.coherent_sequence = ++this->everspace2_snapshot_sequence;
        auto& destination = this->pipeline_states[frame_count % QUEUE_SIZE];

        // A submit-only capture did not produce the texture being submitted.
        // Never replace the pose that was published before ES2 rendered this
        // frame, especially while a cutscene stalls on the same image.
        if (!(destination.frame_count == frame_count &&
              destination.coherent_matches_render &&
              destination.coherent_complete) &&
            (destination.frame_count == 0 ||
             destination.frame_count == frame_count ||
             !is_older_frame_count(frame_count, destination.frame_count)))
        {
            destination = captured;
        }
    }

    snapshot = std::move(captured);
    return true;
}

void OpenXR::log_everspace2_coherent_submit_summary_if_needed() {
    if (!this->is_everspace2_coherent_submit_active()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (this->everspace2_last_summary_log.time_since_epoch().count() != 0 &&
        now - this->everspace2_last_summary_log < EVERSPACE2_COHERENT_SUBMIT_SUMMARY_INTERVAL)
    {
        return;
    }

    this->everspace2_last_summary_log = now;
    spdlog::info(
        "[Everspace2][OpenXR][coherent-submit] summary exact={} nearby={} fresh={} one_frame_hold={} "
        "rejected={} max_pose_age_ms={} real_projection_submitted={}",
        this->everspace2_exact_submit_count,
        this->everspace2_nearby_submit_count,
        this->everspace2_fresh_capture_submit_count,
        this->everspace2_single_frame_hold_submit_count,
        this->everspace2_rejected_submit_count,
        this->everspace2_max_submit_pose_age_ms,
        this->everspace2_has_real_projection_submit.load(std::memory_order_acquire));
}

OpenXR::PipelineState OpenXR::get_submit_state() {
    if (this->is_everspace2_coherent_submit_active()) {
        SPDLOG_INFO_ONCE(
            "[Everspace2][OpenXR][coherent-submit] Active executable=ES2-Win64-Shipping.exe "
            "runtime=OpenXR renderer=D3D12 engine={} persistent_pose_latch=false "
            "slate_target_redirect=false global_mixed_frame_fallback=false",
            utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"unknown")));
        SPDLOG_INFO_ONCE(
            "[Everspace2][OpenXR][coherent-submit] Prior rejected paths remain disabled: "
            "persistent pose latching produced multi-second stale poses; Slate target redirection "
            "caused D3D12 render-target crashes; mixed global fallback combined different frames");

        uint32_t requested_frame{};
        XrTime current_display_time{};
        {
            std::scoped_lock assignment_lock{this->sync_assignment_mtx};
            requested_frame = this->internal_render_frame_count;
            current_display_time = this->frame_state.predictedDisplayTime;
        }

        const auto now = std::chrono::steady_clock::now();
        PipelineState selected{};
        const char* reason = "rejected";
        int64_t selected_age_ms{-1};

        {
            std::scoped_lock assignment_lock{this->sync_assignment_mtx};
            const auto& exact = this->pipeline_states[requested_frame % QUEUE_SIZE];
            if (exact.coherent_source_frame_count == requested_frame &&
                exact.coherent_matches_render &&
                exact.coherent_complete &&
                !exact.stage_views.empty() &&
                exact.frame_state.predictedDisplayTime > 1000 &&
                exact.coherent_capture_time.time_since_epoch().count() != 0)
            {
                selected = exact;
                selected_age_ms = elapsed_ms_since(now, exact.coherent_capture_time);
                reason = "exact";
            } else {
                for (uint32_t delta = 1; delta <= 2 && requested_frame >= delta; ++delta) {
                    const auto candidate_frame = requested_frame - delta;
                    const auto& candidate = this->pipeline_states[candidate_frame % QUEUE_SIZE];
                    int64_t candidate_age_ms{-1};
                    if (candidate.coherent_source_frame_count == candidate_frame &&
                        candidate.coherent_matches_render &&
                        this->is_everspace2_snapshot_fresh(candidate, now, &candidate_age_ms))
                    {
                        selected = candidate;
                        selected_age_ms = candidate_age_ms;
                        reason = "nearby";
                        break;
                    }
                }
            }
        }

        if (selected.stage_views.empty()) {
            if (this->capture_everspace2_submit_snapshot(requested_frame, selected)) {
                selected_age_ms = 0;
                reason = "fresh_capture";
            } else {
                std::scoped_lock assignment_lock{this->sync_assignment_mtx};
                int64_t held_age_ms{-1};
                if (!this->everspace2_single_frame_hold_used &&
                    this->is_everspace2_snapshot_fresh(this->last_submit_state, now, &held_age_ms))
                {
                    selected = this->last_submit_state;
                    selected_age_ms = held_age_ms;
                    reason = "single_frame_hold";
                    this->everspace2_single_frame_hold_used = true;
                } else {
                    selected = {};
                    selected.frame_state = this->frame_state;
                    selected.frame_count = requested_frame;
                    selected.coherent_source_frame_count = requested_frame;
                    selected_age_ms = -1;
                    reason = "rejected";
                }
            }
        }

        {
            std::scoped_lock assignment_lock{this->sync_assignment_mtx};
            if (std::string_view{reason} == "exact") {
                ++this->everspace2_exact_submit_count;
                this->everspace2_single_frame_hold_used = false;
            } else if (std::string_view{reason} == "nearby") {
                ++this->everspace2_nearby_submit_count;
                this->everspace2_single_frame_hold_used = false;
            } else if (std::string_view{reason} == "fresh_capture") {
                ++this->everspace2_fresh_capture_submit_count;
                this->everspace2_single_frame_hold_used = false;
            } else if (std::string_view{reason} == "single_frame_hold") {
                ++this->everspace2_single_frame_hold_submit_count;
            } else {
                ++this->everspace2_rejected_submit_count;
            }

            if (selected_age_ms >= 0) {
                this->everspace2_max_submit_pose_age_ms =
                    std::max(this->everspace2_max_submit_pose_age_ms, selected_age_ms);
            }

            if (!selected.stage_views.empty()) {
                this->last_submit_state = selected;
            }
            this->has_render_frame_count = false;
        }

        if (this->everspace2_last_submit_log.time_since_epoch().count() == 0 ||
            now - this->everspace2_last_submit_log >= EVERSPACE2_COHERENT_SUBMIT_LOG_INTERVAL)
        {
            this->everspace2_last_submit_log = now;
            spdlog::info(
                "[Everspace2][OpenXR][coherent-submit] select reason={} requested_frame={} "
                "selected_frame={} sequence={} age_ms={} display_time={} current_display_time={} "
                "display_time_delta={}",
                reason,
                requested_frame,
                selected.coherent_source_frame_count,
                selected.coherent_sequence,
                selected_age_ms,
                selected.frame_state.predictedDisplayTime,
                current_display_time,
                selected.frame_state.predictedDisplayTime - current_display_time);
        }

        this->log_everspace2_coherent_submit_summary_if_needed();
        return selected;
    }

    std::scoped_lock __{ this->sync_assignment_mtx };

    if (this->has_render_frame_count) {
        last_submit_state = this->pipeline_states[this->internal_render_frame_count % QUEUE_SIZE];

        if (last_submit_state.stage_views.empty() && !this->stage_views.empty()) {
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[OpenXR] Submit state for render frame {} had no stage views; falling back to the latest located views frame_count={}",
                this->internal_render_frame_count,
                this->internal_frame_count);
            last_submit_state.stage_views = this->stage_views;
            last_submit_state.view_space_location = this->view_space_location;
            last_submit_state.frame_state = this->frame_state;
            last_submit_state.frame_count = this->internal_frame_count;
        }
    } else {
        last_submit_state.stage_views = get_current_stage_view();
        last_submit_state.view_space_location = this->view_space_location;
        last_submit_state.frame_state = this->frame_state;
        last_submit_state.frame_count = this->internal_frame_count;
    }

    this->has_render_frame_count = false;

    /*if (get_frame_state(this->internal_render_frame_count-1).predictedDisplayTime > last_submit_state.frame_state.predictedDisplayTime) {
        spdlog::warn("[VR] Frame state is older than previous frame state!");
    }*/

    return last_submit_state;
}


void OpenXR::enqueue_render_poses(uint32_t frame_count) {
    //std::scoped_lock _{ this->sync_mtx };
    //std::unique_lock __{ this->pose_mtx };
    std::scoped_lock __{ this->sync_assignment_mtx };

    enqueue_render_poses_unsafe(frame_count);
}

void OpenXR::enqueue_render_poses_unsafe(uint32_t frame_count) {
    this->internal_render_frame_count = frame_count;
    this->has_render_frame_count = true;
}

std::vector<DXGI_FORMAT> OpenXR::get_supported_swapchain_formats() const {
    uint32_t count{};
    auto result = xrEnumerateSwapchainFormats(this->session, 0, &count, nullptr);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to enumerate swapchain formats: {}", this->get_result_string(result));

        return {};
    }

    std::vector<int64_t> formats(count);
    result = xrEnumerateSwapchainFormats(this->session, count, &count, formats.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to enumerate swapchain formats: {}", this->get_result_string(result));

        return {};
    }

    std::vector<DXGI_FORMAT> dxgi_formats(count);

    for (auto i = 0; i < count; ++i) {
        dxgi_formats[i] = (DXGI_FORMAT)formats[i];
    }

    return dxgi_formats;
}


std::string OpenXR::get_result_string(XrResult result) const {
    char result_string[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(this->instance, result, result_string);

    return result_string;
}

std::string OpenXR::get_session_state_string(XrSessionState state) const {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN:
        return "UNKNOWN";
    case XR_SESSION_STATE_IDLE:
        return "IDLE";
    case XR_SESSION_STATE_READY:
        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
        return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
        return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
        return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
        return "EXITING";
    default:
        return std::to_string((uint32_t)state);
    }
}

std::string OpenXR::get_structure_string(XrStructureType type) const {
    char structure_string[XR_MAX_STRUCTURE_NAME_SIZE]{};
    xrStructureTypeToString(this->instance, type, structure_string);

    return structure_string;
}

std::string OpenXR::get_path_string(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return "XR_NULL_PATH";
    }

    std::string path_string{};
    path_string.resize(XR_MAX_PATH_LENGTH);

    uint32_t real_size{};

    if (auto result = xrPathToString(this->instance, path, XR_MAX_PATH_LENGTH, &real_size, path_string.data()); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path string: {}", this->get_result_string(result));
        return "";
    }

    path_string.resize(real_size-1);
    return path_string;
}

XrPath OpenXR::get_path(const std::string& path) const {
    XrPath path_handle{XR_NULL_PATH};

    if (auto result = xrStringToPath(this->instance, path.c_str(), &path_handle); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path: {}", this->get_result_string(result));
        return XR_NULL_PATH;
    }

    return path_handle;
}

std::string OpenXR::get_current_interaction_profile() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return "";
    }

    return this->get_path_string(state.interactionProfile);
}

XrPath OpenXR::get_current_interaction_profile_path() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }

    return state.interactionProfile;
}

std::optional<std::string> OpenXR::initialize_actions(const std::string& json_string) {
    spdlog::info("[VR] Initializing actions");

    if (auto result = xrStringToPath(this->instance, "/user/hand/left", &this->hands[VRRuntime::Hand::LEFT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (left): " + this->get_result_string(result);
    }

    if (auto result = xrStringToPath(this->instance, "/user/hand/right", &this->hands[VRRuntime::Hand::RIGHT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (right): " + this->get_result_string(result);
    }

    std::array<XrPath, 2> hand_paths{
        this->hands[VRRuntime::Hand::LEFT].path, 
        this->hands[VRRuntime::Hand::RIGHT].path
    };

    if (json_string.empty()) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating action set");

    XrActionSetCreateInfo action_set_create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_create_info.actionSetName, "defaultopenxr");
    strcpy(action_set_create_info.localizedActionSetName, "Default");
    action_set_create_info.priority = 0;

    if (auto result = xrCreateActionSet(this->instance, &action_set_create_info, &this->action_set.handle); result != XR_SUCCESS) {
        return "xrCreateActionSet failed: " + this->get_result_string(result);
    }

    // Parse the JSON string using nlohmann
    json actions_json{};

    try {
        actions_json = json::parse(json_string);
    } catch (const std::exception& e) {
        return std::string{"json parse failed: "} + e.what();
    }

    if (actions_json.count("actions") == 0) {
        return "json missing actions";
    }

    auto actions_list = actions_json["actions"];
    bool has_pose_action = false;

    std::unordered_map<std::string, std::vector<XrActionSuggestedBinding>> profile_bindings{};

    for (const auto& controller : s_supported_controllers) {
        profile_bindings[controller] = {};
    }

    auto attempt_add_binding = [&](const std::string& interaction_profile, const XrActionSuggestedBinding& binding) -> bool {
        XrPath interaction_profile_path{};
        auto result = xrStringToPath(this->instance, interaction_profile.c_str(), &interaction_profile_path);
        auto& bindings = profile_bindings[interaction_profile];

        if (result == XR_SUCCESS) {
            bindings.push_back(binding);

            XrInteractionProfileSuggestedBinding suggested_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested_bindings.interactionProfile = interaction_profile_path;
            suggested_bindings.countSuggestedBindings = (uint32_t)bindings.size();
            suggested_bindings.suggestedBindings = bindings.data();

            result = xrSuggestInteractionProfileBindings(this->instance, &suggested_bindings);

            if (result != XR_SUCCESS) {
                bindings.pop_back();
                spdlog::info("Bad binding passed to xrSuggestInteractionProfileBindings from {}: {}", interaction_profile, this->get_result_string(result));
                return false;
            }

            return true;
        } else {
            spdlog::info("Bad interaction profile passed to xrStringToPath: {}", this->get_result_string(result));
            return false;
        }
    };

    for (auto& action : actions_list) {
        XrActionCreateInfo action_create_info{XR_TYPE_ACTION_CREATE_INFO};
        auto action_name = action["name"].get<std::string>();

        if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
            action_name = action_name.substr(it + 1);
        }

        auto localized_action_name = action_name;
        std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);

        strcpy(action_create_info.actionName, action_name.c_str());
        strcpy(action_create_info.localizedActionName, localized_action_name.c_str());

        action_create_info.countSubactionPaths = (uint32_t)hand_paths.size();
        action_create_info.subactionPaths = hand_paths.data();

        if (action_name == "pose" || action_name == "grippose") {
            has_pose_action = true;
        }

        std::unordered_set<XrAction>* out_actions = nullptr;

        // Translate the OpenVR action types to OpenXR action types
        switch (utility::hash(action["type"].get<std::string>())) {
            case "boolean"_fnv:
                if (action["type"].get<std::string>().ends_with("/value")) {
                    action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                    out_actions = &this->action_set.float_actions;
                } else {
                    action_create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                    out_actions = &this->action_set.bool_actions;
                }
                
                break;
            case "skeleton"_fnv: // idk what this is in OpenXR
                continue;
            case "pose"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                out_actions = &this->action_set.pose_actions;
                break;
            case "vector1"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                out_actions = &this->action_set.float_actions;
                break;
            case "vector2"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                out_actions = &this->action_set.vector2_actions;
                break;
            case "vibration"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                out_actions = &this->action_set.vibration_actions;
                break;
            default:
                continue;
        }
        
        // Create the action
        XrAction xr_action{XR_NULL_HANDLE};
        if (auto result = xrCreateAction(this->action_set.handle, &action_create_info, &xr_action); result != XR_SUCCESS) {
            return "xrCreateAction failed for " + action_name + ": " + this->get_result_string(result);
        }

        if (out_actions != nullptr) {
            out_actions->insert(xr_action);
        }

        spdlog::info("[VR] Created action {} with handle {:x}", action_name, (uintptr_t)xr_action);

        this->action_set.actions.push_back(xr_action);
        this->action_set.action_map[action_name] = xr_action;
        this->action_set.action_names[xr_action] = action_name;

        // Suggest bindings
        for (const auto& map_it : OpenXR::s_bindings_map) {
            if (map_it.action_name != action_name) {
                continue;
            }

            const auto& interaction_string = map_it.interaction_path_name;

            for (auto i = 0; i < 2; ++i) {
                auto hand_string = interaction_string;
                auto it = hand_string.find('*');
                auto index = i;
                bool wildcard = false;

                if (it != std::string::npos) {
                    if (i == VRRuntime::Hand::LEFT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "left");
                    } else if (i == VRRuntime::Hand::RIGHT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "right");
                    }

                    wildcard = true;
                } else {
                    if (hand_string.find("left") != std::string::npos) {
                        index = VRRuntime::Hand::LEFT;
                    } else if (hand_string.find("right") != std::string::npos) {
                        index = VRRuntime::Hand::RIGHT;
                    }
                }

                spdlog::info("[VR] {}", hand_string);

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, hand_string.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", hand_string);

                    if (!wildcard) {
                        break;
                    }

                    continue;
                }

                if (this->action_set.action_map.contains(map_it.action_name)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[map_it.action_name], p })) {
                            this->hands[index].profiles[controller].path_map[map_it.action_name] = p;
                        }
                    }
                }

                if (!wildcard) {
                    break;
                }
            }
        }
    }

    if (!has_pose_action) {
        return "json missing pose action";
    }

    // Check for json files that will override the default suggested bindings
    for (const auto& controller : s_supported_controllers) {
        // Create default action vector associations
        for (const auto& association : s_action_vector_associations) {
            auto& hand = this->hands[association.hand];
            auto& hand_profile = hand.profiles[controller];
            auto& action_map = this->action_set.action_map;
            const auto action_activator = action_map[association.action_activator];
            const auto action_modifier = action_map[association.action_modifier];

            hand_profile.action_vector_associations[action_activator] = action_modifier;

            for (const auto& vector_activator : association.vector_activators) {
                const auto output_action = action_map[vector_activator.action_name];
                hand_profile.vector_activators[action_activator].push_back({ vector_activator.value, output_action });
            }
        }

        auto profile_file = controller + ".json";

        // replace the slashes with underscores
        std::replace(profile_file.begin(), profile_file.end(), '/', '_');

        // If the json exists in the game's profile dir, use that.    
        auto filename = (Framework::get_persistent_dir() / profile_file).string();

        // If not, check for global profile in UEVR\Profiles dir
        if (!std::filesystem::exists(filename)) {
            filename = (Framework::get_persistent_dir() / ".." / "UEVR" / "Profiles" / profile_file).string();
            spdlog::info("[VR] Setting bindings file to {}", filename);
        }

        // check if the file exists
        if (std::filesystem::exists(filename)) {
            spdlog::info("[VR] Loading bindings for {}", filename);

            profile_bindings[controller].clear();

            this->hands[VRRuntime::Hand::LEFT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].path_map.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].path_map.clear();

            // load the json file
            auto j = nlohmann::json::parse(std::ifstream(filename));

            for (auto it : j["bindings"]) {
                auto action_str = it["action"].get<std::string>();
                auto path_str = it["path"].get<std::string>();

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, path_str.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", path_str);
                    continue;
                }

                const auto hand_idx = path_str.find("/left/") != std::string::npos ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;

                if (this->action_set.action_map.contains(action_str)) {
                    if (attempt_add_binding(controller, { this->action_set.action_map[action_str], p })) {
                        this->hands[hand_idx].profiles[controller].path_map[action_str] = p;
                    }
                }
            }

            for (auto it : j["vector2_associations"]) {
                const auto activator_name = it["activator"].get<std::string>();
                const auto modifier_name = it["modifier"].get<std::string>();
                const auto path_name = it["path"].get<std::string>();
                const auto path = this->get_path(path_name);

                for (auto output : it["outputs"]) {
                    const auto action_name = output["action"].get<std::string>();
                    
                    auto value_json = output["value"];
                    const auto value = Vector2f{value_json["x"].get<float>(), value_json["y"].get<float>()};

                    if (this->action_set.action_map.contains(action_name)) {
                        auto& hand = path == this->hands[VRRuntime::Hand::LEFT].path ? this->hands[VRRuntime::Hand::LEFT] : this->hands[VRRuntime::Hand::RIGHT];
                        auto& hand_profile = hand.profiles[controller];

                        spdlog::info("[VR] Adding vector2 association for {} {}", controller, path_name);

                        const auto output_action = this->action_set.action_map[action_name];
                        const auto action_modifier = this->action_set.action_map[modifier_name];
                        const auto action_activator = this->action_set.action_map[activator_name];

                        hand_profile.vector_activators[action_activator].push_back({ value, output_action });
                        hand_profile.action_vector_associations[action_activator] = action_modifier;
                    }
                }
            }
        }
    }

    // Create the action spaces for each hand
    // Grip space
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating grip action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["grippose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].grip_space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Aim space
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating aim action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["pose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].aim_space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Attach the action set to the session
    spdlog::info("[VR] Attaching action set to session");

    XrSessionActionSetsAttachInfo action_sets_attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    action_sets_attach_info.countActionSets = 1;
    action_sets_attach_info.actionSets = &this->action_set.handle;

    if (auto result = xrAttachSessionActionSets(this->session, &action_sets_attach_info); result != XR_SUCCESS) {
        return "xrAttachSessionActionSets failed: " + this->get_result_string(result);
    }

    return std::nullopt;
}

bool OpenXR::is_action_active(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    if (this->action_set.bool_actions.contains(action)) {
        XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
        auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
    } else if (this->action_set.float_actions.contains(action)) {
        XrActionStateFloat active{XR_TYPE_ACTION_STATE_FLOAT};
        auto result = xrGetActionStateFloat(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState > 0.0f;
    } // idk?

    return false;
}

bool OpenXR::is_action_active(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
}

bool OpenXR::is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE && active.changedSinceLastSync == XR_TRUE;
}

Vector2f OpenXR::get_action_axis(XrAction action, VRRuntime::Hand hand) const {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

std::string OpenXR::translate_openvr_action_name(std::string action_name) const {
    if (action_name.empty()) {
        return action_name;
    }

    if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
        action_name = action_name.substr(it + 1);
    }

    std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);
    return action_name;
}

Vector2f OpenXR::get_stick_axis(VRRuntime::Hand hand_idx) const {
    if (hand_idx != VRRuntime::Hand::LEFT && hand_idx != VRRuntime::Hand::RIGHT) {
        return Vector2f{};
    }

    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    const auto& hand = this->hands[hand_idx];

    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    XrAction action{XR_NULL_HANDLE};

    if (hand_profile.path_map.contains("joystick")) {
        auto it = this->action_set.action_map.find("joystick");

        if (it == this->action_set.action_map.end()) {
            return Vector2f{};
        }

        action = it->second;
    } else {
        auto it = this->action_set.action_map.find("touchpad");

        if (it == this->action_set.action_map.end()) {
            return Vector2f{};
        }

        action = it->second;
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

Vector2f OpenXR::get_left_stick_axis() const {
    return this->get_stick_axis(VRRuntime::Hand::LEFT);
}

Vector2f OpenXR::get_right_stick_axis() const {
    return this->get_stick_axis(VRRuntime::Hand::RIGHT);
}

void OpenXR::trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = frequency;

    // cast the duration from seconds to nanoseconds
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>(duration)).count();

    auto result = xrApplyHapticFeedback(this->session, &haptic_info, (XrHapticBaseHeader*)&vibration);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to apply haptic feedback: {}", this->get_result_string(result));
    }
}

void OpenXR::display_bindings_editor() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    ImGui::Text("Interaction Profile: %s", current_interaction_profile.c_str());

    if (ImGui::Button("Restore Default Bindings")) {
        auto filename = current_interaction_profile + ".json";
        
        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        const auto persistent_filename = (Framework::get_persistent_dir() / filename).string();

        if (std::filesystem::exists(persistent_filename)) {
            // Delete the file
            std::filesystem::remove(persistent_filename);
            this->wants_reinitialize = true;
        }
    }

    if (ImGui::Button("Save Bindings")) {
        this->save_bindings();
    }

    auto display_hand = [&](const std::string& name, uint32_t index) {
        if (current_interaction_profile.empty() || current_interaction_profile == "XR_NULL_PATH") {
            ImGui::Text("Interaction profile not loaded, try putting on your headset.");
            return;
        }

        if (ImGui::TreeNode(name.data())) {
            ImGui::PushID(name.data());

            auto& hand = this->hands[index];

            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                if (this->is_action_active(activator, (VRRuntime::Hand)index)) {
                    const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)index);
                    
                    for (const auto& output : it.second) {
                        const auto distance = glm::length(output.value - axis);
                        ImGui::Text("%s: %.2f", this->action_set.action_names[output.action].data(), distance);
                    }
                }
            }

            auto& path_map = hand.profiles[current_interaction_profile].path_map;

            std::vector<std::string> known_actions{};
            std::vector<const char*> known_actions_cstr{};

            std::vector<std::string> known_vector2_actions{};
            std::vector<const char*> known_vector2_actions_cstr{};

            for (auto action :this->action_set.actions) {
                known_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_actions) {
                known_actions_cstr.push_back(it.data());
            };

            for (auto action : this->action_set.vector2_actions) {
                known_vector2_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_vector2_actions) {
                known_vector2_actions_cstr.push_back(it.data());
            }
            
            for (auto& it : path_map) {
                ImGui::PushID(it.first.data());

                int current_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == it.first) {
                        current_combo_index = i;
                        break;
                    }
                }

                if (ImGui::Button("X")) {
                    path_map.erase(it.first);

                    this->save_bindings();
                    ImGui::PopID();
                    break;
                }
                
                auto combo_name = this->get_path_string(it.second) + ": " + known_actions[current_combo_index];

                ImGui::SameLine();
                if (ImGui::Combo(combo_name.c_str(), &current_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    path_map.erase(it.first);
                    path_map[known_actions[current_combo_index]] = it.second;

                    this->save_bindings();
                    ImGui::PopID();

                    break;
                }

                ImGui::PopID();
            }

            // Create a way to add a completely new binding
            // Create a textbox for inputting the path for the new binding
            ImGui::InputText("New Binding (e.g. /user/hand/left/input/trigger)", hand.ui.new_path_name, XR_MAX_PATH_LENGTH);
            ImGui::Combo("Action", &hand.ui.action_combo_index, known_actions_cstr.data(), known_actions_cstr.size());

            if (ImGui::Button("Add Binding")) {
                XrPath p{};
                if (xrStringToPath(this->instance, hand.ui.new_path_name, &p) != XR_SUCCESS) {
                    spdlog::error("[VR] Failed to convert path: {}", hand.ui.new_path_name);
                } else {
                    path_map[known_actions[hand.ui.action_combo_index]] = p;
                    this->save_bindings();
                }
            }

            ImGui::Text("Vector2 Associations");
            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                ImGui::PushID(&it.first);

                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                const auto activator_name = this->action_set.action_names[activator];
                const auto modifier_name = this->action_set.action_names[modifier];

                int activator_combo_index = 0;
                int modifier_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == activator_name) {
                        activator_combo_index = i;
                        break;
                    }
                }

                for (auto i = 0; i < known_vector2_actions.size(); i++) {
                    if (known_vector2_actions[i] == modifier_name) {
                        modifier_combo_index = i;
                        break;
                    }
                }

                ImGui::PushID(modifier_name.data());
                
                if (ImGui::Combo(modifier_name.data(), &modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size())) {
                    hand.profiles[current_interaction_profile].action_vector_associations[activator] = this->action_set.action_map[known_vector2_actions[modifier_combo_index]];
                }
                
                ImGui::PushID(activator_name.data());
                ImGui::Indent();

                if (ImGui::Combo(activator_name.data(), &activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    const auto old_outputs = hand.profiles[current_interaction_profile].vector_activators[activator];
                    const auto new_activator = this->action_set.action_map[known_actions[activator_combo_index]];

                    hand.profiles[current_interaction_profile].action_vector_associations.erase(activator);
                    hand.profiles[current_interaction_profile].action_vector_associations[new_activator] = modifier;
                    hand.profiles[current_interaction_profile].vector_activators.erase(activator);
                    hand.profiles[current_interaction_profile].vector_activators[new_activator] = old_outputs;
                }

                ImGui::Indent();
                for (auto& output : it.second) {
                    int output_combo_index = 0;
                    const auto output_name = this->action_set.action_names[output.action];

                    for (auto i = 0; i < known_actions.size(); i++) {
                        if (known_actions[i] == output_name) {
                            output_combo_index = i;
                            break;
                        }
                    }

                    ImGui::PushID(output_name.data());

                    if (ImGui::Combo(output_name.data(), &output_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                        output.action = this->action_set.action_map[known_actions[output_combo_index]];
                    }

                    ImGui::SliderFloat2("Value", &output.value[0], -1.0f, 1.0f);
                    ImGui::PopID();
                }

                if (ImGui::Button("Insert New Output")) {
                    hand.profiles[current_interaction_profile].vector_activators[activator].push_back({});
                }

                ImGui::Unindent();
                ImGui::Unindent();
                ImGui::PopID();
                ImGui::PopID();

                ImGui::PopID();
            }

            ImGui::Combo("New Vector2 Activator", &hand.ui.activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::Combo("New Vector2 Modifier", &hand.ui.modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size());
            ImGui::Combo("New Vector2 Output", &hand.ui.output_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::SliderFloat2("New Vector2 Value", &hand.ui.output_vector2[0], -1.0f, 1.0f);

            if (ImGui::Button("Add Vector2 Association")) {
                const auto activator = this->action_set.action_map[known_actions[hand.ui.activator_combo_index]];
                const auto modifier = this->action_set.action_map[known_vector2_actions[hand.ui.modifier_combo_index]];
                const auto output = this->action_set.action_map[known_actions[hand.ui.output_combo_index]];

                hand.profiles[current_interaction_profile].action_vector_associations[activator] = modifier;
                hand.profiles[current_interaction_profile].vector_activators[activator].push_back({hand.ui.output_vector2, output});
            }

            ImGui::PopID();
            ImGui::TreePop();
        }
    };

    display_hand("Left", 0);
    display_hand("Right", 1);
}

void OpenXR::save_bindings() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    nlohmann::json j;

    for (auto& hand : this->hands) {
        for (auto& it : hand.profiles[current_interaction_profile].path_map) {
            nlohmann::json binding{};
            binding["action"] = it.first;
            binding["path"] = this->get_path_string(it.second);
            j["bindings"].push_back(binding);
        }

        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            const auto activator_name = this->action_set.action_names[activator];
            const auto modifier_name = this->action_set.action_names[modifier];

            nlohmann::json vector2_association{};
            vector2_association["path"] = this->get_path_string(hand.path);
            vector2_association["activator"] = activator_name;
            vector2_association["modifier"] = modifier_name;

            nlohmann::json outputs{};
            for (auto& output : it.second) {
                const auto output_name = this->action_set.action_names[output.action];

                nlohmann::json output_json{};
                output_json["action"] = output_name;
                output_json["value"]["x"] = output.value.x;
                output_json["value"]["y"] = output.value.y;

                outputs.push_back(output_json);
            }

            vector2_association["outputs"] = outputs;
            j["vector2_associations"].push_back(vector2_association);
        }
    }

    auto filename = current_interaction_profile + ".json";
    
    // replace the slashes with underscores
    std::replace(filename.begin(), filename.end(), '/', '_');
    const auto persistent_filename = (Framework::get_persistent_dir() / filename).string();

    std::ofstream(persistent_filename) << j.dump(4);

    this->wants_reinitialize = true;
}

XrResult OpenXR::begin_frame(const char* caller) {
    std::scoped_lock _{sync_mtx};

    this->trace_begin_frame_request(caller);
    emit_openxr_state_probes(this, "begin_frame");

    if (!this->can_run_frame_loop() || !this->got_first_poses || !this->frame_synced) {
        this->log_frame_lifecycle_state("begin_frame refused because runtime was not ready");
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (this->frame_began) {
        this->log_frame_lifecycle_state("begin_frame called while frame already began");
        return XR_SUCCESS;
    }

    this->begin_profile();

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_frame_start = std::chrono::steady_clock::now();
    auto result = xrBeginFrame(this->session, &frame_begin_info);
    this->begin_frame_timing.add(std::chrono::steady_clock::now() - begin_frame_start);

    this->end_profile("xrBeginFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrBeginFrame failed: {}", this->get_result_string(result));
    }

    if (result == XR_ERROR_CALL_ORDER_INVALID) {
        spdlog::warn("[OpenXR] xrBeginFrame returned XR_ERROR_CALL_ORDER_INVALID, attempting recovery");
        this->recover_wedged_frame("xrBeginFrame call order invalid");
        synchronize_frame(std::nullopt, SyncFrameCallsite::OpenXRBeginFrameRecovery);
        const auto retry_begin_frame_start = std::chrono::steady_clock::now();
        result = xrBeginFrame(this->session, &frame_begin_info);
        this->begin_frame_timing.add(std::chrono::steady_clock::now() - retry_begin_frame_start);
    }

    this->frame_began = result == XR_SUCCESS || result == XR_FRAME_DISCARDED; // discarded means endFrame was not called

    if (this->frame_began) {
        this->frame_began_skip_streak = 0;
        const auto now = std::chrono::steady_clock::now();
        const auto should_log = this->last_successful_begin_frame.time_since_epoch().count() == 0 ||
            now - this->last_successful_begin_frame >= std::chrono::seconds(2);
        this->last_successful_begin_frame = now;

        if (should_log) {
            spdlog::info(
                "[OpenXR] xrBeginFrame succeeded: session_state={} predictedDisplayTime={} predictedDisplayPeriod={}",
                this->get_session_state_string(this->session_state),
                this->frame_state.predictedDisplayTime,
                this->frame_state.predictedDisplayPeriod
            );
        }

        // Some games/runtimes briefly report only partial startup pose state. Do not
        // leave that pre-submit READY frame open, or later setup failures can wedge
        // the frame loop before the D3D12 submit path has anything valid to end.
        if (!this->ever_submitted && !this->got_first_valid_poses) {
            static auto last_empty_startup_frame_log = std::chrono::steady_clock::time_point{};
            XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
            frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
            frame_end_info.environmentBlendMode = this->blend_mode;
            frame_end_info.layerCount = 0;
            frame_end_info.layers = nullptr;

            const auto end_result = xrEndFrame(this->session, &frame_end_info);
            this->frame_began = false;
            this->clear_frame_synced("startup_empty_frame");
            ++this->forced_frame_recovery_count;

            if (last_empty_startup_frame_log.time_since_epoch().count() == 0 ||
                now - last_empty_startup_frame_log >= std::chrono::seconds(1))
            {
                last_empty_startup_frame_log = now;
                spdlog::warn(
                    "[OpenXR] Closed startup frame without layers while waiting for first valid poses: end_result={} session_state={} predictedDisplayTime={} predictedDisplayPeriod={}",
                    this->get_result_string(end_result),
                    this->get_session_state_string(this->session_state),
                    this->frame_state.predictedDisplayTime,
                    this->frame_state.predictedDisplayPeriod
                );
            }
        }
    }

    return result;
}

XrResult OpenXR::end_frame(const std::vector<XrCompositionLayerBaseHeader*>& quad_layers, bool has_depth) {
    std::scoped_lock _{sync_mtx};

    emit_openxr_state_probes(this, "end_frame");

    if (!this->can_run_frame_loop() || !this->got_first_poses || !this->frame_synced) {
        this->log_frame_lifecycle_state("end_frame refused because runtime was not ready");

        if (this->frame_began) {
            this->recover_wedged_frame("end_frame refused while frame_began remained set");
        }

        return XR_ERROR_SESSION_NOT_READY;
    }

    if (!this->frame_began) {
        this->log_frame_lifecycle_state("end_frame called while frame not begun");
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    auto vr = VR::get();
    const auto is_afr = vr->is_using_afr();
    const auto has_native_stereo_array =
        !is_afr &&
        vr->is_native_stereo_fix_texture_array_submit_enabled() &&
        this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::NATIVE_STEREO_ARRAY);

    if (is_afr || has_native_stereo_array) {
        if (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_LEFT_EYE) || !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_RIGHT_EYE)) {
            if (!has_native_stereo_array) {
                spdlog::error("[VR] AFR swapchains not created");
                return XR_ERROR_VALIDATION_FAILURE;
            }
        }

        /*if (has_depth && (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE) || !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE))) {
            spdlog::error("[VR] AFR depth swapchains not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }*/

        has_depth = !has_native_stereo_array &&
            has_depth &&
            this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE) &&
            this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE);
    } else {
        if (!this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DOUBLE_WIDE)) {
            spdlog::error("[VR] Double wide swapchain not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }

        /*if (has_depth && !this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DEPTH)) {
            spdlog::error("[VR] Double wide depth swapchain not created");
            return XR_ERROR_VALIDATION_FAILURE;
        }*/

        has_depth = has_depth && this->swapchains.contains((uint32_t)OpenXR::SwapchainIndex::DEPTH);
    }

    const auto submit_state = this->get_submit_state();
    const auto& pipelined_stage_views = submit_state.stage_views;
    const auto& pipelined_frame_state = submit_state.frame_state;
    const auto debug_submit_empty = this->debug_submit_empty_frame->value();

    if (pipelined_stage_views.empty()) {
        spdlog::warn("[VR] No stage views to submit");
    }

    this->projection_layer_cache.clear();

    std::vector<XrCompositionLayerBaseHeader*> layers{};
    std::vector<XrCompositionLayerProjectionView> projection_layer_views{};
    std::vector<XrCompositionLayerDepthInfoKHR> depth_layers{};

    // Dummy projection layers for Virtual Desktop. If we don't do this, timewarp does not work correctly on VD.
    // the reasoning from ggodin (VD dev) is that VD composites all layers using the top layer's pose (apparently)
    // I am actually not sure why this fixes the issue, but it does. and even makes the SteamVR overlay work completely fine.
    const auto should_push_dummy = this->push_dummy_projection == true && 
                                   !pipelined_stage_views.empty() && 
                                   this->ever_submitted == true;

    XrCompositionLayerProjection dummy_projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> dummy_projection_layer_views{};

    if (should_push_dummy) {
        const auto& vd_swapchain = this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP];

        for (auto i = 0; i < std::min<size_t>(dummy_projection_layer_views.size(), pipelined_stage_views.size()); ++i) {
            auto& view = dummy_projection_layer_views[i];
            view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.pose = pipelined_stage_views[i].pose;
            view.fov = pipelined_stage_views[i].fov;

            view.subImage.swapchain = vd_swapchain.handle;
            view.subImage.imageRect.offset = {(vd_swapchain.width / 2) * i, 0};
            view.subImage.imageRect.extent = {(vd_swapchain.width / 2), vd_swapchain.height};
            view.subImage.imageArrayIndex = 0;
        }

        dummy_projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        dummy_projection_layer.viewCount = 2;
        dummy_projection_layer.views = dummy_projection_layer_views.data();
        dummy_projection_layer.space = this->stage_space;
        dummy_projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }

    // we CANT push the layers every time, it cause some layer error
    // in xrEndFrame, so we must only do it when shouldRender is true
    if (!debug_submit_empty && pipelined_frame_state.shouldRender == XR_TRUE && !pipelined_stage_views.empty()) {
        projection_layer_views.resize(pipelined_stage_views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        depth_layers.resize(projection_layer_views.size(), {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});

        for (auto i = 0; i < projection_layer_views.size(); ++i) {            
            Swapchain* swapchain = nullptr;

            if (has_native_stereo_array) {
                swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::NATIVE_STEREO_ARRAY];
            } else if (is_afr) {
                if (i == 0) {
                    swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_LEFT_EYE];
                } else {
                    swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_RIGHT_EYE];
                }
            } else {
                swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DOUBLE_WIDE];
            }

            projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projection_layer_views[i].pose = pipelined_stage_views[i].pose;
            projection_layer_views[i].fov = pipelined_stage_views[i].fov;
            projection_layer_views[i].subImage.swapchain = swapchain->handle;
            projection_layer_views[i].subImage.imageArrayIndex = has_native_stereo_array ? (uint32_t)i : 0;

            int32_t offset_x = 0, offset_y = 0, extent_x = 0, extent_y = 0;
            // if we're working with a double-wide texture, use half the view bounds adjustment (as they apply to a single eye)
            int texture_area_width = (is_afr || has_native_stereo_array) ? swapchain->width : swapchain->width / 2;
            if (is_afr || has_native_stereo_array || i == 0) {
                offset_x = view_bounds[i][0] * texture_area_width;
                extent_x = view_bounds[i][1] * texture_area_width - offset_x;
            } else {
                // right eye double-wide
                offset_x = texture_area_width + view_bounds[i][0] * texture_area_width;
                extent_x = view_bounds[i][1] * texture_area_width - (offset_x - texture_area_width);
            }
            offset_y = view_bounds[i][2] * swapchain->height;
            extent_y = view_bounds[i][3] * swapchain->height - offset_y;
            
            // SPDLOG_INFO("image calc for eye {} {}, {}, {}, {}", i, offset_x, extent_x, offset_y, extent_y);
            projection_layer_views[i].subImage.imageRect.offset = {offset_x, offset_y};
            projection_layer_views[i].subImage.imageRect.extent = {extent_x, extent_y};

            if (has_depth) {
                Swapchain* depth_swapchain = nullptr;

                if (is_afr) {
                    if (i == 0) {
                        depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE];
                    } else {
                        depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE];
                    }
                } else {
                    depth_swapchain = &this->swapchains[(uint32_t)OpenXR::SwapchainIndex::DEPTH];
                }

                depth_layers[i].type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                depth_layers[i].next = nullptr;
                depth_layers[i].subImage.swapchain = depth_swapchain->handle;

                depth_layers[i].subImage.imageRect.offset = {offset_x, offset_y};
                depth_layers[i].subImage.imageRect.extent = {std::min<int>(get_width(), extent_x), std::min<int>(get_height(), extent_y)};
                depth_layers[i].minDepth = 0.0f;
                depth_layers[i].maxDepth = 1.0f;
                auto wtm = VR::get()->get_world_to_meters();
                if (wtm < 0.0f || wtm == 0.0f) {
                    wtm = 1.0f;
                }

                const auto nearz = sdk::globals::get_near_clipping_plane() / wtm;

                depth_layers[i].nearZ = FLT_MAX;
                depth_layers[i].farZ = nearz * VR::get()->get_depth_scale();
                
                projection_layer_views[i].next = &depth_layers[i];
            }
        }

        auto& layer = this->projection_layer_cache.emplace_back();
        layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.space = this->stage_space;
        layer.viewCount = (uint32_t)projection_layer_views.size();
        layer.views = projection_layer_views.data();
        layer.layerFlags = 0;

        if (should_push_dummy) {
            layers.push_back((XrCompositionLayerBaseHeader*)&dummy_projection_layer);
        }

        for (auto& l : this->projection_layer_cache) {
            layers.push_back((XrCompositionLayerBaseHeader*)&l);
        }

        for (auto& l : quad_layers) {   
            layers.push_back(l);
        }
    } else if (debug_submit_empty) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[OpenXR][debug] Submitting empty frame for perf isolation. shouldRender={} displayTime={} displayPeriod={}",
            pipelined_frame_state.shouldRender,
            pipelined_frame_state.predictedDisplayTime,
            pipelined_frame_state.predictedDisplayPeriod
        );
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = pipelined_frame_state.predictedDisplayTime != 0 ? pipelined_frame_state.predictedDisplayTime : this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = (uint32_t)layers.size();
    frame_end_info.layers = layers.data();

    const auto submitted_real_projection =
        this->is_everspace2_coherent_submit_active() &&
        !projection_layer_views.empty() &&
        !this->projection_layer_cache.empty();

    //spdlog::info("[VR] display time diff: {}", pipelined_frame_state.predictedDisplayTime - this->frame_state.predictedDisplayTime);
    //spdlog::info("[VR] Ending frame, {} layers", frame_end_info.layerCount);
    //spdlog::info("[VR] Ending frame, layer ptr: {:x}", (uintptr_t)frame_end_info.layers);

    this->begin_profile();
    const auto end_frame_start = std::chrono::steady_clock::now();
    auto result = xrEndFrame(this->session, &frame_end_info);
    this->end_frame_timing.add(std::chrono::steady_clock::now() - end_frame_start);
    this->end_profile("xrEndFrame");
    
    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrEndFrame failed: {}", this->get_result_string(result));

        if (result == XR_ERROR_TIME_INVALID) {
             spdlog::error("[VR] xrEndFrame time: submitted: {} vs frame_state: {}", frame_end_info.displayTime, this->frame_state.predictedDisplayTime);
             spdlog::error("[VR] display time diff: {}", frame_end_info.displayTime - this->frame_state.predictedDisplayTime);
        }
    } else {
        this->ever_submitted = true;
        if (submitted_real_projection) {
            this->everspace2_has_real_projection_submit.store(true, std::memory_order_release);
        }
        const auto now = std::chrono::steady_clock::now();
        const auto should_log = this->last_successful_end_frame.time_since_epoch().count() == 0 ||
            now - this->last_successful_end_frame >= std::chrono::seconds(2);
        this->last_successful_end_frame = now;

        if (should_log) {
            spdlog::info(
                "[OpenXR] xrEndFrame succeeded: layers={} shouldRender={} displayTime={}",
                frame_end_info.layerCount,
                pipelined_frame_state.shouldRender,
                frame_end_info.displayTime
            );
        }
    }
    
    this->frame_began = false;
    this->clear_frame_synced("end_frame_complete");
    this->log_frame_timing_stats_if_needed();

    return result;
}

void OpenXR::log_frame_timing_stats_if_needed() {
    const auto now = std::chrono::steady_clock::now();

    if (this->last_frame_timing_log.time_since_epoch().count() == 0) {
        this->last_frame_timing_log = now;
        return;
    }

    if (now - this->last_frame_timing_log < FRAME_TIMING_LOG_INTERVAL) {
        return;
    }

    if (this->wait_frame_timing.count == 0 &&
        this->begin_frame_timing.count == 0 &&
        this->end_frame_timing.count == 0 &&
        this->pose_update_timing.count == 0)
    {
        this->last_frame_timing_log = now;
        return;
    }

    const auto& wait_fix = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::RuntimeFixFrame];
    const auto& wait_early = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::VREarlyRHICommand];
    const auto& wait_late = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::VRLateOnPresent];
    const auto& wait_post_present_initial = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::VRPostPresentInitialSync];
    const auto& wait_very_late = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::VRVeryLatePostPresent];
    const auto& wait_session_ready = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::OpenXRSessionReady];
    const auto& wait_recovery = this->wait_frame_callsite_timing[(size_t)SyncFrameCallsite::OpenXRBeginFrameRecovery];
    const auto pose_age_ms = this->get_pose_update_age_ms(now);

    spdlog::info(
        "[OpenXR][frame-profiler] wait avg={:.2f}ms max={:.2f}ms n={} wait_fix avg={:.2f}ms max={:.2f}ms n={} wait_early avg={:.2f}ms max={:.2f}ms n={} wait_late avg={:.2f}ms max={:.2f}ms n={} wait_post_present_initial avg={:.2f}ms max={:.2f}ms n={} wait_very_late avg={:.2f}ms max={:.2f}ms n={} wait_session_ready avg={:.2f}ms max={:.2f}ms n={} wait_recovery avg={:.2f}ms max={:.2f}ms n={} begin avg={:.2f}ms max={:.2f}ms n={} end avg={:.2f}ms max={:.2f}ms n={} pose_update avg={:.2f}ms max={:.2f}ms n={} pose_age_ms={} pose_calls={} pose_view_ext={} pose_runtime={} stale_refresh={}/{}/{} last_pose_src={} last_pose_frame={} last_pose_result={} last_pose_ms={:.2f} view_locate_ms={:.2f} stage_locate_ms={:.2f} space_locate_ms={:.2f} session={} ready={} synced={} began={} first_poses={} valid_poses={} relaxed_startup={} stale_refresh_enabled={} dbg_empty={} dbg_skip_scene={} dbg_skip_ui={} dbg_no_depth={}",
        this->wait_frame_timing.avg(),
        this->wait_frame_timing.max_ms,
        this->wait_frame_timing.count,
        wait_fix.avg(),
        wait_fix.max_ms,
        wait_fix.count,
        wait_early.avg(),
        wait_early.max_ms,
        wait_early.count,
        wait_late.avg(),
        wait_late.max_ms,
        wait_late.count,
        wait_post_present_initial.avg(),
        wait_post_present_initial.max_ms,
        wait_post_present_initial.count,
        wait_very_late.avg(),
        wait_very_late.max_ms,
        wait_very_late.count,
        wait_session_ready.avg(),
        wait_session_ready.max_ms,
        wait_session_ready.count,
        wait_recovery.avg(),
        wait_recovery.max_ms,
        wait_recovery.count,
        this->begin_frame_timing.avg(),
        this->begin_frame_timing.max_ms,
        this->begin_frame_timing.count,
        this->end_frame_timing.avg(),
        this->end_frame_timing.max_ms,
        this->end_frame_timing.count,
        this->pose_update_timing.avg(),
        this->pose_update_timing.max_ms,
        this->pose_update_timing.count,
        pose_age_ms,
        this->pose_update_call_count,
        this->pose_update_view_extension_count,
        this->pose_update_non_view_extension_count,
        this->stale_pose_refresh_attempt_count,
        this->stale_pose_refresh_success_count,
        this->stale_pose_refresh_failed_count,
        this->last_pose_update_from_view_extensions ? "view_extension" : "runtime",
        this->last_pose_update_frame_count,
        this->last_pose_update_result,
        this->last_pose_update_ms,
        this->last_pose_view_locate_ms,
        this->last_pose_stage_locate_ms,
        this->last_pose_space_locate_ms,
        this->get_session_state_string(this->session_state),
        this->session_ready,
        this->frame_synced,
        this->frame_began,
        this->got_first_poses,
        this->got_first_valid_poses,
        this->accepted_relaxed_startup_poses,
        this->refresh_stale_pose_before_submit_enabled->value(),
        this->debug_submit_empty_frame->value(),
        this->debug_skip_scene_copy->value(),
        this->debug_skip_ui_copy->value(),
        this->debug_disable_depth_submit->value()
    );

    this->last_frame_timing_log = now;
    this->wait_frame_timing.reset();
    this->begin_frame_timing.reset();
    this->end_frame_timing.reset();
    this->pose_update_timing.reset();
    for (auto& timing : this->wait_frame_callsite_timing) {
        timing.reset();
    }
}
}
