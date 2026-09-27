#include "UIAlphaProbe.hpp"
#include <chrono>

namespace uevr::ui_alpha {
namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
constexpr UINT extent = 64;
template<class F> bool retire(bool wait, F&& poll) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (poll()) { return true; }
        if (!wait) { return false; }
        Sleep(1);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
}

struct Probe11::Impl {
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> sampled, readback;
    Ptr<ID3D11Query> fence;
    ui_alpha::GPU11 gpu;
    bool pending{};
    uint64_t sequence{};
};
Probe11::Probe11() : m_impl{std::make_unique<Impl>()} {}
Probe11::~Probe11() { if (!retired(true)) { (void)m_impl.release(); } }
bool Probe11::initialize(ID3D11Device* device, std::span<ID3D11Texture2D* const> sources) {
    auto& s = *m_impl;
    if (!device || s.context) { return false; }
    device->GetImmediateContext(&s.context);
    D3D11_TEXTURE2D_DESC desc{extent, extent, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0},
        D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &s.sampled))) { return false; }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const D3D11_QUERY_DESC fence{D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &s.readback)) ||
        FAILED(device->CreateQuery(&fence, &s.fence))) { return false; }
    ID3D11Texture2D* targets[]{s.sampled.Get()};
    return s.gpu.initialize(device, sources, targets, ui_alpha::PixelOperation::point_copy);
}
bool Probe11::enqueue(uint32_t source) {
    auto& s = *m_impl;
    if (s.pending || !s.fence || !s.gpu.draw(source, 0)) { return false; }
    s.context->CopyResource(s.readback.Get(), s.sampled.Get());
    s.context->End(s.fence.Get()); s.pending = true;
    return true;
}
bool Probe11::retired(bool wait) {
    auto& s = *m_impl;
    if (!s.pending) { return s.gpu.retired(wait); }
    if (wait) { s.context->Flush(); }
    return retire(wait, [&] {
        BOOL done = FALSE;
        return s.context->GetData(s.fence.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done;
    });
}
std::optional<Sample> Probe11::poll() {
    auto& s = *m_impl;
    if (!s.pending || !retired()) { return {}; }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(s.context->Map(s.readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) { return {}; }
    auto result = inspect_bgra(mapped.pData, mapped.RowPitch, extent, extent);
    s.context->Unmap(s.readback.Get(), 0); s.pending = false;
    result.sequence = ++s.sequence;
    return result;
}

struct Probe12::Impl {
    Ptr<ID3D12Device> device;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12Resource> sampled, readback;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    Ptr<ID3D12Fence> fence;
    ui_alpha::GPU12 gpu;
    uint64_t submitted{}, sequence{};
    bool pending{}, poisoned{};
};
Probe12::Probe12() : m_impl{std::make_unique<Impl>()} {}
Probe12::~Probe12() { if (!retired(true)) { (void)m_impl.release(); } }
bool Probe12::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, std::span<ID3D12Resource* const> sources) {
    auto& s = *m_impl;
    if (!device || !queue || s.device) { return false; }
    s.device = device; s.queue = queue;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, extent, extent, 1, 1,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, extent * extent * 4, 1, 1, 1,
        DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, IID_PPV_ARGS(&s.sampled))) ||
        FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&s.readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list))) ||
        FAILED(s.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) { return false; }
    ID3D12Resource* targets[]{s.sampled.Get()};
    return s.gpu.initialize(device, queue, sources, targets, ui_alpha::PixelOperation::point_copy);
}
bool Probe12::enqueue(uint32_t source) {
    auto& s = *m_impl;
    if (s.pending || s.poisoned || !s.fence || !s.gpu.available(0) || !s.gpu.draw(source, 0)) { return false; }
    if (FAILED(s.allocator->Reset()) || FAILED(s.list->Reset(s.allocator.Get(), nullptr))) { s.poisoned = true; return false; }
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {s.sampled.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    s.list->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION from{s.sampled.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION to{s.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, extent, extent, 1, extent * 4};
    s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); s.list->ResourceBarrier(1, &barrier);
    if (FAILED(s.list->Close())) { s.poisoned = true; return false; }
    ID3D12CommandList* lists[]{s.list.Get()}; s.queue->ExecuteCommandLists(1, lists);
    s.pending = true;
    if (FAILED(s.queue->Signal(s.fence.Get(), ++s.submitted))) { s.poisoned = true; return false; }
    return true;
}
bool Probe12::retired(bool wait) {
    auto& s = *m_impl;
    if (!s.submitted) { return s.gpu.retired(wait); }
    return retire(wait, [&] {
        const auto done = s.fence->GetCompletedValue();
        return done != UINT64_MAX && done >= s.submitted && s.gpu.retired();
    });
}
std::optional<Sample> Probe12::poll() {
    auto& s = *m_impl;
    if (!s.pending || s.poisoned || !retired()) { return {}; }
    const D3D12_RANGE read{0, extent * extent * 4}; void* data{};
    if (FAILED(s.readback->Map(0, &read, &data))) { return {}; }
    auto result = inspect_bgra(data, extent * 4, extent, extent);
    const D3D12_RANGE no_write{}; s.readback->Unmap(0, &no_write);
    s.pending = false; result.sequence = ++s.sequence;
    return result;
}

}
