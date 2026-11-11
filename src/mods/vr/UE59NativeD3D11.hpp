#pragma once

#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

#include "NativeFrameDiagnostics.hpp"

namespace uevr::ue59_native::dx11 {

constexpr bool admits_linked_view_proof(bool modern_dx12, bool reviewed_ue59, bool dx11) {
    return modern_dx12 || (reviewed_ue59 && dx11);
}

constexpr bool mode_supported(bool openxr, bool standard_native_output) {
    return openxr && standard_native_output;
}

constexpr bool split_pending(uint64_t clocks, uint32_t engine, int32_t present) {
    if (clocks == UINT64_MAX) { return false; }
    const auto engine_delta = engine - native_frame::engine_clock(clocks);
    const auto render_delta = static_cast<int64_t>(present) - native_frame::present_clock(clocks);
    return engine_delta <= 1 && native_frame::frame_window(render_delta, engine_delta, engine_delta == 0).accepted();
}

struct Binding {
    struct Identity {
        uint64_t generation{};
        uintptr_t viewport_target{}, scene_rhi{}, packed_scene{}, capture_resource{}, device{}, swapchain{};
        bool operator==(const Identity&) const = default;
    } identity;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> packed, right;

    bool matches_capture(uint64_t current_generation, uintptr_t viewport, uintptr_t scene, uintptr_t capture) const {
        return identity.generation != 0 && identity.generation == current_generation &&
            viewport != 0 && viewport == identity.viewport_target && scene != 0 && scene == identity.scene_rhi &&
            capture != 0 && capture == identity.capture_resource;
    }
};

constexpr bool simple_bgra(const D3D11_TEXTURE2D_DESC& desc) {
    return (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) && desc.MipLevels == 1 && desc.ArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 && desc.Usage == D3D11_USAGE_DEFAULT &&
        desc.CPUAccessFlags == 0;
}

constexpr bool source_pair_matches(const D3D11_TEXTURE2D_DESC& packed, const D3D11_TEXTURE2D_DESC& right,
    uint32_t width, uint32_t height) {
    constexpr auto limit = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    return width != 0 && width <= limit / 2 && height != 0 && height <= limit &&
        simple_bgra(packed) && simple_bgra(right) && (packed.BindFlags & D3D11_BIND_RENDER_TARGET) &&
        (right.BindFlags & D3D11_BIND_RENDER_TARGET) && packed.Width == width * 2 &&
        packed.Height == height && right.Width == width && right.Height == height;
}

constexpr bool destination_matches(const D3D11_TEXTURE2D_DESC& packed, const D3D11_TEXTURE2D_DESC& destination) {
    return simple_bgra(destination) && destination.Width == packed.Width && destination.Height == packed.Height;
}

inline bool belongs_to_device(ID3D11Texture2D* texture, ID3D11Device* device) {
    if (!texture || !device) { return false; }
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    return owner.Get() == device;
}

inline bool copy_pair(ID3D11DeviceContext* context, ID3D11Texture2D* packed, ID3D11Texture2D* right,
    ID3D11Texture2D* destination, uint32_t width, uint32_t height) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !packed || !right || !destination ||
        packed == right || packed == destination || right == destination) { return false; }
    D3D11_TEXTURE2D_DESC packed_desc{}, right_desc{}, destination_desc{};
    packed->GetDesc(&packed_desc);
    right->GetDesc(&right_desc);
    destination->GetDesc(&destination_desc);
    if (!source_pair_matches(packed_desc, right_desc, width, height) ||
        !destination_matches(packed_desc, destination_desc)) { return false; }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!belongs_to_device(packed, device.Get()) || !belongs_to_device(right, device.Get()) ||
        !belongs_to_device(destination, device.Get())) { return false; }

    // Validate both copies before writing either half of the compositor image.
    const D3D11_BOX eye{0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(destination, 0, 0, 0, 0, packed, 0, &eye);
    context->CopySubresourceRegion(destination, 0, width, 0, 0, right, 0, &eye);
    return true;
}

} // namespace uevr::ue59_native::dx11
