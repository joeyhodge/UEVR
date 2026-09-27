#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "mods/vr/UIAlpha.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

// Simulated XR ownership, real WARP GPU work. No headset or runtime is loaded.
using Microsoft::WRL::ComPtr;
namespace ui = uevr::ui_alpha;
static ID3D11Device* device11{};
static ID3D12Device* device12{};
static unsigned creations{}, destructions{}, properties{}, acquisitions{}, releases{}, alive{};
static bool fail_create{}, fail_release{}, timeout_wait{};
static int failures{};
struct FakeSwapchain {
    std::array<ComPtr<ID3D11Texture2D>, 2> textures11;
    std::array<ComPtr<ID3D12Resource>, 2> textures12;
    uint32_t next{};
    bool acquired{}, waited{};
};
static void expect(bool ok, const char* why) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); } }
static void require(HRESULT hr, const char* why) { if (FAILED(hr)) { throw std::runtime_error(why); } }

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* props) {
    ++properties;
    props->graphicsProperties = {8192, 8192, 16};
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output) {
    ++creations;
    if (fail_create) { return XR_ERROR_OUT_OF_MEMORY; }
    auto chain = std::make_unique<FakeSwapchain>();
    if (device11) {
        D3D11_TEXTURE2D_DESC desc{info->width, info->height, 1, 1, static_cast<DXGI_FORMAT>(info->format),
            {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        for (auto& texture : chain->textures11) {
            if (FAILED(device11->CreateTexture2D(&desc, nullptr, &texture))) { return XR_ERROR_OUT_OF_MEMORY; }
        }
    } else if (device12) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, info->width, info->height, 1, 1,
            static_cast<DXGI_FORMAT>(info->format), {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        for (auto& texture : chain->textures12) {
            if (FAILED(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                nullptr, IID_PPV_ARGS(&texture)))) { return XR_ERROR_OUT_OF_MEMORY; }
        }
    } else { return XR_ERROR_RUNTIME_FAILURE; }
    *output = reinterpret_cast<XrSwapchain>(chain.release()); ++alive;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain handle, uint32_t capacity,
    uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 2;
    if (!capacity) { return XR_SUCCESS; }
    if (capacity != 2) { return XR_ERROR_SIZE_INSUFFICIENT; }
    const auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
        auto* native = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (int i = 0; i < 2; ++i) { native[i].texture = chain.textures11[i].Get(); }
    } else {
        auto* native = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (int i = 0; i < 2; ++i) { native[i].texture = chain.textures12[i].Get(); }
    }
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain handle) {
    delete reinterpret_cast<FakeSwapchain*>(handle); ++destructions; --alive;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle,
    const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle); ++acquisitions;
    if (chain.acquired) { return XR_ERROR_CALL_ORDER_INVALID; }
    chain.acquired = true; *index = chain.next++ % 2;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo* info) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle);
    expect(info->timeout == 0, "optional XR waits are nonblocking");
    if (!chain.acquired) { return XR_ERROR_CALL_ORDER_INVALID; }
    if (timeout_wait) { return XR_TIMEOUT_EXPIRED; }
    chain.waited = true;
    return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo*) {
    auto& chain = *reinterpret_cast<FakeSwapchain*>(handle); ++releases;
    if (!chain.acquired || !chain.waited) { return XR_ERROR_CALL_ORDER_INVALID; }
    if (fail_release) { return XR_ERROR_RUNTIME_FAILURE; }
    chain.acquired = chain.waited = false;
    if (device11) { ComPtr<ID3D11DeviceContext> context; device11->GetImmediateContext(&context); context->Flush(); }
    return XR_SUCCESS;
}

template<class Helper, class Copy> static void lifecycle(Helper& helper, Copy&& copy) {
    ui::Request r{reinterpret_cast<XrInstance>(uintptr_t{1}),1,reinterpret_cast<XrSession>(uintptr_t{1}),
        reinterpret_cast<XrSwapchain>(uintptr_t{1}),{16,8},ui::Mode::unchanged};
    auto copied = [&] {
        auto status=copy(r);
        for(int i=0;i<1000 && status==ui::Status::gpu_busy;++i) { Sleep(1); status=copy(r); }
        return status;
    };
    auto layer = [&] {
        XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.eyeVisibility=XR_EYE_VISIBILITY_BOTH; q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.subImage={r.source,{{0,0},{16,8}},0}; q.size={4,2}; q.pose.position.z=-2; return q;
    };
    auto apply = [&](auto& q) { return helper.apply(reinterpret_cast<XrCompositionLayerBaseHeader&>(q),r.mode); };
    const auto calls=properties+creations+acquisitions+releases;
    for(int i=0;i<10;++i) { expect(copied()==ui::Status::off,"default unchanged"); }
    expect(properties+creations+acquisitions+releases==calls,"off creates/acquires no XR resources");
    r.mode=ui::Mode::inspect;
    expect(copied()==ui::Status::inspecting,"inspect enabled");
    auto q=layer(); expect(!apply(q),"inspection never changes a layer");
    expect(properties+creations+acquisitions+releases==calls,"inspect needs no XR swapchain or extra XR acquisition");
    helper.reset();
    r.mode=ui::Mode::straight_to_premultiplied;
    expect(copied()==ui::Status::ready,"converted image ready");
    q=layer(); q.layerFlags|=XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    auto lease=apply(q);
    expect(lease && q.subImage.swapchain!=r.source && q.subImage.imageRect.extent.width==16 && q.subImage.imageRect.extent.height==8 &&
        q.layerFlags==XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT && q.size.width==4 && q.size.height==2 && q.pose.position.z==-2,
        "matching pixels/flags published; geometry/input extent unchanged");
    q=layer(); expect(bool(apply(q)),"last converted static image may be reused without flickering back to original");
    q=layer(); expect(!helper.apply(reinterpret_cast<XrCompositionLayerBaseHeader&>(q),ui::Mode::encoded_premultiplied_to_linear),"mid-frame config change cannot use old conversion");
    q=layer(); q.layerFlags=0; expect(!apply(q),"opaque/cinematic layer untouched");
    q=layer(); q.eyeVisibility=XR_EYE_VISIBILITY_LEFT; expect(!apply(q),"eye-specific content untouched");
    q=layer(); q.subImage.imageRect.extent.width=8; expect(!apply(q),"cropped content untouched");
    q=layer(); q.next=&q; expect(!apply(q),"unknown compositor extension chain untouched");
    q=layer(); q.subImage.swapchain=reinterpret_cast<XrSwapchain>(uintptr_t{999}); expect(!apply(q),"foreign layer untouched");
    q=layer(); q.subImage.imageArrayIndex=1; expect(!apply(q),"foreign array slice untouched");
    q=layer(); q.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION; expect(!apply(q),"scene projection untouched");
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    cylinder.eyeVisibility=XR_EYE_VISIBILITY_BOTH; cylinder.subImage=layer().subImage;
    cylinder.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
    cylinder.radius=2; cylinder.centralAngle=1; cylinder.aspectRatio=2;
    expect(bool(apply(cylinder)) && cylinder.subImage.swapchain!=r.source && cylinder.radius==2 && cylinder.centralAngle==1 &&
        cylinder.aspectRatio==2 && (cylinder.layerFlags & XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT),
        "cylinder geometry and unrelated flags preserved");
    helper.begin_frame(); q=layer(); expect(!apply(q),"mode/ownership invalidation prevents stale use");
    const auto destroyed=destructions;
    expect(!helper.reset(),"source handle must be retained during outstanding CPU submission");
    expect(destructions==destroyed,"CPU layer lease holds output until xrEndFrame");
    lease.reset(); expect(destructions==destroyed+1,"CPU lease release retires generation");
    expect(copied()==ui::Status::ready,"recreate after reset");
    r.mode=ui::Mode::encoded_premultiplied_to_linear;
    expect(copied()==ui::Status::ready,"live conversion switch retires previous GPU work");
    r.mode=ui::Mode::unchanged;
    expect(copied()==ui::Status::off,"disable conversion");
    q=layer(); expect(!apply(q),"off restores original layer");
    r.mode=ui::Mode::straight_to_premultiplied;
    timeout_wait=true;
    expect(copied()==ui::Status::waiting,"optional acquire wait is nonblocking");
    const auto acquired=acquisitions;
    expect(copied()==ui::Status::waiting && acquisitions==acquired,"timeout retains same lease");
    timeout_wait=false;
    expect(copied()==ui::Status::ready,"timeout recovery"); helper.reset();
    fail_create=true;
    const auto created=creations;
    expect(copied()==ui::Status::invalid_source && copied()==ui::Status::failed,"allocation fails closed and sticks");
    expect(creations==created+1,"no allocation retry loop");
    fail_create=false; helper.reset();
    fail_release=true; expect(copied()==ui::Status::failed,"failed release cannot publish");
    q=layer(); expect(!apply(q),"failed pixels do not change flags");
    fail_release=false; helper.reset();
    r.extent={17,8}; expect(copied()==ui::Status::invalid_source,"incorrect source extent refused"); helper.reset();
    r.extent={UINT32_MAX,UINT32_MAX}; expect(copied()==ui::Status::invalid_source,"oversize refused before allocation"); helper.reset();
    expect(alive==0,"all optional XR images released");
}

int main() try {
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> context;
    require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &d11, nullptr, &context), "DX11 WARP");
    device11 = d11.Get();
    D3D11_TEXTURE2D_DESC desc11{16, 8, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> source11;
    require(d11->CreateTexture2D(&desc11, nullptr, &source11), "DX11 source");
    ID3D11Texture2D* inputs11[]{source11.Get()};
    ui::D3D11 helper11;
    lifecycle(helper11, [&](const ui::Request& request) { return helper11.copy(request, d11.Get(), inputs11, 0); });
    device11 = nullptr;

    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> d12;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)), "DX12 device");
    device12 = d12.Get();
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    require(d12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "queue");
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC desc12{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 1,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> source12;
    require(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc12, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, IID_PPV_ARGS(&source12)), "DX12 source");
    ID3D12Resource* inputs12[]{source12.Get()};
    ui::D3D12 helper12;
    lifecycle(helper12, [&](const ui::Request& request) { return helper12.copy(request, d12.Get(), queue.Get(), inputs12, 0); });
    device12 = nullptr;
    std::printf("UI alpha lifecycle tests: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
