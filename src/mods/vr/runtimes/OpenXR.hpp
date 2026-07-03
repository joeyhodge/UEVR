#pragma once

#include <atomic>
#include <unordered_set>
#include <deque>
#include <chrono>
#include <limits>
#include <mutex>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <common/xr_linear.h>

#include <sdk/Math.hpp>

#include "Mod.hpp"

#include "VRRuntime.hpp"

namespace runtimes{
struct OpenXR final : public VRRuntime {

    virtual ~OpenXR() {
        this->destroy();
    }

    struct Swapchain {
        XrSwapchain handle;
        int32_t width;
        int32_t height;
    };

    struct SwapchainDimensionSnapshot {
        uint32_t count{};
        uint32_t ui_width{};
        uint32_t ui_height{};
        uint32_t eye_width{};
        uint32_t eye_height{};
        uint32_t depth_width{};
        uint32_t depth_height{};
    };

    VRRuntime::Type type() const override { 
        return VRRuntime::Type::OPENXR;
    }

    std::string_view name() const override {
        return "OpenXR";
    }

    bool ready() const override {
        return VRRuntime::ready() && this->session_ready;
    }

    bool can_run_frame_loop() const {
        return ready() &&
            (this->session_state == XR_SESSION_STATE_READY ||
             this->session_state == XR_SESSION_STATE_SYNCHRONIZED ||
             this->session_state == XR_SESSION_STATE_VISIBLE ||
             this->session_state == XR_SESSION_STATE_FOCUSED);
    }

    bool is_depth_allowed() const override {
        return this->enabled_extensions.contains(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    }

    bool is_cylinder_layer_allowed() const override {
        return this->enabled_extensions.contains(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }

    void on_system_properties_acquired(const XrSystemProperties& props);

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_draw_ui() override;

    void on_device_reset() override {
        std::scoped_lock _{this->sync_mtx};
        std::scoped_lock __{this->pose_mtx};
        //stage_view_queue.clear();
        //stage_view_queue_renderthread.clear();
    }

    void on_pre_render_game_thread(uint32_t frame_count) override;
    void on_pre_render_render_thread(uint32_t frame_count) override {};
    void on_pre_render_rhi_thread(uint32_t frame_count) override {};

    VRRuntime::Error synchronize_frame(
        std::optional<uint32_t> frame_count = std::nullopt,
        SyncFrameCallsite callsite = SyncFrameCallsite::Unknown) override;
    VRRuntime::Error fix_frame() override {
        // sync if necessary.
        VRRuntime::fix_frame();

        if (!this->frame_began) {
            this->begin_frame("runtime_fix_frame");
        }

        return VRRuntime::Error::SUCCESS;
    }
    VRRuntime::Error update_poses(bool from_view_extensions = false, uint32_t frame_count = 0) override;
    VRRuntime::Error update_render_target_size() override;
    uint32_t get_width() const override;
    uint32_t get_height() const override;
    uint32_t get_width_for_scale(float scale) const;
    uint32_t get_height_for_scale(float scale) const;

    VRRuntime::Error consume_events(std::function<void(void*)> callback) override;

    VRRuntime::Error update_matrices(float nearz, float farz) override;
    VRRuntime::Error update_input() override;

    void destroy() override;
    void enqueue_render_poses(uint32_t frame_count) override;
    void enqueue_render_poses_unsafe(uint32_t frame_count);

    std::vector<DXGI_FORMAT> get_supported_swapchain_formats() const;
    bool is_supported_swapchain_format(DXGI_FORMAT format) const {
        const auto supported = this->get_supported_swapchain_formats();

        return std::find(supported.begin(), supported.end(), format) != supported.end();
    }

public:
    // openxr quaternions are xyzw and glm is wxyz
    static glm::quat to_glm(const XrQuaternionf& q) {
    #ifndef GLM_FORCE_QUAT_DATA_XYZW
        return glm::quat{ q.w, q.x, q.y, q.z };
    #else
        return glm::quat{ q.x, q.y, q.z, q.w };
    #endif
    }

    static XrQuaternionf to_openxr(const glm::quat& q) {
        return XrQuaternionf{ q.x, q.y, q.z, q.w };
    }

    static XrVector3f to_openxr(const glm::vec3& v) {
        return XrVector3f{ v.x, v.y, v.z };
    }

public: 
    // OpenXR specific methods
    std::string get_result_string(XrResult result) const;
    std::string get_structure_string(XrStructureType type) const;
    std::string get_path_string(XrPath path) const;
    std::string get_session_state_string(XrSessionState state) const;
    XrPath get_path(const std::string& path) const;
    std::string get_current_interaction_profile() const;
    XrPath get_current_interaction_profile_path() const;

    std::optional<std::string> initialize_actions(const std::string& json_string);

    XrResult begin_frame(const char* caller = "unknown");
    XrResult end_frame(const std::vector<XrCompositionLayerBaseHeader*>& quad_layers, bool has_depth = false);
    XrResult recover_wedged_frame(const char* reason);
    bool close_synced_frame_without_layers(const char* reason);
    void prepare_resolution_scale_reconfigure(const char* reason);
    bool recover_focused_stale_frame_loop(const char* caller);
    void log_frame_lifecycle_state(const char* prefix) const;
    void trace_wait_frame_success(std::optional<uint32_t> frame_count, SyncFrameCallsite callsite);
    void trace_begin_frame_request(const char* caller);
    void clear_frame_synced(const char* reason);
    bool should_trace_frame_flow() const;
    int64_t get_pose_update_age_ms(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    VRRuntime::Error refresh_stale_pose_before_submit(uint32_t frame_count, const char* caller);
    bool is_everspace2_coherent_submit_active() const;
    void set_everspace2_d3d12_submit_active(bool active);

    void begin_profile() {
        if (!this->profile_calls) {
            return;
        }

        this->profiler_start_time = std::chrono::high_resolution_clock::now();
    }

    void end_profile(std::string_view name) {
        if (!this->profile_calls) {
            return;
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto dur = std::chrono::duration<float, std::milli>(end_time - this->profiler_start_time).count();

        spdlog::info("{} took {} ms", name, dur);
    }

    bool is_action_active(XrAction action, VRRuntime::Hand hand) const;
    bool is_action_active(std::string_view action_name, VRRuntime::Hand hand) const;
    bool is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const;
    Vector2f get_action_axis(XrAction action, VRRuntime::Hand hand) const;
    std::string translate_openvr_action_name(std::string action_name) const;

    Vector2f get_stick_axis(VRRuntime::Hand hand) const;
    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const;
    void display_bindings_editor();
    void save_bindings();

public: 
    // OpenXR specific fields
    double prediction_scale{0.0};
    bool session_ready{false};
    bool frame_began{false};
    bool profile_calls{false};

    std::chrono::high_resolution_clock::time_point profiler_start_time{};

    std::recursive_mutex sync_mtx{};
    std::recursive_mutex sync_assignment_mtx{};
    std::recursive_mutex event_mtx{};
    std::recursive_mutex swapchain_mtx{};

    // Making it static because for some reason destroying it doesn't actually completely destroy everything.
    // So we must make sure it always exists if we ever re-initialize OpenXR.
    static inline XrInstance instance{XR_NULL_HANDLE};

    XrSession session{XR_NULL_HANDLE};
    XrSpace stage_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE}; // for generating view matrices
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrFormFactor form_factor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrViewConfigurationType view_config{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrViewState stage_view_state{XR_TYPE_VIEW_STATE};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};

    XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};

    XrSpaceLocation view_space_location{XR_TYPE_SPACE_LOCATION};

    std::unordered_set<std::string> enabled_extensions{};
    std::vector<XrCompositionLayerProjection> projection_layer_cache{};

    std::vector<XrViewConfigurationView> view_configs{};
    std::unordered_map<uint32_t, Swapchain> swapchains{}; // SwapchainIndex -> Swapchain
    std::atomic_uint32_t cached_swapchain_count{};
    std::atomic_uint32_t cached_ui_swapchain_width{};
    std::atomic_uint32_t cached_ui_swapchain_height{};
    std::atomic_uint32_t cached_eye_swapchain_width{};
    std::atomic_uint32_t cached_eye_swapchain_height{};
    std::atomic_uint32_t cached_depth_swapchain_width{};
    std::atomic_uint32_t cached_depth_swapchain_height{};
    std::vector<XrView> views{};
    std::vector<XrView> stage_views{};

    //std::deque<std::vector<XrView>> stage_view_queue{};
    struct PipelineState {
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        XrSpaceLocation view_space_location{XR_TYPE_SPACE_LOCATION};
        std::vector<XrView> stage_views{};
        uint32_t frame_count{0}; // Updated on game thread prior to rendering
        uint32_t prev_frame_count{0}; // Updated right after xrWaitFrame is called
        uint32_t coherent_source_frame_count{};
        uint64_t coherent_sequence{};
        XrTime coherent_display_time{};
        std::chrono::steady_clock::time_point coherent_capture_time{};
        bool coherent_complete{};
        bool coherent_matches_render{};
    };
    /*std::array<std::vector<XrView>, 3> stage_view_queue{};
    std::array<XrSpaceLocation, 3> view_space_location_queue{};
    std::array<XrFrameState, 3> frame_state_queue{};*/

    static constexpr auto QUEUE_SIZE = 6;
    std::array<PipelineState, QUEUE_SIZE> pipeline_states{};

    auto get_stage_view(uint32_t frame_count) {
        std::scoped_lock _{ this->sync_assignment_mtx };
        
        const auto& result = pipeline_states[frame_count % QUEUE_SIZE].stage_views;

        if (result.empty()) {
            return stage_views;
        }

        return result;
    }

    auto get_current_stage_view() {
        std::scoped_lock _{ this->sync_assignment_mtx };

        return get_stage_view(internal_frame_count);
    }

    auto get_view_space_location(uint32_t frame_count) {
        std::scoped_lock _{ this->sync_assignment_mtx };

        return pipeline_states[frame_count % QUEUE_SIZE].view_space_location;
    }

    auto get_current_view_space_location() {
        std::scoped_lock _{ this->sync_assignment_mtx };

        return get_view_space_location(internal_frame_count);
    }

    auto get_frame_state(uint32_t frame_count) {
        std::scoped_lock _{ this->sync_assignment_mtx };

        const auto& result = pipeline_states[frame_count % QUEUE_SIZE].frame_state;

        if (result.predictedDisplayTime == 0) {
            return frame_state;
        }

        return result;
    }

    auto get_current_frame_state() {
        std::scoped_lock _{ this->sync_assignment_mtx };

        return get_frame_state(internal_frame_count);
    }

    bool needs_depth_resize(uint32_t w, uint32_t h) {
        std::scoped_lock _{swapchain_mtx};

        if (!is_depth_allowed()) {
            return false;
        }

        for (auto& [i, swapchain] : swapchains) {
            if (i == (uint32_t)SwapchainIndex::DEPTH ||
                i == (uint32_t)SwapchainIndex::AFR_DEPTH_LEFT_EYE ||
                i == (uint32_t)SwapchainIndex::AFR_DEPTH_RIGHT_EYE) 
            {
                if (swapchain.width != w || swapchain.height != h) {
                    return true;
                }
            }
        }

        return false;
    }

    PipelineState last_submit_state{};
    PipelineState get_submit_state();
    bool capture_everspace2_submit_snapshot(uint32_t frame_count, PipelineState& snapshot);
    bool is_everspace2_snapshot_fresh(
        const PipelineState& snapshot,
        std::chrono::steady_clock::time_point now,
        int64_t* age_ms = nullptr) const;
    void log_everspace2_coherent_submit_summary_if_needed();

    struct FrameTimingStats {
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

    void log_frame_timing_stats_if_needed();
    
    const ModSlider::Ptr resolution_scale{ ModSlider::create("OpenXR_ResolutionScale", 0.1f, 3.0f, 1.0f) };
    const ModToggle::Ptr ignore_vd_checks{ ModToggle::create("OpenXR_IgnoreVirtualDesktopChecks", false) };
    const ModToggle::Ptr debug_frame_trace{ ModToggle::create("OpenXR_DebugFrameTrace", false) };
    const ModToggle::Ptr debug_submit_empty_frame{ ModToggle::create("OpenXR_DebugSubmitEmptyFrame", false) };
    const ModToggle::Ptr debug_skip_scene_copy{ ModToggle::create("OpenXR_DebugSkipSceneCopy", false) };
    const ModToggle::Ptr debug_skip_ui_copy{ ModToggle::create("OpenXR_DebugSkipUICopy", false) };
    const ModToggle::Ptr debug_disable_depth_submit{ ModToggle::create("OpenXR_DebugDisableDepthSubmit", false) };
    const ModToggle::Ptr refresh_stale_pose_before_submit_enabled{ ModToggle::create("OpenXR_RefreshStalePoseBeforeSubmit", true) };
    bool resolution_scale_reconfigure_pending{false};
    bool resolution_scale_live_apply_deferred{false};
    float last_applied_resolution_scale{1.0f};
    uint32_t last_applied_resolution_width{};
    uint32_t last_applied_resolution_height{};
    bool push_dummy_projection{ false };
    bool ever_submitted{false};
    bool has_valid_projection_data{false};
    std::mutex everspace2_pose_capture_mtx{};
    std::atomic_bool everspace2_d3d12_submit_active{false};
    std::atomic_bool everspace2_has_real_projection_submit{false};
    uint64_t everspace2_snapshot_sequence{};
    uint64_t everspace2_exact_submit_count{};
    uint64_t everspace2_retimed_submit_count{};
    uint64_t everspace2_nearby_submit_count{};
    uint64_t everspace2_fresh_capture_submit_count{};
    uint64_t everspace2_single_frame_hold_submit_count{};
    uint64_t everspace2_rejected_submit_count{};
    int64_t everspace2_max_submit_pose_age_ms{};
    bool everspace2_single_frame_hold_used{};
    uint32_t everspace2_last_scene_frame_count{(std::numeric_limits<uint32_t>::max)()};
    std::chrono::steady_clock::time_point everspace2_last_submit_log{};
    std::chrono::steady_clock::time_point everspace2_last_summary_log{};
    uint64_t last_wait_trace_sequence{};
    uint32_t last_wait_trace_frame_count{};
    SyncFrameCallsite last_wait_trace_callsite{SyncFrameCallsite::Unknown};
    const char* last_begin_frame_caller{"none"};
    const char* last_frame_synced_clear_reason{"none"};
    std::chrono::steady_clock::time_point last_frame_synced_clear_time{};
    uint32_t frame_synced_skip_streak{0};
    std::chrono::steady_clock::time_point last_frame_synced_skip_log{};

    uint32_t frame_began_skip_streak{0};
    uint64_t frame_began_skip_suppressed_count{0};
    uint32_t forced_frame_recovery_count{0};
    uint32_t focused_frame_loop_recovery_count{0};
    bool accepted_relaxed_startup_poses{false};
    std::chrono::steady_clock::time_point last_frame_began_log{};
    std::chrono::steady_clock::time_point last_focused_frame_loop_recovery{};
    std::chrono::steady_clock::time_point last_successful_wait_frame{};
    std::chrono::steady_clock::time_point last_successful_begin_frame{};
    std::chrono::steady_clock::time_point last_successful_end_frame{};
    std::chrono::steady_clock::time_point last_successful_pose_update{};
    std::chrono::steady_clock::time_point session_ready_since{};
    std::chrono::steady_clock::time_point last_ready_state_probe_log{};
    std::chrono::steady_clock::time_point last_valid_pose_probe_log{};
    std::chrono::steady_clock::time_point last_pose_validation_failure_log{};
    std::chrono::steady_clock::time_point last_frame_timing_log{};
    std::chrono::steady_clock::time_point last_long_wait_log{};
    std::chrono::steady_clock::time_point last_slow_pose_update_log{};
    std::chrono::steady_clock::time_point last_stale_pose_skip_log{};
    std::chrono::steady_clock::time_point last_stale_pose_submit_log{};
    FrameTimingStats wait_frame_timing{};
    FrameTimingStats begin_frame_timing{};
    FrameTimingStats end_frame_timing{};
    FrameTimingStats pose_update_timing{};
    std::array<FrameTimingStats, (size_t)SyncFrameCallsite::Count> wait_frame_callsite_timing{};
    uint64_t pose_update_call_count{};
    uint64_t pose_update_view_extension_count{};
    uint64_t pose_update_non_view_extension_count{};
    uint64_t stale_pose_skip_suppressed_count{};
    uint64_t stale_pose_refresh_attempt_count{};
    uint64_t stale_pose_refresh_success_count{};
    uint64_t stale_pose_refresh_failed_count{};
    uint64_t long_wait_suppressed_count{};
    double long_wait_max_suppressed_ms{};
    uint32_t last_pose_update_frame_count{};
    bool last_pose_update_from_view_extensions{};
    int64_t last_pose_update_result{};
    double last_pose_update_ms{};
    double last_pose_view_locate_ms{};
    double last_pose_stage_locate_ms{};
    double last_pose_space_locate_ms{};
    
    Mod::ValueList options{
        *resolution_scale,
        *ignore_vd_checks,
        *debug_frame_trace,
        *debug_submit_empty_frame,
        *debug_skip_scene_copy,
        *debug_skip_ui_copy,
        *debug_disable_depth_submit,
        *refresh_stale_pose_before_submit_enabled,
    };

    enum class SwapchainIndex {
        STANDARD_START = 0,

        // Standard native stereo swapchains
        DOUBLE_WIDE = STANDARD_START,
        DEPTH,
        DUMMY_VIRTUAL_DESKTOP,
        NATIVE_STEREO_ARRAY,

        STANDARD_END,

        EXTRA_START,

        UI = EXTRA_START,
        UI_RIGHT, // For 2D view with stereoscopic
        FRAMEWORK_UI,

        EXTRA_END,

        AFR_START,

        // Swapchains when using synchronized sequential or AFR
        AFR_LEFT_EYE = AFR_START,
        AFR_RIGHT_EYE,
        AFR_DEPTH_LEFT_EYE,
        AFR_DEPTH_RIGHT_EYE,

        AFR_END,

        END = AFR_END,

        STANDARD_COUNT = STANDARD_END - STANDARD_START,
        EXTRA_COUNT = EXTRA_END - EXTRA_START,
    };

    void clear_cached_swapchain_dimensions() {
        cached_swapchain_count.store(0, std::memory_order_relaxed);
        cached_ui_swapchain_width.store(0, std::memory_order_relaxed);
        cached_ui_swapchain_height.store(0, std::memory_order_relaxed);
        cached_eye_swapchain_width.store(0, std::memory_order_relaxed);
        cached_eye_swapchain_height.store(0, std::memory_order_relaxed);
        cached_depth_swapchain_width.store(0, std::memory_order_relaxed);
        cached_depth_swapchain_height.store(0, std::memory_order_relaxed);
    }

    void cache_swapchain_dimensions(uint32_t index, int32_t width, int32_t height) {
        const auto w = width > 0 ? (uint32_t)width : 0;
        const auto h = height > 0 ? (uint32_t)height : 0;
        cached_swapchain_count.fetch_add(1, std::memory_order_relaxed);

        switch ((SwapchainIndex)index) {
        case SwapchainIndex::UI:
            cached_ui_swapchain_width.store(w, std::memory_order_relaxed);
            cached_ui_swapchain_height.store(h, std::memory_order_relaxed);
            break;
        case SwapchainIndex::DOUBLE_WIDE:
            cached_eye_swapchain_width.store(w, std::memory_order_relaxed);
            cached_eye_swapchain_height.store(h, std::memory_order_relaxed);
            break;
        case SwapchainIndex::NATIVE_STEREO_ARRAY:
            cached_eye_swapchain_width.store(w, std::memory_order_relaxed);
            cached_eye_swapchain_height.store(h, std::memory_order_relaxed);
            break;
        case SwapchainIndex::AFR_LEFT_EYE:
            if (cached_eye_swapchain_width.load(std::memory_order_relaxed) == 0 ||
                cached_eye_swapchain_height.load(std::memory_order_relaxed) == 0)
            {
                cached_eye_swapchain_width.store(w, std::memory_order_relaxed);
                cached_eye_swapchain_height.store(h, std::memory_order_relaxed);
            }
            break;
        case SwapchainIndex::DEPTH:
            cached_depth_swapchain_width.store(w, std::memory_order_relaxed);
            cached_depth_swapchain_height.store(h, std::memory_order_relaxed);
            break;
        case SwapchainIndex::AFR_DEPTH_LEFT_EYE:
            if (cached_depth_swapchain_width.load(std::memory_order_relaxed) == 0 ||
                cached_depth_swapchain_height.load(std::memory_order_relaxed) == 0)
            {
                cached_depth_swapchain_width.store(w, std::memory_order_relaxed);
                cached_depth_swapchain_height.store(h, std::memory_order_relaxed);
            }
            break;
        default:
            break;
        }
    }

    SwapchainDimensionSnapshot get_cached_swapchain_dimensions() const {
        return {
            .count = cached_swapchain_count.load(std::memory_order_relaxed),
            .ui_width = cached_ui_swapchain_width.load(std::memory_order_relaxed),
            .ui_height = cached_ui_swapchain_height.load(std::memory_order_relaxed),
            .eye_width = cached_eye_swapchain_width.load(std::memory_order_relaxed),
            .eye_height = cached_eye_swapchain_height.load(std::memory_order_relaxed),
            .depth_width = cached_depth_swapchain_width.load(std::memory_order_relaxed),
            .depth_height = cached_depth_swapchain_height.load(std::memory_order_relaxed),
        };
    }

    struct Action {
        std::vector<XrAction> action_collection{};
    };

    struct ActionSet {
        XrActionSet handle;
        std::vector<XrAction> actions{};
        std::unordered_map<std::string, XrAction> action_map{}; // XrActions are handles so it's okay.
        std::unordered_map<XrAction, std::string> action_names{};

        std::unordered_set<XrAction> float_actions{};
        std::unordered_set<XrAction> vector2_actions{};
        std::unordered_set<XrAction> bool_actions{};
        std::unordered_set<XrAction> pose_actions{};
        std::unordered_set<XrAction> vibration_actions{};
    } action_set;

    struct VectorActivator {
        Vector2f value{};
        std::string action_name{};
    };

    struct VectorActivatorTrue {
        Vector2f value{};
        XrAction action{};
    };

    struct HandData {
        XrSpace grip_space{XR_NULL_HANDLE};
        XrSpace aim_space{XR_NULL_HANDLE};
        XrPath path{XR_NULL_PATH};
        XrSpaceLocation aim_location{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity aim_velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation grip_location{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity grip_velocity{XR_TYPE_SPACE_VELOCITY};
        
        // interaction profile -> action -> path map
        struct InteractionProfile {
            std::unordered_map<std::string, XrPath> path_map{};
            std::unordered_map<XrAction, std::vector<VectorActivatorTrue>> vector_activators{};
            std::unordered_map<XrAction, XrAction> action_vector_associations{};
        };

        std::unordered_map<std::string, InteractionProfile> profiles{};
        std::unordered_map<XrAction, bool> forced_actions{};
        std::unordered_map<XrAction, bool> prev_action_states{};

        bool active{false};

        struct UI {
            char new_path_name[XR_MAX_PATH_LENGTH]{};
            uint32_t new_path_name_length{0};
            int action_combo_index{0};

            int activator_combo_index{0};
            int modifier_combo_index{0};
            int output_combo_index{0};
            Vector2f output_vector2{};
        } ui;
    };

    std::array<HandData, 2> hands{};

public:
    struct InteractionBinding {
        std::string interaction_path_name{};
        std::string action_name{};
    };

    struct ActionVectorAssociation {
        VRRuntime::Hand hand{};
        std::string action_modifier{};
        std::string action_activator{};
        std::vector<VectorActivator> vector_activators{};
    };

    static inline std::vector<InteractionBinding> s_bindings_map {
        {"/user/hand/*/input/aim/pose", "pose"},
        {"/user/hand/*/input/grip/pose", "grippose"},
        {"/user/hand/*/input/trigger", "trigger"}, // oculus?
        {"/user/hand/*/input/squeeze", "grip"}, // oculus/vive/index

        {"/user/hand/left/input/x/click", "abuttonleft"}, // oculus?
        {"/user/hand/left/input/x/touch", "abuttontouchleft"}, // oculus?

        {"/user/hand/right/input/x/click", "abuttonright"}, // oculus?
        {"/user/hand/right/input/x/touch", "abuttontouchright"}, // oculus?

        {"/user/hand/left/input/y/click", "bbuttonleft"}, // oculus?
        {"/user/hand/left/input/y/touch", "bbuttontouchleft"}, // oculus?

        {"/user/hand/right/input/y/click", "bbuttonright"}, // oculus?
        {"/user/hand/right/input/y/touch", "bbuttontouchright"}, // oculus?

        {"/user/hand/left/input/a/click", "abuttonleft"}, // oculus?
        {"/user/hand/left/input/a/touch", "abuttontouchleft"}, // oculus?

        {"/user/hand/right/input/a/click", "abuttonright"}, // oculus?
        {"/user/hand/right/input/a/touch", "abuttontouchright"}, // oculus?

        {"/user/hand/left/input/b/click", "bbuttonleft"}, // oculus?
        {"/user/hand/left/input/b/touch", "bbuttontouchleft"}, // oculus?

        {"/user/hand/right/input/b/click", "bbuttonright"}, // oculus?
        {"/user/hand/right/input/b/touch", "bbuttontouchright"}, // oculus?

        {"/user/hand/*/input/thumbstick", "joystick"}, // oculus?
        {"/user/hand/*/input/thumbstick/click", "joystickclick"}, // oculus?
        {"/user/hand/*/input/system/click", "systembutton"}, // oculus/vive/index
        {"/user/hand/*/input/menu/click", "systembutton"}, // oculus/vive/index

        {"/user/hand/left/input/thumbrest/touch", "thumbresttouchleft"}, // cv1/quest pro
        {"/user/hand/right/input/thumbrest/touch", "thumbresttouchright"}, // cv1/quest pro

        {"/user/hand/*/input/trackpad", "touchpad"}, // vive & others
        {"/user/hand/*/input/trackpad/click", "touchpadclick"}, // vive & others
        {"/user/hand/*/output/haptic", "haptic"}, // most of them
    };

    static inline std::vector<ActionVectorAssociation> s_action_vector_associations {
        { 
            VRRuntime::Hand::LEFT, "touchpad", "touchpadclick", {
            { {0.0, -1.0f}, "abuttonleft" },
            { {1.0f, 0.0f}, "bbuttonleft" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
        { 
            VRRuntime::Hand::RIGHT, "touchpad", "touchpadclick", {
            { {0.0, -1.0f}, "abuttonright" },
            { {-1.0f, 0.0f}, "bbuttonright" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
    };

    static inline std::vector<std::string> s_supported_controllers {
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/oculus/go_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/microsoft/motion_controller",
        "/interaction_profiles/htc/vive_controller",
    };

};
}
