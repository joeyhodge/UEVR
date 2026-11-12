#pragma once
#include "DispatchMoviePolicy.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace uevr::dispatch_movie {
inline bool movie_desc(const D3D12_RESOURCE_DESC& d) noexcept {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && d.MipLevels >= 1 && d.MipLevels <= 14 && d.Width <= 8192 &&
        extent(static_cast<uint32_t>(d.Width),d.Height) &&
        (d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}
inline bool copy_movie(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source,
    D3D12_RESOURCE_STATES state) {
    if (!list || !source || !destination || source == destination || !shader_read_state(state)) { return false; }
    const auto a=source->GetDesc(), b=destination->GetDesc();
    if (!movie_desc(a) || !movie_desc(b) || a.Width != b.Width || a.Height != b.Height || b.MipLevels != 1) { return false; }
    Microsoft::WRL::ComPtr<ID3D12Device> x,y,z;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&x))) || FAILED(destination->GetDevice(IID_PPV_ARGS(&y))) ||
        FAILED(list->GetDevice(IID_PPV_ARGS(&z))) || x != y || x != z) { return false; }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier:barriers) { barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; }
    // Only mip zero has been validated. Never transition the other media mips.
    barriers[0].Transition={source,0,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition={destination,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2,barriers);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=source; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=destination; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    for (auto& barrier:barriers) { std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); }
    list->ResourceBarrier(2,barriers);
    return true;
}
}
