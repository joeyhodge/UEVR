#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "MonoRenderingPolicy.hpp"

namespace uevr::mono::dx11 {

// Used only during Mono transitions/rebuilds, on the Present immediate context.
// End/Flush once, then poll without flushing or spinning on the render thread.
class Retirement {
public:
    bool poll(ID3D11DeviceContext* context, uint64_t token) {
        if (context == nullptr || token == 0 || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device || FAILED(device->GetDeviceRemovedReason())) { return false; }
        if (m_device.Get() != device.Get()) {
            m_query.Reset();
            m_device = device;
            m_token = 0;
        }
        if (!m_query) {
            const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            if (FAILED(device->CreateQuery(&desc, &m_query))) { return false; }
        }
        if (m_token != token) {
            context->End(m_query.Get());
            context->Flush();
            m_token = token;
            return false;
        }
        BOOL finished = FALSE;
        return context->GetData(m_query.Get(), &finished, sizeof(finished), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && finished;
    }
    void invalidate() { m_token = 0; }
private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Query> m_query;
    uint64_t m_token{};
};

inline bool bgra8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

inline bool copy_scene(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11Texture2D* destination) {
    if (context == nullptr || source == nullptr || destination == nullptr || source == destination ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { return false; }
    D3D11_TEXTURE2D_DESC src{}, dst{};
    source->GetDesc(&src);
    destination->GetDesc(&dst);
    const auto regions = copy_regions(src.Width, src.Height, dst.Width, dst.Height);
    Microsoft::WRL::ComPtr<ID3D11Device> device, src_device, dst_device;
    context->GetDevice(&device);
    source->GetDevice(&src_device);
    destination->GetDevice(&dst_device);
    if (!regions || src.ArraySize != 1 || dst.ArraySize != 1 || src.MipLevels != 1 || dst.MipLevels != 1 ||
        src.SampleDesc.Count != 1 || dst.SampleDesc.Count != 1 || src.SampleDesc.Quality != 0 || dst.SampleDesc.Quality != 0 ||
        !bgra8(src.Format) || !bgra8(dst.Format) || !(src.BindFlags & D3D11_BIND_RENDER_TARGET) ||
        dst.Usage != D3D11_USAGE_DEFAULT || device.Get() != src_device.Get() || device.Get() != dst_device.Get() ||
        FAILED(device->GetDeviceRemovedReason())) { return false; }
    for (const auto& region : *regions) {
        const D3D11_BOX box{region.left, region.top, 0, region.right, region.bottom, 1};
        context->CopySubresourceRegion(destination, 0, region.destination_x, 0, 0, source, 0, &box);
    }
    return true;
}
} // namespace uevr::mono::dx11
