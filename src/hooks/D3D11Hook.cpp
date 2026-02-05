#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <utility/Thread.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>

#include <SafetyHook.hpp>

#include "WindowFilter.hpp"
#include "Framework.hpp"

#include "D3D11Hook.hpp"

using namespace std;

static D3D11Hook* g_d3d11_hook = nullptr;

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
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
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

static uintptr_t recursive_resolve_jmp(uint8_t* instr) {
    try {
        const auto decoded = utility::decode_one(instr);

        if (decoded) {
            const auto mnem = std::string_view{decoded->Mnemonic};

            if (mnem.starts_with("JMP")) {
                const auto target = utility::resolve_displacement((uintptr_t)instr);

                if (target.has_value()) {
                    if (instr[0] == 0xFF && instr[1] == 0x25) {
                        const auto real_target = *(uintptr_t*)*target;

                        if (real_target == 0) {
                            return (uintptr_t)instr;
                        }

                        return recursive_resolve_jmp((uint8_t*)real_target);
                    }

                    return recursive_resolve_jmp((uint8_t*)target.value());
                }
            }
        }
    } catch (...) {
        SPDLOG_ERROR("[D3D11Hook] recursive_resolve_jmp exception");
    }

    return (uintptr_t)instr;
}

D3D11Hook::~D3D11Hook() {
    unhook();
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

        auto& present_fn = (*(void***)swap_chain)[8];
        auto& resize_buffers_fn = (*(void***)swap_chain)[13];

        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D11Hook::present);
        m_resize_buffers_hook = std::make_unique<PointerHook>(&resize_buffers_fn, (void*)&D3D11Hook::resize_buffers);

        m_hooked = true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to hook D3D11: {}", e.what());
        m_hooked = false;
    }

    if (m_hooked && device != nullptr) {
        hook_create_uav_inline(device);
        hook_create_uav(device);
    }

    if (m_hooked) {
        const auto d3d11_dll = GetModuleHandleW(L"d3d11.dll");
        if (d3d11_dll != nullptr) {
            const auto create_device_fn = (void*)GetProcAddress(d3d11_dll, "D3D11CreateDevice");
            const auto create_device_sc_fn = (void*)GetProcAddress(d3d11_dll, "D3D11CreateDeviceAndSwapChain");

            if (create_device_sc_fn != nullptr) {
                m_create_device_and_swapchain_hook = safetyhook::create_inline(create_device_sc_fn, &D3D11Hook::create_device_and_swapchain);
                if (!m_create_device_and_swapchain_hook) {
                    spdlog::error("Failed to hook D3D11CreateDeviceAndSwapChain, trying jmp");
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)create_device_sc_fn);
                    if (jmp_addr != (uintptr_t)create_device_sc_fn) {
                        m_create_device_and_swapchain_hook = safetyhook::create_inline((void*)jmp_addr, &D3D11Hook::create_device_and_swapchain);
                    }
                }
                if (m_create_device_and_swapchain_hook) {
                    spdlog::info("Hooked D3D11CreateDeviceAndSwapChain");
                }
            } else {
                spdlog::error("Failed to locate D3D11CreateDeviceAndSwapChain");
            }

            if (create_device_fn != nullptr) {
                m_create_device_hook = safetyhook::create_inline(create_device_fn, &D3D11Hook::create_device);
                if (!m_create_device_hook) {
                    spdlog::error("Failed to hook D3D11CreateDevice, trying jmp");
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)create_device_fn);
                    if (jmp_addr != (uintptr_t)create_device_fn) {
                        m_create_device_hook = safetyhook::create_inline((void*)jmp_addr, &D3D11Hook::create_device);
                    }
                }
                if (m_create_device_hook) {
                    spdlog::info("Hooked D3D11CreateDevice");
                }
            } else {
                spdlog::error("Failed to locate D3D11CreateDevice");
            }
        }
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

    if (m_present_hook->remove() && m_resize_buffers_hook->remove()) {
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
    d3d11->hook_create_uav(d3d11->m_device);

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

void D3D11Hook::hook_create_uav(ID3D11Device* device) {
    if (device == nullptr || device == m_uav_hook_device) {
        return;
    }

    if (m_create_uav_inline_hook) {
        m_uav_hook_device = device;
        return;
    }

    try {
        auto& create_uav_fn = (*(void***)device)[8];
        if (create_uav_fn == (void*)&D3D11Hook::create_unordered_access_view) {
            m_uav_hook_device = device;
            return;
        }
        m_create_uav_hook = std::make_unique<PointerHook>(&create_uav_fn, (void*)&D3D11Hook::create_unordered_access_view);
        m_uav_hook_device = device;
        spdlog::info("Hooked ID3D11Device::CreateUnorderedAccessView");
    } catch (const std::exception& e) {
        spdlog::error("Failed to hook CreateUnorderedAccessView: {}", e.what());
    }
}

void D3D11Hook::hook_create_uav_inline(ID3D11Device* device) {
    if (device == nullptr || m_create_uav_inline_hook) {
        return;
    }

    try {
        auto& create_uav_fn = (*(void***)device)[8];
        if (create_uav_fn != nullptr) {
            m_create_uav_inline_hook = safetyhook::create_inline(create_uav_fn, &D3D11Hook::create_unordered_access_view_inline);
            if (!m_create_uav_inline_hook) {
                spdlog::error("Failed to hook ID3D11Device::CreateUnorderedAccessView (inline), trying jmp");
                const auto jmp_addr = recursive_resolve_jmp((uint8_t*)create_uav_fn);
                if (jmp_addr != (uintptr_t)create_uav_fn) {
                    m_create_uav_inline_hook = safetyhook::create_inline((void*)jmp_addr, &D3D11Hook::create_unordered_access_view_inline);
                }
            }
            if (m_create_uav_inline_hook) {
                spdlog::info("Inline-hooked ID3D11Device::CreateUnorderedAccessView");
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to inline-hook CreateUnorderedAccessView: {}", e.what());
    }
}

HRESULT WINAPI D3D11Hook::create_device(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** immediate_context)
{
    auto d3d11 = g_d3d11_hook;
    if (d3d11 == nullptr) {
        return E_FAIL;
    }

    const auto hr = d3d11->m_create_device_hook.call<HRESULT>(
        adapter, driver_type, software, flags, feature_levels, feature_levels_count, sdk_version,
        device, feature_level, immediate_context);

    if (SUCCEEDED(hr) && driver_type != D3D_DRIVER_TYPE_NULL && device != nullptr && *device != nullptr) {
        d3d11->hook_create_uav(*device);
    }

    return hr;
}

HRESULT WINAPI D3D11Hook::create_device_and_swapchain(
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
    ID3D11DeviceContext** immediate_context)
{
    auto d3d11 = g_d3d11_hook;
    if (d3d11 == nullptr) {
        return E_FAIL;
    }

    const auto hr = d3d11->m_create_device_and_swapchain_hook.call<HRESULT>(
        adapter, driver_type, software, flags, feature_levels, feature_levels_count, sdk_version,
        swap_chain_desc, swap_chain, device, feature_level, immediate_context);

    if (SUCCEEDED(hr) && driver_type != D3D_DRIVER_TYPE_NULL && device != nullptr && *device != nullptr) {
        d3d11->hook_create_uav(*device);
    }

    return hr;
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

HRESULT WINAPI D3D11Hook::create_unordered_access_view(
    ID3D11Device* device,
    ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
    ID3D11UnorderedAccessView** uav)
{
    auto d3d11 = g_d3d11_hook;
    if (d3d11 == nullptr) {
        return E_FAIL;
    }

    const auto call_original = [&](const D3D11_UNORDERED_ACCESS_VIEW_DESC* use_desc) -> HRESULT {
        if (d3d11->m_create_uav_inline_hook) {
            return d3d11->m_create_uav_inline_hook.call<HRESULT>(device, resource, use_desc, uav);
        }

        if (d3d11->m_create_uav_hook) {
            auto original = d3d11->m_create_uav_hook->get_original<decltype(D3D11Hook::create_unordered_access_view)*>();
            return original(device, resource, use_desc, uav);
        }

        return E_FAIL;
    };

    HRESULT result = call_original(desc);

    if (FAILED(result)) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (resource != nullptr) {
            resource->GetType(&dim);
        }

        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] CreateUnorderedAccessView failed: hr=0x{:08X} dim={}"),
            (uint32_t)result, to_resource_dim_name(dim));

        if (resource != nullptr && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2d{};
            if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex2d)))) {
                D3D11_TEXTURE2D_DESC tex_desc{};
                tex2d->GetDesc(&tex_desc);
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex2d desc: WxH={}x{} Mips={} Array={} Format={} SampleCount={} BindFlags=0x{:X} Misc=0x{:X}"),
                    tex_desc.Width, tex_desc.Height, tex_desc.MipLevels, tex_desc.ArraySize,
                    (uint32_t)tex_desc.Format, tex_desc.SampleDesc.Count, tex_desc.BindFlags, tex_desc.MiscFlags);

                const auto try_return_dummy_uav = [&]() -> bool {
                    if (device == nullptr || uav == nullptr) {
                        return false;
                    }

                    static std::mutex s_dummy_mutex{};
                    struct DummyUavEntry {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
                    };
                    static std::unordered_map<ID3D11Resource*, DummyUavEntry> s_dummy_uavs{};

                    std::scoped_lock lock{s_dummy_mutex};
                    if (auto it = s_dummy_uavs.find(resource); it != s_dummy_uavs.end()) {
                        *uav = it->second.uav.Get();
                        (*uav)->AddRef();
                        return true;
                    }

                    D3D11_TEXTURE2D_DESC dummy_desc = tex_desc;
                    dummy_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    dummy_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
                    dummy_desc.MiscFlags = 0;
                    dummy_desc.MipLevels = 1;
                    dummy_desc.ArraySize = 1;
                    dummy_desc.SampleDesc.Count = 1;
                    dummy_desc.SampleDesc.Quality = 0;

                    DummyUavEntry entry{};
                    if (FAILED(device->CreateTexture2D(&dummy_desc, nullptr, &entry.tex))) {
                        return false;
                    }

                    D3D11_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc{};
                    dummy_uav_desc.Format = dummy_desc.Format;
                    dummy_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    dummy_uav_desc.Texture2D.MipSlice = 0;

                    if (FAILED(device->CreateUnorderedAccessView(entry.tex.Get(), &dummy_uav_desc, &entry.uav))) {
                        return false;
                    }

                    *uav = entry.uav.Get();
                    (*uav)->AddRef();
                    s_dummy_uavs.emplace(resource, std::move(entry));
                    spdlog::warn("[D3D11] Returned dummy UAV for unsupported UAV format");
                    return true;
                };

                if (desc != nullptr) {
                    DXGI_FORMAT src_format = desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : tex_desc.Format;
                    auto mapped = choose_uav_format(src_format);
                    if (!mapped.has_value() && desc->Format == DXGI_FORMAT_UNKNOWN) {
                        mapped = choose_uav_format(tex_desc.Format);
                    }

                    if (mapped.has_value() && *mapped != src_format) {
                        auto retry_desc = *desc;
                        retry_desc.Format = *mapped;
                        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry: format {} -> {}"),
                            (uint32_t)src_format, (uint32_t)*mapped);
                        const auto retry_result = call_original(&retry_desc);
                        if (SUCCEEDED(retry_result)) {
                            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry succeeded"));
                            return retry_result;
                        }
                        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry failed: hr=0x{:08X}"), (uint32_t)retry_result);
                    }

                    if (tex_desc.Format != DXGI_FORMAT_UNKNOWN && tex_desc.Format != src_format) {
                        auto retry_desc = *desc;
                        retry_desc.Format = tex_desc.Format;
                        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry: format {} -> {}"),
                            (uint32_t)src_format, (uint32_t)tex_desc.Format);
                        const auto retry_result = call_original(&retry_desc);
                        if (SUCCEEDED(retry_result)) {
                            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry succeeded"));
                            return retry_result;
                        }
                        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry failed: hr=0x{:08X}"), (uint32_t)retry_result);
                    }

                    if (try_return_dummy_uav()) {
                        return S_OK;
                    }
                } else {
                    if (tex_desc.SampleDesc.Count > 1) {
                        spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry skipped: MSAA textures cannot have UAVs"));
                    } else {
                        auto mapped = choose_uav_format(tex_desc.Format);
                        if (mapped.has_value()) {
                            D3D11_UNORDERED_ACCESS_VIEW_DESC retry_desc{};
                            retry_desc.Format = *mapped;
                            if (tex_desc.ArraySize > 1) {
                                retry_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                                retry_desc.Texture2DArray.MipSlice = 0;
                                retry_desc.Texture2DArray.FirstArraySlice = 0;
                                retry_desc.Texture2DArray.ArraySize = tex_desc.ArraySize;
                            } else {
                                retry_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                                retry_desc.Texture2D.MipSlice = 0;
                            }

                            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry (null desc): format {}"),
                                (uint32_t)*mapped);
                            const auto retry_result = call_original(&retry_desc);
                            if (SUCCEEDED(retry_result)) {
                                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry succeeded"));
                                return retry_result;
                            }
                            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV retry failed: hr=0x{:08X}"), (uint32_t)retry_result);
                        }
                        if (try_return_dummy_uav()) {
                            return S_OK;
                        }
                    }
                }
            }
        }

        if (desc != nullptr) {
            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV desc: Format={} ViewDim={}"),
                (uint32_t)desc->Format, (uint32_t)desc->ViewDimension);

            switch (desc->ViewDimension) {
            case D3D11_UAV_DIMENSION_BUFFER:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV buffer: FirstElement={} NumElements={} Flags=0x{:X}"),
                    desc->Buffer.FirstElement, desc->Buffer.NumElements, (uint32_t)desc->Buffer.Flags);
                break;
            case D3D11_UAV_DIMENSION_TEXTURE1D:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex1d: MipSlice={}"), desc->Texture1D.MipSlice);
                break;
            case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex1darray: MipSlice={} FirstArray={} ArraySize={}"),
                    desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
                break;
            case D3D11_UAV_DIMENSION_TEXTURE2D:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex2d: MipSlice={}"), desc->Texture2D.MipSlice);
                break;
            case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex2darray: MipSlice={} FirstArray={} ArraySize={}"),
                    desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
                break;
            case D3D11_UAV_DIMENSION_TEXTURE3D:
                spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV tex3d: MipSlice={} FirstWSlice={} WSize={}"),
                    desc->Texture3D.MipSlice, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize);
                break;
            default:
                break;
            }
        } else {
            spdlog::error(SPDLOG_FMT_RUNTIME("[D3D11] UAV desc: <null>"));
        }
    }

    return result;
}

HRESULT WINAPI D3D11Hook::create_unordered_access_view_inline(
    ID3D11Device* device,
    ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
    ID3D11UnorderedAccessView** uav)
{
    return create_unordered_access_view(device, resource, desc, uav);
}
