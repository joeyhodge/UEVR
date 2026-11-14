#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <utility>
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
    return width != 0 && height != 0 && width <= 32768 && height <= 32768 &&
        d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height &&
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

struct ArrayCopyPlan {
    std::array<D3D12_RESOURCE_BARRIER, 3> transitions{};
    UINT count{};
    std::array<ID3D12Resource*, 2> sources{};
    ID3D12Resource* destination{};
    std::array<D3D12_BOX, 2> boxes{};

    template<class Commands, class Observer> void record(Commands& commands, Observer observe) const {
        observe(false, count, transitions.data());
        commands.ResourceBarrier(count, transitions.data());
        for (UINT eye = 0; eye < 2; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = sources[eye]; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = destination; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = eye;
            commands.CopyTextureRegion(&to, 0, 0, 0, &from, &boxes[eye]);
        }
        auto restore = transitions;
        for (UINT i = 0; i < count; ++i) { std::swap(restore[i].Transition.StateBefore, restore[i].Transition.StateAfter); }
        observe(true, count, restore.data());
        commands.ResourceBarrier(count, restore.data());
    }
};

inline std::optional<ArrayCopyPlan> array_copy_plan(ID3D12Resource* left, ID3D12Resource* right, ID3D12Resource* dst,
    const D3D12_BOX& left_box, const D3D12_BOX& right_box, D3D12_RESOURCE_STATES left_state, D3D12_RESOURCE_STATES right_state) {
    if (!left || !right || !dst || left == dst || right == dst || (left == right && left_state != right_state) ||
        !pair_copy(left->GetDesc(), right->GetDesc(), dst->GetDesc(), left_box, right_box)) { return std::nullopt; }
    ArrayCopyPlan plan;
    plan.sources = {left, right}; plan.destination = dst; plan.boxes = {left_box, right_box};
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) { return; }
        auto& barrier = plan.transitions[plan.count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    };
    transition(left, left_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (left != right) { transition(right, right_state, D3D12_RESOURCE_STATE_COPY_SOURCE); }
    // XR_KHR_D3D12_enable requires the color image back in RENDER_TARGET at release.
    transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    return plan;
}
}
