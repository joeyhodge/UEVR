#include "mods/vr/Stalker2NativeD3D12.hpp"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace policy = uevr::stalker2_native;
using Microsoft::WRL::ComPtr;
static int failures{};
static void expect(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static void require(HRESULT hr, const char* message) { if (FAILED(hr)) { throw std::runtime_error(message); } }

int main() try {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
    ComPtr<ID3D12InfoQueue> messages; device.As(&messages);
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "direct queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "list");
    ComPtr<ID3D12Fence> fence;
    require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    struct Event {
        HANDLE value{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        ~Event() { if (value) { CloseHandle(value); } }
    } event;
    if (!event.value) { throw std::runtime_error("fence event"); }
    const auto retire = [&](uint64_t value) {
        require(queue->Signal(fence.Get(), value), "queue signal");
        require(fence->SetEventOnCompletion(value, event.value), "fence event");
        if (WaitForSingleObject(event.value, 5000) != WAIT_OBJECT_0) { throw std::runtime_error("GPU timeout"); }
    };
    const auto resource = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        ComPtr<ID3D12Resource> value;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&value)), "resource");
        return value;
    };
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 8, 1, 1,
        DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    expect(policy::color_descriptor(desc), "supported BGRA render target");
    for (int field = 0; field != 12; ++field) {
        auto changed = desc;
        if (field == 0) { changed.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; }
        if (field == 1) { changed.Alignment = 65536; } if (field == 2) { ++changed.Width; }
        if (field == 3) { ++changed.Height; } if (field == 4) { ++changed.DepthOrArraySize; }
        if (field == 5) { ++changed.MipLevels; } if (field == 6) { changed.Format = DXGI_FORMAT_R8G8B8A8_UNORM; }
        if (field == 7) { ++changed.SampleDesc.Count; } if (field == 8) { ++changed.SampleDesc.Quality; }
        if (field == 9) { changed.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; }
        if (field == 10) { changed.Flags = D3D12_RESOURCE_FLAG_NONE; }
        if (field == 11) { changed.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }
        expect(!policy::same_descriptor(desc, changed), "all descriptor fields participate in cache identity");
    }
    auto huge = desc; huge.Width = 32768; huge.Height = 32768;
    expect(!policy::color_descriptor(huge), "owned-pair allocation has a byte bound");
    auto source = resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto composed = resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto eye_desc = desc; eye_desc.Width /= 2;
    auto right = resource(eye_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    expect(policy::pair_sources(device.Get(), source.Get(), right.Get(), desc, 16, 8), "complete pair source/device/extent gate");
    expect(!policy::pair_sources(device.Get(), source.Get(), right.Get(), desc, 17, 8), "source box overflow rejected");
    ComPtr<ID3D12Device> other_device;
    // CreateDevice on the same adapter returns the same device singleton.
    // Exercise a real distinct device when a hardware adapter is available.
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> hardware;
        if (factory->EnumAdapters1(index, &hardware) == DXGI_ERROR_NOT_FOUND) { break; }
        if (!hardware) { continue; }
        DXGI_ADAPTER_DESC1 info{}; hardware->GetDesc1(&info);
        if (!(info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(hardware.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other_device)))) { break; }
    }
    ComPtr<ID3D12CommandQueue> other_queue;
    policy::PairCache cache;
    if (other_device) {
        expect(!policy::on_device(source.Get(), other_device.Get()), "different device rejected");
        require(other_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&other_queue)), "other queue");
        expect(!cache.prepare(device.Get(), other_queue.Get(), desc), "cross-device queue cannot allocate cache");
    } else { std::printf("Distinct-device GPU check skipped (no hardware adapter); policy identity checks still run\n"); }
    auto pair = cache.prepare(device.Get(), queue.Get(), desc);
    if (!pair) { throw std::runtime_error("owned cache allocation"); }
    auto* original_texture = pair->texture.Get();
    pair->left = source; pair->right = right;
    const policy::PairIdentity identity{3, 7, 42, reinterpret_cast<uintptr_t>(device.Get()),
        reinterpret_cast<uintptr_t>(queue.Get()), reinterpret_cast<uintptr_t>(source.Get()),
        reinterpret_cast<uintptr_t>(right.Get()), 10, 20, 100};
    cache.commit(pair, identity, 1000, true);
    expect(!cache.reuse(identity, 100, 1000, desc), "recorded-but-unfenced image cannot publish");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    auto upload = resource(buffer, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = resource(buffer, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped{}; const D3D12_RANGE no_read{0, 0};
    require(upload->Map(0, &no_read, &mapped), "upload map");
    for (UINT y = 0; y != desc.Height; ++y) {
        auto row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (UINT x = 0; x != desc.Width; ++x) { row[x] = x < 16 ? 0xff112233 : 0xffaabbcc; }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
    to.pResource = source.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET};
    commands->ResourceBarrier(1, &barrier);
    std::array<D3D12_RESOURCE_BARRIER, 2> seed{};
    for (auto& b : seed) { b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; }
    seed[0].Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    seed[1].Transition = {right.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
    commands->ResourceBarrier(2, seed.data());
    from = {}; from.pResource = source.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to = {}; to.pResource = right.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX right_box{16, 0, 0, 32, 8, 1};
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, &right_box);
    for (auto& b : seed) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); }
    commands->ResourceBarrier(2, seed.data());
    expect(policy::record_pair(commands.Get(), device.Get(), source.Get(), right.Get(), composed.Get(), 16, 8),
        "actual exact-pair composer validates devices/boxes and restores all three states");
    expect(!policy::record_pair(commands.Get(), device.Get(), right.Get(), right.Get(), composed.Get(), 16, 8) &&
        !policy::record_pair(commands.Get(), device.Get(), source.Get(), right.Get(), source.Get(), 16, 8),
        "aliased source/destination barriers rejected before recording");
    expect(policy::record_cache_copy(commands.Get(), composed.Get(), pair->texture.Get()), "owned-pair copy recorded with original states restored");
    expect(!policy::record_cache_copy(commands.Get(), source.Get(), source.Get()), "self-copy rejected before recording");
    auto different_desc = desc; different_desc.Width *= 2;
    auto different = resource(different_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    expect(!policy::record_cache_copy(commands.Get(), source.Get(), different.Get()), "descriptor mismatch rejected before recording");
    barrier.Transition = {pair->texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands->ResourceBarrier(1, &barrier);
    from = {}; from.pResource = pair->texture.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to = {}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); commands->ResourceBarrier(1, &barrier);
    require(commands->Close(), "close copy list");
    ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
    retire(1);
    pair->fence = fence; pair->fence_value = 1; pair->gpu_references = true;
    cache.commit(pair, identity, 1000, policy::SubmissionProof{0, 1, true, true, true, true, true, true}.confirmed());
    expect(cache.reuse(identity, 101, 1010, desc) == pair && pair->retired(), "only submitted retired image is reusable");
    require(readback->Map(0, nullptr, &mapped), "readback map");
    bool equal = true;
    for (UINT y = 0; y != desc.Height; ++y) {
        auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (UINT x = 0; x != desc.Width; ++x) { equal &= row[x] == (x < 16 ? 0xff112233 : 0xffaabbcc); }
    }
    readback->Unmap(0, &no_read);
    expect(equal, "cached left/right colors preserved exactly; no gamma/alpha change");
    auto second = cache.prepare(device.Get(), queue.Get(), desc);
    auto third = cache.prepare(device.Get(), queue.Get(), desc);
    expect(second && third && !cache.prepare(device.Get(), queue.Get(), desc), "three-slot ownership bound cannot allocate a fourth image");
    if (!second || !third) { throw std::runtime_error("bounded cache slots"); }
    auto* pending_texture = second->texture.Get();
    second->gpu_references = true; second->fence = fence; second->fence_value = 99;
    second.reset();
    expect(!cache.prepare(device.Get(), queue.Get(), desc), "unretired GPU slot cannot be freed/recycled");
    retire(99);
    second = cache.prepare(device.Get(), queue.Get(), desc);
    expect(second && second->texture.Get() == pending_texture, "completed fence permits safe slot recycling");
    cache.invalidate();
    expect(!cache.reuse(identity, 101, 1010, desc) && pair->texture.Get() == original_texture,
        "epoch invalidation clears publication while consumers retain texture ownership");
    second.reset();
    auto resized = cache.prepare(device.Get(), queue.Get(), different_desc);
    expect(resized && resized->texture->GetDesc().Width == different_desc.Width &&
        resized->texture->GetDesc().Height == different_desc.Height,
        "resize only replaces retired idle slot");
    std::array<XrView, 2> views{};
    for (auto& v : views) { v.type = XR_TYPE_VIEW; v.pose.orientation.w = 1; v.fov = {-0.6f, 0.6f, 0.6f, -0.6f}; }
    expect(policy::valid_views(views), "cached source pose validated");
    views[1].pose.orientation.w = 0;
    expect(!policy::valid_views(views), "invalid pose cannot establish output capability");
    if (messages) {
        for (UINT64 i = 0; i != messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{}; messages->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            require(messages->GetMessage(i, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { expect(false, message->pDescription); }
        }
    }
    std::printf("Stalker 2 DX12 WARP descriptor/copy/pixels/fence/ownership checks: %d failures (debug %s)\n",
        failures, debug ? "enabled" : "unavailable");
    return failures != 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
}
