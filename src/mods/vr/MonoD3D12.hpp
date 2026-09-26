#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "MonoRenderingPolicy.hpp"

namespace uevr::mono::dx12 {
inline bool bgra8(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// The caller owns the source until its submission fence retires, and provides
// the established scene/swapchain states. No engine resource state is guessed.
inline bool copy_scene(ID3D12GraphicsCommandList* commands, ID3D12Device* device,
    ID3D12Resource* source, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES source_state, D3D12_RESOURCE_STATES destination_state) {
    if (commands == nullptr || device == nullptr || source == nullptr || destination == nullptr ||
        source == destination || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(device->GetDeviceRemovedReason())) { return false; }
    const auto src = source->GetDesc();
    const auto dst = destination->GetDesc();
    const auto regions = copy_regions(src.Width, src.Height, dst.Width, dst.Height);
    Microsoft::WRL::ComPtr<ID3D12Device> src_device, dst_device, command_device;
    if (!regions || src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        src.DepthOrArraySize != 1 || dst.DepthOrArraySize != 1 || src.MipLevels != 1 || dst.MipLevels != 1 ||
        src.SampleDesc.Count != 1 || dst.SampleDesc.Count != 1 ||
        src.SampleDesc.Quality != 0 || dst.SampleDesc.Quality != 0 || !bgra8(src.Format) || !bgra8(dst.Format) ||
        !(src.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        !(dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        FAILED(source->GetDevice(IID_PPV_ARGS(&src_device))) ||
        FAILED(destination->GetDevice(IID_PPV_ARGS(&dst_device))) ||
        FAILED(commands->GetDevice(IID_PPV_ARGS(&command_device))) ||
        src_device.Get() != device || dst_device.Get() != device || command_device.Get() != device) { return false; }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto& b = barriers[count++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    if (destination_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto& b = barriers[count++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, destination_state, D3D12_RESOURCE_STATE_COPY_DEST};
    }
    if (count != 0) { commands->ResourceBarrier(count, barriers); }
    const D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    const D3D12_TEXTURE_COPY_LOCATION to{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    for (const auto& region : *regions) {
        const D3D12_BOX box{region.left, region.top, 0, region.right, region.bottom, 1};
        commands->CopyTextureRegion(&to, region.destination_x, 0, 0, &from, &box);
    }
    for (UINT i = 0; i != count; ++i) { std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter); }
    if (count != 0) { commands->ResourceBarrier(count, barriers); }
    return true;
}
} // namespace uevr::mono::dx12
