#pragma once
#include "DispatchMoviePolicy.hpp"
#include <d3d11.h>
#include <wrl/client.h>

namespace uevr::dispatch_movie {
inline bool bgra(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
inline bool copy_movie(ID3D11DeviceContext* context, ID3D11Texture2D* destination, ID3D11Texture2D* source) {
    if (!context || !destination || !source || destination == source || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {return false;}
    D3D11_TEXTURE2D_DESC a{},b{}; source->GetDesc(&a); destination->GetDesc(&b);
    if (!extent(a.Width,a.Height) || a.Width != b.Width || a.Height != b.Height ||
        a.ArraySize != 1 || b.ArraySize != 1 || a.SampleDesc.Count != 1 || b.SampleDesc.Count != 1 ||
        b.MipLevels != 1 || a.Usage != D3D11_USAGE_DEFAULT || b.Usage != D3D11_USAGE_DEFAULT ||
        a.CPUAccessFlags || b.CPUAccessFlags || !bgra(a.Format) || !bgra(b.Format)) {return false;}
    Microsoft::WRL::ComPtr<ID3D11Device> x,y,z;
    source->GetDevice(&x);destination->GetDevice(&y);context->GetDevice(&z);
    if (x != y || x != z) {return false;}
    // The media source can have mipmaps; CopyResource would be invalid against
    // the single-mip XR image. Copy the complete, un-cropped top mip instead.
    context->CopySubresourceRegion(destination,0,0,0,0,source,0,nullptr);
    return true;
}
}
