#pragma once

#include <functional>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>
#include <safetyhook.hpp>

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
    std::unique_ptr<PointerHook> m_create_uav_hook{};
    ID3D11Device* m_uav_hook_device{nullptr};
    safetyhook::InlineHook m_create_uav_inline_hook{};
    safetyhook::InlineHook m_create_device_hook{};
    safetyhook::InlineHook m_create_device_and_swapchain_hook{};
    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    ComPtr<ID3D11Texture2D> m_last_depthstencil_used{};

    static HRESULT WINAPI present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static void WINAPI set_render_targets(
        ID3D11DeviceContext* context, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
    static HRESULT WINAPI create_unordered_access_view(
        ID3D11Device* device,
        ID3D11Resource* resource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
        ID3D11UnorderedAccessView** uav);
    static HRESULT WINAPI create_unordered_access_view_inline(
        ID3D11Device* device,
        ID3D11Resource* resource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
        ID3D11UnorderedAccessView** uav);
    static HRESULT WINAPI create_device(
        IDXGIAdapter* adapter,
        D3D_DRIVER_TYPE driver_type,
        HMODULE software,
        UINT flags,
        const D3D_FEATURE_LEVEL* feature_levels,
        UINT feature_levels_count,
        UINT sdk_version,
        ID3D11Device** device,
        D3D_FEATURE_LEVEL* feature_level,
        ID3D11DeviceContext** immediate_context);
    static HRESULT WINAPI create_device_and_swapchain(
        IDXGIAdapter* adapter,
        D3D_DRIVER_TYPE driver_type,
        HMODULE software,
        UINT flags,
        const D3D_FEATURE_LEVEL* feature_levels,
        UINT feature_levels_count,
        UINT sdk_version,
        const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
        IDXGISwapChain** swap_chain,
        ID3D11Device** device,
        D3D_FEATURE_LEVEL* feature_level,
        ID3D11DeviceContext** immediate_context);

    void hook_create_uav(ID3D11Device* device);
    void hook_create_uav_inline(ID3D11Device* device);
};
