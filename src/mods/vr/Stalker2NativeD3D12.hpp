#pragma once

#include "Stalker2NativePolicy.hpp"
#include <d3d12.h>
#include <wrl.h>
#include <openxr/openxr.h>

namespace uevr::stalker2_native {
using Microsoft::WRL::ComPtr;
inline bool same_descriptor(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
        a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
        a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
        a.Layout == b.Layout && a.Flags == b.Flags;
}
inline bool color_descriptor(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width && desc.Height &&
        desc.Width <= 32768 && desc.Height <= 32768 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.Width * desc.Height <= (256ull * 1024 * 1024 / 4) && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
        !(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) &&
        (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
}
inline bool on_device(ID3D12Resource* resource, ID3D12Device* device) {
    ComPtr<IUnknown> owner, expected;
    return resource && device && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&owner))) &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) && owner.Get() == expected.Get();
}
inline bool pair_sources(ID3D12Device* device, ID3D12Resource* left, ID3D12Resource* right,
    const D3D12_RESOURCE_DESC& output, uint32_t width, uint32_t height) {
    if (left == right || !on_device(left, device) || !on_device(right, device)) { return false; }
    const auto a = left->GetDesc(), b = right->GetDesc();
    return color_descriptor(a) && color_descriptor(b) && color_descriptor(output) &&
        valid_pair_bounds(a.Width, a.Height, b.Width, b.Height, output.Width, output.Height, width, height);
}
inline bool record_pair(ID3D12GraphicsCommandList* commands, ID3D12Device* device,
    ID3D12Resource* left, ID3D12Resource* right, ID3D12Resource* output, uint32_t width, uint32_t height) {
    if (!commands || !on_device(output, device) || output == left || output == right ||
        !pair_sources(device, left, right, output->GetDesc(), width, height)) { return false; }
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers{};
    for (auto& b : barriers) { b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; }
    barriers[0].Transition = {left, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {right, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[2].Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
    commands->ResourceBarrier(3, barriers.data());
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = output;
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    from.pResource = left; commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    from.pResource = right; commands->CopyTextureRegion(&to, width, 0, 0, &from, &box);
    for (auto& b : barriers) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); }
    commands->ResourceBarrier(3, barriers.data());
    return true;
}
inline bool valid_views(const std::array<XrView, 2>& views) {
    for (const auto& v : views) {
        const auto& p = v.pose.position;
        const auto& q = v.pose.orientation;
        const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (v.type != XR_TYPE_VIEW || !std::isfinite(norm) || std::abs(norm - 1.0f) > 0.02f ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(v.fov.angleLeft) || !std::isfinite(v.fov.angleRight) ||
            !std::isfinite(v.fov.angleUp) || !std::isfinite(v.fov.angleDown) ||
            v.fov.angleLeft >= v.fov.angleRight || v.fov.angleDown >= v.fov.angleUp) { return false; }
    }
    return true;
}
inline bool record_cache_copy(ID3D12GraphicsCommandList* commands, ID3D12Resource* source, ID3D12Resource* cache) {
    if (!commands || !source || !cache || source == cache ||
        !color_descriptor(source->GetDesc()) || !same_descriptor(source->GetDesc(), cache->GetDesc())) { return false; }
    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
    for (auto& b : barriers) { b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {cache, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
    commands->ResourceBarrier(2, barriers.data());
    commands->CopyResource(cache, source);
    for (auto& b : barriers) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); }
    commands->ResourceBarrier(2, barriers.data());
    return true;
}

struct PairFrame {
    ComPtr<ID3D12Resource> texture, left, right;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t fence_value{};
    bool gpu_references{};
    PairValidity validity{};
    D3D12_RESOURCE_DESC descriptor{};
    std::array<XrView, 2> views{};
    std::shared_ptr<const void> packet;
    bool retired() const {
        return !gpu_references || (device && FAILED(device->GetDeviceRemovedReason())) ||
            (fence && fence_retired(fence->GetCompletedValue(), fence_value));
    }
    ~PairFrame() {
        if (retired()) { return; }
        // An enqueue with no usable fence must not free live GPU references at
        // teardown. This last-resort quarantine is bounded by the three slots.
        texture.Detach(); left.Detach(); right.Detach();
        fence.Detach(); queue.Detach(); device.Detach();
    }
};

// Three owned image slots, not an unbounded retired-resource list. OpenXR image
// contexts additionally retain each slot through their own command-list fence.
class PairCache {
public:
    void invalidate() { m_current.reset(); for (auto& slot : m_slots) { if (slot) { slot->validity.invalidate(); } } }
    std::shared_ptr<PairFrame> prepare(ID3D12Device* device, ID3D12CommandQueue* queue,
        const D3D12_RESOURCE_DESC& desc) {
        if (!device || !queue || !color_descriptor(desc) || FAILED(device->GetDeviceRemovedReason())) { return {}; }
        ComPtr<IUnknown> queue_device, expected;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) ||
            FAILED(device->QueryInterface(IID_PPV_ARGS(&expected))) || queue_device.Get() != expected.Get()) { return {}; }
        for (auto& slot : m_slots) {
            if (slot && (slot.use_count() != 1 || !slot->retired())) { continue; }
            if (!slot) { slot = std::make_shared<PairFrame>(); }
            if (slot->device.Get() != device || !same_descriptor(slot->descriptor, desc)) {
                *slot = {};
                const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                    D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&slot->texture)))) { slot.reset(); return {}; }
                slot->device = device;
                slot->descriptor = desc;
            }
            slot->validity.invalidate(); slot->packet.reset(); slot->fence.Reset();
            slot->left.Reset(); slot->right.Reset(); slot->fence_value = 0; slot->gpu_references = false;
            slot->queue = queue;
            return slot;
        }
        return {};
    }
    void commit(const std::shared_ptr<PairFrame>& pair, PairIdentity identity, int64_t now, bool success) {
        if (!pair) { return; }
        pair->validity.commit(identity, now, success && pair->fence && pair->fence_value != 0, success);
        if (pair->validity.submitted) { m_current = pair; }
        else { m_current.reset(); }
    }
    std::shared_ptr<PairFrame> reuse(const PairIdentity& identity, uint32_t frame, int64_t now,
        const D3D12_RESOURCE_DESC& desc) const {
        return m_current && m_current->validity.reusable(identity, frame, now) &&
            same_descriptor(m_current->descriptor, desc) &&
            m_current->device && SUCCEEDED(m_current->device->GetDeviceRemovedReason()) ? m_current : nullptr;
    }
private:
    std::array<std::shared_ptr<PairFrame>, 3> m_slots{};
    std::shared_ptr<PairFrame> m_current;
};
} // namespace uevr::stalker2_native
