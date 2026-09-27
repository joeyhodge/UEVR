#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>
#include "UIComposition.hpp"
#include "UICompositionGPU.hpp"
#include "UIAlphaGPU.hpp"
#include "UIAlphaSwapchain.hpp"
#include <algorithm>
#include <vector>
#include <spdlog/spdlog.h>

namespace uevr::ui_composition {
namespace {
template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
constexpr auto format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr uint32_t snapshot_count = 3;

ui_alpha::Mode normalized(ui_alpha::Mode mode) {
    return ui_alpha::converts(mode) ? mode : ui_alpha::Mode::unchanged;
}
ui_alpha::PixelOperation operation(ui_alpha::Mode mode) {
    if (mode == ui_alpha::Mode::straight_to_premultiplied) {
        return ui_alpha::PixelOperation::straight_to_premultiplied;
    }
    if (mode == ui_alpha::Mode::encoded_premultiplied_to_linear) {
        return ui_alpha::PixelOperation::encoded_premultiplied_to_linear;
    }
    return ui_alpha::PixelOperation::point_copy;
}

struct Swapchain {
    XrSwapchain handle{};
    ui_alpha::ImageLease lease;
    uint32_t count{};
    ~Swapchain() {
        if (handle) {
            xrDestroySwapchain(handle);
        }
    }
    bool create(const Frame& f) {
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        if (!budget(f.eye_extent, 1) || xrGetSystemProperties(f.instance, f.system, &props) != XR_SUCCESS ||
            f.eye_extent.width * 2 > props.graphicsProperties.maxSwapchainImageWidth ||
            f.eye_extent.height > props.graphicsProperties.maxSwapchainImageHeight) {
            return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.width = f.eye_extent.width * 2;
        info.height = f.eye_extent.height;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        return xrCreateSwapchain(f.session, &info, &handle) == XR_SUCCESS &&
               xrEnumerateSwapchainImages(handle, 0, &count, nullptr) == XR_SUCCESS && budget(f.eye_extent, count);
    }
    std::optional<uint32_t> acquire() {
        return lease.acquire(
            count,
            [&](uint32_t& index) {
                const XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                return xrAcquireSwapchainImage(handle, &info, &index);
            },
            [&] {
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
    template <class Image> std::vector<Image> images(XrStructureType type) {
        std::vector<Image> result(count);
        for (auto& image : result) {
            image.type = type;
        }
        uint32_t returned{};
        if (xrEnumerateSwapchainImages(handle, count, &returned, reinterpret_cast<XrSwapchainImageBaseHeader*>(result.data())) !=
                XR_SUCCESS ||
            returned != count) {
            result.clear();
        }
        return result;
    }
};

struct DX11 {
    using Texture = ID3D11Texture2D;
    using Device = ID3D11Device;
    using Copy = ui_alpha::GPU11;
    using Project = GPU11;
    using Image = XrSwapchainImageD3D11KHR;
    static constexpr auto image_type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    static bool snapshot(Device* device, Extent e, Ptr<Texture>& out) {
        const D3D11_TEXTURE2D_DESC desc{
            e.width, e.height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &out));
    }
    static bool extent(Texture* t, Extent e) {
        D3D11_TEXTURE2D_DESC desc{};
        t->GetDesc(&desc);
        return desc.Width == e.width && desc.Height == e.height;
    }
    static bool init_copy(Copy& gpu, Device* device, ID3D12CommandQueue*, std::span<Texture* const> in, std::span<Texture* const> out,
        ui_alpha::PixelOperation op) {
        return gpu.initialize(device, in, out, op);
    }
    static bool init_project(
        Project& gpu, Device* device, ID3D12CommandQueue*, std::span<Texture* const> in, std::span<Texture* const> out) {
        return gpu.initialize(device, in, out);
    }
};
struct DX12 {
    using Texture = ID3D12Resource;
    using Device = ID3D12Device;
    using Copy = ui_alpha::GPU12;
    using Project = GPU12;
    using Image = XrSwapchainImageD3D12KHR;
    static constexpr auto image_type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    static bool snapshot(Device* device, Extent e, Ptr<Texture>& out) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, e.width, e.height, 1, 1, format, {1, 0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        return SUCCEEDED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&out)));
    }
    static bool extent(Texture* t, Extent e) {
        const auto d = t->GetDesc();
        return d.Width == e.width && d.Height == e.height;
    }
    static bool init_copy(Copy& gpu, Device* device, ID3D12CommandQueue* queue, std::span<Texture* const> in, std::span<Texture* const> out,
        ui_alpha::PixelOperation op) {
        return gpu.initialize(device, queue, in, out, op);
    }
    static bool init_project(
        Project& gpu, Device* device, ID3D12CommandQueue* queue, std::span<Texture* const> in, std::span<Texture* const> out) {
        return gpu.initialize(device, queue, in, out);
    }
};

struct Key {
    Source source{};
    const void* device{};
    const void* queue{};
    std::array<const void*, 16> textures{};
    uint32_t count{};
    uint64_t request{};
    bool operator==(const Key&) const = default;
};

// An output holds its input generation until both copy and projection commands retire.
// The custom deleter retains the complete generation on device/fence failure.
template <class T> std::shared_ptr<T> make_retained() {
    return std::shared_ptr<T>(new T(), [](T* s) {
        if (s->retired(true)) {
            delete s;
        } else {
            spdlog::error("[UI Composition] Retirement unproven; retaining optional GPU generation");
        }
    });
}
template <class API> struct Input {
    Ptr<typename API::Device> device;
    Ptr<ID3D12CommandQueue> queue;
    std::array<Ptr<typename API::Texture>, snapshot_count> owned;
    std::array<typename API::Texture*, snapshot_count> textures{};
    typename API::Copy copy;
    Key key;
    uint32_t latest{}, next{};
    bool valid{};
    bool retired(bool wait = false) { return copy.retired(wait); }
};
template <class API> struct Output {
    std::shared_ptr<Input<API>> input;
    Swapchain chain;
    typename API::Project gpu;
    Extent extent{};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool retired(bool wait = false) { return gpu.retired(wait); }
};
template <class API> struct Composition {
    struct Slot {
        std::shared_ptr<Input<API>> input;
        std::shared_ptr<Output<API>> output;
        Key key{};
        Extent output_extent{};
        const XrCompositionLayerBaseHeader* binding{};
        bool fresh{}, failed{};
    };
    std::array<Slot, 2> slots;
    uint64_t request{};
    uint64_t rejected_request{};
    Status status{Status::off};

    bool reset() {
        bool safe = true;
        for (auto& s : slots) {
            if ((s.output && (s.output.use_count() > 1 || !s.output->retired(true))) || (s.input && !s.input->retired(true))) {
                safe = false;
            }
            s = {};
        }
        request = rejected_request = 0;
        status = Status::off;
        return safe;
    }
    void begin(uint64_t wanted) {
        // Bit zero enables the option; the remaining bits invalidate toggled generations.
        if (request != wanted) {
            for (auto& s : slots) {
                if (s.input) {
                    s.input->valid = false;
                }
            }
        }
        request = wanted;
        for (auto& s : slots) {
            s.binding = nullptr;
            s.fresh = false;
            if (!(wanted & 1) && (!s.output || (s.output.use_count() == 1 && s.output->retired())) && (!s.input || s.input->retired())) {
                s = {}; // Retire optional storage without waiting on the render thread.
            }
        }
        status = (request & 1) ? (request == rejected_request ? Status::failed : Status::waiting) : Status::off;
    }
    void invalidate(bool framework) {
        auto& s = slots[framework ? 1 : 0];
        s.fresh = false;
        s.binding = nullptr;
        if (s.input) {
            s.input->valid = false;
        }
    }
    void bind(bool framework, const XrCompositionLayerBaseHeader* layer, XrSwapchain original, ui_alpha::Mode effective) {
        auto& s = slots[framework ? 1 : 0];
        s.binding = nullptr;
        if (!(request & 1) || !layer || !s.input || !s.input->valid || s.failed || (!framework && !s.fresh) ||
            s.input->key.request != request || s.input->key.source.swapchain != original ||
            s.input->key.source.alpha != normalized(effective)) {
            return;
        }
        s.binding = layer;
    }
    void capture(bool framework, Source source, typename API::Device* device, ID3D12CommandQueue* queue,
        std::span<typename API::Texture* const> textures, uint32_t index) {
        if (!(request & 1) || request == rejected_request) {
            return;
        }
        auto& s = slots[framework ? 1 : 0];
        s.fresh = false;
        if (!ui_alpha::valid(source.alpha)) {
            invalidate(framework);
            status = Status::unsupported;
            return;
        }
        source.alpha = normalized(source.alpha);
        Key wanted{source, device, queue};
        wanted.count = static_cast<uint32_t>(textures.size());
        wanted.request = request;
        if (textures.size() <= wanted.textures.size()) {
            std::copy(textures.begin(), textures.end(), wanted.textures.begin());
        }
        if (!(s.key == wanted)) {
            if ((s.output && (s.output.use_count() > 1 || !s.output->retired())) || (s.input && !s.input->retired())) {
                status = Status::busy;
                return;
            }
            s = {};
            s.key = wanted;
        }
        if (s.input) {
            s.input->valid = false;
        }
        if (s.failed) {
            status = Status::failed;
            return;
        }
        if (!device || !source.session || !source.swapchain || textures.empty() || textures.size() > 16 || index >= textures.size() ||
            !ui_alpha::image_budget(source.extent, snapshot_count) || !textures[0] || !API::extent(textures[0], source.extent)) {
            status = Status::unsupported;
            return;
        }
        if (!s.input) {
            auto next = make_retained<Input<API>>();
            next->device = device;
            next->queue = queue;
            next->key = wanted;
            for (uint32_t i = 0; i < snapshot_count; ++i) {
                if (!API::snapshot(device, source.extent, next->owned[i])) {
                    s.failed = true;
                    status = Status::failed;
                    return;
                }
                next->textures[i] = next->owned[i].Get();
            }
            if (!API::init_copy(next->copy, device, queue, textures, next->textures, operation(source.alpha))) {
                s.failed = true;
                status = Status::unsupported;
                return;
            }
            s.input = std::move(next);
            spdlog::info("[UI Composition] Validated {} snapshot: {}x{}, alpha={}, request={} (original XR image still acquired)",
                framework ? "ImGui" : "game UI", source.extent.width, source.extent.height, static_cast<uint32_t>(source.alpha), request);
        }
        auto& in = *s.input;
        for (uint32_t i = 0; i < snapshot_count; ++i) {
            const auto target = (in.next + i) % snapshot_count;
            if (!in.copy.available(target)) {
                continue;
            }
            if (!in.copy.draw(index, target)) {
                s.failed = true;
                status = Status::failed;
                return;
            }
            in.latest = target;
            in.next = (target + 1) % snapshot_count;
            in.valid = true;
            s.fresh = true;
            return;
        }
        status = Status::busy;
    }
    Result compose(const Frame& frame, std::span<XrCompositionLayerBaseHeader* const> layers) {
        Result result;
        if (!(request & 1) || request != frame.request) {
            status = Status::off;
            return result;
        }
        if (request == rejected_request) {
            status = Status::failed;
            return result;
        }
        if (!frame.instance || !frame.system || !frame.session || !frame.stage || !frame.view || frame.stage == frame.view ||
            layers.empty() || layers.size() > slots.size() || !budget(frame.eye_extent, 1)) {
            status = Status::unsupported;
            return result;
        }
        struct Prepared {
            Slot* slot{};
            std::array<XrView, 2> eyes{};
            std::array<Draw, 2> draws{};
            XrSpace space{};
        };
        std::array<Prepared, 2> prepared{};
        for (size_t i = 0; i < layers.size(); ++i) {
            if (!layers[i]) {
                status = Status::unsupported;
                return {};
            }
            auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& s) { return s.binding == layers[i]; });
            if (found == slots.end() || !found->input || !found->input->valid || found->failed ||
                found->key.source.session != frame.session || found->key.request != request) {
                if (status != Status::busy && status != Status::failed && status != Status::unsupported) {
                    status = Status::waiting;
                }
                return {};
            }
            if (i && prepared[0].slot == &*found) {
                status = Status::unsupported;
                return {};
            }
            // The layer may reference the alpha helper's converted image, but our snapshot
            // was converted once while the original XR image was still acquired.
            if (layers[i]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
                status = Status::unsupported;
                return {};
            }
            const auto& quad = reinterpret_cast<const XrCompositionLayerQuad&>(*layers[i]);
            if (!supported_quad(*layers[i], quad.subImage.swapchain, found->key.source.extent) ||
                (quad.space != frame.stage && quad.space != frame.view)) {
                status = Status::unsupported;
                return {};
            }
            auto& p = prepared[i];
            p.slot = &*found;
            p.space = quad.space;
            p.eyes = frame.eyes;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                if (!valid_pose(p.eyes[eye].pose)) {
                    status = Status::waiting;
                    return {};
                }
                if (p.space == frame.view) {
                    if (!frame.view_in_stage || !valid_pose(*frame.view_in_stage)) {
                        status = Status::waiting;
                        return {};
                    }
                    p.eyes[eye].pose = relative_pose(*frame.view_in_stage, p.eyes[eye].pose);
                }
                const auto draw = project_quad(quad, p.eyes[eye], eye);
                if (!draw) {
                    status = Status::unsupported;
                    return {};
                }
                p.draws[eye] = *draw;
                p.draws[eye].input = found->input->latest;
            }
        }
        // Never mutate the original layers. Publish the replacements only if EVERY
        // selected UI layer succeeds, so partial allocation cannot duplicate/hide UI.
        for (size_t i = 0; i < layers.size(); ++i) {
            auto& p = prepared[i];
            auto& s = *p.slot;
            if (s.output_extent != frame.eye_extent) {
                if (s.output && (s.output.use_count() > 1 || !s.output->retired())) {
                    status = Status::busy;
                    return {};
                }
                s.output.reset();
                s.output_extent = frame.eye_extent;
            }
            if (!s.output) {
                auto next = make_retained<Output<API>>();
                next->input = s.input;
                next->extent = frame.eye_extent;
                if (!next->chain.create(frame)) {
                    s.failed = true;
                    status = Status::failed;
                    return {};
                }
                auto images = next->chain.template images<typename API::Image>(API::image_type);
                std::vector<typename API::Texture*> targets;
                for (auto& image : images) {
                    targets.push_back(image.texture);
                }
                if (!API::init_project(next->gpu, s.input->device.Get(), s.input->queue.Get(), s.input->textures, targets)) {
                    s.failed = true;
                    status = Status::failed;
                    return {};
                }
                s.output = std::move(next);
                spdlog::info("[UI Composition] Created packed projection UI: {}x{} per eye, {} XR images, no scene/depth input",
                    frame.eye_extent.width, frame.eye_extent.height, s.output->chain.count);
            }
            auto& out = *s.output;
            if (s.output.use_count() > 1) {
                status = Status::busy;
                return {};
            }
            const auto image = out.chain.acquire();
            if (!image) {
                s.failed = out.chain.lease.failed();
                status = s.failed ? Status::failed : Status::busy;
                return {};
            }
            if (!out.gpu.available(*image)) {
                status = Status::busy;
                return {};
            }
            const bool drawn = out.gpu.draw(p.draws, *image);
            const bool released = out.chain.release();
            if (!drawn || !released) {
                s.failed = true;
                status = Status::failed;
                return {};
            }
            for (uint32_t eye = 0; eye < 2; ++eye) {
                auto& view = out.views[eye];
                view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = p.eyes[eye].pose;
                view.fov = p.eyes[eye].fov;
                view.subImage = {out.chain.handle,
                    {{static_cast<int32_t>(eye * frame.eye_extent.width), 0},
                        {static_cast<int32_t>(frame.eye_extent.width), static_cast<int32_t>(frame.eye_extent.height)}},
                    0};
            }
            out.layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            out.layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            out.layer.space = p.space;
            out.layer.viewCount = 2;
            out.layer.views = out.views.data();
            result.layers[i] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&out.layer);
            result.leases[i] = s.output;
        }
        result.count = static_cast<uint32_t>(layers.size());
        status = Status::active;
        return result;
    }
};
} // namespace

struct D3D11::Impl : Composition<DX11> {};
struct D3D12::Impl : Composition<DX12> {};

#define UEVR_UI_COMPOSITION_IMPL(Type)                                                                                       \
    Type::Type()                                                                                                             \
        : m_impl{std::make_unique<Impl>()} {                                                                                 \
    }                                                                                                                        \
    Type::~Type() {                                                                                                          \
        reset();                                                                                                             \
    }                                                                                                                        \
    void Type::begin_frame(uint64_t request) {                                                                               \
        std::scoped_lock lock{m_mutex};                                                                                      \
        m_impl->begin(request);                                                                                              \
    }                                                                                                                        \
    void Type::invalidate(bool framework) {                                                                                  \
        std::scoped_lock lock{m_mutex};                                                                                      \
        m_impl->invalidate(framework);                                                                                       \
    }                                                                                                                        \
    void Type::bind(bool framework, const XrCompositionLayerBaseHeader* layer, XrSwapchain original, ui_alpha::Mode alpha) { \
        std::scoped_lock lock{m_mutex};                                                                                      \
        m_impl->bind(framework, layer, original, alpha);                                                                     \
    }                                                                                                                        \
    Result Type::compose(const Frame& frame, std::span<XrCompositionLayerBaseHeader* const> layers) {                        \
        std::scoped_lock lock{m_mutex};                                                                                      \
        return m_impl->compose(frame, layers);                                                                               \
    }                                                                                                                        \
    Status Type::status() const {                                                                                            \
        std::scoped_lock lock{m_mutex};                                                                                      \
        return m_impl->status;                                                                                               \
    }                                                                                                                        \
    void Type::reject_submission() {                                                                                         \
        std::scoped_lock lock{m_mutex};                                                                                      \
        m_impl->rejected_request = m_impl->request;                                                                          \
        m_impl->status = Status::failed;                                                                                     \
    }                                                                                                                        \
    bool Type::reset() {                                                                                                     \
        std::scoped_lock lock{m_mutex};                                                                                      \
        return m_impl->reset();                                                                                              \
    }

UEVR_UI_COMPOSITION_IMPL(D3D11)
UEVR_UI_COMPOSITION_IMPL(D3D12)
#undef UEVR_UI_COMPOSITION_IMPL

void D3D11::capture(
    bool framework, const Source& source, ID3D11Device* device, std::span<ID3D11Texture2D* const> textures, uint32_t index) {
    std::scoped_lock lock{m_mutex};
    m_impl->capture(framework, source, device, nullptr, textures, index);
}
void D3D12::capture(bool framework, const Source& source, ID3D12Device* device, ID3D12CommandQueue* queue,
    std::span<ID3D12Resource* const> textures, uint32_t index) {
    std::scoped_lock lock{m_mutex};
    m_impl->capture(framework, source, device, queue, textures, index);
}
} // namespace uevr::ui_composition
