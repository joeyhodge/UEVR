#include <d3d11.h>
#include <d3d12.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include "UIAlpha.hpp"
#include "UIAlphaProbe.hpp"
#include "UIAlphaSwapchain.hpp"
#include <array>
#include <chrono>
#include <vector>
#include <utility>
#include <spdlog/spdlog.h>

namespace uevr::ui_alpha {

bool replace_layer(XrCompositionLayerBaseHeader& layer, const Request& request, XrSwapchain converted) {
    if (!converts(request.mode) || layer.next != nullptr ||
        !(layer.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) || converted == request.source) { return false; }
    const XrSwapchainSubImage image{converted, {{0, 0},
        {static_cast<int32_t>(request.extent.width), static_cast<int32_t>(request.extent.height)}}, 0};
    if (!ui_alpha::replace_sub_image(layer, request.source, image, request.extent)) { return false; }
    layer.layerFlags &= ~XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    return true;
}

namespace {
struct Swapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    ui_alpha::ImageLease lease;
    uint32_t count{};
    ~Swapchain() { if (handle != XR_NULL_HANDLE) { xrDestroySwapchain(handle); } }
    bool create(const Request& r) {
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        if (xrGetSystemProperties(r.instance, r.system, &props) != XR_SUCCESS ||
            r.extent.width > props.graphicsProperties.maxSwapchainImageWidth ||
            r.extent.height > props.graphicsProperties.maxSwapchainImageHeight) { return false; }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = r.extent.width; info.height = r.extent.height;
        return xrCreateSwapchain(r.session, &info, &handle) == XR_SUCCESS &&
            xrEnumerateSwapchainImages(handle, 0, &count, nullptr) == XR_SUCCESS && ui_alpha::image_budget(r.extent, count);
    }
    std::optional<uint32_t> acquire() {
        return lease.acquire(count, [&](uint32_t& index) {
            const XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            return xrAcquireSwapchainImage(handle, &info, &index);
        }, [&] {
            const XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 0};
            return xrWaitSwapchainImage(handle, &info);
        });
    }
    bool release() {
        return lease.release([&] {
            const XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            return xrReleaseSwapchainImage(handle, &info);
        });
    }
    template<class Image> std::vector<Image> images(XrStructureType type) {
        std::vector<Image> result(count);
        for (auto& image : result) { image.type = type; }
        uint32_t returned{};
        if (xrEnumerateSwapchainImages(handle, count, &returned,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(result.data())) != XR_SUCCESS || returned != count) { result.clear(); }
        return result;
    }
};

struct Key {
    Request request{};
    const void* device{};
    const void* queue{};
    std::array<const void*, 16> sources{};
    uint32_t count{};
    bool operator==(const Key&) const = default;
};
template<class Texture> Key key(const Request& request, const void* device, const void* queue, std::span<Texture* const> sources) {
    Key k{request, device, queue};
    k.count = static_cast<uint32_t>(sources.size());
    if (sources.size() <= k.sources.size()) { std::copy(sources.begin(), sources.end(), k.sources.begin()); }
    return k;
}

inline ui_alpha::PixelOperation operation(Mode mode) {
    return mode == Mode::straight_to_premultiplied ? ui_alpha::PixelOperation::straight_to_premultiplied
        : ui_alpha::PixelOperation::encoded_premultiplied_to_linear;
}

template<class GPU, class Probe> struct State {
    Swapchain chain;
    GPU gpu;
    Probe probe;
    bool retired(bool wait = false) { return gpu.retired(wait) && probe.retired(wait); }
};

template<class GPU, class Probe> struct Alpha {
    std::shared_ptr<State<GPU, Probe>> state;
    Key current{};
    bool failed{}, ready{};
    std::optional<Sample> observed;
    std::chrono::steady_clock::time_point next_sample{};

    bool reset() {
        ready = false; observed.reset();
        const bool retired = !state || state->retired(true);
        const bool submitted = state && state.use_count() > 1;
        if (!retired) {
            (void)new std::shared_ptr<State<GPU, Probe>>(std::move(state));
            spdlog::error("[UI Alpha] Retirement unproven; retaining source and converted image resources");
        }
        state.reset(); current = {}; failed = false; next_sample = {};
        return retired && !submitted;
    }
    template<class Initialize> Status copy(const Key& wanted, uint32_t index, Initialize&& initialize) {
        ready = false;
        const auto& r = wanted.request;
        if (!(wanted == current)) {
            if (state && !state->retired()) { return r.mode == Mode::unchanged ? Status::off : Status::gpu_busy; }
            state.reset(); current = wanted; failed = false; observed.reset(); next_sample = {};
        }
        if (r.mode == Mode::unchanged) { return Status::off; }
        if (!valid(r.mode) || !wanted.device || !wanted.count || wanted.count > 16 || index >= wanted.count ||
            r.instance == XR_NULL_HANDLE || r.system == XR_NULL_SYSTEM_ID || r.session == XR_NULL_HANDLE ||
            r.source == XR_NULL_HANDLE || !ui_alpha::image_budget(r.extent, 1)) { return Status::invalid_source; }
        if (failed) { return Status::failed; }
        if (!state) {
            auto next = std::shared_ptr<State<GPU, Probe>>(new State<GPU, Probe>(), [](State<GPU, Probe>* s) {
                if (s->retired(true)) { delete s; }
                else { spdlog::error("[UI Alpha] Retaining an in-flight generation after a GPU/runtime failure"); }
            });
            if ((converts(r.mode) && !next->chain.create(r)) || !initialize(*next)) {
                failed = true;
                spdlog::warn("[UI Alpha] Optional processing unavailable; preserving original layer (mode={}, {}x{})",
                    static_cast<uint32_t>(r.mode), r.extent.width, r.extent.height);
                return Status::invalid_source;
            }
            state = std::move(next);
            spdlog::info("[UI Alpha] Validated source resources: mode={} {}x{}, point-sampled, no scale/pose/input changes",
                static_cast<uint32_t>(r.mode), r.extent.width, r.extent.height);
        }
        if (auto result = state->probe.poll()) { observed = *result; }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_sample) {
            state->probe.enqueue(index);
            next_sample = now + std::chrono::seconds(1);
        }
        if (r.mode == Mode::inspect) { return Status::inspecting; }
        const auto image = state->chain.acquire();
        if (!image) {
            if (state->chain.lease.failed()) { failed = true; return Status::failed; }
            return Status::waiting;
        }
        if (!state->gpu.available(*image)) { return Status::gpu_busy; }
        const bool drawn = state->gpu.draw(index, *image);
        const bool released = state->chain.release();
        if (!drawn || !released) { failed = true; return Status::failed; }
        ready = true;
        return Status::ready;
    }
    std::shared_ptr<void> apply(XrCompositionLayerBaseHeader& layer, Mode expected) {
        if (!ready || failed || !state || expected != current.request.mode) { return {}; }
        return replace_layer(layer, current.request, state->chain.handle) ? state : std::shared_ptr<void>{};
    }
};
}

struct D3D11::Impl : Alpha<ui_alpha::GPU11, Probe11> {};
D3D11::D3D11() : m_impl{std::make_unique<Impl>()} {}
D3D11::~D3D11() { reset(); }
bool D3D11::reset() { std::scoped_lock lock{m_mutex}; return m_impl->reset(); }
void D3D11::begin_frame() { std::scoped_lock lock{m_mutex}; m_impl->ready = false; }
std::optional<Sample> D3D11::sample() { std::scoped_lock lock{m_mutex}; return std::exchange(m_impl->observed, {}); }
std::shared_ptr<void> D3D11::apply(XrCompositionLayerBaseHeader& layer, Mode expected) {
    std::scoped_lock lock{m_mutex}; return m_impl->apply(layer, expected);
}
Status D3D11::copy(const Request& r, ID3D11Device* device, std::span<ID3D11Texture2D* const> sources, uint32_t index) {
    std::scoped_lock lock{m_mutex};
    return m_impl->copy(key(r, device, nullptr, sources), index, [&](auto& state) {
        if (sources.empty() || !sources[0]) { return false; }
        D3D11_TEXTURE2D_DESC desc{}; sources[0]->GetDesc(&desc);
        if (desc.Width != r.extent.width || desc.Height != r.extent.height || !state.probe.initialize(device, sources)) { return false; }
        if (!converts(r.mode)) { return true; }
        auto images = state.chain.template images<XrSwapchainImageD3D11KHR>(XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR);
        std::vector<ID3D11Texture2D*> targets;
        for (const auto& image : images) { targets.push_back(image.texture); }
        return state.gpu.initialize(device, sources, targets, operation(r.mode));
    });
}

struct D3D12::Impl : Alpha<ui_alpha::GPU12, Probe12> {};
D3D12::D3D12() : m_impl{std::make_unique<Impl>()} {}
D3D12::~D3D12() { reset(); }
bool D3D12::reset() { std::scoped_lock lock{m_mutex}; return m_impl->reset(); }
void D3D12::begin_frame() { std::scoped_lock lock{m_mutex}; m_impl->ready = false; }
std::optional<Sample> D3D12::sample() { std::scoped_lock lock{m_mutex}; return std::exchange(m_impl->observed, {}); }
std::shared_ptr<void> D3D12::apply(XrCompositionLayerBaseHeader& layer, Mode expected) {
    std::scoped_lock lock{m_mutex}; return m_impl->apply(layer, expected);
}
Status D3D12::copy(const Request& r, ID3D12Device* device, ID3D12CommandQueue* queue,
    std::span<ID3D12Resource* const> sources, uint32_t index) {
    std::scoped_lock lock{m_mutex};
    return m_impl->copy(key(r, device, queue, sources), index, [&](auto& state) {
        if (sources.empty() || !sources[0]) { return false; }
        const auto desc = sources[0]->GetDesc();
        if (desc.Width != r.extent.width || desc.Height != r.extent.height || !state.probe.initialize(device, queue, sources)) { return false; }
        if (!converts(r.mode)) { return true; }
        auto images = state.chain.template images<XrSwapchainImageD3D12KHR>(XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR);
        std::vector<ID3D12Resource*> targets;
        for (const auto& image : images) { targets.push_back(image.texture); }
        return state.gpu.initialize(device, queue, sources, targets, operation(r.mode));
    });
}

}
