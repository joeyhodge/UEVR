#include <array>
#include <thread>
#include <future>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <wrl/client.h>
#include <utility/Thread.hpp>
#include <utility/Module.hpp>
#include <utility/RTTI.hpp>

#include "WindowFilter.hpp"
#include "Framework.hpp"
#include "render/D3D12Diagnostics.hpp"
#include "render/ShaderOverrideRegistry.hpp"

#include "D3D12Hook.hpp"

static D3D12Hook* g_d3d12_hook = nullptr;

namespace {
constexpr size_t CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX = 10;
constexpr size_t CREATE_PIPELINE_STATE_VTABLE_INDEX = 47;
constexpr size_t CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX = 20;
constexpr size_t CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX = 21;
constexpr size_t SET_PIPELINE_STATE_VTABLE_INDEX = 25;

template <typename TInterface>
void add_unique_pointer_hook(
    TInterface* iface,
    size_t vtable_index,
    void* detour,
    std::vector<std::unique_ptr<PointerHook>>& storage,
    std::unordered_map<uintptr_t, PointerHook*>& lookup,
    std::unordered_set<uintptr_t>& seen_slots
) {
    if (iface == nullptr) {
        return;
    }

    auto** slot = &(*(void***)iface)[vtable_index];
    const auto slot_key = reinterpret_cast<uintptr_t>(slot);

    if (!seen_slots.emplace(slot_key).second) {
        return;
    }

    auto hook = std::make_unique<PointerHook>(slot, detour);
    lookup.emplace(slot_key, hook.get());
    storage.emplace_back(std::move(hook));
}
}

D3D12Hook::~D3D12Hook() {
    unhook();
}

bool D3D12Hook::hook() {
    spdlog::info("Hooking D3D12");

    g_d3d12_hook = this;

    IDXGISwapChain1* swap_chain1{ nullptr };
    IDXGISwapChain3* swap_chain{ nullptr };
    ID3D12Device* device{ nullptr };
    ID3D12CommandAllocator* command_allocator{ nullptr };
    ID3D12GraphicsCommandList* command_list{ nullptr };

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc1;

    ZeroMemory(&swap_chain_desc1, sizeof(swap_chain_desc1));

    swap_chain_desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap_chain_desc1.BufferCount = 2;
    swap_chain_desc1.SampleDesc.Count = 1;
    swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swap_chain_desc1.Width = 1;
    swap_chain_desc1.Height = 1;

    // Manually get D3D12CreateDevice export because the user may be running Windows 7
    const auto d3d12_module = LoadLibraryA("d3d12.dll");
    if (d3d12_module == nullptr) {
        spdlog::error("Failed to load d3d12.dll");
        return false;
    }

    auto d3d12_create_device = (decltype(D3D12CreateDevice)*)GetProcAddress(d3d12_module, "D3D12CreateDevice");
    if (d3d12_create_device == nullptr) {
        spdlog::error("Failed to get D3D12CreateDevice export");
        return false;
    }

    spdlog::info("Creating dummy device");

    // Get the original on-disk bytes of the D3D12CreateDevice export
    const auto original_bytes = utility::get_original_bytes(d3d12_create_device);

    // Temporarily unhook D3D12CreateDevice
    // it allows compatibility with ReShade and other overlays that hook it
    // this is just a dummy device anyways, we don't want the other overlays to be able to use it
    if (original_bytes) {
        spdlog::info("D3D12CreateDevice appears to be hooked, temporarily unhooking");

        std::vector<uint8_t> hooked_bytes(original_bytes->size());
        memcpy(hooked_bytes.data(), d3d12_create_device, original_bytes->size());

        ProtectionOverride protection_override{ d3d12_create_device, original_bytes->size(), PAGE_EXECUTE_READWRITE };
        memcpy(d3d12_create_device, original_bytes->data(), original_bytes->size());
        
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
            return false;
        }

        spdlog::info("Restoring hooked bytes for D3D12CreateDevice");
        memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
    } else { // D3D12CreateDevice is not hooked
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            return false;
        }
    }

    spdlog::info("Dummy device: {:x}", (uintptr_t)device);

    // Manually get CreateDXGIFactory export because the user may be running Windows 7
    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("Failed to load dxgi.dll");
        return false;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");

    if (create_dxgi_factory == nullptr) {
        spdlog::error("Failed to get CreateDXGIFactory export");
        return false;
    }

    spdlog::info("Creating dummy DXGI factory");

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory)))) {
        spdlog::error("Failed to create D3D12 Dummy DXGI Factory");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = 0;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;

    spdlog::info("Creating dummy command queue");

    ID3D12CommandQueue* command_queue{ nullptr };
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Queue");
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Allocator");
        return false;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator, nullptr, IID_PPV_ARGS(&command_list)))) {
        spdlog::error("Failed to create D3D12 Dummy Graphics Command List");
        return false;
    }

    spdlog::info("Creating dummy swapchain");

    // used in CreateSwapChainForHwnd fallback
    HWND hwnd = 0;
    WNDCLASSEX wc{};

    auto init_dummy_window = [&]() {
        // fallback to CreateSwapChainForHwnd
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = TEXT("REFRAMEWORK_DX12_DUMMY");
        wc.hIconSm = NULL;

        ::RegisterClassEx(&wc);

        hwnd = ::CreateWindow(wc.lpszClassName, TEXT("REF DX Dummy Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        swap_chain_desc1.BufferCount = 3;
        swap_chain_desc1.Width = 0;
        swap_chain_desc1.Height = 0;
        swap_chain_desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc1.SampleDesc.Count = 1;
        swap_chain_desc1.SampleDesc.Quality = 0;
        swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swap_chain_desc1.Scaling = DXGI_SCALING_STRETCH;
        swap_chain_desc1.Stereo = FALSE;
    };

    std::vector<std::function<bool ()>> swapchain_attempts{
        // we call CreateSwapChainForComposition instead of CreateSwapChainForHwnd
        // because some overlays will have hooks on CreateSwapChainForHwnd
        // and all we're doing is creating a dummy swapchain
        // we don't want to screw up the overlay
        [&]() {
            return !FAILED(factory->CreateSwapChainForComposition(command_queue, &swap_chain_desc1, nullptr, &swap_chain1));
        },
        [&]() {
            init_dummy_window();

            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, hwnd, &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
        [&]() {
            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, GetDesktopWindow(), &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
    };

    bool any_succeed = false;

    for (auto i = 0; i < swapchain_attempts.size(); i++) {
        auto& attempt = swapchain_attempts[i];
        
        try {
            spdlog::info("Trying swapchain attempt {}", i);

            if (attempt()) {
                spdlog::info("Created dummy swapchain on attempt {}", i);
                any_succeed = true;
                break;
            }
        } catch (std::exception& e) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: {}", i, e.what());
        } catch(...) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: unknown exception", i);
        }

        spdlog::error("Attempt {} failed", i);
    }

    if (!any_succeed) {
        spdlog::error("Failed to create D3D12 Dummy Swap Chain");

        if (hwnd) {
            ::DestroyWindow(hwnd);
        }

        if (wc.lpszClassName != nullptr) {
            ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        }

        return false;
    }

    spdlog::info("Querying dummy swapchain");

    if (FAILED(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)))) {
        spdlog::error("Failed to retrieve D3D12 DXGI SwapChain");
        return false;
    }

    if (!m_skip_dummy_swapchain_type_info_probe) {
        try {
            const auto ti = utility::rtti::get_type_info(swap_chain1);
            const auto swapchain_classname = ti != nullptr && ti->name() != nullptr ? std::string_view{ti->name()} : "unknown";
            const auto raw_name = ti != nullptr && ti->raw_name() != nullptr ? std::string_view{ti->raw_name()} : "unknown";

            spdlog::info("Swapchain type info: {}", swapchain_classname);
            spdlog::info("Swapchain raw type info: {}", raw_name);
            
            if (swapchain_classname.contains("interposer::DXGISwapChain")) { // DLSS3
                spdlog::info("Found Streamline (DLSSFG) swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
                m_using_frame_generation_swapchain = true;
            }
            // Need to test this one to see if it actually has the same issues - disabling it for now
            /*else if (swapchain_classname.contains("FrameInterpolationSwapChain")) { // FSR3
                spdlog::info("Found FSR3 swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
                m_using_frame_generation_swapchain = true;
            }*/
        } catch (const std::exception& e) {
            spdlog::error("Failed to get type info: {}. Disabling dummy swapchain RTTI probe for this session.", e.what());
            m_skip_dummy_swapchain_type_info_probe = true;
        } catch (...) {
            spdlog::error("Failed to get type info: unknown exception. Disabling dummy swapchain RTTI probe for this session.");
            m_skip_dummy_swapchain_type_info_probe = true;
        }
    }

    spdlog::info("Finding command queue offset");

    m_command_queue_offset = 0;

    // Find the command queue offset in the swapchain
    for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
        const auto base = (uintptr_t)swap_chain1 + i;

        // reached the end
        if (IsBadReadPtr((void*)base, sizeof(void*))) {
            break;
        }

        auto data = *(ID3D12CommandQueue**)base;

        if (data == command_queue) {
            m_command_queue_offset = i;
            spdlog::info("Found command queue offset: {:x}", i);
            break;
        }
    }

    auto target_swapchain = swap_chain;

    // Scan throughout the swapchain for a valid pointer to scan through
    // this is usually only necessary for Proton
    if (m_command_queue_offset == 0) {
        bool should_break = false;

        for (auto base = 0; base < 512 * sizeof(void*); base += sizeof(void*)) {
            const auto pre_scan_base = (uintptr_t)swap_chain1 + base;

            // reached the end
            if (IsBadReadPtr((void*)pre_scan_base, sizeof(void*))) {
                break;
            }

            const auto scan_base = *(uintptr_t*)pre_scan_base;

            if (scan_base == 0 || IsBadReadPtr((void*)scan_base, sizeof(void*))) {
                continue;
            }

            for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
                const auto pre_data = scan_base + i;

                if (IsBadReadPtr((void*)pre_data, sizeof(void*))) {
                    break;
                }

                auto data = *(ID3D12CommandQueue**)pre_data;

                if (data == command_queue) {
                    // If we hook Streamline's Swapchain, the menu fails to render correctly/flickers
                    // So we switch out the swapchain with the internal one owned by Streamline
                    // Side note: Even though we are scanning for Proton here,
                    // this doubles as an offset scanner for the real swapchain inside Streamline (or FSR3)
                    if (m_using_frame_generation_swapchain) {
                        target_swapchain = (IDXGISwapChain3*)scan_base;
                    }

                    if (!m_using_frame_generation_swapchain) {
                        m_using_proton_swapchain = true;
                    }

                    m_command_queue_offset = i;
                    m_proton_swapchain_offset = base;
                    should_break = true;

                    spdlog::info("Proton potentially detected");
                    spdlog::info("Found command queue offset: {:x}", i);
                    break;
                }
            }

            if (m_using_proton_swapchain || should_break) {
                break;
            }
        }
    }

    if (m_command_queue_offset == 0) {
        spdlog::error("Failed to find command queue offset");
        return false;
    }

    try {
        spdlog::info("Initializing hooks");
        m_present_hook.reset();
        m_present1_hook.reset();
        m_create_graphics_pipeline_state_hooks.clear();
        m_create_pipeline_state_hooks.clear();
        m_create_render_target_view_hooks.clear();
        m_create_depth_stencil_view_hooks.clear();
        m_set_pipeline_state_hooks.clear();
        m_create_graphics_pipeline_state_hook_lookup.clear();
        m_create_pipeline_state_hook_lookup.clear();
        m_create_render_target_view_hook_lookup.clear();
        m_create_depth_stencil_view_hook_lookup.clear();
        m_set_pipeline_state_hook_lookup.clear();
        m_swapchain_hook.reset();

        m_is_phase_1 = true;

        auto& present_fn = (*(void***)target_swapchain)[8]; // Present
        auto& present1_fn = (*(void***)target_swapchain)[22]; // Present1
        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D12Hook::present);
        m_present1_hook = std::make_unique<PointerHook>(&present1_fn, (void*)&D3D12Hook::present1);

        std::unordered_set<uintptr_t> graphics_pipeline_state_slots{};
        std::unordered_set<uintptr_t> pipeline_state_stream_slots{};
        std::unordered_set<uintptr_t> render_target_view_slots{};
        std::unordered_set<uintptr_t> depth_stencil_view_slots{};
        std::unordered_set<uintptr_t> set_pipeline_state_slots{};

        add_unique_pointer_hook(
            device,
            CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::create_graphics_pipeline_state),
            m_create_graphics_pipeline_state_hooks,
            m_create_graphics_pipeline_state_hook_lookup,
            graphics_pipeline_state_slots
        );

        Microsoft::WRL::ComPtr<ID3D12Device1> device1{};
        Microsoft::WRL::ComPtr<ID3D12Device2> device2{};
        Microsoft::WRL::ComPtr<ID3D12Device3> device3{};
        Microsoft::WRL::ComPtr<ID3D12Device4> device4{};
        Microsoft::WRL::ComPtr<ID3D12Device5> device5{};
        Microsoft::WRL::ComPtr<ID3D12Device6> device6{};
        Microsoft::WRL::ComPtr<ID3D12Device7> device7{};
        Microsoft::WRL::ComPtr<ID3D12Device8> device8{};
        Microsoft::WRL::ComPtr<ID3D12Device9> device9{};
        Microsoft::WRL::ComPtr<ID3D12Device10> device10{};

        device->QueryInterface(IID_PPV_ARGS(&device1));
        device->QueryInterface(IID_PPV_ARGS(&device2));
        device->QueryInterface(IID_PPV_ARGS(&device3));
        device->QueryInterface(IID_PPV_ARGS(&device4));
        device->QueryInterface(IID_PPV_ARGS(&device5));
        device->QueryInterface(IID_PPV_ARGS(&device6));
        device->QueryInterface(IID_PPV_ARGS(&device7));
        device->QueryInterface(IID_PPV_ARGS(&device8));
        device->QueryInterface(IID_PPV_ARGS(&device9));
        device->QueryInterface(IID_PPV_ARGS(&device10));

        const std::array<IUnknown*, 10> device_interfaces{
            device1.Get(),
            device2.Get(),
            device3.Get(),
            device4.Get(),
            device5.Get(),
            device6.Get(),
            device7.Get(),
            device8.Get(),
            device9.Get(),
            device10.Get()
        };

        for (auto* iface : device_interfaces) {
            add_unique_pointer_hook(
                iface,
                CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_graphics_pipeline_state),
                m_create_graphics_pipeline_state_hooks,
                m_create_graphics_pipeline_state_hook_lookup,
                graphics_pipeline_state_slots
            );

            add_unique_pointer_hook(
                iface,
                CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_render_target_view),
                m_create_render_target_view_hooks,
                m_create_render_target_view_hook_lookup,
                render_target_view_slots
            );

            add_unique_pointer_hook(
                iface,
                CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_depth_stencil_view),
                m_create_depth_stencil_view_hooks,
                m_create_depth_stencil_view_hook_lookup,
                depth_stencil_view_slots
            );
        }

        const std::array<IUnknown*, 9> pipeline_stream_interfaces{
            device2.Get(),
            device3.Get(),
            device4.Get(),
            device5.Get(),
            device6.Get(),
            device7.Get(),
            device8.Get(),
            device9.Get(),
            device10.Get()
        };

        for (auto* iface : pipeline_stream_interfaces) {
            add_unique_pointer_hook(
                iface,
                CREATE_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_pipeline_state),
                m_create_pipeline_state_hooks,
                m_create_pipeline_state_hook_lookup,
                pipeline_state_stream_slots
            );
        }

        add_unique_pointer_hook(
            command_list,
            SET_PIPELINE_STATE_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::set_pipeline_state),
            m_set_pipeline_state_hooks,
            m_set_pipeline_state_hook_lookup,
            set_pipeline_state_slots
        );

        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> command_list1{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> command_list2{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList3> command_list3{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> command_list4{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList5> command_list5{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> command_list6{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> command_list7{};

        command_list->QueryInterface(IID_PPV_ARGS(&command_list1));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list2));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list3));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list4));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list5));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list6));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list7));

        const std::array<IUnknown*, 7> command_list_interfaces{
            command_list1.Get(),
            command_list2.Get(),
            command_list3.Get(),
            command_list4.Get(),
            command_list5.Get(),
            command_list6.Get(),
            command_list7.Get()
        };

        for (auto* iface : command_list_interfaces) {
            add_unique_pointer_hook(
                iface,
                SET_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::set_pipeline_state),
                m_set_pipeline_state_hooks,
                m_set_pipeline_state_hook_lookup,
                set_pipeline_state_slots
            );
        }

        m_hooked = true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize hooks: {}", e.what());
        m_hooked = false;
    }

    if (command_list != nullptr) {
        command_list->Release();
    }

    if (command_allocator != nullptr) {
        command_allocator->Release();
    }

    device->Release();
    command_queue->Release();
    factory->Release();
    swap_chain1->Release();
    swap_chain->Release();

    if (hwnd) {
        ::DestroyWindow(hwnd);
    }

    if (wc.lpszClassName != nullptr) {
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    }

    return m_hooked;
}

bool D3D12Hook::unhook() {
    if (!m_hooked) {
        return true;
    }

    spdlog::info("Unhooking D3D12");

    m_present_hook.reset();
    m_present1_hook.reset();
    m_create_graphics_pipeline_state_hooks.clear();
    m_create_pipeline_state_hooks.clear();
    m_create_render_target_view_hooks.clear();
    m_create_depth_stencil_view_hooks.clear();
    m_set_pipeline_state_hooks.clear();
    m_create_graphics_pipeline_state_hook_lookup.clear();
    m_create_pipeline_state_hook_lookup.clear();
    m_create_render_target_view_hook_lookup.clear();
    m_create_depth_stencil_view_hook_lookup.clear();
    m_set_pipeline_state_hook_lookup.clear();
    m_swapchain_hook.reset();

    m_hooked = false;
    m_is_phase_1 = true;

    return true;
}

PointerHook* D3D12Hook::find_create_graphics_pipeline_state_hook(void* slot) const {
    if (const auto it = m_create_graphics_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_graphics_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_create_graphics_pipeline_state_hooks.empty() ? nullptr : m_create_graphics_pipeline_state_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_pipeline_state_hook(void* slot) const {
    if (const auto it = m_create_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_create_pipeline_state_hooks.empty() ? nullptr : m_create_pipeline_state_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_render_target_view_hook(void* slot) const {
    if (const auto it = m_create_render_target_view_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_render_target_view_hook_lookup.end()) {
        return it->second;
    }

    return m_create_render_target_view_hooks.empty() ? nullptr : m_create_render_target_view_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_depth_stencil_view_hook(void* slot) const {
    if (const auto it = m_create_depth_stencil_view_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_depth_stencil_view_hook_lookup.end()) {
        return it->second;
    }

    return m_create_depth_stencil_view_hooks.empty() ? nullptr : m_create_depth_stencil_view_hooks.front().get();
}

PointerHook* D3D12Hook::find_set_pipeline_state_hook(void* slot) const {
    if (const auto it = m_set_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_set_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_set_pipeline_state_hooks.empty() ? nullptr : m_set_pipeline_state_hooks.front().get();
}

thread_local int32_t g_present_depth = 0;

HRESULT D3D12Hook::present_internal(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params, bool present1) {
    auto d3d12 = g_d3d12_hook;

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    using Present1Fn = HRESULT(*)(IDXGISwapChain3*, UINT, UINT, DXGI_PRESENT_PARAMETERS*);
    Present1Fn present_fn{nullptr};

    if (!present1) {
        present_fn = d3d12->m_present_hook->get_original<Present1Fn>();
    } else {
        present_fn = d3d12->m_present1_hook->get_original<Present1Fn>();
    }

    if (d3d12->m_is_phase_1 && WindowFilter::get().is_filtered(swapchain_wnd)) {
        return present_fn(swap_chain, sync_interval, flags, params);
    }

    if (!d3d12->m_is_phase_1 && swap_chain != d3d12->m_swapchain_hook->get_instance()) {
        const auto og_instance = d3d12->m_swapchain_hook->get_instance();

        // If the original swapchain instance is invalid, then we should not proceed, and rehook the swapchain
        if (IsBadReadPtr(og_instance, sizeof(void*)) || IsBadReadPtr(og_instance.deref(), sizeof(void*))) {
            spdlog::error("Bad read pointer for original swapchain instance, re-hooking");
            d3d12->m_is_phase_1 = true;
        }

        if (!d3d12->m_is_phase_1) {
            return present_fn(swap_chain, sync_interval, flags, params);
        }
    }

    if (d3d12->m_is_phase_1) {
        //d3d12->m_present_hook.reset();
        d3d12->m_swapchain_hook.reset();

        // vtable hook the swapchain instead of global hooking
        // this seems safer for whatever reason
        // if we globally hook the vtable pointers, it causes all sorts of weird conflicts with other hooks
        // dont hook present though via this hook so other hooks dont get confused
        d3d12->m_swapchain_hook = std::make_unique<VtableHook>(swap_chain);
        //d3d12->m_swapchain_hook->hook_method(8, (uintptr_t)&D3D12Hook::present);
        d3d12->m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
        d3d12->m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);
        d3d12->m_is_phase_1 = false;
    }

    d3d12->m_inside_present = true;
    d3d12->m_swap_chain = swap_chain;

    swap_chain->GetDevice(IID_PPV_ARGS(&d3d12->m_device));

    if (d3d12->m_device != nullptr) {
        if (d3d12->m_using_proton_swapchain) {
            const auto real_swapchain = *(uintptr_t*)((uintptr_t)swap_chain + d3d12->m_proton_swapchain_offset);
            d3d12->m_command_queue = *(ID3D12CommandQueue**)(real_swapchain + d3d12->m_command_queue_offset);
        } else {
            d3d12->m_command_queue = *(ID3D12CommandQueue**)((uintptr_t)swap_chain + d3d12->m_command_queue_offset);
        }

        render::D3D12Diagnostics::get().begin_frame(
            d3d12->m_device,
            swap_chain,
            d3d12->m_command_queue,
            d3d12->m_render_width,
            d3d12->m_render_height,
            d3d12->m_display_width,
            d3d12->m_display_height,
            d3d12->m_using_proton_swapchain,
            d3d12->m_using_frame_generation_swapchain
        );
    }

    if (d3d12->m_swapchain_0 == nullptr) {
        d3d12->m_swapchain_0 = swap_chain;
    } else if (d3d12->m_swapchain_1 == nullptr && swap_chain != d3d12->m_swapchain_0) {
        d3d12->m_swapchain_1 = swap_chain;
    }
    
    // Restore the original bytes
    // if an infinite loop occurs, this will prevent the game from crashing
    // while keeping our hook intact
    if (g_present_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{present_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{present_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(present_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Present fixed");
        }

        if ((uintptr_t)present_fn != (uintptr_t)D3D12Hook::present && g_present_depth == 1) {
            spdlog::info("Attempting to call real present function");

            ++g_present_depth;
            const auto result = present_fn(swap_chain, sync_interval, flags, params);
            --g_present_depth;

            if (result != S_OK) {
                spdlog::error("Present failed: {:x}", result);
            }

            return result;
        }

        spdlog::info("Just returning S_OK");
        return S_OK;
    }

    if (d3d12->m_on_present) {
        d3d12->m_on_present(*d3d12);

        if (d3d12->m_next_present_interval) {
            sync_interval = *d3d12->m_next_present_interval;
            d3d12->m_next_present_interval = std::nullopt;

            if (sync_interval == 0) {
                BOOL is_fullscreen = 0;
                swap_chain->GetFullscreenState(&is_fullscreen, nullptr);
                flags &= ~DXGI_PRESENT_DO_NOT_SEQUENCE;

                DXGI_SWAP_CHAIN_DESC swap_desc{};
                swap_chain->GetDesc(&swap_desc);

                if (!is_fullscreen && (swap_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
                    flags |= DXGI_PRESENT_ALLOW_TEARING;
                }
            }
        }
    }

    ++g_present_depth;

    auto result = S_OK;
    
    if (!d3d12->m_ignore_next_present) {
        result = present_fn(swap_chain, sync_interval, flags, params);

        if (result != S_OK) {
            spdlog::error("Present failed: {:x}", result);
        }
    } else {
        d3d12->m_ignore_next_present = false;
    }

    --g_present_depth;

    if (d3d12->m_on_post_present) {
        d3d12->m_on_post_present(*d3d12);
    }

    d3d12->m_inside_present = false;

    return result;
}

HRESULT WINAPI D3D12Hook::present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};
    
    return D3D12Hook::present_internal(swap_chain, sync_interval, flags, nullptr, false);
}

HRESULT WINAPI D3D12Hook::present1(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    return D3D12Hook::present_internal(swap_chain, sync_interval, flags, params, true);
}

HRESULT WINAPI D3D12Hook::create_graphics_pipeline_state(
    ID3D12Device* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** pipeline_state
) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)device)[CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX];
    auto* hook = d3d12->find_create_graphics_pipeline_state_hook(slot);
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_graphics_pipeline_state)*>() : nullptr;

    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);

    if (SUCCEEDED(result) &&
        pipeline_state != nullptr &&
        *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        desc != nullptr) {
        render::ShaderOverrideRegistry::get().register_d3d12_graphics_pipeline_state_creation(
            device,
            static_cast<ID3D12PipelineState*>(*pipeline_state),
            desc
        );
    }

    return result;
}

HRESULT WINAPI D3D12Hook::create_pipeline_state(
    ID3D12Device2* device,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    REFIID riid,
    void** pipeline_state
) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)device)[CREATE_PIPELINE_STATE_VTABLE_INDEX];
    auto* hook = d3d12->find_create_pipeline_state_hook(slot);
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_pipeline_state)*>() : nullptr;

    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);

    if (SUCCEEDED(result) &&
        pipeline_state != nullptr &&
        *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        desc != nullptr) {
        render::ShaderOverrideRegistry::get().register_d3d12_pipeline_state_stream_creation(
            device,
            static_cast<ID3D12PipelineState*>(*pipeline_state),
            desc
        );
    }

    return result;
}

void WINAPI D3D12Hook::create_render_target_view(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor
) {
    (void)desc;
    auto d3d12 = g_d3d12_hook;
    const auto slot = device != nullptr ? &(*(void***)device)[CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_create_render_target_view_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_render_target_view)*>() : nullptr;

    if (original != nullptr) {
        original(device, resource, desc, descriptor);
    }

    render::D3D12Diagnostics::get().register_rtv_descriptor("D3D12Hook::CreateRenderTargetView", resource, descriptor);
}

void WINAPI D3D12Hook::create_depth_stencil_view(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor
) {
    (void)desc;
    auto d3d12 = g_d3d12_hook;
    const auto slot = device != nullptr ? &(*(void***)device)[CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_create_depth_stencil_view_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_depth_stencil_view)*>() : nullptr;

    if (original != nullptr) {
        original(device, resource, desc, descriptor);
    }

    render::D3D12Diagnostics::get().register_dsv_descriptor("D3D12Hook::CreateDepthStencilView", resource, descriptor);
}

void WINAPI D3D12Hook::set_pipeline_state(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)command_list)[SET_PIPELINE_STATE_VTABLE_INDEX];
    auto* hook = d3d12->find_set_pipeline_state_hook(slot);
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::set_pipeline_state)*>() : nullptr;

    if (original == nullptr) {
        return;
    }

    auto bound_pipeline_state = render::ShaderOverrideRegistry::get().resolve_d3d12_pipeline_state(pipeline_state);
    render::ShaderOverrideRegistry::get().note_d3d12_pipeline_state_bound(pipeline_state, bound_pipeline_state);
    original(command_list, bound_pipeline_state);
}

thread_local int32_t g_resize_buffers_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize buffers called");
    spdlog::info(" Parameters: buffer_count {} width {} height {} new_format {} swap_chain_flags {}", buffer_count, width, height, (uint32_t)new_format, swap_chain_flags);

    auto d3d12 = g_d3d12_hook;
    //auto& hook = d3d12->m_resize_buffers_hook;
    //auto resize_buffers_fn = hook->get_original<decltype(D3D12Hook::resize_buffers)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_buffers_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers)*>(13);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;

    if (g_resize_buffers_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_buffers_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_buffers_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_buffers_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize buffers fixed");
        }

        if ((uintptr_t)resize_buffers_fn != (uintptr_t)&D3D12Hook::resize_buffers && g_resize_buffers_depth == 1) {
            spdlog::info("Attempting to call the real resize buffers function");

            ++g_resize_buffers_depth;
            const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
            --g_resize_buffers_depth;

            if (result != S_OK) {
                spdlog::error("Resize buffers failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_buffers) {
        d3d12->m_on_resize_buffers(*d3d12, width, height);
    }

    ++g_resize_buffers_depth;

    const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    
    if (result != S_OK) {
        spdlog::error("Resize buffers failed: {:x}", result);
    }

    --g_resize_buffers_depth;

    return result;
}

thread_local int32_t g_resize_target_depth = 0;

HRESULT WINAPI D3D12Hook::resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize target called");
    spdlog::info(" Parameters: new_target_parameters {:x}", (uintptr_t)new_target_parameters);

    auto d3d12 = g_d3d12_hook;
    //auto resize_target_fn = d3d12->m_resize_target_hook->get_original<decltype(D3D12Hook::resize_target)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_target_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_target)*>(14);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_target_fn(swap_chain, new_target_parameters);
    }

    d3d12->m_render_width = new_target_parameters->Width;
    d3d12->m_render_height = new_target_parameters->Height;

    // Restore the original code to the resize_buffers function.
    if (g_resize_target_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_target_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_target_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_target_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize target fixed");
        }

        if ((uintptr_t)resize_target_fn != (uintptr_t)&D3D12Hook::resize_target && g_resize_target_depth == 1) {
            spdlog::info("Attempting to call the real resize target function");

            ++g_resize_target_depth;
            const auto result = resize_target_fn(swap_chain, new_target_parameters);
            --g_resize_target_depth;

            if (result != S_OK) {
                spdlog::error("Resize target failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_target) {
        d3d12->m_on_resize_target(*d3d12, new_target_parameters->Width, new_target_parameters->Height);
    }

    ++g_resize_target_depth;

    const auto result = resize_target_fn(swap_chain, new_target_parameters);
    
    if (result != S_OK) {
        spdlog::error("Resize target failed: {:x}", result);
    }

    --g_resize_target_depth;

    return result;
}

/*HRESULT WINAPI D3D12Hook::create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain)
{
    spdlog::info("D3D12 create swapchain called");

    auto d3d12 = g_d3d12_hook;

    d3d12->m_command_queue = (ID3D12CommandQueue*)device;
    
    if (d3d12->m_on_create_swap_chain) {
        d3d12->m_on_create_swap_chain(*d3d12);
    }

    auto create_swap_chain_fn = d3d12->m_create_swap_chain_hook->get_original<decltype(D3D12Hook::create_swap_chain)>();

    return create_swap_chain_fn(factory, device, hwnd, desc, p_fullscreen_desc, p_restrict_to_output, swap_chain);
}*/

