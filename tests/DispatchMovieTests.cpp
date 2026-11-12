#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "mods/vr/DispatchMovieOpenXR.hpp"
#include "DispatchMoviePolicyTests.hpp"
#include "DispatchMovieD3D11Tests.hpp"
#include "DispatchMovieD3D12Tests.hpp"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

// Simulated XR ownership, real WARP GPU work. No headset or runtime is loaded.
using Microsoft::WRL::ComPtr;
static ID3D11Device* device11{};
static ID3D12Device* device12{};
static unsigned creations{}, destructions{}, properties{}, acquisitions{}, releases{}, alive{};
static bool fail_create{}, fail_release{}, timeout_wait{};
static bool fail_movie_enumeration{};
static int failures{};
struct FakeSwapchain {
    std::array<ComPtr<ID3D11Texture2D>, 2> textures11;
    std::array<ComPtr<ID3D12Resource>, 2> textures12;
    uint32_t next{};
    bool acquired{}, waited{};
};
static void expect(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", why);
    }
}
static void require(HRESULT hr, const char* why) {
    if (FAILED(hr)) {
        throw std::runtime_error(why);
    }
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* props) {
    ++properties;
    props->graphicsProperties = {8192, 8192, 16};
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output) {
    ++creations;
    if (fail_create) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    auto chain = std::make_unique<FakeSwapchain>();
    if (device11) {
        D3D11_TEXTURE2D_DESC desc{info->width, info->height, 1, 1, static_cast<DXGI_FORMAT>(info->format), {1, 0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        for (auto& texture : chain->textures11) {
            if (FAILED(device11->CreateTexture2D(&desc, nullptr, &texture))) {
                return XR_ERROR_OUT_OF_MEMORY;
            }
        }
    } else if (device12) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, info->width, info->height, 1, 1,
            static_cast<DXGI_FORMAT>(info->format), {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        for (auto& texture : chain->textures12) {
            if (FAILED(device12->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)))) {
                return XR_ERROR_OUT_OF_MEMORY;
            }
        }
    } else {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *output = reinterpret_cast<XrSwapchain>(chain.release());
    ++alive;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(
    XrSwapchain handle, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 2;
    if (!capacity) {
        return XR_SUCCESS;
    }
    if (fail_movie_enumeration) { return XR_ERROR_RUNTIME_FAILURE; }
    if (capacity != 2) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    const auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
        auto* native = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (int i = 0; i < 2; ++i) {
            native[i].texture = chain.textures11[i].Get();
        }
    } else {
        auto* native = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (int i = 0; i < 2; ++i) {
            native[i].texture = chain.textures12[i].Get();
        }
    }
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain handle) {
    delete reinterpret_cast<FakeSwapchain*>(handle);
    ++destructions;
    --alive;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    ++acquisitions;
    if (chain.acquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain.acquired = true;
    *index = chain.next++ % 2;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo* info) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    expect(info->timeout == 0, "optional XR waits are nonblocking");
    if (!chain.acquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (timeout_wait) {
        return XR_TIMEOUT_EXPIRED;
    }
    chain.waited = true;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo*) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    ++releases;
    if (!chain.acquired || !chain.waited) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (fail_release) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    chain.acquired = chain.waited = false;
    if (device11) {
        ComPtr<ID3D11DeviceContext> context;
        device11->GetImmediateContext(&context);
        context->Flush();
    }
    return XR_SUCCESS;
}

static void dispatch_movie_layers() {
    namespace movie=uevr::dispatch_movie;
    const auto stage=reinterpret_cast<XrSpace>(uintptr_t{1});
    const auto handle=reinterpret_cast<XrSwapchain>(uintptr_t{2});
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space=stage; layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain=handle; layer.subImage.imageRect.extent={64,32};
    layer.pose.orientation.w=1; layer.pose.position.z=-2; layer.size={4,2};
    std::array<XrCompositionLayerBaseHeader*,1> layers{reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    const auto valid=layer;
    expect(movie::opaque_layer(layers,stage,handle,64,32),"Dispatch movie is a separate opaque BOTH-eye stage-space quad");
    layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"transparent media alpha cannot authorize hiding the projection");
    layer=valid; layer.eyeVisibility=XR_EYE_VISIBILITY_LEFT;
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"a one-eye movie cannot authorize hiding the projection");
    layer=valid; layer.space=reinterpret_cast<XrSpace>(uintptr_t{3});
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"view-space/glued movie layers rejected");
    layer=valid; layer.subImage.imageRect.extent.width=32;
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"half-eye or cropped movie layers rejected");
    layer=valid; layer.pose.position.x=std::numeric_limits<float>::quiet_NaN();
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"invalid world pose cannot hide a working projection");
    layer=valid; layer.pose.orientation.w=0;
    expect(!movie::opaque_layer(layers,stage,handle,64,32),"zero movie orientation rejected");
    expect(!movie::opaque_layer({},stage,handle,64,32),"no movie image means normal projection submission");
}
static void dispatch_movie_images() {
    namespace movie=uevr::dispatch_movie;
    const auto session=reinterpret_cast<XrSession>(uintptr_t{1});
    const auto original=alive;
    {
        auto images=movie::create_images<XrSwapchainImageD3D11KHR>(session,64,32,XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        expect(images && images->handle && images->images.size()==2 && images->images[0].texture,"Dispatch movie enumerates an independent native image set");
        if (images) {
            auto moved=std::move(*images);
            expect(!images->handle && moved.handle && alive==original+1,"movie image ownership moves once without premature destruction");
        }
    }
    expect(alive==original,"unused movie candidate retires without touching scene/UI swapchains");
    fail_create=true;
    expect(!movie::create_images<XrSwapchainImageD3D11KHR>(session,64,32,XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),"movie swapchain creation failure is closed");
    fail_create=false; fail_movie_enumeration=true;
    expect(!movie::create_images<XrSwapchainImageD3D11KHR>(session,64,32,XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),"movie image enumeration failure is closed");
    fail_movie_enumeration=false;
    expect(alive==original,"failed movie enumeration rolls back the uncommitted swapchain");
    {
        auto images=movie::create_images<XrSwapchainImageD3D11KHR>(session,64,32,XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        if (!images) { expect(false,"movie protocol test image allocation"); return; }
        movie::ImageAccess image;
        const auto before=acquisitions, release_before=releases;
        timeout_wait=true;
        expect(!image.acquire(images->handle) && image.acquired && !image.writable,"movie XR timeout retains an unwritable acquisition");
        const auto index=image.index;
        expect(!image.acquire(images->handle) && acquisitions==before+1 && image.index==index,"movie timeout does not acquire a second image");
        expect(!image.release(images->handle) && releases==release_before,"movie cannot release an image before a successful wait");
        timeout_wait=false;
        expect(image.acquire(images->handle) && image.writable && acquisitions==before+1,"movie resumes waiting for the original acquisition");
        expect(!image.retire(images->handle,[]{return false;}) && releases==release_before,"movie cannot release while its GPU references are unretired");
        fail_release=true;
        expect(!image.release(images->handle) && image.release_pending && image.acquired,"failed movie release keeps image ownership");
        expect(!image.acquire(images->handle) && acquisitions==before+1,"failed movie release cannot trigger a new acquisition");
        fail_release=false;
        expect(image.retire(images->handle,[]{return true;}) && !image.acquired && !image.writable && !image.release_pending,
            "retired movie work releases exactly the retained image");
    }
    expect(alive==original,"movie protocol cleanup releases only its own swapchain");
}

int main() try {
    dispatch_movie_tests::run(expect);
    dispatch_movie_layers();
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &context);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &context);
    }
    require(hr, "DX11 WARP");
    device11 = d11.Get();
    dispatch_movie_images();
    dispatch_movie_tests::dx11(expect, d11.Get(), context.Get());
    ComPtr<ID3D11InfoQueue> messages11;
    if (SUCCEEDED(d11.As(&messages11))) {
        for (UINT64 i = 0; i < messages11->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            messages11->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> memory(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(memory.data());
            if (SUCCEEDED(messages11->GetMessage(i, message, &size))) {
                expect(message->Severity != D3D11_MESSAGE_SEVERITY_ERROR &&
                    message->Severity != D3D11_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
            }
        }
    }
    device11 = nullptr;
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> d12;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "DX12 WARP adapter");
    require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)), "DX12 WARP device");
    device12 = d12.Get();
    {
        const auto before = alive;
        auto images = uevr::dispatch_movie::create_images<XrSwapchainImageD3D12KHR>(
            reinterpret_cast<XrSession>(uintptr_t{1}), 64, 32,
            XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        expect(images && images->images.size() == 2 && images->images[0].texture && alive == before + 1,
            "Dispatch DX12 owns an independent native image set");
    }
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    require(d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "DX12 direct queue");
    dispatch_movie_tests::dx12(expect, d12.Get(), queue.Get());
    ComPtr<ID3D12InfoQueue> messages12;
    if (SUCCEEDED(d12.As(&messages12))) {
        for (UINT64 i = 0; i < messages12->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            messages12->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> memory(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(memory.data());
            if (SUCCEEDED(messages12->GetMessage(i, message, &size))) {
                expect(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                    message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
            }
        }
    }
    expect(alive == 0 && creations == destructions + 1,
        "all created XR images retire; the deliberate creation failure owns no handle");
    std::printf("Dispatch movie tests: %d failures\n", failures);
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
