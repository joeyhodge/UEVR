#include "UICompositionGPU.hpp"
#include <wrl/client.h>

#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace uevr::ui_composition {
namespace {
template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
constexpr auto format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr char shader[] = R"(
Texture2D<float4> Input : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer Geometry : register(b0) { float4 Corners[4]; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    const uint ids[6]={0,1,2,2,1,3};
    uint i=ids[id]; Vertex v;
    v.position=Corners[i]; v.uv=float2(i&1,(i>>1)&1); return v;
}
float4 PS(Vertex v) : SV_Target { return Input.Sample(LinearClamp,v.uv); }
)";
bool bgra(DXGI_FORMAT f) {
    return f == format || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
bool compile(Ptr<ID3DBlob>& vs, Ptr<ID3DBlob>& ps) {
    struct Code {
        Ptr<ID3DBlob> vs, ps;
        bool valid{};
    };
    static const auto code = [] {
        Code c;
        c.valid = SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, "Per-eye UI", nullptr, nullptr, "VS", "vs_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &c.vs, nullptr)) &&
                  SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, "Per-eye UI", nullptr, nullptr, "PS", "ps_5_0",
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &c.ps, nullptr));
        return c;
    }();
    vs = code.vs;
    ps = code.ps;
    return code.valid;
}

template <class F> bool bounded_retirement(bool wait, F&& poll) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (poll()) {
            return true;
        }
        if (!wait) {
            return false;
        }
        Sleep(1);
    } while (std::chrono::steady_clock::now() < until);
    return false;
}
} // namespace

struct GPU11::Impl {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> immediate, deferred;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RasterizerState> rasterizer;
    Ptr<ID3D11DepthStencilState> depth;
    Ptr<ID3D11Query> fence;
    Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11BlendState> blend;
    std::vector<Ptr<ID3D11ShaderResourceView>> inputs;
    std::vector<Ptr<ID3D11RenderTargetView>> outputs;
    UINT width{}, height{};
    bool submitted{};
};

GPU11::GPU11()
    : m_impl{std::make_unique<Impl>()} {
}
GPU11::~GPU11() {
    // Device/runtime failures must not free resources which may still be executing.
    if (!retired(true)) {
        (void)m_impl.release();
    }
}

bool GPU11::initialize(ID3D11Device* device, std::span<ID3D11Texture2D* const> inputs, std::span<ID3D11Texture2D* const> outputs) {
    auto& s = *m_impl;
    if (!device || s.device || inputs.empty() || outputs.empty() || inputs.size() > 16 || outputs.size() > 16) {
        return false;
    }
    s.device = device;
    device->GetImmediateContext(&s.immediate);
    if (!s.immediate || FAILED(device->CreateDeferredContext(0, &s.deferred))) {
        return false;
    }
    Ptr<ID3DBlob> vs, ps;
    if (!compile(vs, ps) || FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s.vs)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s.ps))) {
        return false;
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.StencilReadMask = depth.StencilWriteMask = 0xff;
    depth.FrontFace = depth.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateRasterizerState(&raster, &s.rasterizer)) || FAILED(device->CreateDepthStencilState(&depth, &s.depth)) ||
        FAILED(device->CreateQuery(&query, &s.fence))) {
        return false;
    }

    D3D11_BUFFER_DESC cb{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SAMPLER_DESC sampling{};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blending{};
    auto& rt = blending.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s.constants)) || FAILED(device->CreateSamplerState(&sampling, &s.sampler)) ||
        FAILED(device->CreateBlendState(&blending, &s.blend))) {
        return false;
    }

    UINT source_width{}, source_height{};
    auto validate = [&](ID3D11Texture2D* texture, bool input) {
        if (!texture) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        Ptr<ID3D11Device> owner;
        texture->GetDevice(&owner);
        if (owner.Get() != device || !bgra(desc.Format) || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            desc.SampleDesc.Quality != 0 || !desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192 ||
            !(desc.BindFlags & (input ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET))) {
            return false;
        }
        auto& w = input ? source_width : s.width;
        auto& h = input ? source_height : s.height;
        if (w && (w != desc.Width || h != desc.Height)) {
            return false;
        }
        w = desc.Width;
        h = desc.Height;
        return true;
    };
    for (auto* texture : inputs) {
        if (!validate(texture, true)) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        Ptr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(texture, &desc, &view))) {
            return false;
        }
        s.inputs.push_back(std::move(view));
    }
    for (auto* texture : outputs) {
        if (!validate(texture, false) || std::find(inputs.begin(), inputs.end(), texture) != inputs.end()) {
            return false;
        }
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Ptr<ID3D11RenderTargetView> view;
        if (FAILED(device->CreateRenderTargetView(texture, &desc, &view))) {
            return false;
        }
        s.outputs.push_back(std::move(view));
    }
    return true;
}

bool GPU11::draw(std::span<const Draw> draws, uint32_t output) {
    auto& s = *m_impl;
    if (draws.size() != 2 || output >= s.outputs.size() || !s.fence || FAILED(s.device->GetDeviceRemovedReason())) {
        return false;
    }
    for (const auto& d : draws) {
        if (d.input >= s.inputs.size() || d.eye > 1) {
            return false;
        }
    }
    if (!s.width || (s.width % 2)) {
        return false;
    }
    auto* ctx = s.deferred.Get();
    ctx->ClearState();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(s.vs.Get(), nullptr, 0);
    ctx->PSSetShader(s.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, s.constants.GetAddressOf());
    ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
    ctx->OMSetRenderTargets(1, s.outputs[output].GetAddressOf(), nullptr);
    ctx->OMSetBlendState(s.blend.Get(), nullptr, UINT_MAX);
    ctx->OMSetDepthStencilState(s.depth.Get(), 0);
    ctx->RSSetState(s.rasterizer.Get());
    const float clear[4]{};
    ctx->ClearRenderTargetView(s.outputs[output].Get(), clear);
    for (const auto& d : draws) {
        ctx->UpdateSubresource(s.constants.Get(), 0, nullptr, d.corners.data(), 0, 0);
        ctx->PSSetShaderResources(0, 1, s.inputs[d.input].GetAddressOf());
        const D3D11_VIEWPORT viewport{
            static_cast<float>(d.eye * (s.width / 2)), 0, static_cast<float>(s.width / 2), static_cast<float>(s.height), 0, 1};
        ctx->RSSetViewports(1, &viewport);
        ctx->Draw(6, 0);
    }
    Ptr<ID3D11CommandList> commands;
    if (FAILED(ctx->FinishCommandList(FALSE, &commands))) {
        return false;
    }
    s.immediate->ExecuteCommandList(commands.Get(), TRUE);
    s.immediate->End(s.fence.Get());
    s.submitted = true;
    return true;
}

bool GPU11::available(uint32_t target) const {
    return target < m_impl->outputs.size();
}

bool GPU11::retired(bool wait) {
    auto& s = *m_impl;
    if (!s.submitted) {
        return true;
    }
    if (wait) {
        s.immediate->Flush();
    }
    return bounded_retirement(wait, [&] {
        BOOL done = FALSE;
        return s.immediate->GetData(s.fence.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done;
    });
}

struct GPU12::Impl {
    struct Commands {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> list;
        uint64_t fence_value{};
    };
    Ptr<ID3D12Device> device;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pso;
    Ptr<ID3D12DescriptorHeap> srvs, rtvs;
    Ptr<ID3D12Fence> fence;
    std::vector<Ptr<ID3D12Resource>> inputs, outputs;
    std::vector<Commands> commands;
    UINT srv_stride{}, rtv_stride{}, width{}, height{};
    uint64_t submitted{};
    bool poisoned{};
};

GPU12::GPU12()
    : m_impl{std::make_unique<Impl>()} {
}
GPU12::~GPU12() {
    if (!retired(true)) {
        (void)m_impl.release();
    }
}

bool GPU12::initialize(
    ID3D12Device* device, ID3D12CommandQueue* queue, std::span<ID3D12Resource* const> inputs, std::span<ID3D12Resource* const> outputs) {
    auto& s = *m_impl;
    if (!device || !queue || s.device || inputs.empty() || outputs.empty() || inputs.size() > 16 || outputs.size() > 16 ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || queue->GetDesc().NodeMask > 1) {
        return false;
    }
    Ptr<ID3D12Device> owner;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device) {
        return false;
    }
    s.device = device;
    s.queue = queue;
    Ptr<ID3DBlob> vs, ps, root;
    if (!compile(vs, ps)) {
        return false;
    }
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 16};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC sig{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&sig, D3D_ROOT_SIGNATURE_VERSION_1, &root, nullptr)) ||
        FAILED(device->CreateRootSignature(0, root->GetBufferPointer(), root->GetBufferSize(), IID_PPV_ARGS(&s.root)))) {
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = s.root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 0xff;
    pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s.pso))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) {
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC srv_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, static_cast<UINT>(inputs.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(outputs.size()), D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&s.srvs))) ||
        FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&s.rtvs)))) {
        return false;
    }
    s.srv_stride = device->GetDescriptorHandleIncrementSize(srv_desc.Type);
    s.rtv_stride = device->GetDescriptorHandleIncrementSize(rtv_desc.Type);
    UINT source_width{}, source_height{};
    auto validate = [&](ID3D12Resource* texture, bool input) {
        if (!texture) {
            return false;
        }
        const auto desc = texture->GetDesc();
        Ptr<ID3D12Device> owner;
        if (FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device || !bgra(desc.Format) ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 || !desc.Width || desc.Width > 8192 || !desc.Height ||
            desc.Height > 8192 || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
            (input && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))) {
            return false;
        }
        auto& w = input ? source_width : s.width;
        auto& h = input ? source_height : s.height;
        if (w && (w != desc.Width || h != desc.Height)) {
            return false;
        }
        w = static_cast<UINT>(desc.Width);
        h = desc.Height;
        return true;
    };
    auto srv = s.srvs->GetCPUDescriptorHandleForHeapStart();
    for (auto* texture : inputs) {
        if (!validate(texture, true)) {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &desc, srv);
        srv.ptr += s.srv_stride;
        s.inputs.emplace_back(texture);
    }
    auto rtv = s.rtvs->GetCPUDescriptorHandleForHeapStart();
    for (auto* texture : outputs) {
        if (!validate(texture, false) || std::find(inputs.begin(), inputs.end(), texture) != inputs.end()) {
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(texture, &desc, rtv);
        rtv.ptr += s.rtv_stride;
        s.outputs.emplace_back(texture);
        Impl::Commands commands;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commands.allocator))) ||
            FAILED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, commands.allocator.Get(), s.pso.Get(), IID_PPV_ARGS(&commands.list))) ||
            FAILED(commands.list->Close())) {
            return false;
        }
        s.commands.push_back(std::move(commands));
    }
    return true;
}

bool GPU12::draw(std::span<const Draw> draws, uint32_t output) {
    auto& s = *m_impl;
    if (s.poisoned || draws.size() != 2 || output >= s.commands.size() || !s.width || (s.width % 2)) {
        return false;
    }
    for (const auto& d : draws) {
        if (d.input >= s.inputs.size() || d.eye > 1) {
            return false;
        }
    }
    auto& cmd = s.commands[output];
    const auto completed = s.fence->GetCompletedValue();
    if (completed == UINT64_MAX || completed < cmd.fence_value || FAILED(s.device->GetDeviceRemovedReason())) {
        return false;
    }
    if (FAILED(cmd.allocator->Reset()) || FAILED(cmd.list->Reset(cmd.allocator.Get(), s.pso.Get()))) {
        s.poisoned = true;
        return false;
    }
    auto* list = cmd.list.Get();
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetDescriptorHeaps(1, s.srvs.GetAddressOf());
    auto rtv = s.rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += size_t{output} * s.rtv_stride;
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float clear[4]{};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const auto& d : draws) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {s.inputs[d.input].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1, &barrier);
        auto srv = s.srvs->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += uint64_t{d.input} * s.srv_stride;
        list->SetGraphicsRootDescriptorTable(0, srv);
        list->SetGraphicsRoot32BitConstants(1, 16, d.corners.data(), 0);
        const auto x = d.eye * (s.width / 2);
        const D3D12_VIEWPORT viewport{static_cast<float>(x), 0, static_cast<float>(s.width / 2), static_cast<float>(s.height), 0, 1};
        const D3D12_RECT rect{static_cast<LONG>(x), 0, static_cast<LONG>(x + s.width / 2), static_cast<LONG>(s.height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &rect);
        list->DrawInstanced(6, 1, 0, 0);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }
    if (FAILED(list->Close())) {
        s.poisoned = true;
        return false;
    }
    ID3D12CommandList* lists[]{list};
    s.queue->ExecuteCommandLists(1, lists);
    cmd.fence_value = ++s.submitted;
    if (FAILED(s.queue->Signal(s.fence.Get(), s.submitted))) {
        s.poisoned = true;
        return false;
    }
    return true;
}

bool GPU12::available(uint32_t target) const {
    auto& s = *m_impl;
    if (s.poisoned || !s.fence || target >= s.commands.size()) {
        return false;
    }
    const auto completed = s.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= s.commands[target].fence_value;
}

bool GPU12::retired(bool wait) {
    auto& s = *m_impl;
    if (!s.submitted) {
        return true;
    }
    return bounded_retirement(wait, [&] {
        const auto value = s.fence->GetCompletedValue();
        return value != UINT64_MAX && value >= s.submitted;
    });
}
} // namespace uevr::ui_composition
