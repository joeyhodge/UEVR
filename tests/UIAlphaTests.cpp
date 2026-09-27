#include "mods/vr/UIAlphaPolicy.hpp"
#include "mods/vr/UIAlphaProbe.hpp"
#include "mods/vr/UIAlphaSwapchain.hpp"
#include "mods/vr/UIAlphaGPU.hpp"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ui = uevr::ui_alpha;
using Microsoft::WRL::ComPtr;
static int failures{};
static void expect(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}
static void require(HRESULT hr, const char* why) { if (FAILED(hr)) { throw std::runtime_error(why); } }
static void require(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }

static void policies() {
    namespace a = uevr::ui_alpha;
    expect(a::mode_from_config("0") == a::Mode::unchanged && a::mode_from_config("1") == a::Mode::inspect &&
        a::mode_from_config("2") == a::Mode::straight_to_premultiplied && a::mode_from_config("3") == a::Mode::encoded_premultiplied_to_linear, "saved selections");
    for (const auto v : {"", "-1", "4", "99", "2.0", "2garbage", "true", " 2"}) { expect(a::mode_from_config(v) == a::Mode::unchanged, "malformed config is off"); }
    expect(a::eligible(true, true, false, false, false) && a::eligible(true, false, true, false, false), "Mono/DIBR only");
    expect(!a::eligible(true, false, false, false, false) && !a::eligible(false, true, false, false, false) &&
        !a::eligible(true, true, false, true, false) && !a::eligible(true, true, false, false, true), "Native/Synced/OpenVR/transition/2D excluded");
    expect(a::layer_alpha(false, true) == a::LayerAlpha::opaque &&
        a::layer_alpha(true, false) == a::LayerAlpha::premultiplied &&
        a::layer_alpha(true, true) == a::LayerAlpha::straight, "observed compositor conventions");
    const auto observed = a::layer_observation(a::LayerAlpha::straight, a::LayerAlpha::premultiplied);
    expect(a::original_alpha(observed) == a::LayerAlpha::straight &&
        a::submitted_alpha(observed) == a::LayerAlpha::premultiplied &&
        a::original_alpha(0) == a::LayerAlpha::unknown, "coherent original/submitted flags");
    const uint8_t pixels[]{0,0,0,0, 255,255,255,0, 64,64,64,128, 255,255,255,128, 255,128,0,255};
    const auto stats=a::inspect_bgra(pixels,sizeof(pixels),5,1);
    expect(stats.pixels==5 && stats.transparent==2 && stats.translucent==2 && stats.opaque==1 && stats.transparent_rgb==1 &&
        stats.exceeds_linear_alpha==2 && stats.exceeds_encoded_alpha==2, "source statistics include transparent RGB and partial alpha");
    expect(a::inspect_bgra(nullptr,4,1,1).pixels==0 && a::inspect_bgra(pixels,4,65,1).pixels==0 &&
        a::inspect_bgra(pixels,0,1,1).pixels==0, "bad probe ranges refused");
    // A dark straight color is also compatible with premultiplication. Statistics
    // must stay evidence, not an automatic source-convention detector.
    const uint8_t ambiguous[]{4,4,4,128};
    const auto both=a::inspect_bgra(ambiguous,4,1,1);
    expect(both.exceeds_linear_alpha==0 && both.exceeds_encoded_alpha==0, "ambiguous dark pixels not misclassified");
}

static std::vector<uint8_t> pixels(UINT width, UINT height) {
    std::vector<uint8_t> result(width * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t i = (y * width + x) * 4;
            result[i] = (x * 29 + y * 11) % 256;
            result[i + 1] = (x * 7 + y * 33) % 256;
            result[i + 2] = (x * 17 + y * 19) % 256;
            result[i + 3] = x == 0 ? 0 : x == width - 1 ? 255 : (x * 31 + y * 3) % 256;
        }
    }
    return result;
}
static float linear(float x) { return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f); }
static float srgb(float x) { return x <= .0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - .055f; }
static void verify_pixels(const void* data, size_t pitch, const std::vector<uint8_t>& source,
    UINT sw, UINT sh, UINT dw, UINT dh, ui::PixelOperation operation) {
    bool good=true;
    for(UINT y=0;y<dh;++y) for(UINT x=0;x<dw;++x) {
        const auto sx=(std::min)(UINT((x+.5f)*sw/dw),sw-1), sy=(std::min)(UINT((y+.5f)*sh/dh),sh-1);
        const auto* p=source.data()+(sy*sw+sx)*4;
        const float alpha=p[3]/255.f;
        for(int c=0;c<4;++c) {
            float expected=p[c]/255.f;
            if(c!=3 && operation==ui::PixelOperation::straight_to_premultiplied) { expected=srgb(linear(expected)*alpha); }
            if(c!=3 && operation==ui::PixelOperation::encoded_premultiplied_to_linear) {
                expected=alpha>0 ? srgb(linear(std::clamp(expected/alpha,0.f,1.f))*alpha):0;
            }
            const int actual=static_cast<const uint8_t*>(data)[y*pitch+x*4+c];
            if(std::abs(actual-int(std::lround(expected*255)))>2) { good=false; }
            if(c==3) { expect(actual==p[3], "alpha coverage unchanged"); }
        }
    }
    expect(good,"point-sampled alpha conversion matches reference, including zero/partial/opaque pixels");
}

template<class Probe> static void verify_probe(Probe& probe, const std::vector<uint8_t>& initial, UINT width, UINT height) {
    require(probe.enqueue(0),"probe enqueue");
    expect(!probe.enqueue(0),"no in-flight readback overwrite");
    require(probe.retired(true),"probe retired");
    const auto sample=probe.poll();
    expect(sample && sample->pixels==4096 && sample->sequence==1,"bounded asynchronous sample");
    expect(!probe.poll(),"readback consumed exactly once");
    std::vector<uint8_t> expected(64*64*4);
    for(UINT y=0;y<64;++y) for(UINT x=0;x<64;++x) {
        const auto sx=UINT((x+.5f)*width/64), sy=UINT((y+.5f)*height/64);
        std::memcpy(expected.data()+(y*64+x)*4,initial.data()+(sy*width+sx)*4,4);
    }
    const auto cpu=uevr::ui_alpha::inspect_bgra(expected.data(),256,64,64);
    expect(sample && sample->transparent==cpu.transparent && sample->translucent==cpu.translucent && sample->opaque==cpu.opaque &&
        sample->transparent_rgb==cpu.transparent_rgb, "GPU probe samples source, not converted result");
}

static void dx11() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context);
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    require(result, "DX11 WARP");
    ComPtr<ID3D11InfoQueue> info; device.As(&info);
    for (const auto width : {8u, 17u}) for (const auto operation : {ui::PixelOperation::point_copy, ui::PixelOperation::straight_to_premultiplied, ui::PixelOperation::encoded_premultiplied_to_linear}) {
        constexpr UINT height = 5;
        const ui::Extent output{width,height};
        auto initial = pixels(width, height);
        D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        const D3D11_SUBRESOURCE_DATA data{initial.data(), width * 4, 0};
        ComPtr<ID3D11Texture2D> src, dst, readback;
        require(device->CreateTexture2D(&desc, &data, &src), "DX11 source");
        desc.Width = output.width; desc.Height = output.height;
        require(device->CreateTexture2D(&desc, nullptr, &dst), "DX11 target");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(device->CreateTexture2D(&desc, nullptr, &readback), "DX11 readback");
        ID3D11Texture2D* sources[]{src.Get()}; ID3D11Texture2D* targets[]{dst.Get()};
        ui::GPU11 gpu;
        require(gpu.initialize(device.Get(), sources, targets, operation), "DX11 resampler initialization");
        const D3D11_VIEWPORT viewport{2, 3, 23, 45, .1f, .8f};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ComPtr<ID3D11RenderTargetView> rtv;
        require(device->CreateRenderTargetView(src.Get(), nullptr, &rtv), "saved target");
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        require(gpu.draw(0, 0), "DX11 resampling");
        D3D11_VIEWPORT after{}; UINT count = 1; context->RSGetViewports(&count, &after);
        D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
        ComPtr<ID3D11RenderTargetView> restored; context->OMGetRenderTargets(1, &restored, nullptr);
        expect(!std::memcmp(&viewport, &after, sizeof(after)) && topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST &&
            restored.Get() == rtv.Get(), "entire immediate-context state restored");
        require(gpu.retired(true), "DX11 retirement");
        context->CopyResource(readback.Get(), dst.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "DX11 map");
        verify_pixels(mapped.pData, mapped.RowPitch, initial, width, height, output.width, output.height, operation);
        context->Unmap(readback.Get(), 0);
        uevr::ui_alpha::Probe11 probe;
        require(probe.initialize(device.Get(),sources),"DX11 probe initialization");
        verify_probe(probe,initial,width,height);
        expect(!gpu.draw(1, 0) && !gpu.draw(0, 1), "invalid DX11 image indexes refused");
        context->ClearState();
        ui::GPU11 invalid;
        expect(!invalid.initialize(device.Get(), sources, sources, ui::PixelOperation::point_copy), "in-place processing refused");
    }
    const D3D11_TEXTURE2D_DESC base{16, 8, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> target;
    require(device->CreateTexture2D(&base, nullptr, &target), "DX11 validation target");
    ID3D11Texture2D* targets[]{target.Get()};
    for (int variant = 0; variant < 5; ++variant) {
        auto desc = base;
        if (variant == 0) { desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; }
        if (variant == 1) { desc.Width = 8; }
        if (variant == 2) { desc.ArraySize = 2; }
        if (variant == 3) { desc.MipLevels = 2; }
        if (variant == 4) { desc.BindFlags = D3D11_BIND_RENDER_TARGET; }
        ComPtr<ID3D11Texture2D> texture;
        require(device->CreateTexture2D(&desc, nullptr, &texture), "DX11 negative fixture");
        ID3D11Texture2D* sources[]{texture.Get()};
        ui::GPU11 invalid;
        expect(!invalid.initialize(device.Get(), sources, targets, ui::PixelOperation::straight_to_premultiplied),
            "DX11 unvalidated format/extent/array/mips/bind flags refused");
    }
    ComPtr<ID3D11Device> foreign;
    require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &foreign, nullptr, nullptr), "DX11 foreign device");
    ComPtr<ID3D11Texture2D> foreign_texture;
    require(foreign->CreateTexture2D(&base, nullptr, &foreign_texture), "DX11 foreign source");
    ID3D11Texture2D* foreign_sources[]{foreign_texture.Get()};
    ui::GPU11 invalid_owner;
    expect(!invalid_owner.initialize(device.Get(), foreign_sources, targets, ui::PixelOperation::straight_to_premultiplied),
        "DX11 foreign resource owner refused");
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); info->GetMessage(i, message, &size);
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr, "%s\n", message->pDescription); ++failures; }
    }
}

static void dx12() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "DX12 device");
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    require(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "DX12 queue");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    uint64_t serial{};
    auto submit = [&] {
        require(list->Close(), "close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        require(queue->Signal(fence.Get(), ++serial), "signal");
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (fence->GetCompletedValue() < serial && GetTickCount64() < deadline) { Sleep(1); }
        require(fence->GetCompletedValue() == serial, "test fence completion");
        require(allocator->Reset(), "reset allocator"); require(list->Reset(allocator.Get(), nullptr), "reset list");
    };
    auto texture = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        ComPtr<ID3D12Resource> r;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)), "DX12 allocation");
        return r;
    };
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; list->ResourceBarrier(1, &barrier);
    };
    for (const auto width : {8u, 17u}) for (const auto operation : {ui::PixelOperation::point_copy, ui::PixelOperation::straight_to_premultiplied, ui::PixelOperation::encoded_premultiplied_to_linear}) {
        constexpr UINT height = 5;
        const ui::Extent output{width,height};
        const auto initial = pixels(width, height);
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        auto source = texture(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_layout{}; UINT64 size{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &src_layout, nullptr, nullptr, &size);
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        auto upload = texture(buffer, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped{}; require(upload->Map(0, nullptr, &mapped), "map upload");
        for (UINT y = 0; y < height; ++y) { std::memcpy(static_cast<uint8_t*>(mapped) + y * src_layout.Footprint.RowPitch, initial.data() + y * width * 4, width * 4); }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src_copy{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src_copy.PlacedFootprint = src_layout;
        D3D12_TEXTURE_COPY_LOCATION dst_copy{source.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&dst_copy, 0, 0, 0, &src_copy, nullptr);
        transition(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET); submit();
        desc.Width = output.width; desc.Height = output.height;
        auto target = texture(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT dst_layout{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &dst_layout, nullptr, nullptr, &size); buffer.Width = size;
        auto readback = texture(buffer, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* sources[]{source.Get()}; ID3D12Resource* targets[]{target.Get()};
        ui::GPU12 gpu;
        require(gpu.initialize(device.Get(), queue.Get(), sources, targets, operation), "DX12 resampler initialization");
        require(gpu.draw(0, 0) && gpu.retired(true), "DX12 render + retire");
        require(gpu.draw(0, 0) && gpu.retired(true), "DX12 allocator reuse + source state restored");
        ComPtr<ID3D12Fence> gate;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "retirement test gate");
        require(queue->Wait(gate.Get(), 1), "hold GPU for nonblocking retirement test");
        const bool queued = gpu.draw(0, 0);
        const bool incorrectly_available = gpu.available(0) || gpu.retired();
        const bool incorrectly_reused = gpu.draw(0, 0);
        require(gate->Signal(1), "release test gate");
        expect(queued && !incorrectly_available && !incorrectly_reused, "pending GPU work cannot recycle allocator/image");
        require(gpu.retired(true) && gpu.available(0), "allocator/image available after GPU retirement");
        expect(!gpu.draw(1, 0) && !gpu.draw(0, 1), "invalid DX12 image indexes refused");
        transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src_copy = {target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst_copy = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst_copy.PlacedFootprint = dst_layout;
        list->CopyTextureRegion(&dst_copy, 0, 0, 0, &src_copy, nullptr);
        transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); submit();
        require(readback->Map(0, nullptr, &mapped), "map readback");
        verify_pixels(mapped, dst_layout.Footprint.RowPitch, initial, width, height, output.width, output.height, operation);
        readback->Unmap(0, nullptr);
        uevr::ui_alpha::Probe12 probe;
        require(probe.initialize(device.Get(),queue.Get(),sources),"DX12 probe initialization");
        verify_probe(probe,initial,width,height);
        ui::GPU12 invalid;
        expect(!invalid.initialize(device.Get(), queue.Get(), sources, sources, ui::PixelOperation::point_copy), "DX12 in-place processing refused");
    }
    require(list->Close(), "close test list");
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC base{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 1,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &base, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, IID_PPV_ARGS(&target)), "DX12 validation target");
    ID3D12Resource* targets[]{target.Get()};
    for (int variant = 0; variant < 5; ++variant) {
        auto desc = base;
        if (variant == 0) { desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; }
        if (variant == 1) { desc.Width = 8; }
        if (variant == 2) { desc.DepthOrArraySize = 2; }
        if (variant == 3) { desc.MipLevels = 2; }
        if (variant == 4) { desc.Flags = D3D12_RESOURCE_FLAG_NONE; }
        ComPtr<ID3D12Resource> texture;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            variant == 4 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr, IID_PPV_ARGS(&texture)), "DX12 negative fixture");
        ID3D12Resource* sources[]{texture.Get()};
        ui::GPU12 invalid;
        expect(!invalid.initialize(device.Get(), queue.Get(), sources, targets, ui::PixelOperation::straight_to_premultiplied),
            "DX12 unvalidated format/extent/array/mips/flags refused");
    }
    ComPtr<ID3D12CommandQueue> copy_queue;
    const D3D12_COMMAND_QUEUE_DESC copy_desc{D3D12_COMMAND_LIST_TYPE_COPY};
    require(device->CreateCommandQueue(&copy_desc, IID_PPV_ARGS(&copy_queue)), "DX12 negative queue fixture");
    ui::GPU12 invalid_queue;
    expect(!invalid_queue.initialize(device.Get(), copy_queue.Get(), targets, targets, ui::PixelOperation::straight_to_premultiplied),
        "DX12 non-direct queue refused");
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); info->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr, "%s\n", message->pDescription); ++failures; }
    }
}

int main() try {
    policies(); dx11(); dx12();
    std::printf("UI alpha pixel/probe tests: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
