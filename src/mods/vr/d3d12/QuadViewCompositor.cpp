#include <algorithm>
#include <optional>

#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>

#include "QuadViewCompositor.hpp"

namespace d3d12 {
namespace {
// The atlas and the swapchain image are copied byte for byte today, so the
// compositor reads and writes through plain UNORM/FLOAT views (no sRGB
// conversion) to produce the same bytes a straight copy would.
struct FormatFamily {
    DXGI_FORMAT typeless;
    DXGI_FORMAT view;
};

std::optional<FormatFamily> family_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return FormatFamily{DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return FormatFamily{DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return FormatFamily{DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return FormatFamily{DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT};
    default:
        return std::nullopt;
    }
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}
}

void QuadViewCompositor::reset() {
    m_source.reset();
    m_retired.clear();
    m_source_resource = nullptr;
    m_output.reset();
    m_output_desc = {};
    m_batch.reset();
    m_batch_format = DXGI_FORMAT_UNKNOWN;
    m_direct_rtv_heap.reset();
    m_failed = false;
}

bool QuadViewCompositor::ensure_source(ID3D12Device* device, ID3D12Resource* atlas) {
    if (m_source_resource == atlas && m_source != nullptr && m_source->srv_heap != nullptr) {
        return true;
    }

    if (m_source != nullptr) {
        m_retired.push_back(std::move(m_source));
        while (m_retired.size() > 4) {
            m_retired.pop_front();
        }
    }

    m_source_resource = nullptr;

    const auto family = family_of(atlas->GetDesc().Format);
    auto source = std::make_unique<TextureContext>();
    source->texture = atlas;

    if (!source->create_srv(device, family ? std::optional<DXGI_FORMAT>{family->view} : std::nullopt)) {
        spdlog::error("[QuadView][D3D12] Could not create the atlas SRV");
        return false;
    }

    m_source = std::move(source);
    m_source_resource = atlas;
    return true;
}

bool QuadViewCompositor::ensure_batch(DXGI_FORMAT view_format, const BatchFactory& make_batch) {
    if (m_batch != nullptr && m_batch_format == view_format) {
        return true;
    }

    m_batch = make_batch(view_format);
    m_batch_format = m_batch != nullptr ? view_format : DXGI_FORMAT_UNKNOWN;

    if (m_batch == nullptr) {
        spdlog::error("[QuadView][D3D12] Could not create the compose pipeline");
        m_failed = true;
        return false;
    }

    return true;
}

bool QuadViewCompositor::ensure_output(ID3D12Device* device, const D3D12_RESOURCE_DESC& like) {
    const auto family = family_of(like.Format);
    if (!family) {
        return false;
    }

    const bool same = m_output.texture != nullptr &&
        m_output_desc.Width == like.Width && m_output_desc.Height == like.Height &&
        m_output_desc.Format == family->typeless;

    if (same) {
        return true;
    }

    m_output.reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = like.Width;
    desc.Height = like.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = family->typeless; // same type group as the swapchain image, so CopyResource is legal
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> tex{};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex))))
    {
        spdlog::error("[QuadView][D3D12] Could not create the {}x{} compose texture", desc.Width, desc.Height);
        m_failed = true;
        return false;
    }

    tex->SetName(L"Quad View Compose");

    if (!m_output.setup(device, tex.Get(), family->view, family->view, L"Quad View Compose")) {
        spdlog::error("[QuadView][D3D12] Could not create the compose texture views");
        m_output.reset();
        m_failed = true;
        return false;
    }

    m_output_desc = desc;
    spdlog::info("[QuadView][D3D12] Compositor ready (compose texture + copy): {}x{}", desc.Width, desc.Height);
    return true;
}

bool QuadViewCompositor::ensure_direct_rtv(ID3D12Device* device) {
    if (m_direct_rtv_heap != nullptr) {
        return true;
    }

    m_direct_rtv_heap = std::make_unique<DirectX::DescriptorHeap>(device,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1);
    return m_direct_rtv_heap != nullptr && m_direct_rtv_heap->Heap() != nullptr;
}

bool QuadViewCompositor::record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* atlas,
    D3D12_RESOURCE_STATES atlas_state, ID3D12Resource* destination, const Frame& frame,
    const BatchFactory& make_batch)
{
    if (m_failed || device == nullptr || cmd == nullptr || atlas == nullptr || destination == nullptr) {
        return false;
    }

    const auto dst_desc = destination->GetDesc();
    const auto family = family_of(dst_desc.Format);

    if (!family) {
        spdlog::error("[QuadView][D3D12] Swapchain format {} is not supported by the compositor", (uint32_t)dst_desc.Format);
        m_failed = true;
        return false;
    }

    if (!ensure_source(device, atlas) || !ensure_batch(family->view, make_batch)) {
        return false;
    }

    // Draw straight into the swapchain image when it can be a render target:
    // that saves a full-size texture and a full-size copy every frame.
    const bool direct = (dst_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && ensure_direct_rtv(device);

    if (!direct && !ensure_output(device, dst_desc)) {
        return false;
    }

    const auto atlas_desc = atlas->GetDesc();
    const auto out_w = (uint32_t)dst_desc.Width;
    const auto out_h = (uint32_t)dst_desc.Height;
    const double eye_w = double(out_w) / 2.0;
    const double eye_h = double(out_h);

    for (const auto& r : frame.layout.views) {
        if (r.w == 0 || r.h == 0 || r.x + r.w > atlas_desc.Width || r.y + r.h > atlas_desc.Height) {
            SPDLOG_WARN_ONCE("[QuadView][D3D12] Layout does not fit the atlas ({}x{}); not composing", atlas_desc.Width, atlas_desc.Height);
            return false;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    if (direct) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = family->view; // plain UNORM/FLOAT: same bytes as a straight copy
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv = m_direct_rtv_heap->GetCpuHandle(0);
        device->CreateRenderTargetView(destination, &rtv_desc, rtv);
        SPDLOG_INFO_ONCE("[QuadView][D3D12] Compositor ready (direct to swapchain): {}x{}", out_w, out_h);
    } else {
        rtv = m_output.get_rtv();
    }

    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool atlas_readable = (atlas_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
    if (!atlas_readable) {
        transition(cmd, atlas, atlas_state, PSR);
    }

    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)out_w, (float)out_h, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH};
    D3D12_RECT scissor{0, 0, (LONG)out_w, (LONG)out_h};

    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    if (frame.sharp_only) {
        // The target is reused every frame; clear it so earlier wide views do
        // not remain around the sharp window.
        const float clear[4]{0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
    }

    ID3D12DescriptorHeap* heaps[] = {m_source->srv_heap->Heap()};
    cmd->SetDescriptorHeaps(1, heaps);

    m_batch->SetViewport(viewport);
    m_batch->Begin(cmd, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate); // keep draw order: wide, then sharp

    const DirectX::XMUINT2 atlas_size{(uint32_t)atlas_desc.Width, (uint32_t)atlas_desc.Height};

    const auto draw = [&](const uevr::quadview::Rect& src, double x, double y, double w, double h,
                          const uevr::quadview::SpriteSettings& settings) {
        const RECT s{(LONG)src.x, (LONG)src.y, (LONG)(src.x + src.w), (LONG)(src.y + src.h)};
        const DirectX::XMFLOAT2 position{(float)x, (float)y};
        const DirectX::XMFLOAT2 scale{(float)(w / src.w), (float)(h / src.h)};
        // The shader reads the destination rectangle from the colour and the
        // sprite settings from the depth (see quad_view_compose.hlsli).
        const auto rect = DirectX::XMVectorSet((float)x, (float)y, (float)(x + w), (float)(y + h));
        const float packed = uevr::quadview::pack_sprite_settings(settings);
        m_batch->Draw(m_source->get_srv_gpu(), atlas_size, position, &s, rect, 0.0f, DirectX::XMFLOAT2{0.0f, 0.0f}, scale,
            DirectX::DX12::SpriteEffects_None, packed);
    };

    uevr::quadview::SpriteSettings wide{};
    wide.wide = frame.wide_bicubic;

    uevr::quadview::SpriteSettings sharp{};
    sharp.shape = frame.shape;
    sharp.outline = frame.outline;
    sharp.blend = frame.edge_blend;
    sharp.sharpen = frame.sharpen;

    for (uint32_t eye = 0; eye < 2 && !frame.sharp_only; ++eye) {
        draw(frame.layout.views[eye], eye * eye_w, 0.0, eye_w, eye_h, wide);
    }

    for (uint32_t eye = 0; eye < 2 && !frame.wide_only; ++eye) {
        const auto d = uevr::quadview::focus_dest_rect(frame.rendered[eye], frame.focus[eye], eye * eye_w, eye_w, eye_h);
        if (d.w > 1.0 && d.h > 1.0) {
            draw(frame.layout.views[eye + 2], d.x, d.y, d.w, d.h, sharp);
        }
    }

    m_batch->End();

    if (!direct) {
        // Composed image -> swapchain image (both stay in RENDER_TARGET outside this block).
        transition(cmd, m_output.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(destination, m_output.texture.Get());
        transition(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(cmd, m_output.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    if (!atlas_readable) {
        transition(cmd, atlas, PSR, atlas_state);
    }

    SPDLOG_INFO_ONCE("[QuadView][D3D12] Composed the first quad view frame ({})", direct ? "direct" : "copy");
    return true;
}
}
