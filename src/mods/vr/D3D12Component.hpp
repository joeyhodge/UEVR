#pragma once

#include <span>
#include <atomic>
#include <chrono>

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

    void draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame, d3d12::TextureContext* game_tex_override = nullptr);
    void clear_backbuffer();
    bool ensure_2d_screen_textures(ID3D12Device* device, const D3D12_RESOURCE_DESC& base_desc);

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
    void reset_dibr_preview();
    void note_dibr_single_view_preview_result(bool success, const D3D12_RESOURCE_DESC* source_desc = nullptr);

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
    d3d12::TextureContext m_game_tex{};
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
    std::atomic<uint64_t> m_dibr_single_view_generation{1};
    std::atomic<uint64_t> m_dibr_single_view_source_signature{};
    std::atomic<uint32_t> m_dibr_single_view_ready_frames{};
    std::atomic<uint32_t> m_dibr_single_view_source_width{};
    std::atomic<uint32_t> m_dibr_single_view_source_height{};
    std::atomic<bool> m_dibr_single_view_preview_ready{};
    std::array<d3d12::CommandContext, 3> m_game_tex_commands{};
    d3d12::CommandContext m_shf_mono_scene_commands{};
    uint64_t m_shf_mono_scene_width{};
    uint32_t m_shf_mono_scene_height{};
    DXGI_FORMAT m_shf_mono_scene_format{DXGI_FORMAT_UNKNOWN};
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
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr);

        void copy(uint32_t swapchain_idx, ID3D12Resource* src,
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr)
        {
            this->copy(swapchain_idx, src, std::nullopt, std::nullopt, src_state, src_box);
        }
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
        };

        std::unordered_map<uint32_t, SwapchainContext> contexts{};
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
        bool made_depth_with_null_defaults{false};

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
};
} // namespace vrmod
