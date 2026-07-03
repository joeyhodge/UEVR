#pragma once

#include <memory>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include <SafetyHook.hpp>

#include <utility/PointerHook.hpp>

#include <sdk/StereoStuff.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/threading/ThreadWorker.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/UObjectReference.hpp>
#include <sdk/AActor.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/DynamicRHI.hpp>

#include "IXRTrackingSystemHook.hpp"
#include "UE57SlateSymbols.hpp"

#include "Mod.hpp"

struct FRHICommandListImmediate;
struct FRDGBuilder;
struct FRDGTexture;
struct VRRenderTargetManager_418;
struct UCanvas;
struct IStereoLayers;

namespace sdk {
struct FSceneViewStateInterface;
class FViewport;
class FCanvas;
class UGameViewportClient;
class AActor;
class UObject;
class USceneCaptureComponent2D;
class UTexture;
class FSceneViewFamily;
class FSceneView;
}

// Injector-specific structure for VRRenderTargetManager that they will all secondarily inherit from
// because different engine versions can have a different IStereoRenderTargetManager virtual table
// so we need a unified way of storing data that can be used for all versions
struct VRRenderTargetManager_Base {
public:
    struct Everspace2D3D12SceneTargetSnapshot {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource{};
        D3D12_RESOURCE_DESC desc{};
        uintptr_t source_texture{};
        uint64_t generation{};
        const char* source{};
    };

    bool allocate_render_target_texture(uintptr_t return_address, FTexture2DRHIRef* tex, FTexture2DRHIRef* shader_resource);

    uint32_t get_number_of_buffered_frames() const { return 1; }

    bool should_use_separate_render_target() const;

    void update_viewport(bool use_separate_rt, const sdk::FViewport& vp, class SViewport* vp_widget = nullptr);

    void calculate_render_target_size(const sdk::FViewport& viewport, uint32_t& x, uint32_t& y);
    bool need_reallocate_view_target(const sdk::FViewport& Viewport);
    bool need_reallocate_depth_texture(const void* DepthTarget);

public:
    FRHITexture2D* get_ui_target() {
        auto& dedicated = static_cast<FRHITexture2D*&>(dedicated_ui_target);

        if (dedicated != nullptr) {
            return dedicated;
        }

        return static_cast<FRHITexture2D*&>(ui_target);
    }

    FRHITexture2D*& get_effective_ui_target_ref() {
        auto& dedicated = static_cast<FRHITexture2D*&>(dedicated_ui_target);

        if (dedicated != nullptr) {
            return dedicated;
        }

        return static_cast<FRHITexture2D*&>(ui_target);
    }

    FRHITexture2D*& get_fallback_ui_target_ref() {
        return static_cast<FRHITexture2D*&>(ui_target);
    }

    FRHITexture2D* get_dedicated_ui_target() {
        return static_cast<FRHITexture2D*&>(dedicated_ui_target);
    }

    bool has_dedicated_ui_target() {
        return get_dedicated_ui_target() != nullptr;
    }

    uint32_t get_dedicated_ui_width() const {
        return dedicated_ui_width;
    }

    uint32_t get_dedicated_ui_height() const {
        return dedicated_ui_height;
    }

    FRHITexture2D* get_render_target() {
        return render_target; 
    }

    std::shared_ptr<const Everspace2D3D12SceneTargetSnapshot> get_everspace2_scene_target_snapshot() const {
        return everspace2_scene_target_snapshot.load(std::memory_order_acquire);
    }

    bool publish_everspace2_scene_target_snapshot(
        FRHITexture2D* source_texture,
        ID3D12Resource* resource,
        const D3D12_RESOURCE_DESC& desc,
        const char* source);

    FRHITexture2D* get_scene_capture_render_target();
    void set_render_target(FRHITexture2D* rt) { render_target = rt; }
    void set_dedicated_ui_target(FRHITexture2D* rt, uint32_t width = 0, uint32_t height = 0);
    void request_dedicated_ui_target(uint32_t width, uint32_t height);
    void destroy_dedicated_ui_target();
    void cancel_dedicated_ui_creation_preserving_target(const char* reason = nullptr);
    void invalidate_resolution_dependent_targets();
    void ensure_dedicated_ui_target(uintptr_t command_list);
    bool create_dedicated_ui_texture();
    bool try_schedule_dedicated_ui_creation();
    bool can_attempt_dedicated_ui_creation() const;
    void reset_dedicated_ui_creation_state();
    bool is_dedicated_ui_generation_current(uint64_t generation) const {
        return in_flight_dedicated_ui_generation == generation;
    }
    bool is_dedicated_ui_target_pending() const {
        return dedicated_ui_creation_pending || in_flight_dedicated_ui_texture != nullptr || in_flight_dedicated_ui_generation != 0;
    }

    bool is_ue_5_0_3() const { return is_version_5_0_3; }

    const std::optional<size_t>& get_viewport_force_separate_rt_offset() const { 
        return m_viewport_force_separate_rt_offset; 
    }

    bool create_scene_capture();
    void destroy_scene_capture();

    sdk::UTexture* get_scene_capture_utexture();
    
    sdk::FViewport* get_viewport() const {
        return last_viewport;
    }

    void set_viewport(sdk::FViewport* vp) {
        last_viewport = vp;
    }

protected:
    void retain_everspace2_dedicated_ui_target(FRHITexture2D* rt);

    struct VerifiedFTexture2D {
        VerifiedFTexture2D() = default;
        VerifiedFTexture2D(FRHITexture2D* tex) 
            : texture{tex}
        {
            if (tex != nullptr) {
                original_vtable = *(void**)tex;
            } else {
                original_vtable = nullptr;
            }
        }

        VerifiedFTexture2D& operator=(FRHITexture2D* tex) {
            texture = tex;
            if (tex != nullptr) {
                original_vtable = *(void**)tex;
            } else {
                original_vtable = nullptr;
            }

            return *this;
        }

        operator FRHITexture2D*&() try {
            if (texture == nullptr) {
                return texture;
            }

            // First line of defense against catching an exception
            if (original_vtable != *(void**)texture) {
                texture = nullptr;
                original_vtable = nullptr;
            }

            return texture;
        } catch (...) {
            // welp
            texture = nullptr;
            original_vtable = nullptr;
            return texture;
        }

        FRHITexture2D* texture{nullptr};
        void* original_vtable{nullptr};
    };

    VerifiedFTexture2D ui_target{};
    VerifiedFTexture2D dedicated_ui_target{};
    VerifiedFTexture2D render_target{};
    static void pre_texture_hook_callback(safetyhook::Context& ctx, bool from_second = false); // only used if pixel format cvar is missing
    static void texture_hook_callback(safetyhook::Context& ctx, bool from_second = false);

    FTexture2DRHIRef* texture_hook_ref{nullptr};
    FTexture2DRHIRef* shader_resource_hook_ref{nullptr};
    safetyhook::MidHook pre_texture_hook{}; // only used if pixel format cvar is missing
    safetyhook::MidHook pre_texture_hook2{}; // only used if pixel format cvar is missing
    safetyhook::MidHook texture_hook{};
    safetyhook::MidHook texture_hook2{};
    uint32_t last_texture_index{0};
    bool allocated_views{false};
    bool set_up_texture_hook{false};
    bool is_pre_texture_call_e8{false};
    bool is_using_texture_desc{false};
    bool is_version_greq_5_1{false};
    bool is_version_5_0_3{false};
    bool wants_depth_reallocate{false};
    bool allocate_texture_called{false}; // used to determine if the pretexture hook should go ahead

    uint32_t last_width{0};
    uint32_t last_height{0};

    uintptr_t texture_desc_prepare_func{0};
    uintptr_t texture_create_wrapper_func{0};
    uintptr_t texture_release_func{0};
    uintptr_t texture_finalize_func{0};
    uintptr_t texture_extract_func{0};
    uintptr_t texture_finalize_callsite{0};
    uintptr_t texture_extract_callsite{0};

    std::vector<uint8_t> texture_create_insn_bytes{};
    std::vector<uint8_t> texture_create_insn_bytes2{};

    std::optional<size_t> m_viewport_force_separate_rt_offset{};
    bool m_attempted_find_force_separate_rt{false};

    sdk::UObjectReference<sdk::AActor> scene_capture_actor{nullptr};
    sdk::UObjectReference<sdk::USceneCaptureComponent2D> scene_capture_component{nullptr};
    sdk::UObjectReference<sdk::UTexture> scene_capture_target{nullptr}; // For custom compatibility rendering
    sdk::UObjectReference<sdk::UTexture> scene_capture_target_rhi_thread{nullptr}; // For custom compatibility rendering
    sdk::UTexture* in_flight_target{nullptr}; // Not a reference because this is basically a barrier against creating a new scene capture target
    sdk::UObjectReference<sdk::UTexture> dedicated_ui_texture{nullptr};
    sdk::UTexture* in_flight_dedicated_ui_texture{nullptr};
    std::unique_ptr<FTexture2DRHIRef> owned_dedicated_ui_target{};
    std::mutex everspace2_dedicated_ui_lifetime_mutex{};
    std::vector<FRHITexture2D*> everspace2_retained_dedicated_ui_targets{};
    uint32_t dedicated_ui_width{0};
    uint32_t dedicated_ui_height{0};
    std::chrono::steady_clock::time_point dedicated_ui_last_attempt{};
    std::chrono::steady_clock::time_point dedicated_ui_pending_since{};
    std::chrono::steady_clock::time_point dedicated_ui_resource_pending_since{};
    bool dedicated_ui_creation_pending{false};
    bool dedicated_ui_object_created{false};
    uint64_t dedicated_ui_generation{0};
    uint64_t in_flight_dedicated_ui_generation{0};
    sdk::FViewport* last_viewport{nullptr};
    std::atomic<std::shared_ptr<const Everspace2D3D12SceneTargetSnapshot>> everspace2_scene_target_snapshot{};
    std::atomic<uint64_t> everspace2_scene_target_generation{};
};

struct VRRenderTargetManager : IStereoRenderTargetManager, VRRenderTargetManager_Base {
public:
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(
        bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override 
    {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override;
    virtual bool NeedReAllocateDepthTexture(const void* DepthTarget) override; // Not actually used, we are just checking the return address
    virtual bool NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) override; // Not actually used, we are just checking the return address

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
        ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
        FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples = 1) override;
    bool AllocateRenderTargetTextures(uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumLayers,
        ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTexture2DRHIRef>& OutTargetableTextures,
        TArray<FTexture2DRHIRef>& OutShaderResourceTextures, uint32_t NumSamples = 1) override;
    uint8_t GetActualColorSwapchainFormat() const override { return 0; }
    int32_t AcquireColorTexture() override { return -1; }
    int32_t AcquireDepthTexture() override { return -1; }

public:
    uintptr_t m_last_calculate_render_size_return_address{0};
    uintptr_t m_last_needs_reallocate_depth_texture_return_address{0};
    uintptr_t m_last_allocate_render_target_return_address{0};

    // Allows signaling to the engine that depth texture reallocation is needed if return address analysis passed.
    bool depth_analysis_passed{false};
};

struct VRRenderTargetManager_418 : IStereoRenderTargetManager_418, VRRenderTargetManager_Base {
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override {
        VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
    }

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    virtual bool NeedReAllocateDepthTexture(const void* DepthTarget) override {
        return VRRenderTargetManager_Base::need_reallocate_depth_texture(&DepthTarget);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint32_t Flags,
        uint32_t TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture, FTexture2DRHIRef& OutShaderResourceTexture,
        uint32_t NumSamples = 1) override;
};

struct VRRenderTargetManager_Special : IStereoRenderTargetManager_Special, VRRenderTargetManager_Base {
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override {
        VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
    }

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
        ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
        FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples = 1) override;
};

class FFakeStereoRenderingHook : public ModComponent {
public:
    FFakeStereoRenderingHook();

    VRRenderTargetManager_Base* get_render_target_manager() {
        if (m_uses_old_rendertarget_manager) {
            return static_cast<VRRenderTargetManager_Base*>(&m_rtm_418);
        }

        if (m_special_detected) {
            return static_cast<VRRenderTargetManager_Base*>(&m_rtm_special);
        }

        return static_cast<VRRenderTargetManager_Base*>(&m_rtm);
    }

    /*void switch_to_old_rendertarget_manager() {
        m_uses_old_rendertarget_manager = true;
    }*/
    
    bool has_pixel_format_cvar() const {
        return m_pixel_format_cvar_found;
    }

    void attempt_hooking();
    void attempt_hook_game_engine_tick(uintptr_t return_address = 0);
    void attempt_hook_slate_thread(uintptr_t return_address = 0, bool alternate = false);
    void attempt_hook_ue57_slate_elements_pass();
    void attempt_hook_ue55_slate_output_texture_register();
    void attempt_hook_ue58_slate_output_texture_register();
    void attempt_hook_daysgone_slate_intermediate_buffer();
    void attempt_hook_daysgone_bend_taa_composite();
    void attempt_hook_update_viewport_rhi(uintptr_t return_address);
    void attempt_hook_fsceneview_constructor();
    

    bool has_double_precision() const {
        return m_has_double_precision;
    }

    bool has_attempted_to_hook_engine() const {
        return m_attempted_hook_game_engine_tick;
    }

    bool has_attempted_to_hook_slate() const {
        return m_attempted_hook_slate_thread;
    }

    bool has_attempted_to_hook_fsceneview() const {
        return m_attempted_hook_fsceneview_constructor;
    }

    bool is_slate_hooked() const {
        return m_hooked_slate_thread;
    }

    bool has_seen_stable_slate_draw() const {
        return m_has_seen_stable_slate_draw;
    }

    bool has_successful_command_list_hijack() const {
        return m_has_successful_command_list_hijack;
    }

    bool prefers_slate_thread_for_session() const {
        return m_prefer_slate_thread_for_session;
    }

    bool has_seen_prerender_viewfamily() const {
        return m_has_seen_prerender_viewfamily;
    }

    bool has_scene_view_family_offsets_ready() const {
        return m_has_scene_view_family_offsets_ready;
    }

    bool set_dune_character_creation_active(bool active) {
        return m_dune_character_creation_active.exchange(active, std::memory_order_acq_rel);
    }

    bool is_dune_character_creation_active() const {
        return m_dune_character_creation_active.load(std::memory_order_acquire);
    }

    void set_dune_has_live_pawn(bool active) {
        m_dune_has_live_pawn.store(active, std::memory_order_release);
    }

    bool dune_has_live_pawn() const {
        return m_dune_has_live_pawn.load(std::memory_order_acquire);
    }

    struct DuneTrueStereoFrameSnapshot {
        uint32_t render_frame{};
        uint8_t eye{};
    };

    std::optional<DuneTrueStereoFrameSnapshot> get_dune_true_stereo_frame_snapshot() const {
        const auto packed = m_dune_true_stereo_frame.load(std::memory_order_acquire);
        if ((packed & 0x2ull) == 0) {
            return std::nullopt;
        }

        return DuneTrueStereoFrameSnapshot{
            .render_frame = static_cast<uint32_t>(packed >> 2),
            .eye = static_cast<uint8_t>(packed & 0x1ull),
        };
    }

    void note_stable_slate_draw() {
        if (!m_has_seen_stable_slate_draw) {
            m_has_seen_stable_slate_draw = true;
            m_first_stable_slate_draw_at = std::chrono::steady_clock::now();
        }
    }

    void note_prerender_viewfamily_seen() {
        m_has_seen_prerender_viewfamily = true;
    }

    void note_scene_view_family_offsets_ready() {
        m_has_scene_view_family_offsets_ready = true;
    }

    void note_successful_command_list_hijack() {
        m_has_successful_command_list_hijack = true;
    }

    uintptr_t get_daysgone_slate_native_ui_target() const {
        return m_daysgone_slate_native_ui_target.load();
    }

    uint32_t get_daysgone_slate_native_ui_width() const {
        return m_daysgone_slate_native_ui_width.load();
    }

    uint32_t get_daysgone_slate_native_ui_height() const {
        return m_daysgone_slate_native_ui_height.load();
    }

    bool should_use_daysgone_slate_ui_overlay() const {
        return m_daysgone_bend_ui_use_slate_overlay->value() &&
            m_daysgone_slate_native_ui_target.load() != 0;
    }

    float get_daysgone_slate_ui_key_threshold() const {
        return m_daysgone_bend_ui_key_threshold->value();
    }

    float get_daysgone_slate_ui_key_softness() const {
        return m_daysgone_bend_ui_key_softness->value();
    }

    float get_daysgone_slate_ui_key_opacity() const {
        return m_daysgone_bend_ui_key_opacity->value();
    }

    float get_daysgone_slate_ui_offset_x() const {
        return m_daysgone_bend_ui_screen_offset_x->value();
    }

    float get_daysgone_slate_ui_offset_y() const {
        return m_daysgone_bend_ui_screen_offset_y->value();
    }

    float get_daysgone_slate_ui_scale() const {
        return m_daysgone_bend_ui_screen_scale->value() * m_daysgone_bend_ui_draw_scale->value();
    }

    bool should_split_daysgone_slate_ui_overlay() const {
        return m_daysgone_bend_ui_split_overlay->value();
    }

    float get_daysgone_slate_ui_menu_src_x() const {
        return m_daysgone_bend_ui_menu_src_x->value();
    }

    float get_daysgone_slate_ui_menu_src_y() const {
        return m_daysgone_bend_ui_menu_src_y->value();
    }

    float get_daysgone_slate_ui_menu_src_w() const {
        return m_daysgone_bend_ui_menu_src_w->value();
    }

    float get_daysgone_slate_ui_menu_src_h() const {
        return m_daysgone_bend_ui_menu_src_h->value();
    }

    float get_daysgone_slate_ui_menu_offset_x() const {
        return m_daysgone_bend_ui_menu_offset_x->value();
    }

    float get_daysgone_slate_ui_menu_offset_y() const {
        return m_daysgone_bend_ui_menu_offset_y->value();
    }

    float get_daysgone_slate_ui_menu_scale() const {
        return m_daysgone_bend_ui_menu_scale->value();
    }

    float get_daysgone_slate_ui_footer_src_y() const {
        return m_daysgone_bend_ui_footer_src_y->value();
    }

    float get_daysgone_slate_ui_footer_src_h() const {
        return m_daysgone_bend_ui_footer_src_h->value();
    }

    bool should_recreate_textures() const {
        return m_wants_texture_recreation;
    }

    void set_should_recreate_textures(bool recreate) {
        m_wants_texture_recreation = recreate;
        m_skip_next_adjust_view_rect = true;
    }

    void on_device_reset() override {
        if (m_recreate_textures_on_reset->value()) {
            m_wants_texture_recreation = true;
        }
    }

    bool invalidate_ue57_resolution_dependent_state(
        uint32_t old_width,
        uint32_t old_height,
        uint32_t new_width,
        uint32_t new_height);

    void on_config_load(const utility::Config& cfg, bool set_defaults) {
        for (IModValue& option : m_options) {
            option.config_load(cfg, set_defaults);
        }
    }

    void on_config_save(utility::Config& cfg) {
        for (IModValue& option : m_options) {
            option.config_save(cfg);
        }
    }

    void on_frame() override;
    void on_draw_ui() override;
    void draw_daysgone_bend_ui_controls();

    auto get_frame_delay_compensation() const {
        return m_frame_delay_compensation->value();
    }

    auto& get_slate_thread_worker() {
        return m_slate_thread_worker;
    }

    bool has_slate_hook() {
        return (bool)m_slate_thread_hook;
    }

    bool has_engine_tick_hook() {
        return m_hooked_game_engine_tick;
    }

    auto& get_embedded_rtm() {
        return m_embedded_rtm;
    }

    bool m_inside_manual_view_offset{false};

    void calculate_stereo_view_offset_(const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        m_inside_manual_view_offset = true;
        calculate_stereo_view_offset(nullptr, view_index, view_rotation, world_to_meters, view_location);
        m_inside_manual_view_offset = false;
    }

    bool is_in_viewport_client_draw() const;

    bool is_ignoring_next_viewport_draw() const {
        return m_ignore_next_viewport_draw;
    }

    auto& get_last_pre_rotation() {
        return m_last_pre_rotation;
    }

    auto& get_last_pre_rotation_double() {
        return m_last_pre_rotation_double;
    }

    // Do not call these directly
    static void setup_view(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family, sdk::FSceneView& view);
    static void setup_viewpoint(ISceneViewExtension* extension, void* player_controller, void* view_info);
    static void setup_view_projection_matrix(ISceneViewExtension* extension, void* projection_data);
    static void localplayer_setup_viewpoint(void* localplayer, void* view_info, void* pass);
    static void setup_view_family(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family);
    static void begin_render_viewfamily_real(void* render_module, sdk::FCanvas* canvas, sdk::FSceneViewFamily* view_family);
    static void begin_render_viewfamily(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family);
    static void pre_render_viewfamily_renderthread(ISceneViewExtension* extension, sdk::FRHICommandListBase* cmd_list, sdk::FSceneViewFamily& view_family);

    const char* get_ghosting_fix_status_text();

private:
    std::atomic_bool m_dune_character_creation_active{false};
    std::atomic_bool m_dune_has_live_pawn{false};
    std::atomic_uint64_t m_dune_true_stereo_frame{0};

    void publish_dune_true_stereo_frame(uint32_t render_frame, uint8_t eye) {
        m_dune_true_stereo_frame.store(
            (static_cast<uint64_t>(render_frame) << 2) | 0x2ull | (eye & 0x1u),
            std::memory_order_release);
    }

    void invalidate_dune_true_stereo_frame() {
        m_dune_true_stereo_frame.store(0, std::memory_order_release);
    }

    bool hook();
    bool standard_fake_stereo_hook(uintptr_t vtable);
    bool nonstandard_create_stereo_device_hook();
    bool nonstandard_create_stereo_device_hook_4_27();
    bool nonstandard_create_stereo_device_hook_4_22();
    bool nonstandard_create_stereo_device_hook_4_18();
    
    bool hook_game_viewport_client();
    bool setup_view_extensions();

    static std::optional<uintptr_t> locate_fake_stereo_rendering_constructor();
    static std::optional<uintptr_t> locate_fake_stereo_rendering_vtable();
    static std::optional<uintptr_t> locate_active_stereo_rendering_device();
    static inline uintptr_t s_stereo_rendering_device_offset{0}; // GEngine

    std::optional<uint32_t> get_stereo_view_offset_index(uintptr_t vtable);

    bool patch_vtable_checks();
    bool attempt_runtime_inject_stereo();
    bool hook_ue418_oculus_pixel_density_sink();
    void post_init_properties(uintptr_t localplayer);
    void try_adopt_scene_viewport_render_target(sdk::FViewport* viewport, const char* source);
    void update_daysgone_ui_telemetry();
    void log_daysgone_ui_telemetry_game_thread();
    void update_daysgone_bend_ui_placement_fix();
    void apply_daysgone_bend_ui_placement_fix_game_thread();
    void restore_daysgone_bend_ui_placement_fix_game_thread();

    // Hooks
    // UGameEngine
    static void* engine_tick_hook(sdk::UGameEngine* engine, float delta, bool idle);

    // FSceneView
    static sdk::FSceneView* sceneview_constructor(sdk::FSceneView* sceneview, sdk::FSceneViewInitOptions* init_options, void* a3, void* a4);
    
    // IStereoRendering
    static bool is_stereo_enabled(FFakeStereoRendering* stereo);
    static void adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h);
    static void calculate_stereo_view_offset(FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation,
        const float world_to_meters, Vector3f* view_location);
    static Matrix4x4f* calculate_stereo_projection_matrix(FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index);
    static void render_texture_render_thread(FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list,
        FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size);
    static void init_canvas(FFakeStereoRendering* stereo, sdk::FSceneView* view, UCanvas* canvas);
    static uint32_t get_desired_number_of_views_hook(FFakeStereoRendering* stereo, bool is_stereo_enabled);
    static EStereoscopicPass get_view_pass_for_index_hook(FFakeStereoRendering* stereo, bool stereo_requested, int32_t view_index);
    static bool ue418_oculus_update_pixel_density_hook(void* settings);

    static IStereoRenderTargetManager* get_render_target_manager_hook(FFakeStereoRendering* stereo);
    static IStereoLayers* get_stereo_layers_hook(FFakeStereoRendering* stereo);

    // LocalPlayer
    static void post_calculate_stereo_projection_matrix(safetyhook::Context& ctx);
    static void pre_get_projection_data(safetyhook::Context& ctx);

    // Slate
    static void* slate_draw_window_render_thread(void* renderer, void* command_list, void* viewport_info, 
                                                 void* elements, void* params, void* unk1, void* unk2);
    static void ue57_add_slate_draw_elements_pass_hook(safetyhook::Context& ctx);
    static void slate_output_texture_register_hook_impl(safetyhook::Context& ctx, bool ue58);
    static void ue55_slate_output_texture_register_hook(safetyhook::Context& ctx);
    static void ue58_slate_output_texture_register_hook(safetyhook::Context& ctx);
    static void daysgone_slate_intermediate_buffer_hook(safetyhook::Context& ctx);
    static void daysgone_bend_taa_composite_hook(safetyhook::Context& ctx);
    static void windrose_hfsm_state_enter_hook(void* state);
    static void windrose_hfsm_state_exit_hook(void* state, uintptr_t destination_name);
    static void windrose_hfsm_component_enter_hook(void* component);
    static void windrose_hfsm_component_exit_hook(void* component, uintptr_t destination_name, int32_t reason);
    static void windrose_layout_template_enter_hook(void* layout);
    static void windrose_layout_template_exit_hook(void* layout, uintptr_t destination_name, int32_t reason);
    bool attempt_hook_windrose_hfsm_ui();

    // FViewport
    static void* viewport_destructor_hook(void* viewport, void* a2, void* a3, void* a4);
    static void viewport_draw_hook(void* viewport, bool should_present);
    static FRHITexture2D** viewport_get_render_target_texture_hook(sdk::FViewport* viewport);

    // UGameViewportClient
    static void game_viewport_client_draw_hook(sdk::UGameViewportClient*, sdk::FViewport*, sdk::FCanvas*, void*);

    // FSceneViewport
    static void update_viewport_rhi_hook(void* viewport, size_t destroyed, size_t new_size_x, size_t new_size_y, size_t new_window_mode, size_t preferred_pixel_format);

    std::unique_ptr<ThreadWorker<FRHICommandListImmediate*>> m_slate_thread_worker{std::make_unique<ThreadWorker<FRHICommandListImmediate*>>()};

    enum class GhostingFixState : uint8_t {
        Off,
        WaitingForHooks,
        LearningViewStates,
        OrientingViewStates,
        PairReady,
        NaturallySeparated,
        Active,
        FailedClosed,
    };

    struct GhostingFixPair {
        sdk::FSceneViewStateInterface* eye_state[2]{};
        sdk::FSceneViewStateInterface* pending_eye_state[2]{};
        uint8_t pending_eye_observations[2]{};
        sdk::FSceneViewStateInterface* pending_left_source_state{};
        uint8_t pending_left_source_observations{};
        uint32_t pending_left_source_frame{};
        bool pending_left_source_frame_valid{};
        uintptr_t scene{};
        uintptr_t pending_scene{};
        uint8_t pending_scene_observations[2]{};
        uint64_t first_seen_observation{};
        uint64_t last_seen_observation{};
        uint32_t generation{};
        bool orientation_confirmed{};
        bool logged_naturally_separated{};
    };

    struct {
        std::recursive_mutex mtx{};
        safetyhook::InlineHook constructor_hook{};
        std::unordered_set<sdk::FSceneViewStateInterface*> known_scene_states;
        bool inside_post_init_properties{false};

        uint32_t last_frame_count{};
        uint32_t last_index{};

        GhostingFixPair ghosting_pair{};
        GhostingFixState ghosting_state{GhostingFixState::Off};
        uint64_t ghosting_observation_serial{};
        uint64_t ghosting_learning_start_observation{};
        uint64_t ghosting_fail_observation{};
        uint64_t ghosting_last_right_eye_remap_observation{};
        uint64_t ghosting_right_eye_remap_count{};
        std::chrono::steady_clock::time_point ghosting_last_right_eye_remap_time{};
        bool ghosting_logged_bootstrap_disabled{};

        // For keeping track of what the states were before our modifications.
        std::unordered_map<sdk::FSceneViewStateInterface*, sdk::FSceneViewInitOptionsUE4> view_init_options_ue4{};
        std::unordered_map<sdk::FSceneViewStateInterface*, sdk::FSceneViewInitOptionsUE5> view_init_options_ue5{};
        std::unordered_set<uintptr_t> seen_retaddrs{};
    } m_sceneview_data;

    safetyhook::InlineHook m_localplayer_get_viewpoint_hook{};
    safetyhook::InlineHook m_tick_hook{};
    safetyhook::InlineHook m_adjust_view_rect_hook{};
    safetyhook::InlineHook m_calculate_stereo_view_offset_hook_inline{};
    std::unique_ptr<PointerHook> m_calculate_stereo_view_offset_hook_ptr{}; // some games have a short jmp which isnt supported by safetyhook right now so we use pointerhook
    safetyhook::InlineHook m_calculate_stereo_projection_matrix_hook{};
    safetyhook::InlineHook m_render_texture_render_thread_hook{};
    safetyhook::InlineHook m_ue418_oculus_pixel_density_hook{};
    safetyhook::InlineHook m_slate_thread_hook{};
    std::vector<safetyhook::MidHook> m_ue57_slate_elements_hooks{};
    safetyhook::MidHook m_ue55_slate_output_texture_register_hook{};
    std::vector<safetyhook::MidHook> m_ue58_slate_output_texture_register_hooks{};
    safetyhook::MidHook m_daysgone_slate_intermediate_buffer_hook{};
    safetyhook::MidHook m_daysgone_bend_taa_composite_hook{};
    safetyhook::InlineHook m_windrose_hfsm_state_enter_hook{};
    safetyhook::InlineHook m_windrose_hfsm_state_exit_hook{};
    safetyhook::InlineHook m_windrose_hfsm_component_enter_hook{};
    safetyhook::InlineHook m_windrose_hfsm_component_exit_hook{};
    safetyhook::InlineHook m_windrose_layout_template_enter_hook{};
    safetyhook::InlineHook m_windrose_layout_template_exit_hook{};
    safetyhook::InlineHook m_gameviewportclient_draw_hook{};
    safetyhook::InlineHook m_viewport_draw_hook{}; // for AFR
    safetyhook::InlineHook m_render_module_begin_render_viewfamily_hook{};

    // both of these are used to figure out where the localplayer is, they aren't actively
    // used for anything else, the second one is an alternative hook if the first one
    // deems fruitless.
    safetyhook::MidHook m_calculate_stereo_projection_matrix_post_hook{};
    safetyhook::MidHook m_get_projection_data_pre_hook{};

    std::unique_ptr<PointerHook> m_is_stereo_enabled_hook{};
    std::unique_ptr<PointerHook> m_get_render_target_manager_hook{};
    std::unique_ptr<PointerHook> m_get_stereo_layers_hook{};
    std::unique_ptr<PointerHook> m_init_canvas_hook{};
    std::unique_ptr<PointerHook> m_get_desired_number_of_views_hook{};
    std::unique_ptr<PointerHook> m_get_view_pass_for_index_hook{};
    std::unique_ptr<PointerHook> m_update_viewport_rhi_hook{};
    std::unique_ptr<PointerHook> m_viewport_get_render_target_texture_hook{};
    std::unique_ptr<PointerHook> m_viewport_destructor_hook{};

    std::unique_ptr<IXRTrackingSystemHook> m_tracking_system_hook{};

    struct {
        std::unordered_set<uintptr_t> seen_retaddrs{};
        std::unordered_set<uintptr_t> call_original_retaddrs{};
        std::unordered_set<uintptr_t> redirected_retaddrs{};
        std::recursive_mutex retaddr_mutex{};
        bool has_view_family_tex{false};
        int32_t selected_retaddr{0};
    } m_viewport_rt_hook_data{};

    VRRenderTargetManager m_rtm{};
    VRRenderTargetManager_418 m_rtm_418{};
    VRRenderTargetManager_Special m_rtm_special{};

    Rotator<float> m_last_afr_rotation{};
    Rotator<double> m_last_afr_rotation_double{};

    Rotator<float> m_last_pre_rotation{};
    Rotator<double> m_last_pre_rotation_double{};

    std::atomic<Rotator<float>> m_last_rotation{};
    std::atomic<Rotator<double>> m_last_rotation_double{};

    std::vector<uintptr_t> m_projection_matrix_stack{};
    bool m_hooked_alternative_localplayer_scan{false};

    bool m_hooked{false};
    bool m_tried_hooking{false};
    bool m_finished_hooking{false};
    bool m_hooked_game_engine_tick{false};
    bool m_hooked_slate_thread{false};
    bool m_hooked_ue57_slate_elements_pass{false};
    bool m_hooked_ue55_slate_output_texture_register{false};
    bool m_hooked_ue58_slate_output_texture_register{false};
    bool m_prefer_slate_thread_for_session{false};
    bool m_has_seen_stable_slate_draw{false};
    bool m_has_seen_prerender_viewfamily{false};
    bool m_has_scene_view_family_offsets_ready{false};
    bool m_has_successful_command_list_hijack{false};
    std::chrono::steady_clock::time_point m_first_stable_slate_draw_at{};
    bool m_attempted_hook_game_engine_tick{false};
    bool m_attempted_hook_slate_thread{false};
    bool m_attempted_hook_slate_thread_alternate{false};
    bool m_attempted_hook_ue57_slate_elements_pass{false};
    std::chrono::steady_clock::time_point m_ue57_dedicated_ui_missing_since{};
    uint32_t m_ue57_dedicated_ui_missing_frames{0};
    bool m_attempted_hook_ue55_slate_output_texture_register{false};
    bool m_attempted_hook_ue58_slate_output_texture_register{false};
    bool m_attempted_hook_daysgone_slate_intermediate_buffer{false};
    bool m_attempted_hook_daysgone_bend_taa_composite{false};
    bool m_attempted_hook_windrose_hfsm_ui{false};
    std::atomic<uintptr_t> m_daysgone_slate_intermediate_last_target{0};
    std::atomic<uintptr_t> m_daysgone_slate_native_ui_target{0};
    std::atomic<uint32_t> m_daysgone_slate_native_ui_width{0};
    std::atomic<uint32_t> m_daysgone_slate_native_ui_height{0};
    std::atomic<uint64_t> m_daysgone_bend_taa_composite_seen{0};
    std::atomic<uint64_t> m_daysgone_bend_taa_composite_crop_suppressed{0};
    std::atomic<uint64_t> m_daysgone_bend_taa_composite_extent_overrides{0};
    std::atomic<uint64_t> m_daysgone_bend_taa_shader_param_overrides{0};
    std::chrono::steady_clock::time_point m_daysgone_ui_telemetry_last_queue{};
    std::atomic_bool m_daysgone_ui_telemetry_queued{false};
    std::string m_daysgone_ui_telemetry_last_signature{};
    uint64_t m_daysgone_ui_telemetry_log_counter{0};
    std::atomic_bool m_daysgone_bend_ui_fix_queued{false};
    std::chrono::steady_clock::time_point m_daysgone_bend_ui_last_apply{};
    std::string m_daysgone_bend_ui_last_apply_signature{};
    std::atomic<uint64_t> m_daysgone_bend_ui_manual_apply_generation{0};
    struct DaysGoneBendUIOriginalState {
        uintptr_t menu3d{};
        uintptr_t widget_main{};
        uintptr_t default_root{};
        bool captured{false};
        float distance_from_camera{};
        float camera_fov{};
        uint8_t use_player_camera{};
        struct {
            float x{};
            float y{};
            float z{};
        } widget_location{}, widget_rotation{}, widget_scale{}, root_location{}, root_rotation{}, root_scale{};
        struct {
            float x{};
            float y{};
        } screen_offset{}, pivot{};
        float screen_scale{};
        float draw_scale{};
        uint8_t disable_occlusion{};
        uint8_t tick_when_offscreen{};
        uint8_t tick_override{};
        uint8_t tick_enabled{};
    } m_daysgone_bend_ui_originals{};
    std::atomic<uintptr_t> m_daysgone_bend_ui_last_menu3d{0};
    std::atomic<uintptr_t> m_daysgone_bend_ui_last_widget_main{0};
    std::atomic<uint64_t> m_daysgone_bend_ui_apply_count{0};
    std::atomic<uint64_t> m_daysgone_bend_ui_restore_count{0};
    bool m_attempted_hook_update_viewport_rhi{false};
    bool m_attempted_hook_fsceneview_constructor{false};
    bool m_uses_old_rendertarget_manager{false};
    bool m_rendertarget_manager_embedded_in_stereo_device{false}; // 4.17 and below...?
    bool m_special_detected{false};
    bool m_special_detected_4_18{false};
    bool m_special_detected_4_22{false};
    bool m_special_detected_4_27{false};
    bool m_manually_constructed{false};
    bool m_pixel_format_cvar_found{false};
    bool m_injected_stereo_at_runtime{false};
    bool m_has_double_precision{false}; // for the projection matrix... AND the view offset... IS UE5 DOING THIS NOW???
    bool m_fixed_localplayer_view_count{false};
    bool m_wants_texture_recreation{false};
    bool m_has_view_extension_hook{false};
    bool m_has_game_viewport_client_draw_hook{false};
    bool m_skip_next_adjust_view_rect{true};
    bool m_inside_slate_draw_window{false};
    int32_t m_skip_next_adjust_view_rect_count{1};
    uint32_t m_slate_draw_window_thread_id{0};

    // Synchronized AFR
    float m_ignored_engine_delta{0.0f};
    bool m_in_engine_tick{false};
    bool m_in_viewport_client_draw{false};
    bool m_was_in_viewport_client_draw{false}; // for IsStereoEnabled
    bool m_ignore_next_viewport_draw{false};
    bool m_ignore_next_engine_tick{false};
    void* m_last_destroyed_viewport{nullptr}; // used to check if the viewport is destroyed when we call FViewport::Draw again
    void** m_last_viewport_vtable{nullptr};


    bool m_analyzing_view_extensions{false};
    bool m_has_view_extensions_installed{false};

    std::chrono::time_point<std::chrono::high_resolution_clock> m_analyze_view_extensions_start_time{};

    /*FFakeStereoRendering m_stereo_recreation {
        90.0f, 
        (int32_t)1920, 
        (int32_t)1080, 
        (int32_t)2
    };*/

    struct FallbackDevice {
        void* vtable;
        char padding[0x20]{};
    } m_fallback_device;
    std::vector<void*> m_fallback_vtable{};

    // Seems to be the case in <= 4.17
    struct EmbeddedRenderTargetManagerInfo {
        std::unique_ptr<PointerHook> should_use_separate_render_target_hook{};
        std::unique_ptr<PointerHook> calculate_render_target_size_hook{};
        std::unique_ptr<PointerHook> allocate_render_target_texture_hook{};
        std::unique_ptr<PointerHook> need_reallocate_viewport_render_target_hook{};
        std::chrono::steady_clock::time_point last_time_needed_hmd_reallocate{};
        bool should_use_separate_rt_called{true};
        bool need_reallocate_viewport_render_target_called{true};
    } m_embedded_rtm;

    const ModToggle::Ptr m_recreate_textures_on_reset{ ModToggle::create("VR_RecreateTexturesOnReset", true) };
    const ModInt32::Ptr m_frame_delay_compensation{ ModInt32::create("VR_FrameDelayCompensation", 0) };
    const ModToggle::Ptr m_asynchronous_scan{ ModToggle::create("VR_AsynchronousScan", true) };
    // Off by default because it can cause issues with some games
    const ModToggle::Ptr m_use_fmalloc_scene_view_extensions{ ModToggle::create("VR_UseFMallocSceneViewExtensions", false) };
    // Off by default: restores safetyhook's trampoline lock path for games that dislike the faster original-call path.
    const ModToggle::Ptr m_safe_tick_hook{ ModToggle::create("VR_SafeTickHook", false) };
    const ModInt32::Ptr m_daysgone_bend_ui_mode{ ModInt32::create("VR_DaysGoneBendUI_Mode", 2, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_force_player_camera{ ModToggle::create("VR_DaysGoneBendUI_ForcePlayerCamera", true, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_override_widget_transform{ ModToggle::create("VR_DaysGoneBendUI_OverrideWidgetTransform", true, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_override_root_transform{ ModToggle::create("VR_DaysGoneBendUI_OverrideRootTransform", false, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_force_widget_refresh{ ModToggle::create("VR_DaysGoneBendUI_ForceWidgetRefresh", true, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_viewport_slot_fix{ ModToggle::create("VR_DaysGoneBendUI_ViewportSlotFix", true, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_live_watchdog{ ModToggle::create("VR_DaysGoneBendUI_LiveWatchdog", false, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_apply_child_render_transform{ ModToggle::create("VR_DaysGoneBendUI_ApplyChildRenderTransform", false, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_viewport_slot_offset_x{ ModSlider::create("VR_DaysGoneBendUI_ViewportSlotOffsetX", -1920.0f, 1920.0f, -240.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_viewport_slot_offset_y{ ModSlider::create("VR_DaysGoneBendUI_ViewportSlotOffsetY", -1080.0f, 1080.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_viewport_slot_scale{ ModSlider::create("VR_DaysGoneBendUI_ViewportSlotScale", 0.1f, 4.0f, 0.85f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_viewport_slot_opacity{ ModSlider::create("VR_DaysGoneBendUI_ViewportSlotOpacity", 0.0f, 2.0f, 1.0f, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_use_slate_overlay{ ModToggle::create("VR_DaysGoneBendUI_UseSlateOverlay", false, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_suppress_in_scene_composite{ ModToggle::create("VR_DaysGoneBendUI_SuppressInSceneComposite", false, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_split_overlay{ ModToggle::create("VR_DaysGoneBendUI_SplitOverlay", true, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_src_x{ ModSlider::create("VR_DaysGoneBendUI_MenuSrcX", 0.0f, 1.0f, 0.52f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_src_y{ ModSlider::create("VR_DaysGoneBendUI_MenuSrcY", 0.0f, 1.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_src_w{ ModSlider::create("VR_DaysGoneBendUI_MenuSrcW", 0.05f, 1.0f, 0.48f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_src_h{ ModSlider::create("VR_DaysGoneBendUI_MenuSrcH", 0.05f, 1.0f, 0.48f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_offset_x{ ModSlider::create("VR_DaysGoneBendUI_MenuOffsetX", -2400.0f, 2400.0f, -450.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_offset_y{ ModSlider::create("VR_DaysGoneBendUI_MenuOffsetY", -2400.0f, 2400.0f, -650.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_menu_scale{ ModSlider::create("VR_DaysGoneBendUI_MenuScale", 0.1f, 4.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_footer_src_y{ ModSlider::create("VR_DaysGoneBendUI_FooterSrcY", 0.0f, 1.0f, 0.68f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_footer_src_h{ ModSlider::create("VR_DaysGoneBendUI_FooterSrcH", 0.05f, 1.0f, 0.32f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_key_threshold{ ModSlider::create("VR_DaysGoneBendUI_KeyThreshold", 0.0f, 0.5f, 0.025f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_key_softness{ ModSlider::create("VR_DaysGoneBendUI_KeySoftness", 0.001f, 0.5f, 0.045f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_key_opacity{ ModSlider::create("VR_DaysGoneBendUI_KeyOpacity", 0.0f, 2.0f, 1.0f, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_disable_taa_crop{ ModToggle::create("VR_DaysGoneBendUI_DisableBendTAACrop", true, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_override_composite_extent{ ModToggle::create("VR_DaysGoneBendUI_OverrideCompositeExtent", false, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_composite_width{ ModSlider::create("VR_DaysGoneBendUI_CompositeWidth", 320.0f, 8192.0f, 1920.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_composite_height{ ModSlider::create("VR_DaysGoneBendUI_CompositeHeight", 180.0f, 8192.0f, 1080.0f, true) };
    const ModToggle::Ptr m_daysgone_bend_ui_override_shader_params{ ModToggle::create("VR_DaysGoneBendUI_OverrideShaderParams", false, true) };
    const ModInt32::Ptr m_daysgone_bend_ui_shader_param_target{ ModInt32::create("VR_DaysGoneBendUI_ShaderParamTarget", 3, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_shader_offset_x{ ModSlider::create("VR_DaysGoneBendUI_ShaderOffsetX", -4.0f, 4.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_shader_offset_y{ ModSlider::create("VR_DaysGoneBendUI_ShaderOffsetY", -4.0f, 4.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_shader_scale_x{ ModSlider::create("VR_DaysGoneBendUI_ShaderScaleX", 0.05f, 8.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_shader_scale_y{ ModSlider::create("VR_DaysGoneBendUI_ShaderScaleY", 0.05f, 8.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_distance_from_camera{ ModSlider::create("VR_DaysGoneBendUI_DistanceFromCamera", -6000.0f, 6000.0f, -1371.022f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_camera_fov{ ModSlider::create("VR_DaysGoneBendUI_CameraFOV", 10.0f, 140.0f, 70.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_loc_x{ ModSlider::create("VR_DaysGoneBendUI_WidgetLocX", -4000.0f, 4000.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_loc_y{ ModSlider::create("VR_DaysGoneBendUI_WidgetLocY", -4000.0f, 4000.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_loc_z{ ModSlider::create("VR_DaysGoneBendUI_WidgetLocZ", -6000.0f, 2000.0f, -1371.022f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_rot_pitch{ ModSlider::create("VR_DaysGoneBendUI_WidgetRotPitch", -180.0f, 180.0f, 90.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_rot_yaw{ ModSlider::create("VR_DaysGoneBendUI_WidgetRotYaw", -180.0f, 180.0f, 90.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_rot_roll{ ModSlider::create("VR_DaysGoneBendUI_WidgetRotRoll", -180.0f, 180.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_widget_scale{ ModSlider::create("VR_DaysGoneBendUI_WidgetScale", 0.05f, 8.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_screen_offset_x{ ModSlider::create("VR_DaysGoneBendUI_ScreenOffsetX", -1920.0f, 1920.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_screen_offset_y{ ModSlider::create("VR_DaysGoneBendUI_ScreenOffsetY", -1080.0f, 1080.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_screen_scale{ ModSlider::create("VR_DaysGoneBendUI_ScreenScale", 0.1f, 4.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_draw_scale{ ModSlider::create("VR_DaysGoneBendUI_DrawScale", 0.1f, 4.0f, 1.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_root_loc_x{ ModSlider::create("VR_DaysGoneBendUI_RootLocX", -4000.0f, 4000.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_root_loc_y{ ModSlider::create("VR_DaysGoneBendUI_RootLocY", -4000.0f, 4000.0f, 0.0f, true) };
    const ModSlider::Ptr m_daysgone_bend_ui_root_loc_z{ ModSlider::create("VR_DaysGoneBendUI_RootLocZ", -6000.0f, 2000.0f, -1200.0f, true) };

    void setup_options() {
        m_options = {
            *m_recreate_textures_on_reset,
            *m_frame_delay_compensation,
            *m_asynchronous_scan,
            *m_use_fmalloc_scene_view_extensions,
            *m_safe_tick_hook,
            *m_daysgone_bend_ui_mode,
            *m_daysgone_bend_ui_force_player_camera,
            *m_daysgone_bend_ui_override_widget_transform,
            *m_daysgone_bend_ui_override_root_transform,
            *m_daysgone_bend_ui_force_widget_refresh,
            *m_daysgone_bend_ui_viewport_slot_fix,
            *m_daysgone_bend_ui_live_watchdog,
            *m_daysgone_bend_ui_apply_child_render_transform,
            *m_daysgone_bend_ui_viewport_slot_offset_x,
            *m_daysgone_bend_ui_viewport_slot_offset_y,
            *m_daysgone_bend_ui_viewport_slot_scale,
            *m_daysgone_bend_ui_viewport_slot_opacity,
            *m_daysgone_bend_ui_use_slate_overlay,
            *m_daysgone_bend_ui_suppress_in_scene_composite,
            *m_daysgone_bend_ui_split_overlay,
            *m_daysgone_bend_ui_menu_src_x,
            *m_daysgone_bend_ui_menu_src_y,
            *m_daysgone_bend_ui_menu_src_w,
            *m_daysgone_bend_ui_menu_src_h,
            *m_daysgone_bend_ui_menu_offset_x,
            *m_daysgone_bend_ui_menu_offset_y,
            *m_daysgone_bend_ui_menu_scale,
            *m_daysgone_bend_ui_footer_src_y,
            *m_daysgone_bend_ui_footer_src_h,
            *m_daysgone_bend_ui_key_threshold,
            *m_daysgone_bend_ui_key_softness,
            *m_daysgone_bend_ui_key_opacity,
            *m_daysgone_bend_ui_disable_taa_crop,
            *m_daysgone_bend_ui_override_composite_extent,
            *m_daysgone_bend_ui_composite_width,
            *m_daysgone_bend_ui_composite_height,
            *m_daysgone_bend_ui_override_shader_params,
            *m_daysgone_bend_ui_shader_param_target,
            *m_daysgone_bend_ui_shader_offset_x,
            *m_daysgone_bend_ui_shader_offset_y,
            *m_daysgone_bend_ui_shader_scale_x,
            *m_daysgone_bend_ui_shader_scale_y,
            *m_daysgone_bend_ui_distance_from_camera,
            *m_daysgone_bend_ui_camera_fov,
            *m_daysgone_bend_ui_widget_loc_x,
            *m_daysgone_bend_ui_widget_loc_y,
            *m_daysgone_bend_ui_widget_loc_z,
            *m_daysgone_bend_ui_widget_rot_pitch,
            *m_daysgone_bend_ui_widget_rot_yaw,
            *m_daysgone_bend_ui_widget_rot_roll,
            *m_daysgone_bend_ui_widget_scale,
            *m_daysgone_bend_ui_screen_offset_x,
            *m_daysgone_bend_ui_screen_offset_y,
            *m_daysgone_bend_ui_screen_scale,
            *m_daysgone_bend_ui_draw_scale,
            *m_daysgone_bend_ui_root_loc_x,
            *m_daysgone_bend_ui_root_loc_y,
            *m_daysgone_bend_ui_root_loc_z
        };
    }

    friend class IXRTrackingSystemHook;
};
