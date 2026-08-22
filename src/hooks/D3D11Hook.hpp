#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>

#include <safetyhook/inline_hook.hpp>

#include "utility/PointerHook.hpp"

class D3D11Hook {
public:
    typedef std::function<void(D3D11Hook&)> OnPresentFn;
    typedef std::function<void(D3D11Hook&, uint32_t w, uint32_t h)> OnResizeBuffersFn;

    D3D11Hook() = default;
    virtual ~D3D11Hook();

	bool is_hooked() {
		return m_hooked;
	}

    bool is_inside_present() const {
        return m_inside_present;
    }

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    void set_next_present_interval(uint32_t interval) {
        m_next_present_interval = interval;
    }

    bool hook();
    bool unhook();

    void on_present(OnPresentFn fn) { m_on_present = fn; }
    void on_post_present(OnPresentFn fn) { m_on_post_present = fn; }
    void on_resize_buffers(OnResizeBuffersFn fn) { m_on_resize_buffers = fn; }

    // Naruto/UE4.16 draws the scene viewport as a Slate element while Slate is
    // redirected to the dedicated UI target. Limit suppression to that draw.
    static void begin_naruto_slate_ui_capture(
        ID3D11Resource* ui_target,
        ID3D11Resource* scene_target,
        ID3D11Resource* original_target);
    static void end_naruto_slate_ui_capture();

    struct DaysGoneNativeSnapshot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
        D3D11_TEXTURE2D_DESC desc{};
        uint64_t capture_generation{};
        uint64_t transaction_serial{};
    };

    // Days Gone finalizes each renderer into the left region of its packed
    // BGRA target. Snapshot the secondary transaction after that compute pass
    // so Present receives stable, correctly converted eye pixels.
    bool prepare_daysgone_native_snapshot();
    uint64_t begin_daysgone_native_snapshot(uint64_t capture_generation, uint32_t width, uint32_t height);
    void cancel_daysgone_native_snapshot(uint64_t transaction_serial);
    std::shared_ptr<const DaysGoneNativeSnapshot> get_daysgone_native_snapshot(
        uint64_t transaction_serial,
        uint64_t capture_generation) const;

    ID3D11Device* get_device() { return m_device; }
    IDXGISwapChain* get_swap_chain() { return m_swap_chain; } // The "active" swap chain.
    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }
    auto& get_last_depthstencil_used() { return m_last_depthstencil_used; }

protected:
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ID3D11Device* m_device{ nullptr };
    IDXGISwapChain* m_swap_chain{ nullptr };
    IDXGISwapChain* m_swapchain_0{};
    IDXGISwapChain* m_swapchain_1{};
    bool m_hooked{ false };
    bool m_inside_present{false};
    bool m_ignore_next_present{false};

    std::optional<uint32_t> m_next_present_interval{};

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<PointerHook> m_resize_buffers_hook{};
    std::unique_ptr<PointerHook> m_set_render_targets_hook{};
    std::unique_ptr<PointerHook> m_create_texture2d_hook{};
    ID3D11Device* m_create_texture2d_hook_device{};
    std::unique_ptr<PointerHook> m_create_uav_hook{};
    ID3D11Device* m_create_uav_hook_device{};
    std::unique_ptr<PointerHook> m_create_vertex_shader_hook{};
    std::unique_ptr<PointerHook> m_create_pixel_shader_hook{};
    std::unique_ptr<PointerHook> m_vs_set_shader_hook{};
    std::unique_ptr<PointerHook> m_ps_set_shader_hook{};
    std::unique_ptr<PointerHook> m_draw_indexed_hook{};
    void** m_naruto_draw_context_vtable{};
    std::unique_ptr<PointerHook> m_daysgone_cs_set_uavs_hook{};
    safetyhook::InlineHook m_daysgone_dispatch_hook{};
    void** m_daysgone_context_vtable{};
    ID3D11DeviceContext* m_daysgone_native_snapshot_context{};
    std::mutex m_daysgone_context_hook_mutex{};
    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    ComPtr<ID3D11Texture2D> m_last_depthstencil_used{};

    static constexpr size_t DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT = 4;
    mutable std::mutex m_daysgone_native_snapshot_mutex{};
    std::atomic_bool m_daysgone_native_snapshot_armed{false};
    std::atomic_uint64_t m_daysgone_native_snapshot_transaction_counter{};
    std::atomic_uint64_t m_daysgone_native_snapshot_transaction{};
    std::atomic_uint64_t m_daysgone_native_snapshot_generation{};
    std::atomic_uint32_t m_daysgone_native_snapshot_width{};
    std::atomic_uint32_t m_daysgone_native_snapshot_height{};
    ComPtr<ID3D11Device> m_daysgone_native_snapshot_device{};
    ComPtr<ID3D11Texture2D> m_daysgone_native_packed_target{};
    D3D11_TEXTURE2D_DESC m_daysgone_native_packed_desc{};
    uint64_t m_daysgone_native_packed_transaction{};
    std::array<std::shared_ptr<DaysGoneNativeSnapshot>, DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT>
        m_daysgone_native_snapshot_slots{};
    size_t m_daysgone_native_snapshot_next_slot{};
    std::atomic<std::shared_ptr<const DaysGoneNativeSnapshot>> m_daysgone_native_snapshot_ready{};

    static HRESULT WINAPI present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI create_vertex_shader(ID3D11Device* device, const void* bytecode, SIZE_T bytecode_size, ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader);
    static HRESULT WINAPI create_pixel_shader(ID3D11Device* device, const void* bytecode, SIZE_T bytecode_size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader);
    static HRESULT WINAPI create_texture2d(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial_data, ID3D11Texture2D** texture);
    static HRESULT WINAPI create_unordered_access_view(ID3D11Device* device, ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, ID3D11UnorderedAccessView** uav);
    static void WINAPI vs_set_shader(ID3D11DeviceContext* context, ID3D11VertexShader* shader, ID3D11ClassInstance* const* class_instances, UINT num_class_instances);
    static void WINAPI ps_set_shader(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11ClassInstance* const* class_instances, UINT num_class_instances);
    static void WINAPI draw_indexed(ID3D11DeviceContext* context, UINT index_count, UINT start_index_location, INT base_vertex_location);
    static void WINAPI daysgone_cs_set_unordered_access_views(
        ID3D11DeviceContext* context,
        UINT start_slot,
        UINT num_uavs,
        ID3D11UnorderedAccessView* const* uavs,
        const UINT* initial_counts);
    static void WINAPI daysgone_dispatch(
        ID3D11DeviceContext* context,
        UINT thread_group_count_x,
        UINT thread_group_count_y,
        UINT thread_group_count_z);
    static void WINAPI set_render_targets(
        ID3D11DeviceContext* context, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);

    void hook_create_uav(ID3D11Device* device);
    void hook_create_texture2d(ID3D11Device* device);
    void hook_naruto_draw_indexed(ID3D11Device* device);
    bool hook_daysgone_native_context(ID3D11Device* device);
    void reset_daysgone_native_snapshot_state();
    bool create_daysgone_native_snapshot_slots_locked(
        const D3D11_TEXTURE2D_DESC& packed_desc,
        uint32_t eye_width,
        uint32_t eye_height);
};
