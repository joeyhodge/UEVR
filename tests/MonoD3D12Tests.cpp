#include "mods/vr/MonoD3D12.hpp"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace mono = uevr::mono::dx12;
static int failures{};
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
static void require(HRESULT hr, const char* message) {
    if (FAILED(hr)) { throw std::runtime_error(message); }
}

int main() try {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "DX12 WARP device");
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "command queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&commands)), "command list");
    ComPtr<ID3D12Fence> fence;
    require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    struct Event {
        HANDLE value{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        ~Event() { if (value) { CloseHandle(value); } }
    } event;
    if (!event.value) { throw std::runtime_error("fence event"); }
    uint64_t serial = 0;

    const auto resource = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap_type,
                              D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES heap{heap_type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        ComPtr<ID3D12Resource> result;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
            nullptr, IID_PPV_ARGS(&result)), "resource allocation");
        return result;
    };
    for (const auto width : {16u, 64u, 32u}) {
        constexpr UINT height = 8;
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
            DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        auto source = resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto destination = resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1,
            DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        auto upload = resource(buffer, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = resource(buffer, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        require(upload->Map(0, &no_read, &mapped), "upload map");
        for (UINT y = 0; y != height; ++y) {
            auto row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
            for (UINT x = 0; x != width; ++x) { row[x] = x < width / 2 ? (0xff000000 | (y << 8) | x) : 0xff00ff00; }
        }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
        to.pResource = source.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET};
        commands->ResourceBarrier(1, &barrier);
        for (const auto state : {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}) {
            if (state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                barrier.Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, state};
                commands->ResourceBarrier(1, &barrier);
            }
            expect(mono::copy_scene(commands.Get(), device.Get(), source.Get(), destination.Get(), state,
                D3D12_RESOURCE_STATE_RENDER_TARGET), "current left image copied into both eyes, original states restored");
        }
        expect(!mono::copy_scene(commands.Get(), device.Get(), source.Get(), source.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET), "self-copy rejected");
        std::vector<ComPtr<ID3D12Resource>> rejected;
        for (int mismatch = 0; mismatch != 4; ++mismatch) {
            auto invalid = desc;
            if (mismatch == 0) { invalid.Width /= 2; }
            if (mismatch == 1) { invalid.Format = DXGI_FORMAT_R8G8B8A8_UNORM; }
            if (mismatch == 2) { invalid.DepthOrArraySize = 2; }
            if (mismatch == 3) { invalid.MipLevels = 2; }
            rejected.push_back(resource(invalid, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET));
            expect(!mono::copy_scene(commands.Get(), device.Get(), source.Get(), rejected.back().Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                "resize, format, array and mip mismatch rejected before recording");
        }
        barrier.Transition = {destination.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &barrier);
        from = {}; from.pResource = destination.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to = {}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        require(commands->Close(), "close valid copy list");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        require(queue->Signal(fence.Get(), ++serial), "signal copy fence");
        require(fence->SetEventOnCompletion(serial, event.value), "copy completion event");
        if (WaitForSingleObject(event.value, 5000) != WAIT_OBJECT_0) { throw std::runtime_error("copy completion timeout"); }
        expect(fence->GetCompletedValue() >= serial && SUCCEEDED(device->GetDeviceRemovedReason()), "copy consumers retired without device removal");
        // Native DX12 commands do not retain resource ownership: release only after retirement.
        source.Reset(); upload.Reset();
        require(readback->Map(0, nullptr, &mapped), "readback map");
        bool equal = true;
        for (UINT y = 0; y != height; ++y) {
            const auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
            for (UINT x = 0; x != width; ++x) { equal &= row[x] == (0xff000000 | (y << 8) | (x % (width / 2))); }
        }
        expect(equal, "both eyes receive identical current pixels across resizes");
        readback->Unmap(0, &no_read);
        require(allocator->Reset(), "retired allocator reset");
        require(commands->Reset(allocator.Get(), nullptr), "retired list reset");
    }
    require(commands->Close(), "final empty list close");
    if (messages) {
        for (UINT64 i = 0; i != messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            require(messages->GetMessage(i, message, &size), "debug message read");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                expect(false, message->pDescription);
            }
        }
    }
    std::printf("Mono DX12 WARP copy/state/lifetime checks: %d failures (debug layer %s)\n", failures, debug ? "enabled" : "unavailable");
    return failures != 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
