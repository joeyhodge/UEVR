#pragma once

#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <optional>
#include <vector>

#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <wrl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <../../directxtk12-src/Inc/GraphicsMemory.h>
#include <../../directxtk12-src/Inc/SpriteBatch.h>
#include <../../directxtk12-src/Inc/DescriptorHeap.h>

#include "d3d12/CommandContext.hpp"
#include "d3d12/DIBRPreview.hpp"
#include "d3d12/TextureContext.hpp"

class VR;
namespace render {
class FrameResourceInspector;
}

namespace vrmod {
class D3D12Component {
public:
    D3D12Component() 
        : m_openvr{this}
    {

    }

    vr::EVRCompositorError on_frame(VR* vr);
    void on_post_present(VR* vr);
    void on_reset(VR* vr);

    void force_reset() { m_force_reset = true; }

    const auto& get_backbuffer_size() const { return m_backbuffer_size; }

    auto is_initialized() const { return m_openvr.left_eye_tex[0].texture != nullptr; }
    const auto& get_last_on_frame_time() const { return m_last_on_frame; }

    auto& openxr() { return m_openxr; }
    auto& get_openvr_ui_tex() { return m_openvr.ui_tex; }

    struct HitchFrameSnapshot {
        bool initialized{};
        bool force_reset{};
        bool last_afr_state{};
        bool has_prev_backbuffer{};
        bool has_game_tex{};
        bool has_ui_tex{};
        bool has_scene_capture_tex{};
        uint32_t backbuffer_width{};
        uint32_t backbuffer_height{};
        uint32_t ui_extent_width{};
        uint32_t ui_extent_height{};
        uint32_t hmd_width{};
        uint32_t hmd_height{};
        uint32_t openxr_swapchain_count{};
        uint32_t ui_swapchain_width{};
        uint32_t ui_swapchain_height{};
        uint32_t eye_swapchain_width{};
        uint32_t eye_swapchain_height{};
        uint32_t depth_swapchain_width{};
        uint32_t depth_swapchain_height{};
        uint64_t swapchain_recreate_count{};
        uint32_t last_swapchain_recreate_reasons{};
        uint64_t perf_on_frame_count{};
        double perf_on_frame_avg_ms{};
        double perf_on_frame_max_ms{};
        uint64_t perf_ui_copy_count{};
        double perf_ui_copy_avg_ms{};
        double perf_ui_copy_max_ms{};
        uint64_t perf_swapchain_copy_count{};
        double perf_swapchain_copy_avg_ms{};
        double perf_swapchain_copy_max_ms{};
        uint64_t perf_openxr_submit_count{};
        double perf_openxr_submit_avg_ms{};
        double perf_openxr_submit_max_ms{};
    };

    HitchFrameSnapshot get_hitch_frame_snapshot(VR* vr) const;
    bool has_game_and_ui_textures() const;
    const char* get_dibr_preview_status() const { return m_dibr_slots[0].preview.status_name(); }
    std::string get_dibr_preview_failure_reason() const { return m_dibr_slots[0].preview.failure_reason(); }
    std::string get_dibr_preview_depth_trace_summary() const { return m_dibr_depth_capture.depth_trace_summary(); }

    struct DIBRSingleViewReadiness {
        uint64_t generation{};
        uint32_t consecutive_ready_frames{};
        uint32_t source_width{};
        uint32_t source_height{};
        bool preview_ready{};
    };

    DIBRSingleViewReadiness get_dibr_single_view_readiness() const;

private:
    friend class render::FrameResourceInspector;

    bool setup();
    std::unique_ptr<DirectX::DX12::SpriteBatch> setup_sprite_batch_pso(
        DXGI_FORMAT output_format, 
        std::span<const uint8_t> vs = {}, std::span<const uint8_t> ps = {},
        std::optional<DirectX::SpriteBatchPipelineStateDescription> pd = std::nullopt
    );

    void draw_spectator_view(
        ID3D12GraphicsCommandList* command_list,
        bool is_right_eye_frame,
        d3d12::TextureContext* game_tex_override = nullptr,
        std::optional<D3D12_RESOURCE_STATES> game_tex_state = std::nullopt,
        bool prefer_left_eye = false,
        bool source_is_single_eye = false,
        d3d12::TextureContext* ui_tex_override = nullptr);
    bool ensure_ue58_spectator_texture(ID3D12Device* device, ID3D12Resource* source);
    void reset_ue58_converted_ui_textures(bool reset_sources = true);
    bool ensure_ue58_slate_ui_consumer_fence(ID3D12Device* device);
    bool is_ue58_converted_ui_slot_reusable(uint32_t slot_index);
    void release_ue58_converted_ui_source_slot(uint32_t converted_slot_index);
    d3d12::TextureContext* acquire_ue58_slate_ui_source_slot(
        ID3D12Device* device,
        ID3D12Resource* resource,
        DXGI_FORMAT view_format);
    void mark_ue58_converted_ui_slot_consumed(uint32_t slot_index);
    void wait_for_ue58_slate_ui_consumers();
    void clear_backbuffer();
    bool ensure_2d_screen_textures(ID3D12Device* device, const D3D12_RESOURCE_DESC& base_desc);
    bool ensure_halo_electra_quad_source_texture(ID3D12Device* device, uint64_t width, uint32_t height);

    enum class ShfSceneMode {
        Unknown,
        Stereo3D,
        Mono2D,
    };

    static const char* shf_scene_mode_name(ShfSceneMode mode);
    ShfSceneMode classify_shf_scene_mode(const D3D12_RESOURCE_DESC& source_desc, const D3D12_RESOURCE_DESC& real_desc) const;
    void log_shf_scene_mode_if_needed(
        ShfSceneMode mode,
        const D3D12_RESOURCE_DESC& source_desc,
        const D3D12_RESOURCE_DESC& real_desc,
        uint64_t frame_count,
        bool using_mono_expansion);
    bool ensure_shf_mono_scene_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc);
    d3d12::TextureContext* render_shf_mono_scene_texture(ID3D12Device* device);
    bool run_dibr_preview(
        VR* vr,
        ID3D12Device* device,
        ID3D12Resource* scene_color,
        D3D12_RESOURCE_STATES scene_color_state,
        ID3D12Resource* scene_depth,
        D3D12_RESOURCE_STATES scene_depth_state);
    bool ensure_dibr_present_texture(d3d12::TextureContext& texture, ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc);
    bool capture_dibr_ui_alpha_snapshot(
        ID3D12Device* device,
        d3d12::CommandContext& commands,
        ID3D12Resource* submitted_ui_texture);
    void reset_dibr_preview();
    void note_dibr_single_view_preview_result(bool success, const D3D12_RESOURCE_DESC* source_desc = nullptr);
    bool ensure_dune_hmd_mono_scene_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc);
    d3d12::TextureContext* render_dune_hmd_mono_scene_texture(
        ID3D12Device* device,
        D3D12_RESOURCE_STATES source_state);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

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

    void log_frame_timing_stats_if_needed(VR* vr);
    void log_openxr_swapchain_recreate(VR* vr, uint32_t reasons, uint32_t new_depth_width = 0, uint32_t new_depth_height = 0);

    enum class DepthCandidateDecision {
        Use,
        Defer,
        Reject,
        ResizeReady,
    };

    void sync_depth_target_stability_guard_state(VR* vr);
    void clear_depth_target_stability_candidates(bool clear_stable);
    DepthCandidateDecision evaluate_depth_candidate(VR* vr, const D3D12_RESOURCE_DESC& desc);

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    std::array<d3d12::CommandContext, 3> m_generic_commands{};
    std::chrono::steady_clock::time_point m_last_on_frame{};
    std::chrono::steady_clock::time_point m_last_frame_timing_log{};
    FrameTimingStats m_perf_on_frame{};
    FrameTimingStats m_perf_ui_copy{};
    FrameTimingStats m_perf_swapchain_copy{};
    FrameTimingStats m_perf_openxr_submit{};
    FrameTimingStats m_perf_spectator_mirror{};
    FrameTimingStats m_perf_post_present{};

    d3d12::TextureContext m_backbuffer_copy{};

    d3d12::TextureContext m_game_ui_tex{};
    static constexpr uint32_t UE58_CONVERTED_UI_SLOT_COUNT = 3;
    // Slate can rotate native D3D12 resources between frames. Keep each source
    // SRV alive until the conversion command list that binds it has completed.
    static constexpr uint32_t UE58_SLATE_UI_SOURCE_SLOT_COUNT = UE58_CONVERTED_UI_SLOT_COUNT + 1;
    struct UE58SlateUiSourceSlot {
        d3d12::TextureContext texture{};
        uint32_t conversion_references{};
    };
    std::array<UE58SlateUiSourceSlot, UE58_SLATE_UI_SOURCE_SLOT_COUNT> m_ue58_ui_source_slots{};
    std::array<d3d12::TextureContext, UE58_CONVERTED_UI_SLOT_COUNT> m_ue58_converted_ui_tex{};
    uint32_t m_ue58_converted_ui_slot_cursor{};
    d3d12::TextureContext* m_ue58_active_converted_ui_tex{};
    uint32_t m_ue58_active_converted_ui_slot{UE58_CONVERTED_UI_SLOT_COUNT};
    std::array<int32_t, UE58_CONVERTED_UI_SLOT_COUNT> m_ue58_converted_ui_source_slots{ -1, -1, -1 };
    std::array<uint64_t, UE58_CONVERTED_UI_SLOT_COUNT> m_ue58_converted_ui_consumer_fence_values{};
    ComPtr<ID3D12Fence> m_ue58_converted_ui_consumer_fence{};
    uint64_t m_ue58_converted_ui_consumer_fence_value{};
    d3d12::TextureContext m_game_tex{};
    d3d12::TextureContext m_ue58_spectator_tex{};
    d3d12::TextureContext m_scene_capture_tex{};
    d3d12::TextureContext m_shf_mono_scene_tex{};
    static constexpr uint32_t DIBR_FRAME_SLOT_COUNT = 3;
    struct DIBRFrameSlot {
        d3d12::TextureContext present_tex{};
        d3d12::DIBRPreview preview{};
        d3d12::CommandContext commands{};
    };

    // Capture runs every frame. Each frame slot has independent source,
    // output, descriptors and command allocator so one slow DIBR dispatch
    // cannot stall the CPU before the next frame is recorded.
    d3d12::DIBRPreview m_dibr_depth_capture{};
    std::array<DIBRFrameSlot, DIBR_FRAME_SLOT_COUNT> m_dibr_slots{};
    uint32_t m_dibr_slot_cursor{};
    d3d12::TextureContext* m_dibr_active_present_tex{};
    // Owned copy of the OpenXR UI swapchain image. It exists solely for the
    // opt-in DIBR Single View UI-edge guard and is never submitted or rendered
    // back into the UI path.
    ComPtr<ID3D12Resource> m_dibr_ui_alpha_snapshot{};
    // A resize can replace the snapshot while an older DIBR command list is
    // still queued. Retain replaced resources until the DIBR ring is drained.
    std::vector<ComPtr<ID3D12Resource>> m_dibr_retired_ui_alpha_snapshots{};
    uint64_t m_dibr_ui_alpha_snapshot_width{};
    uint32_t m_dibr_ui_alpha_snapshot_height{};
    DXGI_FORMAT m_dibr_ui_alpha_snapshot_format{DXGI_FORMAT_UNKNOWN};
    bool m_dibr_ui_alpha_captured_this_frame{};
    std::atomic<uint64_t> m_dibr_single_view_generation{1};
    std::atomic<uint64_t> m_dibr_single_view_source_signature{};
    std::atomic<uint32_t> m_dibr_single_view_ready_frames{};
    std::atomic<uint32_t> m_dibr_single_view_source_width{};
    std::atomic<uint32_t> m_dibr_single_view_source_height{};
    std::atomic<bool> m_dibr_single_view_preview_ready{};
    d3d12::TextureContext m_dune_hmd_mono_scene_tex{};
    d3d12::TextureContext m_halo_electra_quad_source_tex{};
    std::array<d3d12::CommandContext, 3> m_game_tex_commands{};
    d3d12::CommandContext m_shf_mono_scene_commands{};
    d3d12::CommandContext m_dune_hmd_mono_scene_commands{};
    uint64_t m_shf_mono_scene_width{};
    uint32_t m_shf_mono_scene_height{};
    DXGI_FORMAT m_shf_mono_scene_format{DXGI_FORMAT_UNKNOWN};
    uint64_t m_dune_hmd_mono_scene_width{};
    uint32_t m_dune_hmd_mono_scene_height{};
    DXGI_FORMAT m_dune_hmd_mono_scene_format{DXGI_FORMAT_UNKNOWN};
    std::array<d3d12::TextureContext, 2> m_2d_screen_tex{};
    std::vector<std::unique_ptr<d3d12::TextureContext>> m_backbuffer_textures{};
    bool m_skip_spectator_view_for_volatile_external_rt{};
    ShfSceneMode m_shf_scene_mode{ShfSceneMode::Unknown};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_backbuffer_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_game_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ui_batch_alpha_invert{};

    ID3D12Resource* m_last_checked_native{nullptr};

    // Mimicking what OpenXR does.
    struct OpenVR {
        OpenVR(D3D12Component* p) : parent{p} {}
        
        d3d12::TextureContext& get_left() {
            auto& ctx = this->left_eye_tex[this->texture_counter % left_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& get_right() {
            auto& ctx = this->right_eye_tex[this->texture_counter % right_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& acquire_left() {
            auto& ctx = get_left();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        d3d12::TextureContext& acquire_right() {
            auto& ctx = get_right();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        void copy_left(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_left();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the left half of the backbuffer to the left eye texture.
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0] / 2;
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_right();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the right half of the backbuffer to the right eye texture.
            D3D12_BOX src_box{};
            src_box.left = parent->m_backbuffer_size[0] / 2;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0];
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }
        
        // For AFR
        void copy_left_to_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_right();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the right half of the backbuffer to the right eye texture.
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0] / 2;
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        std::array<d3d12::TextureContext, 3> left_eye_tex{};
        std::array<d3d12::TextureContext, 3> right_eye_tex{};
        d3d12::TextureContext ui_tex{};
        uint32_t texture_counter{0};
        D3D12Component* parent{};

        friend class D3D12Component;
    } m_openvr;

    struct OpenXR {
        void initialize(XrSessionCreateInfo& session_info);
        std::optional<std::string> create_swapchains();
        void destroy_swapchains();
        bool pre_acquire(uint32_t swapchain_idx);
        void release_acquired(uint32_t swapchain_idx);
        void copy(uint32_t swapchain_idx, ID3D12Resource* src,
            std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands = std::nullopt,
            std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands = std::nullopt,
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
            D3D12_BOX* src_box = nullptr,
            std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> post_copy_commands = std::nullopt);

        void copy(uint32_t swapchain_idx, ID3D12Resource* src,
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr)
        {
            this->copy(swapchain_idx, src, std::nullopt, std::nullopt, src_state, src_box);
        }
        void retire_framework_ui_delayed_release(bool force_wait = false);
        void copy_framework_ui_ue58(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT);
        void wait_for_all_copies() {
            std::scoped_lock _{this->mtx};

            for (auto& it : this->contexts) {
                if (it.second.num_textures_acquired > 0) {
                    release_acquired(it.first);
                }

                for (auto& texture_ctx : it.second.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }
            }
        }

        bool ever_acquired(uint32_t swapchain_idx) {
            std::scoped_lock _{this->mtx};

            auto it = this->contexts.find(swapchain_idx);
            if (it == this->contexts.end()) {
                return false;
            }

            return it->second.ever_acquired;
        }

        uint32_t get_last_acquired_frame(uint32_t swapchain_idx) {
            std::scoped_lock _{this->mtx};

            auto it = this->contexts.find(swapchain_idx);
            if (it == this->contexts.end()) {
                return 0;
            }

            return it->second.last_acquired_frame;
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
            uint32_t last_acquired_texture{0};
            uint32_t last_acquired_frame{0};
            bool ever_acquired{false};
            bool pre_acquired{false};
            bool framework_ui_pending_release{false};
            uint32_t framework_ui_pending_texture{0};
            uint32_t framework_ui_pending_frame{0};
            uint64_t framework_ui_stale_reuse_count{0};
            uint64_t framework_ui_recovery_wait_count{0};
            bool framework_ui_has_released_texture{false};
            uint32_t framework_ui_last_released_texture{0};
            uint32_t framework_ui_last_release_frame{0};
            uint64_t framework_ui_front_buffer_skip_count{0};
        };

        std::unordered_map<uint32_t, SwapchainContext> contexts{};
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
        bool made_depth_with_null_defaults{false};
        D3D12_RESOURCE_DESC stable_depth_desc{};
        bool has_stable_depth_desc{};

        friend class D3D12Component;
    } m_openxr;

    uint32_t m_backbuffer_size[2]{};

    uint32_t m_last_rendered_frame{0};
    bool m_force_reset{true};
    bool m_last_afr_state{false};
    bool m_submitted_left_eye{false};
    bool m_dibr_was_active{};
    uint64_t m_swapchain_recreate_count{};
    uint32_t m_last_swapchain_recreate_reasons{};
    D3D12_RESOURCE_DESC m_pending_depth_desc{};
    bool m_has_pending_depth_desc{};
    uint32_t m_pending_depth_frames{};
    std::chrono::steady_clock::time_point m_pending_depth_since{};
    bool m_depth_target_stability_guard_was_active{};
};
} // namespace vrmod
