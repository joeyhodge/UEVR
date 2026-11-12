#pragma once
#include "mods/vr/DispatchMovieD3D11.hpp"
#include <vector>

namespace dispatch_movie_tests {
template<class Expect> void dx11(Expect&& expect, ID3D11Device* device, ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT width=64,height=32;
    std::vector<uint32_t> pixels(width*height);
    for (UINT y=0;y<height;++y) {
        for (UINT x=0;x<width;++x) { pixels[y*width+x]=(y<<16)|(x<<8)|0x37; }
    }
    D3D11_TEXTURE2D_DESC desc{width,height,3,1,DXGI_FORMAT_B8G8R8A8_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> source,destination,staging;
    expect(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&source)),"Dispatch DX11 three-mip source allocation");
    if (!source) { return; }
    context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),width*4,0);
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.MipLevels=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    expect(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&destination)),"Dispatch DX11 single-mip XR-like destination");
    if (!destination) { return; }
    expect(uevr::dispatch_movie::copy_movie(context,destination.Get(),source.Get()),"Dispatch DX11 copies a complete multi-mip movie into single-mip output");
    desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.BindFlags=0;
    expect(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Dispatch DX11 readback allocation");
    if (!staging) { return; }
    context->CopyResource(staging.Get(),destination.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    expect(SUCCEEDED(hr),"Dispatch DX11 movie readback");
    if (SUCCEEDED(hr)) {
        bool equal=true;
        for (UINT y=0;y<height;++y) {
            const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch);
            for (UINT x=0;x<width;++x) { equal &= row[x]==pixels[y*width+x]; }
        }
        expect(equal,"Dispatch DX11 preserves every RGB/alpha bit and all four borders without eye cropping");
        context->Unmap(staging.Get(),0);
    }
    expect(!uevr::dispatch_movie::copy_movie(context,source.Get(),source.Get()) &&
        !uevr::dispatch_movie::copy_movie(context,staging.Get(),source.Get()),"Dispatch DX11 rejects self-copy and staging output");
    desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0; desc.BindFlags=D3D11_BIND_RENDER_TARGET; desc.MipLevels=2;
    ComPtr<ID3D11Texture2D> wrong;
    device->CreateTexture2D(&desc,nullptr,&wrong);
    expect(!uevr::dispatch_movie::copy_movie(context,wrong.Get(),source.Get()),"Dispatch DX11 rejects multi-mip destinations");
    desc.MipLevels=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; wrong.Reset();
    device->CreateTexture2D(&desc,nullptr,&wrong);
    expect(!uevr::dispatch_movie::copy_movie(context,wrong.Get(),source.Get()),"Dispatch DX11 rejects channel swizzles instead of guessing");
    ComPtr<ID3D11DeviceContext> deferred;
    device->CreateDeferredContext(0,&deferred);
    expect(!uevr::dispatch_movie::copy_movie(deferred.Get(),destination.Get(),source.Get()),"Dispatch DX11 copies only on the immediate context");
}
}
