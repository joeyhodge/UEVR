#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility/Thread.hpp>
#include <utility/Module.hpp>

#include "WindowFilter.hpp"
#include "Framework.hpp"
#include "render/ShaderOverrideRegistry.hpp"

#include "D3D11Hook.hpp"

using namespace std;

static D3D11Hook* g_d3d11_hook = nullptr;

namespace {
struct NarutoSlateUICaptureState {
    std::atomic_bool active{false};
    std::atomic_uint64_t expires_at_ms{0};
    std::atomic<ID3D11Resource*> ui_target{nullptr};
    std::atomic<ID3D11Resource*> scene_target{nullptr};
    std::atomic<ID3D11Resource*> original_target{nullptr};
};

NarutoSlateUICaptureState g_naruto_slate_ui_capture{};
std::atomic_bool g_logged_naruto_slate_viewport_suppression{false};
std::atomic_uint32_t g_logged_naruto_slate_unmatched_quads{0};

bool naruto_d3d11_slate_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"NARUTO-Win64-Shipping.exe") != std::wstring::npos;
    }();

    return result;
}
}

static bool daysgone_d3d11_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"DaysGone.exe") != std::wstring::npos ||
             exe_path->find(L"BendGame") != std::wstring::npos);
    }();

    return result;
}

static const char* to_resource_dim_name(D3D11_RESOURCE_DIMENSION dim) {
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return "BUFFER";
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return "TEX1D";
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return "TEX2D";
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return "TEX3D";
    default: return "UNKNOWN";
    }
}

static bool is_depth_or_stencil_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return true;
    default:
        return false;
    }
}

static std::optional<DXGI_FORMAT> choose_uav_format(DXGI_FORMAT format) {
    if (is_depth_or_stencil_format(format)) {
        return std::nullopt;
    }

    switch (format) {
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return std::nullopt;
    }
}

static bool is_daysgone_scene_uav_candidate(const D3D11_TEXTURE2D_DESC& desc) {
    if (!daysgone_d3d11_guard_enabled()) {
        return false;
    }

    if (desc.Width < 1280 || desc.Height < 720 || desc.MipLevels != 1 || desc.ArraySize != 1) {
        return false;
    }

    if (desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0 || desc.SampleDesc.Count != 1) {
        return false;
    }

    const auto required_bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if ((desc.BindFlags & required_bind) != required_bind || (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0) {
        return false;
    }

    if (is_depth_or_stencil_format(desc.Format)) {
        return false;
    }

    switch (desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

D3D11Hook::~D3D11Hook() {
    unhook();
}

void D3D11Hook::begin_naruto_slate_ui_capture(
    ID3D11Resource* ui_target,
    ID3D11Resource* scene_target,
    ID3D11Resource* original_target)
{
    auto& state = g_naruto_slate_ui_capture;
    state.active.store(false, std::memory_order_release);

    if (!naruto_d3d11_slate_guard_enabled() || ui_target == nullptr) {
        state.ui_target.store(nullptr, std::memory_order_relaxed);
        state.scene_target.store(nullptr, std::memory_order_relaxed);
        state.original_target.store(nullptr, std::memory_order_relaxed);
        state.expires_at_ms.store(0, std::memory_order_relaxed);
        return;
    }

    state.ui_target.store(ui_target, std::memory_order_relaxed);
    state.scene_target.store(scene_target, std::memory_order_relaxed);
    state.original_target.store(original_target, std::memory_order_relaxed);
    state.expires_at_ms.store(GetTickCount64() + 1000, std::memory_order_relaxed);
    state.active.store(true, std::memory_order_release);
}

void D3D11Hook::end_naruto_slate_ui_capture() {
    // UE4.16 records Slate RHI commands here but executes them later on the
    // RHI thread. Keep the exact resource identities alive briefly; Begin()
    // renews them every Slate frame and the timeout fails closed on teardown.
}

bool D3D11Hook::hook() {
    spdlog::info("Hooking D3D11");

    g_d3d11_hook = this;

    HWND h_wnd = GetDesktopWindow();
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC swap_chain_desc;

    ZeroMemory(&swap_chain_desc, sizeof(swap_chain_desc));

    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferCount = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = h_wnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swap_chain_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const auto original_bytes = utility::get_original_bytes(&D3D11CreateDeviceAndSwapChain);

    // Temporarily unhook D3D11CreateDeviceAndSwapChain
    // it allows compatibility with ReShade and other overlays that hook it
    // this is just a dummy device anyways, we don't want the other overlays to be able to use it
    if (original_bytes) {
        spdlog::info("D3D11CreateDeviceAndSwapChain appears to be hooked, temporarily unhooking");

        std::vector<uint8_t> hooked_bytes(original_bytes->size());
        memcpy(hooked_bytes.data(), &D3D11CreateDeviceAndSwapChain, original_bytes->size());

        ProtectionOverride protection_override{ &D3D11CreateDeviceAndSwapChain, original_bytes->size(), PAGE_EXECUTE_READWRITE };
        memcpy(&D3D11CreateDeviceAndSwapChain, original_bytes->data(), original_bytes->size());
        
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, &feature_level, 1, D3D11_SDK_VERSION,
                &swap_chain_desc, &swap_chain, &device, nullptr, &context))) 
        {
            spdlog::error("Failed to create D3D11 device");
            memcpy(&D3D11CreateDeviceAndSwapChain, hooked_bytes.data(), hooked_bytes.size());
            return false;
        }
        
        spdlog::info("Restoring hooked bytes for D3D11CreateDeviceAndSwapChain");
        memcpy(&D3D11CreateDeviceAndSwapChain, hooked_bytes.data(), hooked_bytes.size());
    } else {
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, &feature_level, 1, D3D11_SDK_VERSION,
                &swap_chain_desc, &swap_chain, &device, nullptr, &context))) 
        {
            spdlog::error("Failed to create D3D11 device");
            return false;
        }
    }

    try {
        m_present_hook.reset();
        m_resize_buffers_hook.reset();
        m_create_vertex_shader_hook.reset();
        m_create_pixel_shader_hook.reset();
        m_vs_set_shader_hook.reset();
        m_ps_set_shader_hook.reset();
        m_draw_indexed_hook.reset();
        m_naruto_draw_context_vtable = nullptr;

        auto& present_fn = (*(void***)swap_chain)[8];
        auto& resize_buffers_fn = (*(void***)swap_chain)[13];
        auto& create_vertex_shader_fn = (*(void***)device)[12];
        auto& create_pixel_shader_fn = (*(void***)device)[15];
        auto& ps_set_shader_fn = (*(void***)context)[5];
        auto& vs_set_shader_fn = (*(void***)context)[7];

        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D11Hook::present);
        m_resize_buffers_hook = std::make_unique<PointerHook>(&resize_buffers_fn, (void*)&D3D11Hook::resize_buffers);
        m_create_vertex_shader_hook = std::make_unique<PointerHook>(&create_vertex_shader_fn, (void*)&D3D11Hook::create_vertex_shader);
        m_create_pixel_shader_hook = std::make_unique<PointerHook>(&create_pixel_shader_fn, (void*)&D3D11Hook::create_pixel_shader);
        m_ps_set_shader_hook = std::make_unique<PointerHook>(&ps_set_shader_fn, (void*)&D3D11Hook::ps_set_shader);
        m_vs_set_shader_hook = std::make_unique<PointerHook>(&vs_set_shader_fn, (void*)&D3D11Hook::vs_set_shader);

        if (daysgone_d3d11_guard_enabled()) {
            hook_create_texture2d(device);
            hook_create_uav(device);
        }

        render::ShaderOverrideRegistry::get().set_d3d11_create_callbacks(
            m_create_vertex_shader_hook->get_original<render::ShaderOverrideRegistry::CreateVertexShaderFn>(),
            m_create_pixel_shader_hook->get_original<render::ShaderOverrideRegistry::CreatePixelShaderFn>()
        );

        m_hooked = true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to hook D3D11: {}", e.what());
        m_hooked = false;
    }

    device->Release();
    context->Release();
    swap_chain->Release();
    return m_hooked;
}

bool D3D11Hook::unhook() {
    if (!m_hooked) {
        return true;
    }

    spdlog::info("Unhooking D3D11");

    const auto uav_unhooked = m_create_uav_hook == nullptr || m_create_uav_hook->remove();
    m_create_uav_hook.reset();
    m_create_uav_hook_device = nullptr;

    const auto tex2d_unhooked = m_create_texture2d_hook == nullptr || m_create_texture2d_hook->remove();
    m_create_texture2d_hook.reset();
    m_create_texture2d_hook_device = nullptr;

    const auto naruto_draw_unhooked = m_draw_indexed_hook == nullptr || m_draw_indexed_hook->remove();
    m_draw_indexed_hook.reset();
    m_naruto_draw_context_vtable = nullptr;

    if (uav_unhooked &&
        tex2d_unhooked &&
        naruto_draw_unhooked &&
        m_present_hook->remove() &&
        m_resize_buffers_hook->remove() &&
        m_create_vertex_shader_hook->remove() &&
        m_create_pixel_shader_hook->remove() &&
        m_vs_set_shader_hook->remove() &&
        m_ps_set_shader_hook->remove())
    {
        m_hooked = false;
        return true;
    }

    return false;
}

thread_local bool g_inside_d3d11_present = false;
HRESULT last_d3d11_present_result = S_OK;

HRESULT WINAPI D3D11Hook::present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d11 = g_d3d11_hook;

    // This line must be called before calling our detour function because we might have to unhook the function inside our detour.
    auto present_fn = d3d11->m_present_hook->get_original<decltype(D3D11Hook::present)*>();

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_chain->GetDesc(&swap_desc);

    if (WindowFilter::get().is_filtered(swap_desc.OutputWindow)) {
        return present_fn(swap_chain, sync_interval, flags);
    }

    d3d11->m_inside_present = true;

    if (d3d11->m_swapchain_0 == nullptr) {
        d3d11->m_swapchain_0 = swap_chain;
        d3d11->m_swap_chain = swap_chain;
    } else if (d3d11->m_swapchain_1 == nullptr && swap_chain != d3d11->m_swapchain_0) {
        d3d11->m_swapchain_1 = swap_chain;
    }

    /*if (d3d11->m_swap_chain != d3d11->m_swapchain_0) {
        d3d11->m_inside_present = false;
        return present_fn(swap_chain, sync_interval, flags);
    }*/

    swap_chain->GetDevice(__uuidof(d3d11->m_device), (void**)&d3d11->m_device);

    if (naruto_d3d11_slate_guard_enabled()) {
        d3d11->hook_naruto_draw_indexed(d3d11->m_device);
    }

    if (daysgone_d3d11_guard_enabled()) {
        d3d11->hook_create_texture2d(d3d11->m_device);
        d3d11->hook_create_uav(d3d11->m_device);
    }

    /*if (d3d11->m_set_render_targets_hook == nullptr) {
        ComPtr<ID3D11DeviceContext> context{};

        d3d11->m_device->GetImmediateContext(&context);
        auto& set_render_targets_fn = (*(void***)context.Get())[33];
        d3d11->m_set_render_targets_hook = std::make_unique<PointerHook>(&set_render_targets_fn, (void*)&set_render_targets);
        OutputDebugString("Hooked ID3D11DeviceContext::SetRenderTargets");
    }*/

    /*if (GetAsyncKeyState(VK_INSERT) & 1) {
        OutputDebugString(fmt::format("Depth stencil @ {:p} used", (void*)d3d11->m_last_depthstencil_used.Get()).c_str());
    }*/

    // Restore the original bytes
    // if an infinite loop occurs, this will prevent the game from crashing
    // while keeping our hook intact
    if (g_inside_d3d11_present) {
        auto original_bytes = utility::get_original_bytes(Address{present_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{present_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(present_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Present fixed");
        }

        return last_d3d11_present_result;
    }

    if (d3d11->m_on_present) {
        d3d11->m_on_present(*d3d11);

        if (d3d11->m_next_present_interval) {
            sync_interval = *d3d11->m_next_present_interval;
            d3d11->m_next_present_interval = std::nullopt;

            if (sync_interval == 0) {
                BOOL is_fullscreen = 0;
                swap_chain->GetFullscreenState(&is_fullscreen, nullptr);
                flags &= ~DXGI_PRESENT_DO_NOT_SEQUENCE;

                if (!is_fullscreen && (swap_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
                    flags |= DXGI_PRESENT_ALLOW_TEARING;
                }
            }
        }
    }

    HRESULT result = S_OK;
    g_inside_d3d11_present = true;

    if (!d3d11->m_ignore_next_present) {
        result = present_fn(swap_chain, sync_interval, flags);
        last_d3d11_present_result = result;
    } else {
        d3d11->m_ignore_next_present = false;
        last_d3d11_present_result = S_OK;
    }

    g_inside_d3d11_present = false;

    if (d3d11->m_on_post_present) {
        d3d11->m_on_post_present(*d3d11);
    }

    d3d11->m_last_depthstencil_used.Reset();
    d3d11->m_inside_present = false;

    return result;
}

thread_local bool g_inside_d3d11_resize_buffers = false;
HRESULT last_d3d11_resize_buffers_result = S_OK;

HRESULT WINAPI D3D11Hook::resize_buffers(
    IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d11 = g_d3d11_hook;
    auto resize_buffers_fn = d3d11->m_resize_buffers_hook->get_original<decltype(D3D11Hook::resize_buffers)*>();

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_chain->GetDesc(&swap_desc);

    if (WindowFilter::get().is_filtered(swap_desc.OutputWindow)) {
        return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    d3d11->m_swap_chain = swap_chain;
    d3d11->m_swapchain_0 = nullptr;
    d3d11->m_swapchain_1 = nullptr;
    d3d11->m_last_depthstencil_used.Reset();

    if (d3d11->m_on_resize_buffers) {
        d3d11->m_on_resize_buffers(*d3d11, width, height);
    }

    if (g_inside_d3d11_resize_buffers) {
        auto original_bytes = utility::get_original_bytes(Address{resize_buffers_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_buffers_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_buffers_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize buffers fixed");
        }

        return last_d3d11_resize_buffers_result;
    }

    g_inside_d3d11_resize_buffers = true;

    last_d3d11_resize_buffers_result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);

    g_inside_d3d11_resize_buffers = false;

    return last_d3d11_resize_buffers_result;
}

void D3D11Hook::hook_create_texture2d(ID3D11Device* device) {
    if (!daysgone_d3d11_guard_enabled() || device == nullptr || device == m_create_texture2d_hook_device) {
        return;
    }

    try {
        if (m_create_texture2d_hook != nullptr) {
            m_create_texture2d_hook->remove();
            m_create_texture2d_hook.reset();
            m_create_texture2d_hook_device = nullptr;
        }

        auto& create_texture2d_fn = (*(void***)device)[5];
        if (create_texture2d_fn == nullptr || create_texture2d_fn == (void*)&D3D11Hook::create_texture2d) {
            m_create_texture2d_hook_device = device;
            return;
        }

        m_create_texture2d_hook = std::make_unique<PointerHook>(&create_texture2d_fn, (void*)&D3D11Hook::create_texture2d);
        m_create_texture2d_hook_device = device;
        spdlog::warn("[DaysGone][D3D11] Hooked ID3D11Device::CreateTexture2D for UE4.11 scene RT UAV bind guard");
    } catch (const std::exception& e) {
        spdlog::error("[DaysGone][D3D11] Failed to hook CreateTexture2D: {}", e.what());
    } catch (...) {
        spdlog::error("[DaysGone][D3D11] Failed to hook CreateTexture2D");
    }
}

void D3D11Hook::hook_create_uav(ID3D11Device* device) {
    if (!daysgone_d3d11_guard_enabled() || device == nullptr || device == m_create_uav_hook_device) {
        return;
    }

    try {
        if (m_create_uav_hook != nullptr) {
            m_create_uav_hook->remove();
            m_create_uav_hook.reset();
            m_create_uav_hook_device = nullptr;
        }

        auto& create_uav_fn = (*(void***)device)[8];
        if (create_uav_fn == nullptr || create_uav_fn == (void*)&D3D11Hook::create_unordered_access_view) {
            m_create_uav_hook_device = device;
            return;
        }

        m_create_uav_hook = std::make_unique<PointerHook>(&create_uav_fn, (void*)&D3D11Hook::create_unordered_access_view);
        m_create_uav_hook_device = device;
        spdlog::warn("[DaysGone][D3D11] Hooked ID3D11Device::CreateUnorderedAccessView for UE4.11 Slate UAV fallback");
    } catch (const std::exception& e) {
        spdlog::error("[DaysGone][D3D11] Failed to hook CreateUnorderedAccessView: {}", e.what());
    } catch (...) {
        spdlog::error("[DaysGone][D3D11] Failed to hook CreateUnorderedAccessView");
    }
}

HRESULT WINAPI D3D11Hook::create_texture2d(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initial_data,
    ID3D11Texture2D** texture
) {
    auto d3d11 = g_d3d11_hook;
    if (d3d11 == nullptr || d3d11->m_create_texture2d_hook == nullptr) {
        return E_FAIL;
    }

    auto original = d3d11->m_create_texture2d_hook->get_original<decltype(D3D11Hook::create_texture2d)*>();

    if (desc == nullptr || !is_daysgone_scene_uav_candidate(*desc)) {
        return original(device, desc, initial_data, texture);
    }

    auto patched_desc = *desc;
    patched_desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    const auto patched_result = original(device, &patched_desc, initial_data, texture);
    if (SUCCEEDED(patched_result)) {
        spdlog::warn(
            "[DaysGone][D3D11] Added UAV bind to large scene RT texture {}x{} format={} bind=0x{:X}->0x{:X}",
            desc->Width,
            desc->Height,
            (uint32_t)desc->Format,
            desc->BindFlags,
            patched_desc.BindFlags);
        return patched_result;
    }

    // Some drivers reject UAV on B8 formats. If so, fail closed to the original
    // engine desc; the UAV hook below still prevents the UE4.11 fatal.
    spdlog::warn(
        "[DaysGone][D3D11] UAV-bind scene RT create failed 0x{:08X}; retrying original desc {}x{} format={} bind=0x{:X}",
        (uint32_t)patched_result,
        desc->Width,
        desc->Height,
        (uint32_t)desc->Format,
        desc->BindFlags);

    return original(device, desc, initial_data, texture);
}

HRESULT WINAPI D3D11Hook::create_unordered_access_view(
    ID3D11Device* device,
    ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
    ID3D11UnorderedAccessView** uav
) {
    auto d3d11 = g_d3d11_hook;
    if (d3d11 == nullptr || d3d11->m_create_uav_hook == nullptr) {
        return E_FAIL;
    }

    auto original = d3d11->m_create_uav_hook->get_original<decltype(D3D11Hook::create_unordered_access_view)*>();
    const auto result = original(device, resource, desc, uav);
    if (SUCCEEDED(result) || !daysgone_d3d11_guard_enabled()) {
        return result;
    }

    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (resource != nullptr) {
        resource->GetType(&dim);
    }

    spdlog::warn("[DaysGone][D3D11] CreateUnorderedAccessView failed hr=0x{:08X} dim={}", (uint32_t)result, to_resource_dim_name(dim));

    if (device == nullptr || resource == nullptr || uav == nullptr || dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        return result;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);

    spdlog::warn(
        "[DaysGone][D3D11] UAV texture desc: {}x{} mips={} array={} format={} samples={} bind=0x{:X} misc=0x{:X}",
        texture_desc.Width,
        texture_desc.Height,
        texture_desc.MipLevels,
        texture_desc.ArraySize,
        (uint32_t)texture_desc.Format,
        texture_desc.SampleDesc.Count,
        texture_desc.BindFlags,
        texture_desc.MiscFlags);

    if (desc != nullptr) {
        spdlog::warn("[DaysGone][D3D11] UAV desc: format={} view_dim={}", (uint32_t)desc->Format, (uint32_t)desc->ViewDimension);
    } else {
        spdlog::warn("[DaysGone][D3D11] UAV desc: <null>");
    }

    if (texture_desc.SampleDesc.Count > 1 || is_depth_or_stencil_format(texture_desc.Format)) {
        spdlog::warn("[DaysGone][D3D11] Refusing dummy UAV fallback for MSAA/depth texture");
        return result;
    }

    if (desc != nullptr) {
        const auto source_format = desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : texture_desc.Format;
        if (auto mapped = choose_uav_format(source_format); mapped && *mapped != desc->Format) {
            auto retry_desc = *desc;
            retry_desc.Format = *mapped;
            const auto retry_result = original(device, resource, &retry_desc, uav);
            spdlog::warn(
                "[DaysGone][D3D11] UAV typed-format retry {} -> {} returned 0x{:08X}",
                (uint32_t)source_format,
                (uint32_t)*mapped,
                (uint32_t)retry_result);

            if (SUCCEEDED(retry_result)) {
                return retry_result;
            }
        }
    }

    struct DummyUavEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    static std::mutex s_dummy_uav_mutex{};
    static std::unordered_map<ID3D11Resource*, DummyUavEntry> s_dummy_uavs{};

    std::scoped_lock lock{s_dummy_uav_mutex};
    if (auto it = s_dummy_uavs.find(resource); it != s_dummy_uavs.end()) {
        *uav = it->second.uav.Get();
        (*uav)->AddRef();
        spdlog::warn("[DaysGone][D3D11] Reusing dummy UAV for unsupported texture UAV request");
        return S_OK;
    }

    D3D11_TEXTURE2D_DESC dummy_desc = texture_desc;
    dummy_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dummy_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    dummy_desc.MiscFlags = 0;
    dummy_desc.MipLevels = 1;
    dummy_desc.ArraySize = 1;
    dummy_desc.SampleDesc.Count = 1;
    dummy_desc.SampleDesc.Quality = 0;

    DummyUavEntry entry{};
    const auto dummy_texture_result = device->CreateTexture2D(&dummy_desc, nullptr, &entry.texture);
    if (FAILED(dummy_texture_result) || entry.texture == nullptr) {
        spdlog::warn("[DaysGone][D3D11] Failed to create dummy UAV texture: 0x{:08X}", (uint32_t)dummy_texture_result);
        return result;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc{};
    dummy_uav_desc.Format = dummy_desc.Format;
    dummy_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    dummy_uav_desc.Texture2D.MipSlice = 0;

    const auto dummy_uav_result = device->CreateUnorderedAccessView(entry.texture.Get(), &dummy_uav_desc, &entry.uav);
    if (FAILED(dummy_uav_result) || entry.uav == nullptr) {
        spdlog::warn("[DaysGone][D3D11] Failed to create dummy UAV: 0x{:08X}", (uint32_t)dummy_uav_result);
        return result;
    }

    *uav = entry.uav.Get();
    (*uav)->AddRef();
    s_dummy_uavs.emplace(resource, std::move(entry));
    spdlog::warn("[DaysGone][D3D11] Returned dummy UAV for unsupported UE4.11 Slate/RT UAV request");
    return S_OK;
}

HRESULT WINAPI D3D11Hook::create_vertex_shader(
    ID3D11Device* device,
    const void* bytecode,
    SIZE_T bytecode_size,
    ID3D11ClassLinkage* linkage,
    ID3D11VertexShader** shader
) {
    auto d3d11 = g_d3d11_hook;
    auto original = d3d11->m_create_vertex_shader_hook->get_original<decltype(D3D11Hook::create_vertex_shader)*>();
    const auto result = original(device, bytecode, bytecode_size, linkage, shader);

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (shader_registry.should_track_d3d11_shaders() && SUCCEEDED(result) && shader != nullptr && *shader != nullptr) {
        shader_registry.register_d3d11_shader_creation(
            render::ShaderOverrideRegistry::Stage::Vertex,
            device,
            *shader,
            bytecode,
            bytecode_size
        );
    }

    return result;
}

HRESULT WINAPI D3D11Hook::create_pixel_shader(
    ID3D11Device* device,
    const void* bytecode,
    SIZE_T bytecode_size,
    ID3D11ClassLinkage* linkage,
    ID3D11PixelShader** shader
) {
    auto d3d11 = g_d3d11_hook;
    auto original = d3d11->m_create_pixel_shader_hook->get_original<decltype(D3D11Hook::create_pixel_shader)*>();
    const auto result = original(device, bytecode, bytecode_size, linkage, shader);

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (shader_registry.should_track_d3d11_shaders() && SUCCEEDED(result) && shader != nullptr && *shader != nullptr) {
        shader_registry.register_d3d11_shader_creation(
            render::ShaderOverrideRegistry::Stage::Pixel,
            device,
            *shader,
            bytecode,
            bytecode_size
        );
    }

    return result;
}

void WINAPI D3D11Hook::vs_set_shader(
    ID3D11DeviceContext* context,
    ID3D11VertexShader* shader,
    ID3D11ClassInstance* const* class_instances,
    UINT num_class_instances
) {
    auto d3d11 = g_d3d11_hook;
    auto original = d3d11->m_vs_set_shader_hook->get_original<decltype(D3D11Hook::vs_set_shader)*>();

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (!shader_registry.should_track_d3d11_shaders()) {
        original(context, shader, class_instances, num_class_instances);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device{};
    context->GetDevice(&device);

    auto bound_shader = shader_registry.resolve_d3d11_vertex_shader(device.Get(), shader);
    shader_registry.note_d3d11_shader_bound(render::ShaderOverrideRegistry::Stage::Vertex, shader, bound_shader);
    original(context, bound_shader, class_instances, num_class_instances);
}

void WINAPI D3D11Hook::ps_set_shader(
    ID3D11DeviceContext* context,
    ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* class_instances,
    UINT num_class_instances
) {
    auto d3d11 = g_d3d11_hook;
    auto original = d3d11->m_ps_set_shader_hook->get_original<decltype(D3D11Hook::ps_set_shader)*>();

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (!shader_registry.should_track_d3d11_shaders()) {
        original(context, shader, class_instances, num_class_instances);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device{};
    context->GetDevice(&device);

    auto bound_shader = shader_registry.resolve_d3d11_pixel_shader(device.Get(), shader);
    shader_registry.note_d3d11_shader_bound(render::ShaderOverrideRegistry::Stage::Pixel, shader, bound_shader);
    original(context, bound_shader, class_instances, num_class_instances);
}

void D3D11Hook::hook_naruto_draw_indexed(ID3D11Device* device) {
    if (!naruto_d3d11_slate_guard_enabled() || device == nullptr) {
        return;
    }

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    if (context == nullptr) {
        return;
    }

    auto** const vtable = *reinterpret_cast<void***>(context.Get());
    if (vtable == nullptr || (m_draw_indexed_hook != nullptr && m_naruto_draw_context_vtable == vtable)) {
        return;
    }

    try {
        if (m_draw_indexed_hook != nullptr) {
            m_draw_indexed_hook->remove();
            m_draw_indexed_hook.reset();
            m_naruto_draw_context_vtable = nullptr;
        }

        // ID3D11DeviceContext::DrawIndexed is vtable slot 12. Hook the real
        // game context: the NULL-driver context used during bootstrap has a
        // different implementation table on Naruto's D3D11 device.
        auto& draw_indexed_fn = vtable[12];
        if (draw_indexed_fn == nullptr || draw_indexed_fn == reinterpret_cast<void*>(&D3D11Hook::draw_indexed)) {
            return;
        }

        m_draw_indexed_hook = std::make_unique<PointerHook>(&draw_indexed_fn, reinterpret_cast<void*>(&D3D11Hook::draw_indexed));
        m_naruto_draw_context_vtable = vtable;
        spdlog::warn("[Naruto][UE4.16][SlateUI] Hooked DrawIndexed on the live D3D11 immediate context");
    } catch (const std::exception& e) {
        spdlog::error("[Naruto][UE4.16][SlateUI] Failed to hook live DrawIndexed: {}", e.what());
    } catch (...) {
        spdlog::error("[Naruto][UE4.16][SlateUI] Failed to hook live DrawIndexed");
    }
}

void WINAPI D3D11Hook::draw_indexed(
    ID3D11DeviceContext* context,
    UINT index_count,
    UINT start_index_location,
    INT base_vertex_location)
{
    auto d3d11 = g_d3d11_hook;
    auto original = d3d11->m_draw_indexed_hook->get_original<decltype(D3D11Hook::draw_indexed)*>();
    auto& state = g_naruto_slate_ui_capture;
    const auto capture_active = state.active.load(std::memory_order_acquire);
    const auto expires_at_ms = state.expires_at_ms.load(std::memory_order_relaxed);
    const auto ui_target = state.ui_target.load(std::memory_order_relaxed);
    const auto scene_target = state.scene_target.load(std::memory_order_relaxed);
    const auto original_target = state.original_target.load(std::memory_order_relaxed);

    if (capture_active && (expires_at_ms == 0 || GetTickCount64() > expires_at_ms)) {
        state.active.store(false, std::memory_order_release);
    }

    // A Slate viewport is one quad. Requiring both the dedicated UI RTV and
    // the exact old/scene resource keeps this from touching ordinary UI.
    if (capture_active && GetTickCount64() <= expires_at_ms && ui_target != nullptr &&
        context != nullptr && index_count == 6)
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv{};
        context->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        Microsoft::WRL::ComPtr<ID3D11Resource> destination{};
        if (rtv != nullptr) {
            rtv->GetResource(destination.GetAddressOf());
        }

        if (destination.Get() == ui_target) {
            constexpr UINT srv_count = 8;
            ID3D11ShaderResourceView* srvs[srv_count]{};
            context->PSGetShaderResources(0, srv_count, srvs);

            ID3D11Resource* matched_source = nullptr;
            UINT matched_slot = 0;
            ID3D11Resource* first_source = nullptr;

            for (UINT slot = 0; slot < srv_count; ++slot) {
                if (srvs[slot] == nullptr) {
                    continue;
                }

                ID3D11Resource* source = nullptr;
                srvs[slot]->GetResource(&source);

                if (first_source == nullptr) {
                    first_source = source;
                    if (first_source != nullptr) {
                        first_source->AddRef();
                    }
                }

                if (source != nullptr &&
                    ((scene_target != nullptr && source == scene_target) ||
                     (original_target != nullptr && source == original_target)))
                {
                    matched_source = source;
                    matched_slot = slot;
                }

                if (source != nullptr) {
                    source->Release();
                }
                srvs[slot]->Release();
            }

            if (matched_source != nullptr) {
                if (!g_logged_naruto_slate_viewport_suppression.exchange(true)) {
                    spdlog::warn(
                        "[Naruto][UE4.16][SlateUI] Suppressed the original viewport composite from the dedicated UI target (srv_slot={})",
                        matched_slot);
                }

                if (first_source != nullptr) {
                    first_source->Release();
                }
                return;
            }

            const auto log_index = g_logged_naruto_slate_unmatched_quads.fetch_add(1);
            if (log_index < 8) {
                spdlog::info(
                    "[Naruto][UE4.16][SlateUI] Observed unmatched 6-index UI-target draw: first_srv={} scene={} original={}",
                    reinterpret_cast<uintptr_t>(first_source),
                    reinterpret_cast<uintptr_t>(scene_target),
                    reinterpret_cast<uintptr_t>(original_target));
            }

            if (first_source != nullptr) {
                first_source->Release();
            }
        }
    }

    original(context, index_count, start_index_location, base_vertex_location);
}

void WINAPI D3D11Hook::set_render_targets(
    ID3D11DeviceContext* context, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d11 = g_d3d11_hook;

    if (dsv != nullptr) {
        //auto obj_name = fmt::format("Depthstencil @ {:p}", (void*)d3d11->m_last_depthstencil_used.Get());
        //d3d11->m_last_depthstencil_used->SetPrivateData(WKPDID_D3DDebugObjectName, obj_name.size(), obj_name.c_str());
        //OutputDebugString(fmt::format("Depth stencil @ {:p} used", (void*)d3d11->m_last_depthstencil_used.Get()).c_str());

        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        dsv->GetDesc(&desc);

        if (desc.Flags & D3D11_DSV_FLAG::D3D11_DSV_READ_ONLY_DEPTH) {
            dsv->GetResource((ID3D11Resource**)d3d11->m_last_depthstencil_used.GetAddressOf());

            //OutputDebugString(fmt::format("Flags: {}", desc.Flags).c_str());
            //OutputDebugString(fmt::format("Format: {}", desc.Format).c_str());
            //OutputDebugString(fmt::format("ViewDimension: {}", desc.ViewDimension).c_str());   
        }
    }

    auto set_render_targets_fn = d3d11->m_set_render_targets_hook->get_original<decltype(set_render_targets)*>();

    return set_render_targets_fn(context, num_views, rtvs, dsv);
}
