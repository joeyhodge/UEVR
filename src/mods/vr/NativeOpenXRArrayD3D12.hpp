#pragma once

#include <cstdint>
#include <d3d12.h>

namespace uevr::native_array {
inline uint32_t format_family(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 2;
    default: return 0;
    }
}
inline bool array_destination(const D3D12_RESOURCE_DESC& d, uint32_t width, uint32_t height) noexcept {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height &&
        d.DepthOrArraySize == 2 && d.MipLevels == 1 && d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0 &&
        format_family(d.Format) != 0;
}
inline bool eye_copy(const D3D12_RESOURCE_DESC& src, const D3D12_RESOURCE_DESC& dst, const D3D12_BOX& box) noexcept {
    return src.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src.DepthOrArraySize == 1 && src.MipLevels >= 1 &&
        src.SampleDesc.Count == 1 && src.SampleDesc.Quality == 0 &&
        format_family(src.Format) && format_family(src.Format) == format_family(dst.Format) &&
        box.left < box.right && box.top < box.bottom && box.right <= src.Width && box.bottom <= src.Height &&
        box.front == 0 && box.back == 1 && box.right - box.left == dst.Width && box.bottom - box.top == dst.Height;
}
inline bool pair_copy(const D3D12_RESOURCE_DESC& left, const D3D12_RESOURCE_DESC& right,
    const D3D12_RESOURCE_DESC& dst, const D3D12_BOX& left_box, const D3D12_BOX& right_box) noexcept {
    return dst.Width <= UINT32_MAX && array_destination(dst, static_cast<uint32_t>(dst.Width), dst.Height) &&
        eye_copy(left, dst, left_box) && eye_copy(right, dst, right_box);
}
}
