#pragma once

#include <span>

#include <algorithm>
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
#include "d3d12/TextureContext.hpp"

class VR;

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

    auto& openxr() { return m_openxr; }
    auto& get_openvr_ui_tex() { return m_openvr.ui_tex; }

private:
    bool setup();
    std::unique_ptr<DirectX::DX12::SpriteBatch> setup_sprite_batch_pso(
        DXGI_FORMAT output_format, 
        std::span<const uint8_t> vs = {}, std::span<const uint8_t> ps = {},
        std::optional<DirectX::SpriteBatchPipelineStateDescription> pd = std::nullopt
    );

    void draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame);
    void clear_backbuffer();

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    std::array<d3d12::CommandContext, 3> m_generic_commands{};

    d3d12::TextureContext m_backbuffer_copy{};

    d3d12::TextureContext m_game_ui_tex{};
    d3d12::TextureContext m_game_tex{};
    d3d12::TextureContext m_scene_capture_tex{};
    std::array<d3d12::CommandContext, 3> m_game_tex_commands{};
    std::array<d3d12::TextureContext, 2> m_2d_screen_tex{};
    std::vector<std::unique_ptr<d3d12::TextureContext>> m_backbuffer_textures{};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_backbuffer_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_game_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ui_batch_alpha_invert{};
    DXGI_FORMAT m_game_batch_format{DXGI_FORMAT_UNKNOWN};

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
            if (src == nullptr) {
                return;
            }

            auto& ctx = this->acquire_left();

            const auto src_desc = src->GetDesc();
            const auto src_width = (uint32_t)src_desc.Width;
            const auto src_height = (uint32_t)src_desc.Height;
            const auto expected_width = parent->m_backbuffer_size[0];
            const auto tolerance = expected_width > 0 ? expected_width / 10 : 0;
            const auto is_double_wide = expected_width > 0 &&
                src_width + tolerance >= expected_width &&
                src_width >= (expected_width > tolerance ? expected_width - tolerance : expected_width);
            const auto eye_width = is_double_wide ? src_width / 2 : src_width;
            const auto dst_desc = ctx.texture->GetDesc();
            const auto copy_width = std::min<uint32_t>(eye_width, (uint32_t)dst_desc.Width);
            const auto copy_height = std::min<uint32_t>(src_height, (uint32_t)dst_desc.Height);

            if (copy_width == 0 || copy_height == 0) {
                return;
            }

            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = copy_width;
            src_box.bottom = copy_height;
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            if (src == nullptr) {
                return;
            }

            auto& ctx = this->acquire_right();

            const auto src_desc = src->GetDesc();
            const auto src_width = (uint32_t)src_desc.Width;
            const auto src_height = (uint32_t)src_desc.Height;
            const auto expected_width = parent->m_backbuffer_size[0];
            const auto tolerance = expected_width > 0 ? expected_width / 10 : 0;
            const auto is_double_wide = expected_width > 0 &&
                src_width + tolerance >= expected_width &&
                src_width >= (expected_width > tolerance ? expected_width - tolerance : expected_width);
            const auto eye_width = is_double_wide ? src_width / 2 : src_width;
            const auto dst_desc = ctx.texture->GetDesc();
            const auto src_left = is_double_wide ? eye_width : 0u;
            const auto available_width = src_width > src_left ? src_width - src_left : 0u;
            const auto copy_width = std::min<uint32_t>(available_width, (uint32_t)dst_desc.Width);
            const auto copy_height = std::min<uint32_t>(src_height, (uint32_t)dst_desc.Height);

            if (copy_width == 0 || copy_height == 0) {
                return;
            }

            D3D12_BOX src_box{};
            src_box.left = src_left;
            src_box.top = 0;
            src_box.right = src_left + copy_width;
            src_box.bottom = copy_height;
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }
        
        // For AFR
        void copy_left_to_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            if (src == nullptr) {
                return;
            }

            auto& ctx = this->acquire_right();

            const auto src_desc = src->GetDesc();
            const auto src_width = (uint32_t)src_desc.Width;
            const auto src_height = (uint32_t)src_desc.Height;
            const auto expected_width = parent->m_backbuffer_size[0];
            const auto tolerance = expected_width > 0 ? expected_width / 10 : 0;
            const auto is_double_wide = expected_width > 0 &&
                src_width + tolerance >= expected_width &&
                src_width >= (expected_width > tolerance ? expected_width - tolerance : expected_width);
            const auto eye_width = is_double_wide ? src_width / 2 : src_width;
            const auto dst_desc = ctx.texture->GetDesc();
            const auto copy_width = std::min<uint32_t>(eye_width, (uint32_t)dst_desc.Width);
            const auto copy_height = std::min<uint32_t>(src_height, (uint32_t)dst_desc.Height);

            if (copy_width == 0 || copy_height == 0) {
                return;
            }

            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = copy_width;
            src_box.bottom = copy_height;
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
        d3d12::TextureContext* find_texture_context(ID3D12Resource* resource);
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

        bool submitted_this_frame(uint32_t swapchain_idx) {
            std::scoped_lock _{this->mtx};

            auto it = this->contexts.find(swapchain_idx);
            if (it == this->contexts.end()) {
                return false;
            }

            return it->second.submitted_this_frame;
        }

        void begin_frame_submission() {
            std::scoped_lock _{this->mtx};

            for (auto& [_, ctx] : this->contexts) {
                ctx.submitted_this_frame = false;
            }
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
            uint32_t last_acquired_texture{0};
            bool ever_acquired{false};
            bool submitted_this_frame{false};
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
    bool m_last_frame_used_real_backbuffer_source{false};
    d3d12::TextureContext m_guarded_scene_source_tex{};
    ComPtr<ID3D12Resource> m_guarded_last_scene_source{};
    DXGI_FORMAT m_guarded_scene_source_srv_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT m_guarded_sticky_openxr_copy_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_guarded_sticky_openxr_copy_width{0};
    uint32_t m_guarded_sticky_openxr_copy_height{0};
};
} // namespace vrmod
