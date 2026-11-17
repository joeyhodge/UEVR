#pragma once

// Quad View compositor (D3D12).
//
// Unreal renders the four Quad View views into one atlas (wide pair on top,
// sharp pair below, see QuadView.hpp). This turns that atlas back into the
// plain double-wide stereo image UEVR has always submitted:
//   1. each eye's wide view is stretched over that eye's whole image
//      (bicubic, so the low-resolution periphery does not shimmer),
//   2. its sharp view is drawn on top where the focus window lies, optionally
//      sharpened, fading out over the edge-blend band so the seam is not
//      visible,
//   3. optionally a thin outline marks the sharp area (debugging aid).
// The result is drawn straight into the OpenXR swapchain image when that image
// can be a render target, otherwise into a private texture that is then
// copied. Either way the runtime only ever sees ordinary stereo (no
// quad-views extension or overlay layer needed).

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

#include <d3d12.h>

#include <../../directxtk12-src/Inc/SpriteBatch.h>

#include "TextureContext.hpp"
#include "../QuadView.hpp"

namespace d3d12 {
class QuadViewCompositor {
public:
    struct Frame {
        uevr::quadview::Layout layout{};          // where the views sit in the atlas
        uevr::quadview::Tangents rendered[2]{};   // field of view each wide view was rendered with
        uevr::quadview::Tangents focus[2]{};      // the sharp windows this frame was rendered with
        float edge_blend{0.2f};                   // share of each side of the sharp view that fades out
        uevr::quadview::FocusShape shape{uevr::quadview::FocusShape::RECTANGLE}; // blended shape of the sharp area
        bool outline{false};
        float sharpen{0.0f};                      // contrast-adaptive sharpening of the sharp views, 0..1
        bool wide_bicubic{true};                  // bicubic upscaling of the wide views
        float brightness{1.0f};                   // shared-exposure brightness multiplier
        bool wide_only{false};                    // debug: skip the sharp views
        bool sharp_only{false};                   // debug: skip the wide views
    };

    // Creates a SpriteBatch for the given output format using the compositor's
    // shaders and blend state (provided by D3D12Component, which owns the queue).
    using BatchFactory = std::function<std::unique_ptr<DirectX::DX12::SpriteBatch>(DXGI_FORMAT)>;

    // Records the composition of `atlas` (in `atlas_state`, left in that state)
    // into `destination` (an OpenXR swapchain image in RENDER_TARGET state,
    // left in that state). Returns false, recording nothing, when it cannot run;
    // the caller then falls back to the overlay-layer submit.
    bool record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* atlas,
        D3D12_RESOURCE_STATES atlas_state, ID3D12Resource* destination, const Frame& frame,
        const BatchFactory& make_batch);

    void reset();

private:
    bool ensure_source(ID3D12Device* device, ID3D12Resource* atlas);
    bool ensure_batch(DXGI_FORMAT view_format, const BatchFactory& make_batch);
    bool ensure_output(ID3D12Device* device, const D3D12_RESOURCE_DESC& like);
    bool ensure_direct_rtv(ID3D12Device* device);

    std::unique_ptr<TextureContext> m_source{};   // SRV over Unreal's atlas
    ID3D12Resource* m_source_resource{nullptr};
    // Views replaced while earlier command lists may still read them: kept
    // for a few frames instead of being destroyed under the GPU.
    std::deque<std::unique_ptr<TextureContext>> m_retired{};
    // Fallback when the swapchain image cannot be a render target: compose
    // here, then copy. Same size/format as the swapchain image.
    TextureContext m_output{};
    D3D12_RESOURCE_DESC m_output_desc{};
    // One RTV slot, rewritten for whichever swapchain image is drawn into
    // (an RTV descriptor is read when OMSetRenderTargets is recorded).
    std::unique_ptr<DirectX::DescriptorHeap> m_direct_rtv_heap{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_batch{};
    DXGI_FORMAT m_batch_format{DXGI_FORMAT_UNKNOWN};
    bool m_failed{false};
};
}
