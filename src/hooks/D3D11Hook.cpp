#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility/Logging.hpp>
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

static bool daysgone_native_snapshot_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        if (!exe_path) {
            return false;
        }

        auto lower = std::wstring{*exe_path};
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        return lower.ends_with(L"\\daysgone.exe") ||
               lower.ends_with(L"/daysgone.exe") ||
               lower == L"daysgone.exe";
    }();

    return result;
}

static bool is_daysgone_native_packed_uav(
    const D3D11_TEXTURE2D_DESC& desc,
    uint32_t eye_width,
    uint32_t eye_height)
{
    const bool bgra_format =
        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    return eye_width != 0 && eye_height != 0 && eye_width <= UINT32_MAX / 2 &&
           desc.Width == eye_width * 2 && desc.Height == eye_height &&
           desc.MipLevels == 1 && desc.ArraySize == 1 &&
           desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags == 0 &&
           desc.SampleDesc.Count == 1 &&
           (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 &&
           bgra_format;
}

static bool get_daysgone_view_texture(
    ID3D11View* view,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    D3D11_TEXTURE2D_DESC& desc)
{
    if (view == nullptr) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Resource> resource{};
    view->GetResource(&resource);
    if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr) {
        return false;
    }

    texture->GetDesc(&desc);
    return true;
}

static bool is_daysgone_native_final_color_dispatch(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* packed_target,
    uint32_t eye_width,
    uint32_t eye_height,
    UINT groups_x,
    UINT groups_y,
    UINT groups_z)
{
    if (context == nullptr || packed_target == nullptr || eye_width == 0 || eye_height == 0 ||
        groups_x != (eye_width + 7) / 8 || groups_y != (eye_height + 7) / 8 || groups_z != 1)
    {
        return false;
    }

    std::array<ID3D11ShaderResourceView*, 4> raw_srvs{};
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> srvs{};
    context->CSGetShaderResources(0, static_cast<UINT>(raw_srvs.size()), raw_srvs.data());
    for (size_t index = 0; index < raw_srvs.size(); ++index) {
        srvs[index].Attach(raw_srvs[index]);
    }

    std::array<ID3D11UnorderedAccessView*, 2> raw_uavs{};
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 2> uavs{};
    context->CSGetUnorderedAccessViews(0, static_cast<UINT>(raw_uavs.size()), raw_uavs.data());
    for (size_t index = 0; index < raw_uavs.size(); ++index) {
        uavs[index].Attach(raw_uavs[index]);
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> constant_texture{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> packed_uav_texture{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdr_scene_texture{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> postprocess_texture{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> auxiliary_uav_texture{};
    D3D11_TEXTURE2D_DESC constant_desc{};
    D3D11_TEXTURE2D_DESC packed_uav_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};
    D3D11_TEXTURE2D_DESC hdr_scene_desc{};
    D3D11_TEXTURE2D_DESC postprocess_desc{};
    D3D11_TEXTURE2D_DESC auxiliary_uav_desc{};

    if (!get_daysgone_view_texture(srvs[0].Get(), constant_texture, constant_desc) ||
        !get_daysgone_view_texture(uavs[0].Get(), packed_uav_texture, packed_uav_desc) ||
        packed_uav_texture.Get() != packed_target ||
        !is_daysgone_native_packed_uav(packed_uav_desc, eye_width, eye_height) ||
        !get_daysgone_view_texture(srvs[1].Get(), depth_texture, depth_desc) ||
        !get_daysgone_view_texture(srvs[2].Get(), hdr_scene_texture, hdr_scene_desc) ||
        !get_daysgone_view_texture(srvs[3].Get(), postprocess_texture, postprocess_desc) ||
        !get_daysgone_view_texture(uavs[1].Get(), auxiliary_uav_texture, auxiliary_uav_desc))
    {
        return false;
    }

    const auto is_eye_sized = [&](const D3D11_TEXTURE2D_DESC& candidate) {
        return candidate.Width == eye_width && candidate.Height == eye_height &&
               candidate.MipLevels == 1 && candidate.ArraySize == 1 &&
               candidate.SampleDesc.Count == 1;
    };

    const bool constant_matches =
        constant_desc.Width == 1 && constant_desc.Height == 1 &&
        constant_desc.MipLevels == 1 && constant_desc.ArraySize == 1 &&
        constant_desc.SampleDesc.Count == 1 &&
        constant_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;

    return constant_matches &&
           is_eye_sized(depth_desc) && depth_desc.Format == DXGI_FORMAT_R32_UINT &&
           is_eye_sized(hdr_scene_desc) && hdr_scene_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
           is_eye_sized(postprocess_desc) && postprocess_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
           is_eye_sized(auxiliary_uav_desc) && auxiliary_uav_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
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

bool D3D11Hook::prepare_daysgone_native_snapshot() {
    return daysgone_native_snapshot_guard_enabled() &&
           m_device != nullptr &&
           hook_daysgone_native_context(m_device);
}

uint64_t D3D11Hook::begin_daysgone_native_snapshot(
    uint64_t capture_generation,
    uint32_t width,
    uint32_t height)
{
    if (capture_generation == 0 || width == 0 || height == 0 ||
        !prepare_daysgone_native_snapshot())
    {
        return 0;
    }

    const auto transaction =
        m_daysgone_native_snapshot_transaction_counter.fetch_add(1, std::memory_order_acq_rel) + 1;

    {
        std::scoped_lock lock{m_daysgone_native_snapshot_mutex};
        m_daysgone_native_packed_target.Reset();
        m_daysgone_native_packed_desc = {};
        m_daysgone_native_packed_transaction = 0;
        m_daysgone_native_snapshot_generation.store(capture_generation, std::memory_order_relaxed);
        m_daysgone_native_snapshot_width.store(width, std::memory_order_relaxed);
        m_daysgone_native_snapshot_height.store(height, std::memory_order_relaxed);
        m_daysgone_native_snapshot_transaction.store(transaction, std::memory_order_relaxed);
        m_daysgone_native_snapshot_armed.store(true, std::memory_order_release);
    }

    return transaction;
}

void D3D11Hook::cancel_daysgone_native_snapshot(uint64_t transaction_serial) {
    if (transaction_serial == 0) {
        return;
    }

    std::scoped_lock lock{m_daysgone_native_snapshot_mutex};
    if (m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed) != transaction_serial) {
        return;
    }

    m_daysgone_native_snapshot_armed.store(false, std::memory_order_release);
    m_daysgone_native_packed_target.Reset();
    m_daysgone_native_packed_desc = {};
    m_daysgone_native_packed_transaction = 0;
}

std::shared_ptr<const D3D11Hook::DaysGoneNativeSnapshot>
D3D11Hook::get_daysgone_native_snapshot(
    uint64_t transaction_serial,
    uint64_t capture_generation) const
{
    if (transaction_serial == 0 || capture_generation == 0) {
        return nullptr;
    }

    const auto snapshot = m_daysgone_native_snapshot_ready.load(std::memory_order_acquire);
    if (snapshot == nullptr || snapshot->transaction_serial != transaction_serial ||
        snapshot->capture_generation != capture_generation || snapshot->texture == nullptr)
    {
        return nullptr;
    }

    return snapshot;
}

void D3D11Hook::reset_daysgone_native_snapshot_state() {
    m_daysgone_native_snapshot_armed.store(false, std::memory_order_release);
    m_daysgone_native_snapshot_ready.store(nullptr, std::memory_order_release);

    std::scoped_lock lock{m_daysgone_native_snapshot_mutex};
    m_daysgone_native_packed_target.Reset();
    m_daysgone_native_packed_desc = {};
    m_daysgone_native_packed_transaction = 0;
    m_daysgone_native_snapshot_transaction.store(0, std::memory_order_relaxed);
    m_daysgone_native_snapshot_generation.store(0, std::memory_order_relaxed);
    m_daysgone_native_snapshot_width.store(0, std::memory_order_relaxed);
    m_daysgone_native_snapshot_height.store(0, std::memory_order_relaxed);
    m_daysgone_native_snapshot_next_slot = 0;
    for (auto& slot : m_daysgone_native_snapshot_slots) {
        slot.reset();
    }
}

bool D3D11Hook::create_daysgone_native_snapshot_slots_locked(
    const D3D11_TEXTURE2D_DESC& packed_desc,
    uint32_t eye_width,
    uint32_t eye_height)
{
    if (m_daysgone_native_snapshot_device == nullptr ||
        !is_daysgone_native_packed_uav(packed_desc, eye_width, eye_height))
    {
        return false;
    }

    auto snapshot_desc = packed_desc;
    snapshot_desc.Width = eye_width;
    snapshot_desc.Height = eye_height;
    snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
    snapshot_desc.CPUAccessFlags = 0;
    snapshot_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    snapshot_desc.MiscFlags = 0;

    const auto slots_match = std::all_of(
        m_daysgone_native_snapshot_slots.begin(),
        m_daysgone_native_snapshot_slots.end(),
        [&](const auto& slot) {
            return slot != nullptr && slot->texture != nullptr &&
                   slot->desc.Width == snapshot_desc.Width &&
                   slot->desc.Height == snapshot_desc.Height &&
                   slot->desc.Format == snapshot_desc.Format &&
                   slot->desc.MipLevels == snapshot_desc.MipLevels &&
                   slot->desc.ArraySize == snapshot_desc.ArraySize &&
                   slot->desc.SampleDesc.Count == snapshot_desc.SampleDesc.Count &&
                   slot->desc.SampleDesc.Quality == snapshot_desc.SampleDesc.Quality;
        });

    if (slots_match) {
        return true;
    }

    m_daysgone_native_snapshot_ready.store(nullptr, std::memory_order_release);
    for (auto& slot : m_daysgone_native_snapshot_slots) {
        slot.reset();
    }
    m_daysgone_native_snapshot_next_slot = 0;

    for (auto& slot : m_daysgone_native_snapshot_slots) {
        auto created = std::make_shared<DaysGoneNativeSnapshot>();
        const auto result = m_daysgone_native_snapshot_device->CreateTexture2D(
            &snapshot_desc,
            nullptr,
            &created->texture);
        if (FAILED(result) || created->texture == nullptr) {
            for (auto& cleanup : m_daysgone_native_snapshot_slots) {
                cleanup.reset();
            }
            spdlog::error(
                "[DaysGone][NativeFix][D3D11] Failed to create persistent right-eye snapshot pool: "
                "hr=0x{:08X} size={}x{} format={}",
                static_cast<uint32_t>(result),
                snapshot_desc.Width,
                snapshot_desc.Height,
                static_cast<uint32_t>(snapshot_desc.Format));
            return false;
        }

        created->texture->GetDesc(&created->desc);
        slot = std::move(created);
    }

    spdlog::info(
        "[DaysGone][NativeFix][D3D11] Created {} persistent right-eye snapshot slots [{}x{} format={}]",
        DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT,
        snapshot_desc.Width,
        snapshot_desc.Height,
        static_cast<uint32_t>(snapshot_desc.Format));
    return true;
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
        m_daysgone_cs_set_uavs_hook.reset();
        m_daysgone_dispatch_hook.reset();
        m_daysgone_context_vtable = nullptr;
        m_daysgone_native_snapshot_context = nullptr;

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

    const auto daysgone_cs_unhooked =
        m_daysgone_cs_set_uavs_hook == nullptr ||
        m_daysgone_cs_set_uavs_hook->remove();
    m_daysgone_cs_set_uavs_hook.reset();
    m_daysgone_dispatch_hook.reset();
    const auto daysgone_dispatch_unhooked = true;
    m_daysgone_context_vtable = nullptr;
    m_daysgone_native_snapshot_context = nullptr;
    reset_daysgone_native_snapshot_state();
    m_daysgone_native_snapshot_device.Reset();

    if (uav_unhooked &&
        tex2d_unhooked &&
        naruto_draw_unhooked &&
        daysgone_cs_unhooked &&
        daysgone_dispatch_unhooked &&
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
        d3d11->hook_daysgone_native_context(d3d11->m_device);
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
    d3d11->reset_daysgone_native_snapshot_state();

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

bool D3D11Hook::hook_daysgone_native_context(ID3D11Device* device) {
    if (!daysgone_native_snapshot_guard_enabled() || device == nullptr) {
        return false;
    }

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        return false;
    }

    auto** const vtable = *reinterpret_cast<void***>(context.Get());
    if (vtable == nullptr) {
        return false;
    }

    std::scoped_lock hook_lock{m_daysgone_context_hook_mutex};
    if (m_daysgone_cs_set_uavs_hook != nullptr &&
        m_daysgone_dispatch_hook &&
        m_daysgone_context_vtable == vtable &&
        m_daysgone_native_snapshot_context == context.Get())
    {
        // The D3D11 runtime can lazily rewrite individual context-vtable
        // entries. Keep the context-local CS hook restored; Dispatch is hooked
        // inline because Days Gone rewrites that slot on every use.
        if (!m_daysgone_cs_set_uavs_hook->restore()) {
            return false;
        }
        if (!m_daysgone_dispatch_hook.enabled()) {
            const auto enable_result = m_daysgone_dispatch_hook.enable();
            if (!enable_result.has_value()) {
                return false;
            }
        }
        if (m_daysgone_native_snapshot_device.Get() != device) {
            reset_daysgone_native_snapshot_state();
            m_daysgone_native_snapshot_device = device;
        }
        return true;
    }

    // Do not replace a live context hook from another implementation table.
    // A device transition resets through Framework before a new game context
    // can safely be instrumented.
    if (m_daysgone_cs_set_uavs_hook != nullptr ||
        m_daysgone_dispatch_hook)
    {
        return false;
    }

    try {
        // ID3D11DeviceContext vtable slots from the public D3D11 ABI.
        auto& dispatch_fn = vtable[41];
        auto& cs_set_uavs_fn = vtable[68];
        if (dispatch_fn == nullptr || cs_set_uavs_fn == nullptr ||
            cs_set_uavs_fn == reinterpret_cast<void*>(&D3D11Hook::daysgone_cs_set_unordered_access_views))
        {
            return false;
        }

        auto dispatch_hook = safetyhook::create_inline(
            dispatch_fn,
            reinterpret_cast<void*>(&D3D11Hook::daysgone_dispatch),
            safetyhook::InlineHook::StartDisabled);
        if (!dispatch_hook) {
            spdlog::error(
                "[DaysGone][NativeFix][D3D11] Failed to inline-hook the live Dispatch implementation");
            return false;
        }

        m_daysgone_cs_set_uavs_hook = std::make_unique<PointerHook>(
            &cs_set_uavs_fn,
            reinterpret_cast<void*>(&D3D11Hook::daysgone_cs_set_unordered_access_views));
        m_daysgone_dispatch_hook = std::move(dispatch_hook);
        m_daysgone_context_vtable = vtable;
        m_daysgone_native_snapshot_context = context.Get();
        m_daysgone_native_snapshot_device = device;
        const auto enable_result = m_daysgone_dispatch_hook.enable();
        if (!enable_result.has_value()) {
            throw std::runtime_error("failed to enable the Days Gone Dispatch inline hook");
        }
        spdlog::info(
            "[DaysGone][NativeFix][D3D11] Hooked live CS binding and the stable Dispatch implementation for persistent right-eye snapshots");
        return true;
    } catch (const std::exception& e) {
        if (m_daysgone_cs_set_uavs_hook != nullptr) {
            m_daysgone_cs_set_uavs_hook->remove();
            m_daysgone_cs_set_uavs_hook.reset();
        }
        m_daysgone_dispatch_hook.reset();
        m_daysgone_context_vtable = nullptr;
        m_daysgone_native_snapshot_context = nullptr;
        spdlog::error(
            "[DaysGone][NativeFix][D3D11] Failed to hook the live CS/Dispatch final-color boundary: {}",
            e.what());
    } catch (...) {
        if (m_daysgone_cs_set_uavs_hook != nullptr) {
            m_daysgone_cs_set_uavs_hook->remove();
            m_daysgone_cs_set_uavs_hook.reset();
        }
        m_daysgone_dispatch_hook.reset();
        m_daysgone_context_vtable = nullptr;
        m_daysgone_native_snapshot_context = nullptr;
        spdlog::error("[DaysGone][NativeFix][D3D11] Failed to hook the live CS/Dispatch final-color boundary");
    }

    return false;
}

void WINAPI D3D11Hook::daysgone_cs_set_unordered_access_views(
    ID3D11DeviceContext* context,
    UINT start_slot,
    UINT num_uavs,
    ID3D11UnorderedAccessView* const* uavs,
    const UINT* initial_counts)
{
    auto* const d3d11 = g_d3d11_hook;
    auto original = d3d11->m_daysgone_cs_set_uavs_hook
        ->get_original<decltype(D3D11Hook::daysgone_cs_set_unordered_access_views)*>();

    if (d3d11->m_daysgone_native_snapshot_armed.load(std::memory_order_acquire) &&
        context != nullptr && start_slot == 0 && num_uavs >= 1 &&
        uavs != nullptr && uavs[0] != nullptr)
    {
        const auto transaction =
            d3d11->m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed);
        const auto width = d3d11->m_daysgone_native_snapshot_width.load(std::memory_order_relaxed);
        const auto height = d3d11->m_daysgone_native_snapshot_height.load(std::memory_order_relaxed);

        ComPtr<ID3D11Resource> packed_resource{};
        ComPtr<ID3D11Texture2D> packed_texture{};
        uavs[0]->GetResource(&packed_resource);
        if (packed_resource != nullptr &&
            SUCCEEDED(packed_resource.As(&packed_texture)) &&
            packed_texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC packed_desc{};
            packed_texture->GetDesc(&packed_desc);

            ComPtr<ID3D11Device> packed_device{};
            packed_texture->GetDevice(&packed_device);
            if (packed_device.Get() == d3d11->m_daysgone_native_snapshot_device.Get() &&
                is_daysgone_native_packed_uav(packed_desc, width, height))
            {
                std::scoped_lock lock{d3d11->m_daysgone_native_snapshot_mutex};
                if (d3d11->m_daysgone_native_snapshot_armed.load(std::memory_order_relaxed) &&
                    d3d11->m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed) == transaction)
                {
                    d3d11->m_daysgone_native_packed_target = packed_texture;
                    d3d11->m_daysgone_native_packed_desc = packed_desc;
                    d3d11->m_daysgone_native_packed_transaction = transaction;
                }
            }
        }
    }

    original(context, start_slot, num_uavs, uavs, initial_counts);
}

void WINAPI D3D11Hook::daysgone_dispatch(
    ID3D11DeviceContext* context,
    UINT thread_group_count_x,
    UINT thread_group_count_y,
    UINT thread_group_count_z)
{
    auto* const d3d11 = g_d3d11_hook;
    auto original = d3d11->m_daysgone_dispatch_hook
        .original<decltype(D3D11Hook::daysgone_dispatch)*>();

    if (original == nullptr) {
        return;
    }

    if (!d3d11->m_daysgone_native_snapshot_armed.load(std::memory_order_acquire) ||
        context == nullptr ||
        context != d3d11->m_daysgone_native_snapshot_context)
    {
        original(context, thread_group_count_x, thread_group_count_y, thread_group_count_z);
        return;
    }

    const auto transaction =
        d3d11->m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed);
    const auto generation =
        d3d11->m_daysgone_native_snapshot_generation.load(std::memory_order_relaxed);
    const auto width = d3d11->m_daysgone_native_snapshot_width.load(std::memory_order_relaxed);
    const auto height = d3d11->m_daysgone_native_snapshot_height.load(std::memory_order_relaxed);
    ComPtr<ID3D11Texture2D> packed_target{};
    D3D11_TEXTURE2D_DESC packed_desc{};

    {
        std::scoped_lock lock{d3d11->m_daysgone_native_snapshot_mutex};
        if (d3d11->m_daysgone_native_snapshot_armed.load(std::memory_order_relaxed) &&
            d3d11->m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed) == transaction &&
            d3d11->m_daysgone_native_packed_transaction == transaction &&
            d3d11->m_daysgone_native_packed_target != nullptr)
        {
            packed_target = d3d11->m_daysgone_native_packed_target;
            packed_desc = d3d11->m_daysgone_native_packed_desc;
        }
    }

    const bool is_final_color_dispatch =
        packed_target != nullptr &&
        is_daysgone_native_packed_uav(packed_desc, width, height) &&
        is_daysgone_native_final_color_dispatch(
            context,
            packed_target.Get(),
            width,
            height,
            thread_group_count_x,
            thread_group_count_y,
            thread_group_count_z);

    original(context, thread_group_count_x, thread_group_count_y, thread_group_count_z);

    if (!is_final_color_dispatch) {
        return;
    }

    std::scoped_lock lock{d3d11->m_daysgone_native_snapshot_mutex};
    if (!d3d11->m_daysgone_native_snapshot_armed.load(std::memory_order_relaxed) ||
        d3d11->m_daysgone_native_snapshot_transaction.load(std::memory_order_relaxed) != transaction ||
        d3d11->m_daysgone_native_packed_transaction != transaction ||
        d3d11->m_daysgone_native_packed_target.Get() != packed_target.Get() ||
        !d3d11->create_daysgone_native_snapshot_slots_locked(packed_desc, width, height))
    {
        return;
    }

    std::shared_ptr<DaysGoneNativeSnapshot> selected{};
    for (size_t offset = 0; offset < DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT; ++offset) {
        const auto index =
            (d3d11->m_daysgone_native_snapshot_next_slot + offset) %
            DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT;
        auto& slot = d3d11->m_daysgone_native_snapshot_slots[index];
        if (slot != nullptr && slot.use_count() == 1) {
            selected = slot;
            d3d11->m_daysgone_native_snapshot_next_slot =
                (index + 1) % DAYS_GONE_NATIVE_SNAPSHOT_SLOT_COUNT;
            break;
        }
    }

    if (selected != nullptr) {
        const D3D11_BOX secondary_region{0, 0, 0, width, height, 1};
        context->CopySubresourceRegion(
            selected->texture.Get(),
            0,
            0,
            0,
            0,
            packed_target.Get(),
            0,
            &secondary_region);
        selected->capture_generation = generation;
        selected->transaction_serial = transaction;
        d3d11->m_daysgone_native_snapshot_ready.store(
            std::shared_ptr<const DaysGoneNativeSnapshot>{selected},
            std::memory_order_release);
        d3d11->m_daysgone_native_snapshot_armed.store(false, std::memory_order_release);

        static std::atomic_bool logged_snapshot{false};
        if (!logged_snapshot.exchange(true, std::memory_order_relaxed)) {
            spdlog::info(
                "[DaysGone][NativeFix][D3D11] Snapshotted the final-color secondary eye "
                "from the packed BGRA target after its validated compute dispatch");
        }
    } else {
        d3d11->m_daysgone_native_snapshot_armed.store(false, std::memory_order_release);
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[DaysGone][NativeFix][D3D11] Persistent snapshot pool is still in use; "
            "skipping this right-eye transaction");
    }

    d3d11->m_daysgone_native_packed_target.Reset();
    d3d11->m_daysgone_native_packed_desc = {};
    d3d11->m_daysgone_native_packed_transaction = 0;
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
