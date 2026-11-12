#pragma once
#include "mods/vr/DispatchMovieD3D12.hpp"
#include <stdexcept>
#include <vector>

namespace dispatch_movie_tests {
template<class Expect> void dx12(Expect&& expect, ID3D12Device* device, ID3D12CommandQueue* queue) {
    using Microsoft::WRL::ComPtr;
    const auto check=[](HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("Dispatch DX12 WARP setup/copy failed"); } };
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    const auto make=[&](D3D12_RESOURCE_DESC desc,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES heap{type,D3D12_CPU_PAGE_PROPERTY_UNKNOWN,D3D12_MEMORY_POOL_UNKNOWN,1,1};
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)));
        return resource;
    };
    constexpr UINT width=64,height=32;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,width,height,1,3,DXGI_FORMAT_B8G8R8A8_TYPELESS,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto source=make(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    desc.MipLevels=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    auto destination=make(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 size{};
    device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&size);
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
    auto upload=make(buffer,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=make(buffer,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* memory{};
    check(upload->Map(0,nullptr,&memory));
    for (UINT y=0;y<height;++y) {
        auto* row=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(memory)+layout.Offset+y*layout.Footprint.RowPitch);
        for (UINT x=0;x<width;++x) { row[x]=(y<<16)|(x<<8)|0x37; }
    }
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=upload.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint=layout;
    to.pResource=source.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    constexpr auto state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={source.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,state};
    list->ResourceBarrier(1,&barrier);
    expect(uevr::dispatch_movie::copy_movie(list.Get(),destination.Get(),source.Get(),state),"Dispatch DX12 full movie top-mip copy");
    expect(!uevr::dispatch_movie::copy_movie(list.Get(),destination.Get(),source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        !uevr::dispatch_movie::copy_movie(list.Get(),source.Get(),source.Get(),state),"Dispatch DX12 rejects write-state guesses and self copies");
    // Other mips remained COPY_DEST. This all-mip barrier is valid only if the
    // copy helper restored mip zero and never transitioned the remaining mips.
    barrier.Transition={source.Get(),0,state,D3D12_RESOURCE_STATE_COPY_DEST}; list->ResourceBarrier(1,&barrier);
    barrier.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,state}; list->ResourceBarrier(1,&barrier);
    barrier.Transition={destination.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1,&barrier);
    from={}; from.pResource=destination.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to={}; to.pResource=readback.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=layout;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    check(list->Close());
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(),1));
    struct Event { HANDLE value{CreateEventW(nullptr,FALSE,FALSE,nullptr)}; ~Event() { if (value) { CloseHandle(value); } } } event;
    if (!event.value) { throw std::runtime_error("Dispatch DX12 fence event allocation failed"); }
    check(fence->SetEventOnCompletion(1,event.value));
    if (WaitForSingleObject(event.value,5000)!=WAIT_OBJECT_0) { throw std::runtime_error("Dispatch DX12 copy fence did not retire"); }
    expect(SUCCEEDED(device->GetDeviceRemovedReason()),"Dispatch DX12 copy/restore does not remove the device");
    check(readback->Map(0,nullptr,&memory));
    bool equal=true;
    for (UINT y=0;y<height;++y) {
        const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(memory)+layout.Offset+y*layout.Footprint.RowPitch);
        for (UINT x=0;x<width;++x) { equal &= row[x]==((y<<16)|(x<<8)|0x37); }
    }
    expect(equal,"Dispatch DX12 preserves all pixels including transparent alpha and every movie border");
    readback->Unmap(0,nullptr);
}
}
