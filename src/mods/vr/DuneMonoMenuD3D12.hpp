#pragma once

#include "DuneMonoMenu.hpp"
#include "MonoD3D12.hpp"

namespace uevr::dune_menu::dx12 {

// A full UI image, not Mono's left-half scene expansion. The source lease and
// command/descriptors remain owned by the caller until the copy fence retires.
template <typename Before>
bool copy_ui(ID3D12GraphicsCommandList* commands, ID3D12Device* device,
    ID3D12Resource* source, ID3D12Resource* destination,
    D3D12_RESOURCE_STATES source_state, D3D12_RESOURCE_STATES destination_state, Before&& before) {
    if (!commands || !device || !source || !destination || source == destination ||
        commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(device->GetDeviceRemovedReason())) { return false; }
    const auto src = source->GetDesc(), dst = destination->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> src_device, dst_device, command_device;
    if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        !uevr::dune_menu::ui_extent(src.Width, src.Height, src.DepthOrArraySize, src.MipLevels,
            src.SampleDesc.Count, dst.Width, dst.Height) || dst.DepthOrArraySize != 1 || dst.MipLevels != 1 ||
        dst.SampleDesc.Count != 1 || src.SampleDesc.Quality != 0 || dst.SampleDesc.Quality != 0 ||
        !uevr::mono::dx12::bgra8(src.Format) || !uevr::mono::dx12::bgra8(dst.Format) ||
        !(src.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || (src.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        !(dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        FAILED(source->GetDevice(IID_PPV_ARGS(&src_device))) || FAILED(destination->GetDevice(IID_PPV_ARGS(&dst_device))) ||
        FAILED(commands->GetDevice(IID_PPV_ARGS(&command_device))) || src_device.Get() != device ||
        dst_device.Get() != device || command_device.Get() != device) { return false; }
    before();
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count{};
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    if (destination_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, destination_state, D3D12_RESOURCE_STATE_COPY_DEST};
    }
    if (count) { commands->ResourceBarrier(count, barriers); }
    commands->CopyResource(destination, source);
    for (UINT i = 0; i < count; ++i) { std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter); }
    if (count) { commands->ResourceBarrier(count, barriers); }
    return true;
}

} // namespace uevr::dune_menu::dx12
