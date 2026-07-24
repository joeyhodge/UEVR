#include <imgui.h>
#include <imgui_internal.h>
#include <openvr.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace vertex_shader1 {
#include "shaders/vs.hpp"
}

namespace pixel_shader1 {
#include "shaders/ps.hpp"
}

#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>
#include <utility/Module.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include "D3D11Component.hpp"

//#define VERBOSE_D3D11

#ifdef VERBOSE_D3D11
#define LOG_VERBOSE(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_VERBOSE 
#endif

#define SHADER_TEMP_DISABLED
//#define AFR_DEPTH_TEMP_DISABLED

namespace vrmod {
namespace {
constexpr const char* k_ui_invert_ps_hlsl = R"(
Texture2D Texture : register(t0);
SamplerState TextureSampler : register(s0);

float4 SpritePixelShader(float4 color : COLOR0, float2 texCoord : TEXCOORD0) : SV_Target
{
    float4 tex = Texture.Sample(TextureSampler, texCoord);
    float invertAmount = saturate(color.a);
    float blendedAlpha = lerp(tex.a, 1.0 - tex.a, invertAmount);
    float3 blendedColor = tex.rgb * color.rgb;
    return float4(blendedColor, blendedAlpha);
}
)";

constexpr const char* k_daysgone_ui_key_ps_hlsl = R"(
Texture2D Texture : register(t0);
SamplerState TextureSampler : register(s0);

float4 SpritePixelShader(float4 color : COLOR0, float2 texCoord : TEXCOORD0) : SV_Target
{
    float4 tex = Texture.Sample(TextureSampler, texCoord);
    float threshold = saturate(color.r);
    float softness = max(color.g, 0.001);
    float opacity = saturate(color.a);
    float luma = max(max(tex.r, tex.g), tex.b);
    float alpha = smoothstep(threshold, threshold + softness, luma) * opacity;
    return float4(tex.rgb, alpha);
}
)";

bool is_daysgone_executable() {
    static const auto is_daysgone = []() {
        const auto path = utility::get_module_pathw(utility::get_executable());
        if (!path) {
            return false;
        }

        auto lower = std::wstring{*path};
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        return lower.ends_with(L"\\daysgone.exe") || lower.ends_with(L"/daysgone.exe") || lower == L"daysgone.exe";
    }();

    return is_daysgone;
}

bool is_naruto_executable() {
    static const auto is_naruto = []() {
        const auto path = utility::get_module_pathw(utility::get_executable());
        if (!path) {
            return false;
        }

        auto lower = std::wstring{*path};
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        return lower.ends_with(L"\\naruto-win64-shipping.exe") ||
               lower.ends_with(L"/naruto-win64-shipping.exe") ||
               lower == L"naruto-win64-shipping.exe";
    }();

    return is_naruto;
}

bool needs_naruto_d3d11_color_conversion(const D3D11_TEXTURE2D_DESC& desc) {
    return is_naruto_executable() &&
           (desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
            desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
}

bool is_d3d11_or_dxgi_com_object(IUnknown* object) {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return false;
    }

    auto* const vtable = *(void**)object;
    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        return false;
    }

    const auto module = utility::get_module_within(vtable);
    if (!module) {
        return false;
    }

    const auto module_path = utility::get_module_path(*module);
    if (!module_path) {
        return false;
    }

    auto lower = std::string{*module_path};
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("d3d11") != std::string::npos || lower.find("dxgi") != std::string::npos;
}

bool is_valid_daysgone_d3d11_desc(const D3D11_TEXTURE2D_DESC& desc) {
    return desc.Width >= 640 && desc.Height >= 360 && desc.Width <= 8192 && desc.Height <= 8192;
}

struct DaysGoneViewFormats {
    DXGI_FORMAT rtv{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    DXGI_FORMAT srv{DXGI_FORMAT_B8G8R8A8_UNORM};
};

DaysGoneViewFormats get_daysgone_ui_view_formats(const D3D11_TEXTURE2D_DESC& desc) {
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    default:
        return {};
    }
}

DaysGoneViewFormats get_naruto_ui_view_formats(const D3D11_TEXTURE2D_DESC& desc) {
    switch (desc.Format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    default:
        return {};
    }
}

bool try_query_daysgone_texture(IUnknown* object, ID3D11Texture2D*& out_native, D3D11_TEXTURE2D_DESC& out_desc) {
    out_native = nullptr;
    out_desc = {};

    if (!is_d3d11_or_dxgi_com_object(object)) {
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();

        if (is_valid_daysgone_d3d11_desc(desc)) {
            out_native = texture;
            out_desc = desc;
            return true;
        }
    }

    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&srv))) && srv != nullptr) {
        ID3D11Resource* resource = nullptr;
        srv->GetResource(&resource);
        srv->Release();

        if (resource != nullptr) {
            texture = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                texture->Release();
                resource->Release();

                if (is_valid_daysgone_d3d11_desc(desc)) {
                    out_native = texture;
                    out_desc = desc;
                    return true;
                }

                return false;
            }

            resource->Release();
        }
    }

    return false;
}

bool read_daysgone_ptr(uintptr_t address, uintptr_t& out) {
    out = 0;
    if (address == 0 || IsBadReadPtr((void*)address, sizeof(uintptr_t))) {
        return false;
    }

    out = *reinterpret_cast<const uintptr_t*>(address);
    return out >= 0x10000;
}

ID3D11Texture2D* try_get_daysgone_native_texture(FRHITexture2D* texture, D3D11_TEXTURE2D_DESC* out_desc = nullptr) {
    if (!is_daysgone_executable() || texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    // Days Gone's Bend Slate path hands UEVR a UE4.11 wrapper, not the native
    // resource itself. Live evidence shows wrapper +0xA0 is the native
    // ID3D11Texture2D. Avoid broad COM probing on injection.
    constexpr std::array<uintptr_t, 2> kWrapperOffsets{0x8, 0x10};
    constexpr uintptr_t kNativeTextureOffset = 0xA0;

    const auto base = reinterpret_cast<uintptr_t>(texture);
    for (const auto outer_offset : kWrapperOffsets) {
        uintptr_t wrapper{};
        if (!read_daysgone_ptr(base + outer_offset, wrapper)) {
            continue;
        }

        uintptr_t raw_native{};
        if (!read_daysgone_ptr(wrapper + kNativeTextureOffset, raw_native)) {
            continue;
        }

        auto* const native = reinterpret_cast<ID3D11Texture2D*>(raw_native);
        if (!is_d3d11_or_dxgi_com_object(reinterpret_cast<IUnknown*>(native))) {
            continue;
        }

        D3D11_TEXTURE2D_DESC desc{};
        native->GetDesc(&desc);
        if (!is_valid_daysgone_d3d11_desc(desc)) {
            continue;
        }

        if (out_desc != nullptr) {
            *out_desc = desc;
        }

        return native;
    }

    return nullptr;
}
} // namespace

class DX11StateBackup {
public:
    DX11StateBackup(ID3D11DeviceContext* context) {
        if (context == nullptr) {
            throw std::runtime_error("DX11StateBackup: context is null");
        }

        m_context = context;

        m_old.ScissorRectsCount = m_old.ViewportsCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_context->RSGetScissorRects(&m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSGetViewports(&m_old.ViewportsCount, m_old.Viewports);
        m_context->RSGetState(&m_old.RS);
        m_context->OMGetBlendState(&m_old.BlendState, m_old.BlendFactor, &m_old.SampleMask);
        m_context->OMGetDepthStencilState(&m_old.DepthStencilState, &m_old.StencilRef);
        m_context->PSGetShaderResources(0, 1, &m_old.PSShaderResource);
        m_context->PSGetSamplers(0, 1, &m_old.PSSampler);
        m_old.PSInstancesCount = m_old.VSInstancesCount = m_old.GSInstancesCount = 256;
        m_context->PSGetShader(&m_old.PS, m_old.PSInstances, &m_old.PSInstancesCount);
        m_context->VSGetShader(&m_old.VS, m_old.VSInstances, &m_old.VSInstancesCount);
        m_context->VSGetConstantBuffers(0, 1, &m_old.VSConstantBuffer);
        m_context->GSGetShader(&m_old.GS, m_old.GSInstances, &m_old.GSInstancesCount);

        m_context->IAGetPrimitiveTopology(&m_old.PrimitiveTopology);
        m_context->IAGetIndexBuffer(&m_old.IndexBuffer, &m_old.IndexBufferFormat, &m_old.IndexBufferOffset);
        m_context->IAGetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset);
        m_context->IAGetInputLayout(&m_old.InputLayout);
    }

    virtual ~DX11StateBackup() {
        m_context->RSSetScissorRects(m_old.ScissorRectsCount, m_old.ScissorRects);
        m_context->RSSetViewports(m_old.ViewportsCount, m_old.Viewports);
        m_context->RSSetState(m_old.RS); if (m_old.RS) m_old.RS->Release();
        m_context->OMSetBlendState(m_old.BlendState, m_old.BlendFactor, m_old.SampleMask); if (m_old.BlendState) m_old.BlendState->Release();
        m_context->OMSetDepthStencilState(m_old.DepthStencilState, m_old.StencilRef); if (m_old.DepthStencilState) m_old.DepthStencilState->Release();
        m_context->PSSetShaderResources(0, 1, &m_old.PSShaderResource); if (m_old.PSShaderResource) m_old.PSShaderResource->Release();
        m_context->PSSetSamplers(0, 1, &m_old.PSSampler); if (m_old.PSSampler) m_old.PSSampler->Release();
        m_context->PSSetShader(m_old.PS, m_old.PSInstances, m_old.PSInstancesCount); if (m_old.PS) m_old.PS->Release();
        for (UINT i = 0; i < m_old.PSInstancesCount; i++) if (m_old.PSInstances[i]) m_old.PSInstances[i]->Release();
        m_context->VSSetShader(m_old.VS, m_old.VSInstances, m_old.VSInstancesCount); if (m_old.VS) m_old.VS->Release();
        m_context->VSSetConstantBuffers(0, 1, &m_old.VSConstantBuffer); if (m_old.VSConstantBuffer) m_old.VSConstantBuffer->Release();
        m_context->GSSetShader(m_old.GS, m_old.GSInstances, m_old.GSInstancesCount); if (m_old.GS) m_old.GS->Release();
        for (UINT i = 0; i < m_old.VSInstancesCount; i++) if (m_old.VSInstances[i]) m_old.VSInstances[i]->Release();
        m_context->IASetPrimitiveTopology(m_old.PrimitiveTopology);
        m_context->IASetIndexBuffer(m_old.IndexBuffer, m_old.IndexBufferFormat, m_old.IndexBufferOffset); if (m_old.IndexBuffer) m_old.IndexBuffer->Release();
        m_context->IASetVertexBuffers(0, 1, &m_old.VertexBuffer, &m_old.VertexBufferStride, &m_old.VertexBufferOffset); if (m_old.VertexBuffer) m_old.VertexBuffer->Release();
        m_context->IASetInputLayout(m_old.InputLayout); if (m_old.InputLayout) m_old.InputLayout->Release();
    }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context{};

    struct BACKUP_DX11_STATE {
        UINT                        ScissorRectsCount, ViewportsCount;
        D3D11_RECT                  ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT              Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState*      RS;
        ID3D11BlendState*           BlendState;
        FLOAT                       BlendFactor[4];
        UINT                        SampleMask;
        UINT                        StencilRef;
        ID3D11DepthStencilState*    DepthStencilState;
        ID3D11ShaderResourceView*   PSShaderResource;
        ID3D11SamplerState*         PSSampler;
        ID3D11PixelShader*          PS;
        ID3D11VertexShader*         VS;
        ID3D11GeometryShader*       GS;
        UINT                        PSInstancesCount, VSInstancesCount, GSInstancesCount;
        ID3D11ClassInstance         *PSInstances[256], *VSInstances[256], *GSInstances[256];   // 256 is max according to PSSetShader documentation
        D3D11_PRIMITIVE_TOPOLOGY    PrimitiveTopology;
        ID3D11Buffer*               IndexBuffer, *VertexBuffer, *VSConstantBuffer;
        UINT                        IndexBufferOffset, VertexBufferStride, VertexBufferOffset;
        DXGI_FORMAT                 IndexBufferFormat;
        ID3D11InputLayout*          InputLayout;
    } m_old{};
};

D3D11Component::TextureContext::TextureContext(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    set(in_tex, rtv_format, srv_format);
}

bool D3D11Component::TextureContext::set(ID3D11Resource* in_tex, std::optional<DXGI_FORMAT> rtv_format, std::optional<DXGI_FORMAT> srv_format) {
    bool is_same_tex = this->tex.Get() == in_tex;

    if (!is_same_tex) {
        this->tex.Reset();
    }

    this->tex = in_tex;

    if (in_tex == nullptr) {
        this->rtv.Reset();
        this->srv.Reset();
        return false;
    }
   
    if (!is_same_tex) {
        this->rtv.Reset();
        this->srv.Reset();

        auto device = g_framework->get_d3d11_hook()->get_device();
        bool made_rtv = false;
        bool made_srv = false;

        if (!rtv_format) {
            if (!FAILED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
                made_rtv = true;
            }
        }

        if (!srv_format) {  
            if (!FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
                made_srv = true;
            }
        }

        if (!made_rtv) {
            if (!rtv_format) {
                rtv_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            }

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = *rtv_format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;

            made_rtv = !FAILED(device->CreateRenderTargetView(tex.Get(), &rtv_desc, &this->rtv));
        }

        if (!made_srv) {
            if (!srv_format) {
                srv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = *srv_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            srv_desc.Texture2D.MostDetailedMip = 0;

            made_srv = !FAILED(device->CreateShaderResourceView(tex.Get(), &srv_desc, &this->srv));
        }

        if (!made_rtv) {
            spdlog::error("Failed to create render target view for texture");
        }

        if (!made_srv) {
            spdlog::error("Failed to create shader resource view for texture");
        }

        return made_rtv && made_srv;
    }

    return true;
}

bool D3D11Component::TextureContext::clear_rtv(float* color) {
    if (tex == nullptr || rtv == nullptr) {
        return false;
    }

    if (color == nullptr) {
        return false;
    }

    auto device = g_framework->get_d3d11_hook()->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    context->ClearRenderTargetView(rtv.Get(), color);
    return true;
}

vr::EVRCompositorError D3D11Component::on_frame(VR* vr) {
    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "Failed to setup D3D11Component, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d11_hook();

    hook->set_next_present_interval(0); // disable vsync for vr

    // get device
    auto device = hook->get_device();

    // Get the context.
    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D11Texture2D> real_backbuffer{};
    swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer));

    ComPtr<ID3D11Texture2D> backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D11Texture2D*)ue4_texture->get_native_resource();
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        m_engine_tex_ref.reset();
        m_scene_capture_tex_ref.reset();
        return vr::VRCompositorError_None;
    }

    auto runtime = vr->get_runtime();

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    D3D11_TEXTURE2D_DESC selected_backbuffer_desc{};
    backbuffer->GetDesc(&selected_backbuffer_desc);

    if (is_naruto_executable() &&
        !vr->is_extreme_compatibility_mode_enabled() &&
        m_backbuffer_size[0] != 0 &&
        m_backbuffer_size[1] != 0 &&
        (selected_backbuffer_desc.Width != m_backbuffer_size[0] ||
         selected_backbuffer_desc.Height != m_backbuffer_size[1]))
    {
        SPDLOG_WARN(
            "[Naruto][UE4.16][D3D11] Scene target changed from [{}x{}] to [{}x{}]; deferring submission for one guarded rebuild",
            m_backbuffer_size[0],
            m_backbuffer_size[1],
            selected_backbuffer_desc.Width,
            selected_backbuffer_desc.Height);
        m_force_reset = true;
        return vr::VRCompositorError_None;
    }

    const auto naruto_color_conversion =
        !vr->is_extreme_compatibility_mode_enabled() &&
        needs_naruto_d3d11_color_conversion(selected_backbuffer_desc);
    const auto needs_color_conversion =
        vr->is_extreme_compatibility_mode_enabled() || naruto_color_conversion;

    // We use SRGB for the RTV but not for the SRV because it screws up the colors when drawing the spectator view.
    if (!needs_color_conversion) {
        m_engine_tex_ref.set(backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM);
    } else {
        // OpenXR does not advertise Naruto's R10G10B10A2 scene format. Convert
        // through an SRV-capable texture instead of issuing an invalid
        // R10G10B10A2 -> BGRA8 CopyResource.
        DX11StateBackup backup{context.Get()};

        auto source_desc = selected_backbuffer_desc;
        source_desc.Usage = D3D11_USAGE_DEFAULT;
        if (m_extreme_compat_backbuffer == nullptr) {
            source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            source_desc.CPUAccessFlags = 0;
            source_desc.MiscFlags = 0;

            const auto result = device->CreateTexture2D(&source_desc, nullptr, &m_extreme_compat_backbuffer);
            if (FAILED(result)) {
                spdlog::error(
                    "[VR] Failed to create D3D11 format-conversion source (hr=0x{:08X}, {}x{}, fmt={}, samples={}, bind=0x{:X})",
                    static_cast<uint32_t>(result),
                    source_desc.Width,
                    source_desc.Height,
                    static_cast<uint32_t>(source_desc.Format),
                    source_desc.SampleDesc.Count,
                    source_desc.BindFlags);
                return vr::VRCompositorError_None;
            }
        }

        context->CopyResource(m_extreme_compat_backbuffer.Get(), backbuffer.Get());

        if (m_converted_backbuffer == nullptr) {
            auto converted_desc = source_desc;
            converted_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            converted_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            converted_desc.CPUAccessFlags = 0;
            converted_desc.MiscFlags = 0;

            const auto result = device->CreateTexture2D(&converted_desc, nullptr, &m_converted_backbuffer);
            if (FAILED(result)) {
                spdlog::error(
                    "[VR] Failed to create D3D11 BGRA conversion target (hr=0x{:08X}, {}x{}, samples={})",
                    static_cast<uint32_t>(result),
                    converted_desc.Width,
                    converted_desc.Height,
                    converted_desc.SampleDesc.Count);
                return vr::VRCompositorError_None;
            }
        }

        if (!m_engine_tex_ref.set(m_converted_backbuffer.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Failed to set engine tex ref");
            return vr::VRCompositorError_None;
        }

        if (!m_extreme_compat_backbuffer_ctx.set(m_extreme_compat_backbuffer.Get(), std::nullopt, std::nullopt)) {
            spdlog::error("[VR] Failed to set D3D11 format-conversion source context");
            return vr::VRCompositorError_None;
        }

        float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_engine_tex_ref.clear_rtv(&clear_color[0]);

        render_srv_to_rtv(m_backbuffer_batch.get(), m_extreme_compat_backbuffer_ctx, m_engine_tex_ref, m_backbuffer_size[0], m_backbuffer_size[1]);

        backbuffer = m_converted_backbuffer;

        if (naruto_color_conversion) {
            SPDLOG_INFO_ONCE(
                "[Naruto][UE4.16][D3D11] Converting adopted scene target {}x{} format {} to BGRA8 for OpenXR submission",
                selected_backbuffer_desc.Width,
                selected_backbuffer_desc.Height,
                static_cast<uint32_t>(selected_backbuffer_desc.Format));
        }
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D11Texture2D*)scene_capture->get_native_resource() : nullptr;

        if (scene_capture_rt != nullptr && scene_capture_rt != m_scene_capture_tex_ref.tex.Get()) {
            D3D11_TEXTURE2D_DESC desc{};
            scene_capture_rt->GetDesc(&desc);

            spdlog::info("[VR] Scene capture texture format: {}, {}x{}", (uint32_t)desc.Format, desc.Width, desc.Height);
            m_scene_capture_tex_ref.set(scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM);
        } else if (scene_capture_rt == nullptr && m_scene_capture_tex_ref.has_texture()) {
            m_scene_capture_tex_ref.reset();
        }
    } else {
        m_scene_capture_tex_ref.reset();
    }

    // Update the UI overlay. Days Gone normally composites Slate through BendTemporalAA
    // into the scene. When the game-specific overlay path is enabled, use the captured
    // SlateIntermediateBuffer and key black out into a real OpenXR UI layer.
    const auto daysgone_menu_is_in_scene = is_daysgone_executable();
    auto* daysgone_native_ui_target = static_cast<ID3D11Texture2D*>(nullptr);
    D3D11_TEXTURE2D_DESC daysgone_native_ui_desc{};
    auto daysgone_native_ui_valid = false;

    const bool daysgone_bend_fix_active =
        daysgone_menu_is_in_scene &&
        vr != nullptr &&
        vr->is_daysgone_bend_ui_placement_fix_enabled();

    if (daysgone_bend_fix_active && ffsr != nullptr && ffsr->should_use_daysgone_slate_ui_overlay()) {
        daysgone_native_ui_target = reinterpret_cast<ID3D11Texture2D*>(ffsr->get_daysgone_slate_native_ui_target());
        if (daysgone_native_ui_target != nullptr &&
            is_d3d11_or_dxgi_com_object(reinterpret_cast<IUnknown*>(daysgone_native_ui_target)))
        {
            daysgone_native_ui_target->GetDesc(&daysgone_native_ui_desc);
            daysgone_native_ui_valid = is_valid_daysgone_d3d11_desc(daysgone_native_ui_desc);
        }
    }

    if (!daysgone_native_ui_valid) {
        daysgone_native_ui_target = nullptr;
        daysgone_native_ui_desc = {};
    }

    const auto using_daysgone_native_ui_target = daysgone_native_ui_target != nullptr;
    const auto ui_target = (!using_daysgone_native_ui_target && !daysgone_menu_is_in_scene)
        ? ffsr->get_render_target_manager()->get_ui_target()
        : nullptr;

    if (ui_target != nullptr || daysgone_native_ui_target != nullptr) {
        auto* const native_ui_target = daysgone_native_ui_target != nullptr
            ? daysgone_native_ui_target
            : is_daysgone_executable()
            ? try_get_daysgone_native_texture(ui_target)
            : (ID3D11Texture2D*)ui_target->get_native_resource();

        auto rtv_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        auto srv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (using_daysgone_native_ui_target && native_ui_target != nullptr) {
            const auto formats = get_daysgone_ui_view_formats(daysgone_native_ui_desc);
            rtv_format = formats.rtv;
            srv_format = formats.srv;

            SPDLOG_INFO_EVERY_N_SEC(
                5,
                "[DaysGone][SlateOverlay] using captured SlateIntermediateBuffer native={:x} [{}x{} fmt={} bind=0x{:X}] key=({:.3f},{:.3f},{:.3f}) offset=({:.1f},{:.1f}) scale={:.3f}",
                (uintptr_t)native_ui_target,
                daysgone_native_ui_desc.Width,
                daysgone_native_ui_desc.Height,
                (uint32_t)daysgone_native_ui_desc.Format,
                daysgone_native_ui_desc.BindFlags,
                ffsr->get_daysgone_slate_ui_key_threshold(),
                ffsr->get_daysgone_slate_ui_key_softness(),
                ffsr->get_daysgone_slate_ui_key_opacity(),
                ffsr->get_daysgone_slate_ui_offset_x(),
                ffsr->get_daysgone_slate_ui_offset_y(),
                ffsr->get_daysgone_slate_ui_scale());
        } else if (is_naruto_executable() && native_ui_target != nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            native_ui_target->GetDesc(&desc);
            const auto formats = get_naruto_ui_view_formats(desc);
            rtv_format = formats.rtv;
            srv_format = formats.srv;

            SPDLOG_INFO_ONCE(
                "[Naruto][UE4.16][SlateUI] Using dedicated UI texture {}x{} fmt={} bind=0x{:X} with RTV fmt={} SRV fmt={}",
                desc.Width,
                desc.Height,
                (uint32_t)desc.Format,
                desc.BindFlags,
                (uint32_t)rtv_format,
                (uint32_t)srv_format);
        }

        // We use SRGB for the RTV but not for the SRV because it screws up the colors when drawing the spectator view
        m_engine_ui_ref.set(native_ui_target, rtv_format, srv_format);

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const auto native = native_ui_target;
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                // get desc
                D3D11_TEXTURE2D_DESC desc{};
                native->GetDesc(&desc);

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_ui_tex != nullptr) {
                    D3D11_TEXTURE2D_DESC ui_desc{};
                    m_ui_tex->GetDesc(&ui_desc);

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        m_engine_ui_ref.reset();
    }

    const auto is_2d_screen = vr->is_using_2d_screen();
    const auto ui_invert_alpha = vr->get_overlay_component().get_ui_invert_alpha();

    auto draw_2d_view = [&]() {
        if (!is_2d_screen || !m_engine_tex_ref.has_texture() || !m_engine_tex_ref.has_srv()) {
            return;
        }

        DX11StateBackup backup{context.Get()};

        float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        // Clear previous frame
        for (auto& screen : m_2d_screen_tex) {
            context->ClearRenderTargetView(screen, clear_color);
        }

        // Render left side to left screen tex
        render_srv_to_rtv(
            m_game_batch.get(),
            m_engine_tex_ref,
            m_2d_screen_tex[0],
            RECT{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]}
        );

        if (m_engine_ui_ref.has_texture() && m_engine_ui_ref.has_srv()) {
            render_srv_to_rtv(
                m_game_batch.get(),
                m_engine_ui_ref,
                m_2d_screen_tex[0]
            );
        }

        if (!is_afr) {
            // Render right side to right screen tex
            if (m_scene_capture_tex_ref.has_texture() && m_scene_capture_tex_ref.has_srv()) {
                render_srv_to_rtv(
                    m_game_batch.get(),
                    m_scene_capture_tex_ref,
                    m_2d_screen_tex[1]
                );
            } else {
                render_srv_to_rtv(
                    m_game_batch.get(),
                    m_engine_tex_ref,
                    m_2d_screen_tex[1],
                    RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]}
                );
            }

            if (m_engine_ui_ref.has_texture() && m_engine_ui_ref.has_srv()) {
                render_srv_to_rtv(
                    m_game_batch.get(),
                    m_engine_ui_ref,
                    m_2d_screen_tex[1]
                );
            }
        }

        // Clear the RT so the entire background is black when submitting to the compositor
        context->ClearRenderTargetView(m_engine_tex_ref, clear_color);

        if (m_scene_capture_tex_ref.has_rtv()) {
            context->ClearRenderTargetView(m_scene_capture_tex_ref, clear_color);
        }
    };

    if (is_2d_screen) {
        draw_2d_view();
    }

    // Duplicate frames can sometimes cause the UI to get stuck on the screen.
    // and can lock up the compositor.
    if (runtime->is_openvr() && get_ui_tex().Get() != nullptr) {
        if (is_right_eye_frame) {
            if (is_2d_screen) {
                copy_tex(m_2d_screen_tex[0], get_ui_tex().Get());
            } else if (m_engine_ui_ref.has_texture()) {
                copy_tex(m_engine_ui_ref, get_ui_tex().Get());
            }
        } else if (is_2d_screen) {
            copy_tex(m_2d_screen_tex[0], get_ui_tex().Get());
        }
    } else if (runtime->is_openxr() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0]);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0]);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1]);
                }
            } else {
                if (m_engine_ui_ref.has_texture() && m_engine_ui_ref.has_srv()) {
                    if (using_daysgone_native_ui_target && ensure_daysgone_ui_key_resources()) {
                        m_openxr.copy(
                            (uint32_t)runtimes::OpenXR::SwapchainIndex::UI,
                            nullptr,
                            nullptr,
                            [&](ID3D11Texture2D* render_target) {
                                render_daysgone_ui_key_to_rt(
                                    render_target,
                                    m_engine_ui_ref,
                                    ffsr->get_daysgone_slate_ui_key_threshold(),
                                    ffsr->get_daysgone_slate_ui_key_softness(),
                                    ffsr->get_daysgone_slate_ui_key_opacity(),
                                    ffsr->get_daysgone_slate_ui_offset_x(),
                                    ffsr->get_daysgone_slate_ui_offset_y(),
                                    ffsr->get_daysgone_slate_ui_scale(),
                                    ffsr->should_split_daysgone_slate_ui_overlay(),
                                    ffsr->get_daysgone_slate_ui_menu_src_x(),
                                    ffsr->get_daysgone_slate_ui_menu_src_y(),
                                    ffsr->get_daysgone_slate_ui_menu_src_w(),
                                    ffsr->get_daysgone_slate_ui_menu_src_h(),
                                    ffsr->get_daysgone_slate_ui_menu_offset_x(),
                                    ffsr->get_daysgone_slate_ui_menu_offset_y(),
                                    ffsr->get_daysgone_slate_ui_menu_scale(),
                                    ffsr->get_daysgone_slate_ui_footer_src_y(),
                                    ffsr->get_daysgone_slate_ui_footer_src_h());
                            });
                    } else if (ui_invert_alpha > 0.0f && ensure_ui_invert_resources()) {
                        m_openxr.copy(
                            (uint32_t)runtimes::OpenXR::SwapchainIndex::UI,
                            nullptr,
                            nullptr,
                            [&](ID3D11Texture2D* render_target) {
                                render_ui_invert_to_rt(render_target, m_engine_ui_ref, ui_invert_alpha);
                            });
                    } else {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_engine_ui_ref);
                    }
                }
            }

            auto fw_rt = g_framework->get_rendertarget_d3d11();

            if (fw_rt != nullptr && g_framework->is_drawing_anything()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, fw_rt.Get());
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0]);
        }
    }

    utility::ScopeGuard engine_ui_guard([&]() {
        if (using_daysgone_native_ui_target) {
            return;
        }

        // clear the game's UI texture
        float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        m_engine_ui_ref.clear_rtv(clear_color);
    });

    ComPtr<ID3D11Texture2D> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D11Texture2D>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            scene_depth_tex->GetDesc(&desc);

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        if (runtime->is_openxr() && runtime->ready()) {
            LOG_VERBOSE("Copying left eye");
            //m_openxr.copy(0, backbuffer.Get());
            D3D11_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), &src_box);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), nullptr);
            }

            /*if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH_LEFT, scene_depth_tex.Get(), &src_box);
            }*/
        }

        if (runtime->is_openvr()) {
            D3D11_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            context->CopySubresourceRegion(m_left_eye_tex.Get(), 0, 0, 0, 0, backbuffer.Get(), 0, &src_box);

            const auto submit_pose = vr->m_openvr->get_pose_for_submit();

            if (m_is_shader_setup) {
                ID3D11RenderTargetView* views[] = { m_left_eye_rtv.Get() };
                context->OMSetRenderTargets(1, views, nullptr);
                //context->ClearRenderTargetView(m_right_eye_rtv.Get(), clear_color);
                invoke_shader(vr->m_frame_count, 0, m_backbuffer_size[0] / 2, m_backbuffer_size[1]);
            }

            vr::VRTextureWithPose_t left_eye{
                (void*)m_left_eye_tex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            const auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                vr->m_submitted = false;
                return e;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        if (runtime->ready() && runtime->is_openxr()) {
            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                LOG_VERBOSE("Copying left eye");
                D3D11_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), nullptr);
                }
            }

            LOG_VERBOSE("Copying right eye");

            if (is_actually_afr) {
                D3D11_BOX src_box{};
                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), nullptr);
                }
            } else {
                // Copy over the entire double wide back buffer instead
                if (!m_scene_capture_tex_ref.has_texture()) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), nullptr);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, nullptr, [&](ID3D11Texture2D* render_target) {
                        D3D11_BOX left_src_box{
                            .left = 0,
                            .top = 0,
                            .front = 0,
                            .right = m_backbuffer_size[0] / 2,
                            .bottom = m_backbuffer_size[1],
                            .back = 1
                        };

                        context->CopySubresourceRegion(render_target, 0, 0, 0, 0, backbuffer.Get(), 0, &left_src_box);
                        context->CopySubresourceRegion(render_target, 0, m_backbuffer_size[0] / 2, 0, 0, m_scene_capture_tex_ref.tex.Get(), 0, &left_src_box);
                    });
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), nullptr);
                }
            }

            LOG_VERBOSE("Ending frame");
            thread_local std::vector<XrCompositionLayerBaseHeader*> quad_layers{}; quad_layers.clear();

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }

            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();

                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }
            
            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex != nullptr);

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::EVRCompositorError e = vr::VRCompositorError_None;

            if (!is_afr) {
                D3D11_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                context->CopySubresourceRegion(m_left_eye_tex.Get(), 0, 0, 0, 0, backbuffer.Get(), 0, &src_box);

                if (m_is_shader_setup) {
                    ID3D11RenderTargetView* views[] = { m_left_eye_rtv.Get() };
                    context->OMSetRenderTargets(1, views, nullptr);
                    //context->ClearRenderTargetView(m_right_eye_rtv.Get(), clear_color);
                    invoke_shader(vr->m_frame_count, 0, m_backbuffer_size[0] / 2, m_backbuffer_size[1]);
                }

                vr::VRTextureWithPose_t left_eye{
                    (void*)m_left_eye_tex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    vr->m_submitted = false;
                    return e;
                }
            }

            // Copy the back buffer to the right eye texture.
            if (!m_scene_capture_tex_ref.has_texture()) {
                D3D11_BOX src_box{};
                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                context->CopySubresourceRegion(m_right_eye_tex.Get(), 0, 0, 0, 0, backbuffer.Get(), 0, &src_box);
            } else {
                D3D11_BOX left_src_box{
                    .left = 0,
                    .top = 0,
                    .front = 0,
                    .right = m_backbuffer_size[0] / 2,
                    .bottom = m_backbuffer_size[1],
                    .back = 1
                };

                context->CopySubresourceRegion(m_right_eye_tex.Get(), 0, 0, 0, 0, m_scene_capture_tex_ref.tex.Get(), 0, &left_src_box);
            }

            if (m_is_shader_setup) {
                ID3D11RenderTargetView* views[] = { m_right_eye_rtv.Get() };
                context->OMSetRenderTargets(1, views, nullptr);
                invoke_shader(vr->m_frame_count, 1, m_backbuffer_size[0] / 2, m_backbuffer_size[1]);
                //context->OMSetRenderTargets(1, &prev_rtv, prev_depth_rtv.Get());     
            }

            vr::VRTextureWithPose_t right_eye{
                (void*)m_right_eye_tex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            bool submitted = true;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                vr->m_submitted = false;
                return e;
            }

            vr->m_submitted = true;
        }

        /*if (runtime->ready() && vr->m_desktop_fix->value()) {
            if (vr->m_desktop_fix_skip_present->value()) {
                hook->ignore_next_present();
            } else {
                context->CopyResource(real_backbuffer.Get(), m_right_eye_tex.Get());
            }
        }*/
    }

    const auto should_draw_desktop = m_backbuffer_rtv != nullptr && vr->is_hmd_active() && vr->m_desktop_fix->value();

    // Desktop fix
    if (is_right_eye_frame && should_draw_desktop) {
        DX11StateBackup backup{context.Get()};
        
        ID3D11RenderTargetView* views[] = { m_backbuffer_rtv.Get() };
        context->OMSetRenderTargets(1, views, nullptr);

        m_backbuffer_batch->Begin();

        D3D11_VIEWPORT viewport{};
        viewport.Width = m_real_backbuffer_size[0];
        viewport.Height = m_real_backbuffer_size[1];
        m_backbuffer_batch->SetViewport(viewport);

        context->RSSetViewports(1, &viewport);
        
        D3D11_RECT scissor_rect{};
        scissor_rect.left = 0;
        scissor_rect.top = 0;
        scissor_rect.right = m_real_backbuffer_size[0];
        scissor_rect.bottom = m_real_backbuffer_size[1];
        context->RSSetScissorRects(1, &scissor_rect);

        RECT dest_rect{};
        dest_rect.left = 0;
        dest_rect.top = 0;
        dest_rect.right = m_real_backbuffer_size[0];
        dest_rect.bottom = m_real_backbuffer_size[1];

        // Game tex
        if (m_engine_tex_ref.has_srv()) {
            RECT source_rect{};

            const auto aspect_ratio = (float)m_real_backbuffer_size[0] / (float)m_real_backbuffer_size[1];
            const auto full_source_width = (float)m_backbuffer_size[0];
            const auto full_source_height = (float)m_backbuffer_size[1];

            const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
            const auto eye_height = full_source_height;
            const auto eye_aspect_ratio = eye_width / eye_height;
            const auto full_source_aspect_ratio = full_source_width / full_source_height;
            auto aspect_matches = [](float a, float b) {
                const auto diff = a > b ? a - b : b - a;
                return diff <= (b * 0.12f);
            };

            const bool source_looks_mono =
                !vr->is_using_afr() &&
                !vr->is_native_stereo_fix_enabled() &&
                full_source_width > 0.0f &&
                full_source_height > 0.0f &&
                aspect_matches(full_source_aspect_ratio, aspect_ratio) &&
                !aspect_matches(eye_aspect_ratio, aspect_ratio);

            const auto source_width_for_aspect = source_looks_mono ? full_source_width : eye_width;
            const auto source_height_for_aspect = full_source_height;
            const auto source_aspect_ratio = source_looks_mono ? full_source_aspect_ratio : eye_aspect_ratio;

            const auto original_centerw = source_width_for_aspect / 2.0f;
            const auto original_centerh = source_height_for_aspect / 2.0f;

            if (source_looks_mono) {
                SPDLOG_INFO_ONCE("[VR] D3D11 spectator using full scene source rect [{}x{}] because it does not look double-wide", m_backbuffer_size[0], m_backbuffer_size[1]);
                source_rect.left = 0;
                source_rect.top = 0;
                source_rect.right = (LONG)full_source_width;
                source_rect.bottom = (LONG)full_source_height;
            } else if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled()) {
                // left side of double wide tex only on AFR/synced
                source_rect.left = 0;
                source_rect.top = 0;
                source_rect.right = (LONG)eye_width;
                source_rect.bottom = (LONG)eye_height;
            } else {
                source_rect.left = (LONG)eye_width;
                source_rect.top = 0;
                source_rect.right = (LONG)(eye_width * 2);
                source_rect.bottom = (LONG)eye_height;
            }

            // Correct left/top/right/bottom to match the aspect ratio of the game
            if (source_aspect_ratio > aspect_ratio) {
                const auto new_width = source_height_for_aspect * aspect_ratio;
                const auto new_centerw = new_width / 2.0f;
                source_rect.left += (LONG)(original_centerw - new_centerw);
                source_rect.right = source_rect.left + (LONG)new_width;
            } else {
                const auto new_height = source_width_for_aspect / aspect_ratio;
                const auto new_centerh = new_height / 2.0f;
                source_rect.top = (LONG)(original_centerh - new_centerh);
                source_rect.bottom = (LONG)(original_centerh + new_centerh);
            }

            m_backbuffer_batch->Draw(m_engine_tex_ref, dest_rect, &source_rect, DirectX::Colors::White);
        }

        // UI tex
        if (vr->get_desktop_mirror_mode() == VR::DESKTOP_MIRROR_FULL && m_engine_ui_ref.has_srv()) {
            m_backbuffer_batch->Draw(m_engine_ui_ref, dest_rect, DirectX::Colors::White);
        }

        m_backbuffer_batch->End();

        // Create a copy of the backbuffer if we're using AFR
        if (is_actually_afr) {
            ComPtr<ID3D11Texture2D> real_backbuffer{};
            swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer));

            if (m_spectator_view_backbuffer == nullptr && real_backbuffer != nullptr) {
                D3D11_TEXTURE2D_DESC real_backbuffer_desc{};
                real_backbuffer->GetDesc(&real_backbuffer_desc);
                if (FAILED(device->CreateTexture2D(&real_backbuffer_desc, nullptr, &m_spectator_view_backbuffer))) {
                    spdlog::error("[VR] Failed to create copied backbuffer for desktop view");
                }
            }

            // Copy over the backbuffer every other frame
            if (real_backbuffer != nullptr && m_spectator_view_backbuffer != nullptr) {
                context->CopyResource(m_spectator_view_backbuffer.Get(), real_backbuffer.Get());
            }
        } else {
            m_spectator_view_backbuffer.Reset(); // Free as we have no use for it
        }
    }

    // Copy the previous right eye frame to the backbuffer if we're using AFR on an non-right eye frame
    if (is_actually_afr && !is_right_eye_frame && should_draw_desktop && m_spectator_view_backbuffer != nullptr) {
        ComPtr<ID3D11Texture2D> real_backbuffer{};
        swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer));

        if (real_backbuffer != nullptr) {
            context->CopyResource(real_backbuffer.Get(), m_spectator_view_backbuffer.Get());
        }
    }

    return vr::VRCompositorError_None;
}

void D3D11Component::on_post_present(VR* vr) {
    // Never erase the desktop while no validated scene source has reached the
    // compositor. Setup retries on the next frame without turning a recoverable
    // target-discovery delay into a black game window.
    if (vr->is_hmd_active() && m_force_reset) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] D3D11 scene textures are not ready; preserving the desktop back buffer");
        return;
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        auto& hook = g_framework->get_d3d11_hook();
        auto device = hook->get_device();
        auto swapchain = hook->get_swap_chain();

        if (device == nullptr || swapchain == nullptr) {
            return;
        }

        ComPtr<ID3D11DeviceContext> context{};
        device->GetImmediateContext(&context);

        ComPtr<ID3D11Texture2D> backbuffer{};

        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

        if (backbuffer == nullptr) {
            return;
        }

        if (backbuffer.Get() != m_backbuffer.Get()) {
            m_backbuffer = backbuffer.Get();
            m_backbuffer_rtv.Reset();

            // Create new rtv
            if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_backbuffer_rtv))) {
                m_backbuffer_rtv.Reset();
            }
        }

        if (m_backbuffer_rtv != nullptr) {
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            context->ClearRenderTargetView(m_backbuffer_rtv.Get(), clear_color);
        }
    }
}

void D3D11Component::on_reset(VR* vr) {
    m_force_reset = true;

    m_backbuffer_rtv.Reset();
    m_backbuffer.Reset();
    m_spectator_view_backbuffer.Reset();
    m_extreme_compat_backbuffer.Reset();
    m_converted_backbuffer.Reset();
    m_extreme_compat_backbuffer_ctx.reset();
    m_left_eye_tex.Reset();
    m_right_eye_tex.Reset();
    m_left_eye_rtv.Reset();
    m_right_eye_rtv.Reset();
    m_left_eye_srv.Reset();
    m_right_eye_srv.Reset();
    m_ui_tex.Reset();
    m_vs_shader_blob.Reset();
    m_ps_shader_blob.Reset();
    m_vs_shader.Reset();
    m_ps_shader.Reset();
    m_input_layout.Reset();
    m_constant_buffer.Reset();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_invert_ps.Reset();
    m_ui_invert_blend.Reset();
    m_ui_invert_ready = false;
    m_daysgone_ui_key_ps.Reset();
    m_daysgone_ui_key_blend.Reset();
    m_daysgone_ui_key_ready = false;
    m_is_shader_setup = false;

    for (auto& tex : m_2d_screen_tex) {
        tex.reset();
    }

    if (vr->get_runtime()->is_openxr() && vr->get_runtime()->loaded) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D11Texture2D> scene_depth_tex{rt_pool->get_texture<ID3D11Texture2D>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            scene_depth_tex->GetDesc(&desc);
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }

        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            g_framework->get_d3d11_rt_size()[0] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            g_framework->get_d3d11_rt_size()[1] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }
    }
}

// Quick function for one-time rtv clearing
bool D3D11Component::clear_tex(ID3D11Resource* tex, std::optional<DXGI_FORMAT> format) {
    auto ctx = TextureContext{tex, format};

    float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    return ctx.clear_rtv(color);
}

bool D3D11Component::clear_tex(ID3D11Resource* tex, float* color, std::optional<DXGI_FORMAT> format) {
    auto ctx = TextureContext{tex, format};

    return ctx.clear_rtv(color);
}

void D3D11Component::copy_tex(ID3D11Resource* src, ID3D11Resource* dst) {
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    context->CopyResource(dst, src);
}

void D3D11Component::render_srv_to_rtv(DirectX::DX11::SpriteBatch* batch, TextureContext& srv, TextureContext& rtv, float w, float h) {
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    // Finally do the rendering.
    ID3D11RenderTargetView* views[] = { rtv };
    context->OMSetRenderTargets(1, views, nullptr);

    batch->Begin();

    D3D11_VIEWPORT viewport{};
    viewport.Width = w;
    viewport.Height = h;
    batch->SetViewport(viewport);

    context->RSSetViewports(1, &viewport);
    
    D3D11_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = w;
    scissor_rect.bottom = h;
    context->RSSetScissorRects(1, &scissor_rect);

    RECT dest_rect{};
    dest_rect.left = 0;
    dest_rect.top = 0;
    dest_rect.right = w;
    dest_rect.bottom = h;

    batch->Draw(srv, dest_rect, DirectX::Colors::White);
    batch->End();
}

void D3D11Component::render_srv_to_rtv(DirectX::DX11::SpriteBatch* batch, TextureContext& srv, TextureContext& rtv) {
    // get src and dest descs
    D3D11_TEXTURE2D_DESC src_desc{};
    D3D11_TEXTURE2D_DESC dest_desc{};

    ((ID3D11Texture2D*)srv.tex.Get())->GetDesc(&src_desc);
    ((ID3D11Texture2D*)rtv.tex.Get())->GetDesc(&dest_desc);
    
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    // Finally do the rendering.
    ID3D11RenderTargetView* views[] = { rtv };
    context->OMSetRenderTargets(1, views, nullptr);

    batch->Begin();

    D3D11_VIEWPORT viewport{};
    viewport.Width = dest_desc.Width;
    viewport.Height = dest_desc.Height;
    batch->SetViewport(viewport);

    context->RSSetViewports(1, &viewport);
    
    D3D11_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = dest_desc.Width;
    scissor_rect.bottom = dest_desc.Height;
    context->RSSetScissorRects(1, &scissor_rect);

    RECT dest_rect{};
    dest_rect.left = 0;
    dest_rect.top = 0;
    dest_rect.right = dest_desc.Width;
    dest_rect.bottom = dest_desc.Height;

    RECT src_rect{};
    src_rect.left = 0;
    src_rect.top = 0;
    src_rect.right = src_desc.Width;
    src_rect.bottom = src_desc.Height;

    batch->Draw(srv, dest_rect, &src_rect, DirectX::Colors::White);
    batch->End();
}

void D3D11Component::render_srv_to_rtv(DirectX::DX11::SpriteBatch* batch, TextureContext& srv, TextureContext& rtv, const RECT& src_rect) {
    // get src and dest descs
    D3D11_TEXTURE2D_DESC src_desc{};
    D3D11_TEXTURE2D_DESC dest_desc{};

    ((ID3D11Texture2D*)srv.tex.Get())->GetDesc(&src_desc);
    ((ID3D11Texture2D*)rtv.tex.Get())->GetDesc(&dest_desc);
    
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    // Finally do the rendering.
    ID3D11RenderTargetView* views[] = { rtv };
    context->OMSetRenderTargets(1, views, nullptr);

    batch->Begin();

    D3D11_VIEWPORT viewport{};
    viewport.Width = dest_desc.Width;
    viewport.Height = dest_desc.Height;
    batch->SetViewport(viewport);

    context->RSSetViewports(1, &viewport);
    
    D3D11_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = dest_desc.Width;
    scissor_rect.bottom = dest_desc.Height;
    context->RSSetScissorRects(1, &scissor_rect);

    RECT dest_rect{};
    dest_rect.left = 0;
    dest_rect.top = 0;
    dest_rect.right = dest_desc.Width;
    dest_rect.bottom = dest_desc.Height;

    batch->Draw(srv, dest_rect, &src_rect, DirectX::Colors::White);
    batch->End();
}

bool D3D11Component::ensure_ui_invert_resources() {
    if (m_ui_invert_ready) {
        return m_ui_invert_ps != nullptr && m_ui_invert_blend != nullptr;
    }

    m_ui_invert_ready = true;

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        spdlog::error("[VR] D3D11 UI invert: device is null");
        return false;
    }

    // Create an opaque blend state (write shader output directly).
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&blend_desc, &m_ui_invert_blend))) {
        spdlog::error("[VR] D3D11 UI invert: failed to create blend state");
        m_ui_invert_blend.Reset();
        return false;
    }

    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> error_blob{};

    const UINT flags =
#if defined(_DEBUG)
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_STRICTNESS;
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
#endif

    const auto hr = D3DCompile(
        k_ui_invert_ps_hlsl,
        strlen(k_ui_invert_ps_hlsl),
        "UEVR_UIInvert",
        nullptr,
        nullptr,
        "SpritePixelShader",
        "ps_4_0",
        flags,
        0,
        &ps_blob,
        &error_blob);

    if (FAILED(hr)) {
        if (error_blob != nullptr) {
            spdlog::error("[VR] D3D11 UI invert: shader compile failed: {}", (const char*)error_blob->GetBufferPointer());
        } else {
            spdlog::error("[VR] D3D11 UI invert: shader compile failed (hr=0x{:08x})", (uint32_t)hr);
        }
        return false;
    }

    if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ui_invert_ps))) {
        spdlog::error("[VR] D3D11 UI invert: failed to create pixel shader");
        m_ui_invert_ps.Reset();
        return false;
    }

    spdlog::info("[VR] D3D11 UI invert shader ready");
    return true;
}

void D3D11Component::render_ui_invert_to_rt(ID3D11Texture2D* render_target, TextureContext& srv, float invert_amount) {
    if (render_target == nullptr || !srv.has_srv()) {
        return;
    }

    if (!ensure_ui_invert_resources()) {
        return;
    }

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    DX11StateBackup backup{context.Get()};

    TextureContext rtv{render_target};
    if (!rtv.has_rtv()) {
        return;
    }

    D3D11_TEXTURE2D_DESC dest_desc{};
    render_target->GetDesc(&dest_desc);

    float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(rtv, clear_color);

    ID3D11RenderTargetView* views[] = { rtv };
    context->OMSetRenderTargets(1, views, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)dest_desc.Width;
    viewport.Height = (float)dest_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;

    m_game_batch->SetViewport(viewport);
    context->RSSetViewports(1, &viewport);

    D3D11_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dest_desc.Width;
    scissor_rect.bottom = (LONG)dest_desc.Height;
    context->RSSetScissorRects(1, &scissor_rect);

    RECT dest_rect{};
    dest_rect.left = 0;
    dest_rect.top = 0;
    dest_rect.right = (LONG)dest_desc.Width;
    dest_rect.bottom = (LONG)dest_desc.Height;

    const auto tint = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, invert_amount);

    auto set_custom_shaders = [&]() {
        context->PSSetShader(m_ui_invert_ps.Get(), nullptr, 0);
    };

    m_game_batch->Begin(
        DirectX::DX11::SpriteSortMode_Immediate,
        m_ui_invert_blend.Get(),
        nullptr,
        nullptr,
        nullptr,
        set_custom_shaders);

    m_game_batch->Draw(srv, dest_rect, tint);
    m_game_batch->End();
}

bool D3D11Component::ensure_daysgone_ui_key_resources() {
    if (m_daysgone_ui_key_ready) {
        return m_daysgone_ui_key_ps != nullptr && m_daysgone_ui_key_blend != nullptr;
    }

    m_daysgone_ui_key_ready = true;

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        spdlog::error("[DaysGone][SlateOverlay] D3D11 device is null");
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&blend_desc, &m_daysgone_ui_key_blend))) {
        spdlog::error("[DaysGone][SlateOverlay] failed to create key blend state");
        m_daysgone_ui_key_blend.Reset();
        return false;
    }

    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> error_blob{};

    const UINT flags =
#if defined(_DEBUG)
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_STRICTNESS;
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
#endif

    const auto hr = D3DCompile(
        k_daysgone_ui_key_ps_hlsl,
        strlen(k_daysgone_ui_key_ps_hlsl),
        "UEVR_DaysGoneUIKey",
        nullptr,
        nullptr,
        "SpritePixelShader",
        "ps_4_0",
        flags,
        0,
        &ps_blob,
        &error_blob);

    if (FAILED(hr)) {
        if (error_blob != nullptr) {
            spdlog::error("[DaysGone][SlateOverlay] key shader compile failed: {}", (const char*)error_blob->GetBufferPointer());
        } else {
            spdlog::error("[DaysGone][SlateOverlay] key shader compile failed (hr=0x{:08x})", (uint32_t)hr);
        }
        return false;
    }

    if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_daysgone_ui_key_ps))) {
        spdlog::error("[DaysGone][SlateOverlay] failed to create key pixel shader");
        m_daysgone_ui_key_ps.Reset();
        return false;
    }

    spdlog::info("[DaysGone][SlateOverlay] key shader ready");
    return true;
}

void D3D11Component::render_daysgone_ui_key_to_rt(
    ID3D11Texture2D* render_target,
    TextureContext& srv,
    float threshold,
    float softness,
    float opacity,
    float offset_x,
    float offset_y,
    float scale,
    bool split_overlay,
    float menu_src_x,
    float menu_src_y,
    float menu_src_w,
    float menu_src_h,
    float menu_offset_x,
    float menu_offset_y,
    float menu_scale,
    float footer_src_y,
    float footer_src_h)
{
    if (render_target == nullptr || !srv.has_srv()) {
        return;
    }

    if (!ensure_daysgone_ui_key_resources()) {
        return;
    }

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    DX11StateBackup backup{context.Get()};

    TextureContext rtv{render_target};
    if (!rtv.has_rtv()) {
        return;
    }

    D3D11_TEXTURE2D_DESC dest_desc{};
    render_target->GetDesc(&dest_desc);
    D3D11_TEXTURE2D_DESC src_desc{};
    if (srv.tex != nullptr) {
        static_cast<ID3D11Texture2D*>(srv.tex.Get())->GetDesc(&src_desc);
    }

    float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(rtv, clear_color);

    ID3D11RenderTargetView* views[] = { rtv };
    context->OMSetRenderTargets(1, views, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)dest_desc.Width;
    viewport.Height = (float)dest_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;

    m_game_batch->SetViewport(viewport);
    context->RSSetViewports(1, &viewport);

    D3D11_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)dest_desc.Width;
    scissor_rect.bottom = (LONG)dest_desc.Height;
    context->RSSetScissorRects(1, &scissor_rect);

    const auto safe_scale = std::clamp(scale, 0.05f, 8.0f);
    const auto scaled_w = static_cast<float>(dest_desc.Width) * safe_scale;
    const auto scaled_h = static_cast<float>(dest_desc.Height) * safe_scale;

    RECT dest_rect{};
    dest_rect.left = (LONG)(((float)dest_desc.Width - scaled_w) * 0.5f + offset_x);
    dest_rect.top = (LONG)(((float)dest_desc.Height - scaled_h) * 0.5f + offset_y);
    dest_rect.right = (LONG)(((float)dest_desc.Width + scaled_w) * 0.5f + offset_x);
    dest_rect.bottom = (LONG)(((float)dest_desc.Height + scaled_h) * 0.5f + offset_y);

    const auto tint = DirectX::XMVectorSet(
        std::clamp(threshold, 0.0f, 0.5f),
        std::clamp(softness, 0.001f, 0.5f),
        1.0f,
        std::clamp(opacity, 0.0f, 2.0f));

    auto set_custom_shaders = [&]() {
        context->PSSetShader(m_daysgone_ui_key_ps.Get(), nullptr, 0);
    };

    m_game_batch->Begin(
        DirectX::DX11::SpriteSortMode_Immediate,
        m_daysgone_ui_key_blend.Get(),
        nullptr,
        nullptr,
        nullptr,
        set_custom_shaders);

    if (split_overlay && src_desc.Width != 0 && src_desc.Height != 0) {
        const auto clamp01 = [](float value) {
            return std::clamp(value, 0.0f, 1.0f);
        };
        const auto make_src_rect = [&](float nx, float ny, float nw, float nh) {
            nx = clamp01(nx);
            ny = clamp01(ny);
            nw = std::clamp(nw, 0.01f, 1.0f - nx);
            nh = std::clamp(nh, 0.01f, 1.0f - ny);

            RECT rect{};
            rect.left = static_cast<LONG>(std::round(nx * static_cast<float>(src_desc.Width)));
            rect.top = static_cast<LONG>(std::round(ny * static_cast<float>(src_desc.Height)));
            rect.right = static_cast<LONG>(std::round((nx + nw) * static_cast<float>(src_desc.Width)));
            rect.bottom = static_cast<LONG>(std::round((ny + nh) * static_cast<float>(src_desc.Height)));
            rect.right = std::max(rect.left + 1L, std::min<LONG>(rect.right, static_cast<LONG>(src_desc.Width)));
            rect.bottom = std::max(rect.top + 1L, std::min<LONG>(rect.bottom, static_cast<LONG>(src_desc.Height)));
            return rect;
        };
        const auto make_centered_dest_rect = [&](const RECT& src_rect, float extra_offset_x, float extra_offset_y, float extra_scale) {
            const auto src_w = static_cast<float>(std::max<LONG>(1, src_rect.right - src_rect.left));
            const auto src_h = static_cast<float>(std::max<LONG>(1, src_rect.bottom - src_rect.top));
            const auto draw_scale = std::clamp(safe_scale * extra_scale, 0.05f, 8.0f);
            const auto draw_w = src_w * draw_scale;
            const auto draw_h = src_h * draw_scale;

            RECT rect{};
            rect.left = static_cast<LONG>((static_cast<float>(dest_desc.Width) - draw_w) * 0.5f + offset_x + extra_offset_x);
            rect.top = static_cast<LONG>((static_cast<float>(dest_desc.Height) - draw_h) * 0.5f + offset_y + extra_offset_y);
            rect.right = static_cast<LONG>((static_cast<float>(dest_desc.Width) + draw_w) * 0.5f + offset_x + extra_offset_x);
            rect.bottom = static_cast<LONG>((static_cast<float>(dest_desc.Height) + draw_h) * 0.5f + offset_y + extra_offset_y);
            return rect;
        };
        const auto make_footer_dest_rect = [&](const RECT& src_rect) {
            const auto src_w = static_cast<float>(std::max<LONG>(1, src_rect.right - src_rect.left));
            const auto src_h = static_cast<float>(std::max<LONG>(1, src_rect.bottom - src_rect.top));
            const auto draw_w = src_w * safe_scale;
            const auto draw_h = src_h * safe_scale;

            RECT rect{};
            rect.left = static_cast<LONG>((static_cast<float>(dest_desc.Width) - draw_w) * 0.5f + offset_x);
            rect.top = static_cast<LONG>(static_cast<float>(dest_desc.Height) - draw_h + offset_y);
            rect.right = static_cast<LONG>((static_cast<float>(dest_desc.Width) + draw_w) * 0.5f + offset_x);
            rect.bottom = static_cast<LONG>(static_cast<float>(dest_desc.Height) + offset_y);
            return rect;
        };

        const auto footer_src = make_src_rect(0.0f, footer_src_y, 1.0f, footer_src_h);
        const auto footer_dest = make_footer_dest_rect(footer_src);
        m_game_batch->Draw(srv, footer_dest, &footer_src, tint);

        const auto menu_src = make_src_rect(menu_src_x, menu_src_y, menu_src_w, menu_src_h);
        const auto menu_dest = make_centered_dest_rect(menu_src, menu_offset_x, menu_offset_y, menu_scale);
        m_game_batch->Draw(srv, menu_dest, &menu_src, tint);
    } else {
        m_game_batch->Draw(srv, dest_rect, tint);
    }
    m_game_batch->End();
}

bool D3D11Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up D3D11 textures...");

    auto& vr = VR::get();
    on_reset(vr.get());

    // Get device and swapchain.
    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);

    // Get back buffer.
    ComPtr<ID3D11Texture2D> real_backbuffer{};
    swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer));

    if (real_backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get real back buffer (D3D11).");
        return false;
    }

    ComPtr<ID3D11Texture2D> backbuffer{};
    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D11Texture2D*)ue4_texture->get_native_resource();
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D11).");
        return false;
    }

    m_backbuffer_batch = std::make_unique<DirectX::DX11::SpriteBatch>(context.Get());
    m_game_batch = std::make_unique<DirectX::DX11::SpriteBatch>(context.Get());

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};
    backbuffer->GetDesc(&backbuffer_desc);

    if (is_naruto_executable() && !vr->is_extreme_compatibility_mode_enabled()) {
        const uint64_t expected_width =
            static_cast<uint64_t>(vr->get_hmd_width()) * (vr->is_using_afr() ? 1ull : 2ull);
        const uint64_t expected_height = vr->get_hmd_height();

        if (expected_width == 0 || expected_height == 0 ||
            backbuffer_desc.Width != expected_width ||
            backbuffer_desc.Height != expected_height)
        {
            SPDLOG_INFO_EVERY_N_SEC(
                1,
                "[Naruto][UE4.16][D3D11] Waiting for verified VR-sized scene target; observed [{}x{} fmt={}], expected [{}x{}]",
                backbuffer_desc.Width,
                backbuffer_desc.Height,
                static_cast<uint32_t>(backbuffer_desc.Format),
                expected_width,
                expected_height);
            return false;
        }
    }

    const auto naruto_color_conversion =
        !vr->is_extreme_compatibility_mode_enabled() &&
        needs_naruto_d3d11_color_conversion(backbuffer_desc);

    m_backbuffer_size[0] = backbuffer_desc.Width;
    m_backbuffer_size[1] = backbuffer_desc.Height;

    D3D11_TEXTURE2D_DESC real_backbuffer_desc{};

    if (real_backbuffer != nullptr) {
        real_backbuffer->GetDesc(&real_backbuffer_desc);

        m_real_backbuffer_size[0] = real_backbuffer_desc.Width;
        m_real_backbuffer_size[1] = real_backbuffer_desc.Height;
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer_desc.Width = backbuffer_desc.Width / 2;
    } else {
        backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    spdlog::info("[VR] W: {}, H: {}", backbuffer_desc.Width, backbuffer_desc.Height);
    spdlog::info("[VR] Format: {}", (uint32_t)backbuffer_desc.Format);

    if (naruto_color_conversion) {
        SPDLOG_INFO(
            "[Naruto][UE4.16][D3D11] Preparing BGRA-compatible local textures for source {}x{} fmt={} mips={} array={} samples={} bind=0x{:X} misc=0x{:X}",
            backbuffer_desc.Width * 2,
            backbuffer_desc.Height,
            static_cast<uint32_t>(backbuffer_desc.Format),
            backbuffer_desc.MipLevels,
            backbuffer_desc.ArraySize,
            backbuffer_desc.SampleDesc.Count,
            backbuffer_desc.BindFlags,
            backbuffer_desc.MiscFlags);

        // UEVR's eye, 2D-screen and OpenXR textures use BGRA8 views. The
        // engine-owned R10 target is converted every frame before reaching
        // these resources; do not attach incompatible BGRA views to R10.
        backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        backbuffer_desc.Usage = D3D11_USAGE_DEFAULT;
        backbuffer_desc.CPUAccessFlags = 0;
        backbuffer_desc.MiscFlags = 0;
    }

    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    backbuffer_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    // Create eye textures.
    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_left_eye_tex))) {
        spdlog::error("[VR] Failed to create left eye texture (D3D11).");
        return false;
    }

    if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &m_right_eye_tex))) {
        spdlog::error("[VR] Failed to create right eye texture (D3D11).");
        return false;
    }

    backbuffer_desc.Width = (uint32_t)g_framework->get_d3d11_rt_size().x;
    backbuffer_desc.Height = (uint32_t)g_framework->get_d3d11_rt_size().y;
    device->CreateTexture2D(&backbuffer_desc, nullptr, &m_ui_tex);

    for (auto& ctx : m_2d_screen_tex) {
        ComPtr<ID3D11Texture2D> tex{};
        if (FAILED(device->CreateTexture2D(&backbuffer_desc, nullptr, &tex))) {
            spdlog::error("[VR] Failed to create 2D screen texture (D3D11).");
            continue;
        }

        std::optional<DXGI_FORMAT> tex_format{};
        
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        if (!ctx.set(tex.Get(), tex_format, tex_format)) {
            spdlog::error("[VR] Failed to setup 2D screen texture context (D3D11).");
            continue;
        }
    }

    // No need to pass the format as the backbuffer is not a typeless format.
    clear_tex(m_ui_tex.Get());

    // copy backbuffer into right eye
    if (!vr->is_extreme_compatibility_mode_enabled() && !naruto_color_conversion) {
        context->CopyResource(m_right_eye_tex.Get(), backbuffer.Get());
        context->CopyResource(m_left_eye_tex.Get(), backbuffer.Get());
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = -1;

    if (FAILED(device->CreateShaderResourceView(m_left_eye_tex.Get(), &srv_desc, &m_left_eye_srv))) {
        spdlog::error("[VR] Failed to create left eye shader resource view (D3D11).");
        return false;
    }

    if (FAILED(device->CreateShaderResourceView(m_right_eye_tex.Get(), &srv_desc, &m_right_eye_srv))) {
        spdlog::error("[VR] Failed to create right eye shader resource view (D3D11).");
        return false;
    }

    // Create render target views for the eye textures.
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    if (FAILED(device->CreateRenderTargetView(m_left_eye_tex.Get(), &rtv_desc, &m_left_eye_rtv))) {
        spdlog::error("[VR] Failed to create left eye render target view (D3D11).");
        return false;
    }

    if (FAILED(device->CreateRenderTargetView(m_right_eye_tex.Get(), &rtv_desc, &m_right_eye_rtv))) {
        spdlog::error("[VR] Failed to create right eye render target view (D3D11).");
        return false;
    }

    spdlog::info("[VR] d3d11 textures have been setup");

    m_is_shader_setup = setup_shader();
    m_force_reset = false;

    return true;
}

bool D3D11Component::setup_shader() {
    if (true) {
#ifdef SHADER_TEMP_DISABLED
        return false;
#endif
    }

    spdlog::info("[VR] Setting up D3D11 shader...");

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();

    // Create a new shader blob and copy the memory from the shader.
    const auto vs_shader_size = sizeof(vertex_shader1::g_main);
    const auto ps_shader_size = sizeof(pixel_shader1::g_main);

    if (FAILED(D3DCreateBlob(vs_shader_size, &m_vs_shader_blob))) {
        spdlog::error("[VR] Failed to create vertex shader blob.");
        return false;
    }

    if (FAILED(D3DCreateBlob(ps_shader_size, &m_ps_shader_blob))) {
        spdlog::error("[VR] Failed to create pixel shader blob.");
        return false;
    }

    memcpy(m_vs_shader_blob->GetBufferPointer(), vertex_shader1::g_main, vs_shader_size);
    memcpy(m_ps_shader_blob->GetBufferPointer(), pixel_shader1::g_main, ps_shader_size);

    // Create the vertex shader.
    if (FAILED(device->CreateVertexShader(m_vs_shader_blob->GetBufferPointer(), m_vs_shader_blob->GetBufferSize(), nullptr, &m_vs_shader))) {
        spdlog::error("[VR] Failed to create vertex shader.");
        return false;
    }

    // Create the pixel shader.
    if (FAILED(device->CreatePixelShader(m_ps_shader_blob->GetBufferPointer(), m_ps_shader_blob->GetBufferSize(), nullptr, &m_ps_shader))) {
        spdlog::error("[VR] Failed to create pixel shader.");
        return false;
    }

    /*
    struct VSInput {
        float3 position : POSITION;
        float4 color : COLOR;
        float2 texuv0 : TEXCOORD0;
        float2 texuv1 : TEXCOORD1;
        float3 normal : NORMAL;
        float3 tangent : TANGENT;
        float3 bitangent : BITANGENT;
    };

    struct PSInput {
        float4 position : SV_POSITION;
        float4 color : COLOR;
        float2 texuv0 : TEXCOORD0;
        float2 texuv1 : TEXCOORD1;
        float3 normal : NORMAL;
        float3 tangent : TANGENT;
        float3 bitangent : BITANGENT;
    };
    */

    // Create the input layout.
    D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    if (FAILED(device->CreateInputLayout(input_layout_desc, 7, m_vs_shader_blob->GetBufferPointer(), m_vs_shader_blob->GetBufferSize(), &m_input_layout))) {
        spdlog::error("[VR] Failed to create input layout.");
        return false;
    }

    // Create the constant buffer.
    D3D11_BUFFER_DESC constant_buffer_desc{};
    constant_buffer_desc.ByteWidth = sizeof(D3D11Component::ShaderGlobals);
    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;
    constant_buffer_desc.StructureByteStride = 0;

    m_shader_globals.resolution = DirectX::XMFLOAT4(1920.0f, 1080.0f, 0.0f, 1920.0f / 1080.0f);
    m_shader_globals.time = 1.0f;

    const auto hmd_transform = VR::get()->get_hmd_transform(0);

    m_model_mat[3] = hmd_transform[3] + (hmd_transform[2] * 2.0f);

    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = &m_shader_globals;
    init_data.SysMemPitch = 0;
    init_data.SysMemSlicePitch = 0;

    if (FAILED(device->CreateBuffer(&constant_buffer_desc, &init_data, &m_constant_buffer))) {
        spdlog::error("[VR] Failed to create constant buffer.");
        return false;
    };

    if (m_constant_buffer == nullptr) {
        spdlog::error("[VR] Failed to create constant buffer (nullptr).");
        return false;
    }

    // Create the vertex buffer.
    D3D11_BUFFER_DESC vertex_buffer_desc{};
    vertex_buffer_desc.ByteWidth = sizeof(D3D11Component::Vertex) * 4;
    vertex_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertex_buffer_desc.CPUAccessFlags = 0;

    Vertex vertices[] =
    {
        { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } },
        { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } },
        { {  1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } },
        { {  1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } }
    };

    D3D11_SUBRESOURCE_DATA vertex_buffer_data{};
    ZeroMemory(&vertex_buffer_data, sizeof(D3D11_SUBRESOURCE_DATA));
    vertex_buffer_data.pSysMem = vertices;

    if (FAILED(device->CreateBuffer(&vertex_buffer_desc, &vertex_buffer_data, &m_vertex_buffer))) {
        spdlog::error("[VR] Failed to create vertex buffer.");
        return false;
    }

    // Create the index buffer.
    D3D11_BUFFER_DESC index_buffer_desc{};
    index_buffer_desc.ByteWidth = sizeof(uint32_t) * 6;
    index_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    index_buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    index_buffer_desc.CPUAccessFlags = 0;
    index_buffer_desc.MiscFlags = 0;

    // Quad (2 triangles)
    uint32_t indices[] = { 
        0, 1, 2, 
        2, 1, 3
    };

    D3D11_SUBRESOURCE_DATA index_buffer_data{};
    ZeroMemory(&index_buffer_data, sizeof(D3D11_SUBRESOURCE_DATA));
    index_buffer_data.pSysMem = indices;
    index_buffer_data.SysMemPitch = 0;
    index_buffer_data.SysMemSlicePitch = 0;

    if (FAILED(device->CreateBuffer(&index_buffer_desc, &index_buffer_data, &m_index_buffer))) {
        spdlog::error("[VR] Failed to create index buffer.");
        return false;
    }


    spdlog::info("[VR] D3D11 shaders initialized.");

    return true;
}

void D3D11Component::invoke_shader(uint32_t frame_count, uint32_t eye, uint32_t width, uint32_t height) {
    if (m_constant_buffer == nullptr) {
        spdlog::error("[VR] Constant buffer is null. Cannot invoke shader.");
        return;
    }

    if (m_vertex_buffer == nullptr) {
        spdlog::error("[VR] Vertex buffer is null. Cannot invoke shader.");
        return;
    }

    auto& hook = g_framework->get_d3d11_hook();
    auto& vr = VR::get();
    auto runtime = vr->get_runtime();

    auto device = hook->get_device();
    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);

    // Backup DX state that will be modified to restore it afterwards (unfortunately this is very ugly looking and verbose. Close your eyes!)
    struct BACKUP_DX11_STATE {
        UINT                        ScissorRectsCount, ViewportsCount;
        D3D11_RECT                  ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT              Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState*      RS;
        ID3D11BlendState*           BlendState;
        FLOAT                       BlendFactor[4];
        UINT                        SampleMask;
        UINT                        StencilRef;
        ID3D11DepthStencilState*    DepthStencilState;
        ID3D11ShaderResourceView*   PSShaderResource;
        ID3D11SamplerState*         PSSampler;
        ID3D11PixelShader*          PS;
        ID3D11VertexShader*         VS;
        ID3D11GeometryShader*       GS;
        UINT                        PSInstancesCount, VSInstancesCount, GSInstancesCount;
        ID3D11ClassInstance         *PSInstances[256], *VSInstances[256], *GSInstances[256];   // 256 is max according to PSSetShader documentation
        D3D11_PRIMITIVE_TOPOLOGY    PrimitiveTopology;
        ID3D11Buffer*               IndexBuffer, *VertexBuffer, *VSConstantBuffer;
        UINT                        IndexBufferOffset, VertexBufferStride, VertexBufferOffset;
        DXGI_FORMAT                 IndexBufferFormat;
        ID3D11InputLayout*          InputLayout;
    };
    BACKUP_DX11_STATE old = {};
    old.ScissorRectsCount = old.ViewportsCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetScissorRects(&old.ScissorRectsCount, old.ScissorRects);
    context->RSGetViewports(&old.ViewportsCount, old.Viewports);
    context->RSGetState(&old.RS);
    context->OMGetBlendState(&old.BlendState, old.BlendFactor, &old.SampleMask);
    context->OMGetDepthStencilState(&old.DepthStencilState, &old.StencilRef);
    context->PSGetShaderResources(0, 1, &old.PSShaderResource);
    context->PSGetSamplers(0, 1, &old.PSSampler);
    old.PSInstancesCount = old.VSInstancesCount = old.GSInstancesCount = 256;
    context->PSGetShader(&old.PS, old.PSInstances, &old.PSInstancesCount);
    context->VSGetShader(&old.VS, old.VSInstances, &old.VSInstancesCount);
    context->VSGetConstantBuffers(0, 1, &old.VSConstantBuffer);
    context->GSGetShader(&old.GS, old.GSInstances, &old.GSInstancesCount);

    context->IAGetPrimitiveTopology(&old.PrimitiveTopology);
    context->IAGetIndexBuffer(&old.IndexBuffer, &old.IndexBufferFormat, &old.IndexBufferOffset);
    context->IAGetVertexBuffers(0, 1, &old.VertexBuffer, &old.VertexBufferStride, &old.VertexBufferOffset);
    context->IAGetInputLayout(&old.InputLayout);

    // Update the constant buffer.
    const auto glm_eye_transform_offset = vr->get_eye_transform(eye);
    auto glm_proj = vr->get_projection_matrix((VRRuntime::Eye)eye);

    const auto hmd_transform = vr->get_hmd_transform(frame_count);
    auto glm_view = hmd_transform * glm_eye_transform_offset;

    const auto conv = glm::mat4 {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -1, 0,
        0, 0, 0, 1
    };

    if (GetAsyncKeyState(VK_SPACE)) {
        m_model_mat = hmd_transform * conv;
        m_model_mat[3] += m_model_mat[2] * 5.0f;
        // flip the rotation around the y axis
        //m_shader_globals.model *= DirectX::XMMatrixRotationY(DirectX::XM_PI / 2.0f);
    }

    glm_view = conv * glm::inverse(glm_view);

    const auto& dx_proj = *(DirectX::XMMATRIX*)&glm_proj;
    const auto dx_view = *(DirectX::XMMATRIX*)&glm_view;
    const auto& dx_model = *(DirectX::XMMATRIX*)&m_model_mat;

    m_shader_globals.mvp = dx_model * dx_view * dx_proj;
    m_shader_globals.resolution = DirectX::XMFLOAT4((float)width, (float)height, 0.0f, (float)width / (float)height);
    m_shader_globals.time += 0.01f;

    D3D11_MAPPED_SUBRESOURCE mapped_resource{};
    if (FAILED(context->Map(m_constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource))) {
        spdlog::error("[VR] Failed to map constant buffer.");
        return;
    }
    
    memcpy(mapped_resource.pData, &m_shader_globals, sizeof(D3D11Component::ShaderGlobals));
    context->Unmap(m_constant_buffer.Get(), 0);

    // Set the vertex input layout.
    context->IASetInputLayout(m_input_layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(D3D11Component::Vertex);
    const UINT offset = 0;

    ID3D11Buffer* vertex_buffers[] = {m_vertex_buffer.Get()};
    context->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
    
    ID3D11Buffer* index_buffers[] = {m_index_buffer.Get()};
    context->IASetIndexBuffer(m_index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    // Set the vertex and pixel shaders that will be used to render this triangle.
    context->VSSetShader(m_vs_shader.Get(), nullptr, 0);
    context->PSSetShader(m_ps_shader.Get(), nullptr, 0);

    // Set the constant buffer in the vertex shader with the updated values.
    ID3D11Buffer* constant_buffers[] = {m_constant_buffer.Get()};
    context->VSSetConstantBuffers(0, 1, constant_buffers);

    // Set the constant buffer in the pixel shader with the updated values.
    constant_buffers[0] = m_constant_buffer.Get();
    context->PSSetConstantBuffers(0, 1, constant_buffers);

    // Set the sampler state in the pixel shader.
    //context->PSSetSamplers(0, 1, &m_sampler_state);

    // Set the texture in the pixel shader.
    //ID3D11ShaderResourceView* srvs[] = {m_right_eye_srv.Get()};
    //context->PSSetShaderResources(0, 1, srvs);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    // Set scissors to cover the whole screen
    D3D11_RECT r;
    r.left = 0;
    r.top = 0;
    r.right = width;
    r.bottom = height;
    context->RSSetScissorRects(1, &r);

    // Set blend state
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

    // Set depth stencil state
    context->OMSetDepthStencilState(nullptr, 0);
    
    // Null out other shader stages
    context->DSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->CSSetShader(nullptr, nullptr, 0);

    context->PSSetShaderResources(0, 0, nullptr);
    context->PSSetSamplers(0, 0, nullptr);
    context->VSSetShaderResources(0, 0, nullptr);
    context->VSSetSamplers(0, 0, nullptr);
    context->CSSetShaderResources(0, 0, nullptr);
    context->CSSetSamplers(0, 0, nullptr);

    // Set rasterizer state
    context->RSSetState(nullptr);

    // Render the quad.
    context->DrawIndexed(6, 0, 0);

    // Restore modified DX state
    context->RSSetScissorRects(old.ScissorRectsCount, old.ScissorRects);
    context->RSSetViewports(old.ViewportsCount, old.Viewports);
    context->RSSetState(old.RS); if (old.RS) old.RS->Release();
    context->OMSetBlendState(old.BlendState, old.BlendFactor, old.SampleMask); if (old.BlendState) old.BlendState->Release();
    context->OMSetDepthStencilState(old.DepthStencilState, old.StencilRef); if (old.DepthStencilState) old.DepthStencilState->Release();
    context->PSSetShaderResources(0, 1, &old.PSShaderResource); if (old.PSShaderResource) old.PSShaderResource->Release();
    context->PSSetSamplers(0, 1, &old.PSSampler); if (old.PSSampler) old.PSSampler->Release();
    context->PSSetShader(old.PS, old.PSInstances, old.PSInstancesCount); if (old.PS) old.PS->Release();
    for (UINT i = 0; i < old.PSInstancesCount; i++) if (old.PSInstances[i]) old.PSInstances[i]->Release();
    context->VSSetShader(old.VS, old.VSInstances, old.VSInstancesCount); if (old.VS) old.VS->Release();
    context->VSSetConstantBuffers(0, 1, &old.VSConstantBuffer); if (old.VSConstantBuffer) old.VSConstantBuffer->Release();
    context->GSSetShader(old.GS, old.GSInstances, old.GSInstancesCount); if (old.GS) old.GS->Release();
    for (UINT i = 0; i < old.VSInstancesCount; i++) if (old.VSInstances[i]) old.VSInstances[i]->Release();
    context->IASetPrimitiveTopology(old.PrimitiveTopology);
    context->IASetIndexBuffer(old.IndexBuffer, old.IndexBufferFormat, old.IndexBufferOffset); if (old.IndexBuffer) old.IndexBuffer->Release();
    context->IASetVertexBuffers(0, 1, &old.VertexBuffer, &old.VertexBufferStride, &old.VertexBufferOffset); if (old.VertexBuffer) old.VertexBuffer->Release();
    context->IASetInputLayout(old.InputLayout); if (old.InputLayout) old.InputLayout->Release();
}

void D3D11Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

    auto& hook = g_framework->get_d3d11_hook();

    auto device = hook->get_device();

    this->binding.device = device;

    PFN_xrGetD3D11GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    if (fn == nullptr) {
        spdlog::error("[VR] xrGetD3D11GraphicsRequirementsKHR not found");
        return;
    }

    // get existing adapter from device
    ComPtr<IDXGIDevice> dxgi_device{};
    
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        spdlog::error("[VR] failed to get DXGI device from D3D11 device");
        return;
    }
    
    ComPtr<IDXGIAdapter> adapter{};

    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        spdlog::error("[VR] failed to get DXGI adapter from DXGI device");
        return;
    }

    DXGI_ADAPTER_DESC desc{};

    if (FAILED(adapter->GetDesc(&desc))) {
        spdlog::error("[VR] failed to get DXGI adapter description");
        return;
    }
    
    XrGraphicsRequirementsD3D11KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    gr.adapterLuid = desc.AdapterLuid;
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D11Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D11");

    this->destroy_swapchains();

    auto& hook = g_framework->get_d3d11_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    // Get back buffer.
    ComPtr<ID3D11Texture2D> backbuffer{};
    ComPtr<ID3D11Texture2D> og_backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D11Texture2D*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    if (backbuffer == nullptr) {
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    }

    swapchain->GetBuffer(0, IID_PPV_ARGS(&og_backbuffer));
    
    if (backbuffer == nullptr || og_backbuffer == nullptr) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};
    backbuffer->GetDesc(&backbuffer_desc);

    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    auto& openxr = *vr->m_openxr;

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, D3D11_TEXTURE2D_DESC desc) -> std::optional<std::string> {
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr.session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D11: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;
        vr->m_openxr->cache_swapchain_dimensions(i, swapchain.width, swapchain.height);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                // we dgaf so just acquire/wait/release. maybe later we can actually write something to it, but we dont care rn
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);
                this->parent->clear_tex(ctx.textures[index].texture);
                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = 1;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

    auto hmd_desc = backbuffer_desc;
    /*if (!has_actual_vr_backbuffer) {
        hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }*/
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();

    // Create eye textures.
    /*for (auto i = 0; i < openxr.views.size(); ++i) {
        spdlog::info("[VR] Creating swapchain for eye {}", i);
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain(i, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }*/

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;
    virtual_desktop_dummy_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait


    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_swapchain_create_info.width = g_framework->get_d3d11_rt_size().x;
    desktop_rt_swapchain_create_info.height = g_framework->get_d3d11_rt_size().y;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = g_framework->get_d3d11_rt_size().x;
    desktop_rt_desc.Height = g_framework->get_d3d11_rt_size().y;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;

    }
    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D11Texture2D>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_tex->GetDesc(&depth_desc);

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            spdlog::error("[VR] Depth texture is null! Using default values");
            this->made_depth_with_null_defaults = true;
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    spdlog::info("[VR] Successfully created OpenXR swapchains for D3D11");

    return std::nullopt;
}

void D3D11Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};
    auto vr = VR::get();

    if (vr != nullptr && vr->m_openxr != nullptr) {
        vr->m_openxr->clear_cached_swapchain_dimensions();
    }

	if (this->contexts.empty()) {
        return;
    }

    if (vr == nullptr || vr->m_openxr == nullptr) {
        return;
    }

    spdlog::info("[VR] Destroying swapchains.");

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        std::vector<ID3D11Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

void D3D11Component::OpenXR::copy(uint32_t swapchain_idx, ID3D11Texture2D* resource, D3D11_BOX* src_box, std::function<void(ID3D11Texture2D*)> pre_commands) {
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
        //return;
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    auto device = g_framework->get_d3d11_hook()->get_device();
    
    // get immediate context
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    LOG_VERBOSE("Acquiring swapchain image for {}", swapchain_idx);
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;

        LOG_VERBOSE("Waiting on swapchain image for {}", swapchain_idx);
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            LOG_VERBOSE("Copying swapchain image {} for {}", texture_index, swapchain_idx);

            if (pre_commands != nullptr) {
                pre_commands(ctx.textures[texture_index].texture);
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                if (src_box == nullptr) {
                    context->CopyResource(ctx.textures[texture_index].texture, resource);
                } else {
                    context->CopySubresourceRegion(ctx.textures[texture_index].texture, 0, 0, 0, 0, resource, 0, src_box);
                }
            }

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

            LOG_VERBOSE("Releasing swapchain image for {}", swapchain_idx);
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod
