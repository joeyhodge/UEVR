#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "mods/vr/UIComposition.hpp"
#include "mods/vr/UICompositionGPU.hpp"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <limits>
#include <vector>
#include <array>
#include <cstdio>
#include <stdexcept>

// Simulated XR ownership, real WARP GPU work. No headset or runtime is loaded.
using Microsoft::WRL::ComPtr;
namespace ui = uevr::ui_composition;
namespace alpha = uevr::ui_alpha;
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

static void require(bool ok, const char* why) {
    if (!ok) {
        throw std::runtime_error(why);
    }
}
static XrCompositionLayerBaseHeader* base(XrCompositionLayerQuad& q) {
    return reinterpret_cast<XrCompositionLayerBaseHeader*>(&q);
}
static ui::Frame frame() {
    ui::Frame f{reinterpret_cast<XrInstance>(uintptr_t{1}), 1, reinterpret_cast<XrSession>(uintptr_t{1}),
        reinterpret_cast<XrSpace>(uintptr_t{1}), reinterpret_cast<XrSpace>(uintptr_t{2})};
    f.eye_extent = {64, 64};
    f.request = 1;
    for (int i = 0; i < 2; ++i) {
        f.eyes[i] = {XR_TYPE_VIEW};
        f.eyes[i].pose = {{0, 0, 0, 1}, {i ? .032f : -.032f, 0, 0}};
        f.eyes[i].fov = {-.785398163f, .785398163f, .785398163f, -.785398163f};
    }
    f.view_in_stage = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    return f;
}
static XrCompositionLayerQuad quad(XrSwapchain handle = reinterpret_cast<XrSwapchain>(uintptr_t{1})) {
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage = {handle, {{0, 0}, {16, 8}}, 0};
    q.size = {2, 1};
    q.pose = {{0, 0, 0, 1}, {0, 0, -2}};
    q.space = frame().stage;
    return q;
}
static void policies() {
    expect(ui::enabled_config("1"), "only explicit opt-in");
    for (auto v : {"0", "-1", "2", "1.0", " 1", "true", "1junk", ""}) {
        expect(!ui::enabled_config(v), "invalid configuration defaults off");
    }
    expect(ui::eligible(true, true, false, false, false) && ui::eligible(true, false, true, false, false), "Mono/DIBR eligible");
    expect(!ui::eligible(true, false, false, false, false) && !ui::eligible(false, true, false, false, false) &&
               !ui::eligible(true, true, false, true, false) && !ui::eligible(true, true, false, false, true),
        "Native/AFR/OpenVR/transitions/2D unaffected");
    expect(ui::budget({1920, 1920}, 3) && !ui::budget({4096, 8192}, 3) && !ui::budget({UINT32_MAX, 1}, 3) && !ui::budget({1, 0}, 1) &&
               !ui::budget({1, 1}, 17),
        "bounded packed per-eye memory");
    const auto f = frame();
    auto q = quad();
    const auto l = ui::project_quad(q, f.eyes[0], 0), r = ui::project_quad(q, f.eyes[1], 1);
    expect(
        l && r && l->corners[0][0] / l->corners[0][3] > r->corners[0][0] / r->corners[0][3], "true eye parallax, not centered Mono scene");
    expect(l && std::abs(l->corners[0][1] / l->corners[0][3] - .25f) < 1e-5, "quad physical height/position preserved");
    q.pose.orientation = {0, 1, 0, 0};
    const auto back = ui::project_quad(q, f.eyes[0], 0);
    expect(back && back->corners[0][3] == 0, "backface not drawn");
    q = quad();
    q.pose.orientation.w = 0;
    expect(!ui::project_quad(q, f.eyes[0], 0), "invalid orientation refused");
    q = quad();
    q.size.width = std::numeric_limits<float>::infinity();
    expect(!ui::project_quad(q, f.eyes[0], 0), "nonfinite geometry refused");
    auto e = f.eyes[0];
    e.fov.angleLeft = e.fov.angleRight;
    expect(!ui::project_quad(quad(), e, 0), "singular FOV refused");
    const auto rotated = glm::angleAxis(.5f, glm::vec3{0, 1, 0});
    XrPosef space{{rotated.x, rotated.y, rotated.z, rotated.w}, {1, 2, 3}};
    const auto relative = ui::relative_pose(space, space);
    expect(std::abs(relative.position.x) < 1e-6 && std::abs(relative.orientation.w - 1) < 1e-6, "head-locked space transform");
}

template <class Helper, class Capture> void lifecycle(Helper& h, Capture&& capture) {
    auto f = frame();
    auto q = quad(), imgui = quad(reinterpret_cast<XrSwapchain>(uintptr_t{2}));
    imgui.space = f.view;
    ui::Source source{f.session, q.subImage.swapchain, {16, 8}, alpha::Mode::unchanged};
    std::array<XrCompositionLayerBaseHeader*, 2> layers{base(q), base(imgui)};
    const auto original = q;
    const auto initial = properties + creations + acquisitions + releases;
    h.begin_frame(0);
    capture(false, source);
    h.bind(false, base(q), source.swapchain, source.alpha);
    expect(!h.compose(f, std::span{layers}.first(1)), "default off cannot replace layer");
    expect(properties + creations + acquisitions + releases == initial, "off makes no XR allocations/calls");
    auto fresh = [&](bool framework = false) {
        auto s = source;
        if (framework) {
            s.swapchain = imgui.subImage.swapchain;
        }
        capture(framework, s);
        h.bind(framework, framework ? base(imgui) : base(q), s.swapchain, s.alpha);
    };
    auto render = [&](size_t n = 1) {
        auto result = h.compose(f, std::span{layers}.first(n));
        for (int i = 0; i < 1000 && !result && h.status() == ui::Status::busy; ++i) {
            Sleep(1);
            fresh();
            if (n == 2)
                fresh(true);
            result = h.compose(f, std::span{layers}.first(n));
        }
        return result;
    };
    // Missing-snapshot checks must not let the GPU retry helper recapture that snapshot.
    auto without_refresh = [&](size_t n = 1) {
        auto result = h.compose(f, std::span{layers}.first(n));
        for (int i = 0; i < 8 && !result; ++i) {
            Sleep(1);
            result = h.compose(f, std::span{layers}.first(n));
        }
        return result;
    };
    h.begin_frame(f.request);
    fresh();
    auto result = render();
    expect(result.count == 1 && h.status() == ui::Status::active, "snapshot produces per-eye UI projection");
    expect(!std::memcmp(&q, &original, sizeof(q)), "original quad including mouse geometry unchanged");
    if (result) {
        const auto& projection = *reinterpret_cast<XrCompositionLayerProjection*>(result.layers[0]);
        expect(projection.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && projection.viewCount == 2 && projection.space == f.stage &&
                   projection.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT && !projection.next,
            "separate transparent stage-locked projection, not scene depth layer");
        expect(projection.views[0].pose.position.x == f.eyes[0].pose.position.x &&
                   projection.views[1].pose.position.x == f.eyes[1].pose.position.x && projection.views[0].next == nullptr &&
                   projection.views[1].next == nullptr && projection.views[1].subImage.imageRect.offset.x == 64,
            "real per-eye transforms, packed disjoint rects, no DIBR depth warp");
    }
    expect(!h.compose(f, std::span{layers}.first(1)), "outstanding CPU submission cannot be mutated");
    result = {};
    h.begin_frame(f.request);
    h.bind(false, base(q), source.swapchain, source.alpha);
    expect(!without_refresh(), "game UI requires a fresh frame snapshot");
    fresh();
    fresh(true);
    result = render(2);
    expect(result.count == 2 && reinterpret_cast<XrCompositionLayerProjection*>(result.layers[1])->space == f.view,
        "game UI and head-locked ImGui retain independent original spaces and ordering");
    result = {};
    h.begin_frame(f.request);
    fresh();
    h.bind(true, base(imgui), imgui.subImage.swapchain, source.alpha);
    result = render(2);
    expect(result.count == 2, "last released static ImGui snapshot remains valid");
    result = {};

    auto unsupported = [&](const char* why) {
        fresh();
        fresh(true);
        expect(!render(2), why);
    };
    imgui.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
    unsupported("eye-specific second layer preserves BOTH original layers");
    imgui.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    imgui.subImage.imageRect.extent.width = 8;
    unsupported("crop fallback");
    imgui.subImage.imageRect.extent.width = 16;
    imgui.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
    unsupported("cylinder fallback without changing geometry");
    imgui.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    imgui.next = &q;
    unsupported("extension-chain fallback");
    imgui.next = nullptr;
    imgui.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    unsupported("straight layer without matching conversion refused");
    imgui.layerFlags = q.layerFlags;
    imgui.space = reinterpret_cast<XrSpace>(uintptr_t{999});
    unsupported("unknown reference space fallback");
    imgui.space = f.view;
    imgui.subImage.imageArrayIndex = 1;
    unsupported("array-slice fallback");
    imgui.subImage.imageArrayIndex = 0;
    f.view_in_stage.reset();
    unsupported("missing head-locked transform fallback");
    f.view_in_stage = frame().view_in_stage;
    h.invalidate(true);
    fresh();
    h.bind(true, base(imgui), imgui.subImage.swapchain, source.alpha);
    expect(!without_refresh(2), "failed original release invalidates snapshot");
    require(h.reset(), "clean reset");

    h.begin_frame(f.request);
    fresh();
    fresh(true);
    result = render(2);
    require(bool(result), "before mode change");
    result = {};
    h.begin_frame(0);
    h.begin_frame(f.request);
    fresh();
    h.bind(true, base(imgui), imgui.subImage.swapchain, source.alpha);
    expect(!without_refresh(2), "returning from another rendering mode cannot reuse stale ImGui");
    fresh(true);
    result = render(2);
    expect(result.count == 2, "mode return waits for new framework copy");
    result = {};
    f.eye_extent = {80, 48};
    h.begin_frame(f.request);
    fresh();
    result = render();
    expect(result && reinterpret_cast<XrCompositionLayerProjection*>(result.layers[0])->views[1].subImage.imageRect.offset.x == 80,
        "resolution change retires previous output and uses a new packed generation");
    result = {};
    auto wrong = f;
    wrong.session = reinterpret_cast<XrSession>(uintptr_t{99});
    expect(!h.compose(wrong, std::span{layers}.first(1)), "old-session image cannot submit");
    wrong = f;
    wrong.request += 2;
    expect(!h.compose(wrong, std::span{layers}.first(1)), "mid-frame toggle cannot submit stale generation");
    f.eye_extent = {64, 64};
    require(h.reset(), "resize reset");

    h.begin_frame(f.request);
    fresh();
    timeout_wait = true;
    expect(!render(), "optional XR timeout falls back");
    const auto acquired = acquisitions;
    expect(!h.compose(f, std::span{layers}.first(1)) && acquisitions == acquired, "no repeated acquisition while wait pending");
    timeout_wait = false;
    result = render();
    expect(bool(result), "timeout recovers on same lease");
    result = {};
    require(h.reset(), "timeout reset");
    h.begin_frame(f.request);
    fresh();
    fail_create = true;
    const auto created = creations;
    expect(!render(), "allocation failure fallback");
    fresh();
    expect(!render() && creations == created + 1, "allocation failure cached without repeated XR calls");
    fail_create = false;
    require(h.reset(), "allocation reset");
    h.begin_frame(f.request);
    fresh();
    fail_release = true;
    expect(!render(), "output release failure never publishes");
    fail_release = false;
    require(h.reset(), "release reset");

    h.begin_frame(f.request);
    fresh();
    result = render();
    expect(bool(result), "restored before rejection");
    result = {};
    h.reject_submission();
    h.begin_frame(f.request);
    fresh();
    expect(!render() && h.status() == ui::Status::failed, "runtime rejection persists without retry loop");
    f.request += 2;
    h.begin_frame(f.request);
    fresh();
    result = render();
    expect(bool(result), "explicit toggle retries generation");
    result = {};
    source.alpha = alpha::Mode::straight_to_premultiplied;
    h.begin_frame(f.request);
    fresh();
    result = render();
    expect(bool(result), "explicit alpha conversion composed exactly once");
    result = {};
    h.bind(false, base(q), source.swapchain, alpha::Mode::unchanged);
    expect(!render(), "alpha failure cannot substitute differently encoded snapshot");
    source.alpha = alpha::Mode::unchanged;
    h.begin_frame(f.request);
    fresh();
    result = render();
    const auto destroyed = destructions;
    expect(!h.reset(), "CPU lease holds XR generation through submission");
    expect(destructions == destroyed, "reset does not destroy live submitted projection");
    result = {};
    expect(destructions > destroyed, "submission lease retires output");
    expect(alive == 0, "optional output swapchains retired");
    h.begin_frame(f.request);
    fresh();
    f.eye_extent = {UINT32_MAX, 1};
    expect(!render(), "invalid output budget fallback");
    require(h.reset(), "final reset");
    f.eye_extent = {64, 64};
    for (int n = 0; n < 8; ++n) {
        f.request += 2;
        h.begin_frame(f.request);
        fresh();
        result = render();
        expect(bool(result), "repeated live option transitions");
        result = {};
        h.begin_frame(f.request + 1);
        expect(!render(), "off immediately restores original layers");
    }
    require(h.reset(), "toggle stress retirement");
    expect(alive == 0, "no leaked optional swapchains on successful transitions");

    f.request += 2;
    h.begin_frame(f.request);
    fresh();
    result = render();
    require(bool(result), "before disabling with outstanding submit lease");
    const auto disabled_creations = creations;
    h.begin_frame(0);
    expect(alive == 1, "disabling cannot destroy an outstanding submission");
    result = {};
    for (int i = 0; i < 1000 && alive; ++i) {
        h.begin_frame(0);
        Sleep(1);
    }
    expect(alive == 0 && creations == disabled_creations, "disabled option retires storage without reallocating");
    require(h.reset(), "disabled cleanup reset");
}

static std::vector<uint8_t> input_pixels() {
    std::vector<uint8_t> p(16 * 8 * 4);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) {
            const auto i = (y * 16 + x) * 4;
            p[i] = uint8_t(x * 9);
            p[i + 1] = uint8_t(y * 15);
            p[i + 2] = uint8_t(15 + x * 5 + y * 2);
            p[i + 3] = uint8_t(x % 3 ? 128 : 255);
        }
    return p;
}
static std::array<ui::Draw, 2> draws(const XrCompositionLayerQuad& q, const ui::Frame& f) {
    return {*ui::project_quad(q, f.eyes[0], 0), *ui::project_quad(q, f.eyes[1], 1)};
}
static void verify_pixels(
    const uint8_t* bytes, size_t pitch, const XrCompositionLayerQuad& q, const ui::Frame& f, const std::vector<uint8_t>& src) {
    bool good = true;
    unsigned visible[2]{};
    for (int eye = 0; eye < 2; ++eye)
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const auto& e = f.eyes[eye];
                const auto inv = glm::inverse(ui::rotation(q.pose));
                const auto o = inv * (ui::position(e.pose) - ui::position(q.pose));
                const auto ray = inv * ui::rotation(e.pose) *
                                 glm::vec3{std::lerp(std::tan(e.fov.angleLeft), std::tan(e.fov.angleRight), (x + .5f) / 64),
                                     std::lerp(std::tan(e.fov.angleUp), std::tan(e.fov.angleDown), (y + .5f) / 64), -1};
                const auto p = o + ray * (-o.z / ray.z);
                const float u = p.x / q.size.width + .5f, v = .5f - p.y / q.size.height;
                if (std::min({std::abs(u), std::abs(v), std::abs(1 - u), std::abs(1 - v)}) < .001f) {
                    continue;
                }
                const bool inside = o.z > 0 && -o.z / ray.z > 0 && u >= 0 && u <= 1 && v >= 0 && v <= 1;
                if (inside) {
                    ++visible[eye];
                }
                for (int c = 0; c < 4; ++c) {
                    float value = 0;
                    if (inside) {
                        const float sx = u * 16 - .5f, sy = v * 8 - .5f;
                        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                        auto sample = [&](int px, int py) {
                            const auto b = src[(std::clamp(py, 0, 7) * 16 + std::clamp(px, 0, 15)) * 4 + c] / 255.f;
                            return c == 3 ? b : alpha::decode_srgb(b);
                        };
                        value = std::lerp(std::lerp(sample(ix, iy), sample(ix + 1, iy), sx - ix),
                            std::lerp(sample(ix, iy + 1), sample(ix + 1, iy + 1), sx - ix), sy - iy);
                        if (c != 3) {
                            value = alpha::encode_srgb(value);
                        }
                    }
                    const int actual = bytes[y * pitch + (eye * 64 + x) * 4 + c], expected = int(std::lround(value * 255));
                    if (std::abs(actual - expected) > 3) {
                        good = false;
                    }
                }
            }
    expect(good, "per-eye pixels match independent ray/plane projection, linear filtering and premultiplied coverage");
    if (q.pose.orientation.w == 1) {
        expect(visible[0] > 0 && visible[1] > 0, "both eyes contain projected UI");
    }
}
static void debug11(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> info;
    device->QueryInterface(IID_PPV_ARGS(&info));
    if (!info)
        return;
    for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        info->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> storage(size);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        info->GetMessage(i, m, &size);
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "DX11: %s\n", m->pDescription);
            expect(false, "DX11 debug validation");
        }
    }
}
static void debug12(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> info;
    device->QueryInterface(IID_PPV_ARGS(&info));
    if (!info)
        return;
    for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        info->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> storage(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        info->GetMessage(i, m, &size);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr, "DX12: %s\n", m->pDescription);
            expect(false, "DX12 debug validation");
        }
    }
}

int main() try {
    policies();
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> ctx;
    auto hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &ctx);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &ctx);
    require(hr, "DX11 WARP");
    device11 = d11.Get();
    const auto pixels = input_pixels();
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 64};
    D3D11_TEXTURE2D_DESC desc{
        16, 8, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> source11, target11, read11;
    require(d11->CreateTexture2D(&desc, &data, &source11), "DX11 source");
    ID3D11Texture2D* inputs11[]{source11.Get()};
    {
        ui::D3D11 helper;
        lifecycle(helper, [&](bool fw, const ui::Source& s) {
            helper.capture(fw, s, d11.Get(), inputs11, 0);
            ctx->Flush(); // Models the original XR image release.
        });
    }
    desc.Width = 128;
    desc.Height = 64;
    require(d11->CreateTexture2D(&desc, nullptr, &target11), "DX11 target");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(d11->CreateTexture2D(&desc, nullptr, &read11), "DX11 readback");
    ID3D11Texture2D* outputs11[]{target11.Get()};
    {
        ui::GPU11 gpu;
        require(gpu.initialize(d11.Get(), inputs11, outputs11), "DX11 projection pipeline");
        auto f = frame();
        auto q = quad();
        for (int test = 0; test < 4; ++test) {
            if (test == 1) {
                q.pose.orientation = {0, .17364818f, 0, .98480775f};
                q.pose.position.x = .2f;
            }
            if (test == 2) {
                f.eyes[0].fov = {-.6f, .8f, .7f, -.9f};
                f.eyes[1].pose.orientation = {0, 0, .08715574f, .9961947f};
            }
            if (test == 3) {
                q.pose.orientation = {0, 1, 0, 0};
            }
            const D3D11_VIEWPORT saved{2, 3, 23, 45, .1f, .8f};
            ctx->RSSetViewports(1, &saved);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            require(gpu.draw(draws(q, f), 0), "DX11 draw");
            require(gpu.retired(true), "DX11 completion");
            D3D11_VIEWPORT after{};
            UINT count = 1;
            ctx->RSGetViewports(&count, &after);
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            ctx->IAGetPrimitiveTopology(&topology);
            expect(
                !std::memcmp(&saved, &after, sizeof(saved)) && topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "DX11 host state restored");
            ctx->CopyResource(read11.Get(), target11.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require(ctx->Map(read11.Get(), 0, D3D11_MAP_READ, 0, &mapped), "DX11 map");
            verify_pixels(static_cast<uint8_t*>(mapped.pData), mapped.RowPitch, q, f, pixels);
            ctx->Unmap(read11.Get(), 0);
        }
        expect(!gpu.draw({}, 0) && !gpu.draw(draws(q, f), 1), "invalid DX11 draw bounds");
    }
    debug11(d11.Get());
    device11 = nullptr;

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> d12;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)), "DX12 WARP");
    device12 = d12.Get();
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    require(d12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "queue");
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> source12, target12, upload, readback;
    require(
        d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source12)),
        "DX12 source");
    rd.Width = 128;
    rd.Height = 64;
    require(d12->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target12)),
        "DX12 target");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total{};
    d12->GetCopyableFootprints(&rd, 0, 1, 0, &layout, nullptr, nullptr, &total);
    rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    require(
        d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
        "DX12 readback");
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    require(
        d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
        "DX12 upload");
    void* mapped{};
    require(upload->Map(0, nullptr, &mapped), "upload map");
    for (int y = 0; y < 8; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped) + y * 256, pixels.data() + y * 64, 64);
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    require(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    require(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    require(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    uint64_t seq{};
    auto finish = [&] {
        require(list->Close(), "close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        require(queue->Signal(fence.Get(), ++seq), "signal");
        for (int i = 0; i < 5000 && fence->GetCompletedValue() < seq; ++i)
            Sleep(1);
        require(fence->GetCompletedValue() >= seq && fence->GetCompletedValue() != UINT64_MAX, "GPU readback retirement");
    };
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    };
    D3D12_TEXTURE_COPY_LOCATION dst{source12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = {0, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 16, 8, 1, 256}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(source12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    finish();
    ID3D12Resource* inputs12[]{source12.Get()};
    ID3D12Resource* outputs12[]{target12.Get()};
    {
        ui::D3D12 helper;
        lifecycle(helper, [&](bool fw, const ui::Source& s) { helper.capture(fw, s, d12.Get(), queue.Get(), inputs12, 0); });
    }
    {
        ui::GPU12 gpu;
        require(gpu.initialize(d12.Get(), queue.Get(), inputs12, outputs12), "DX12 projection pipeline");
        auto f = frame();
        auto q = quad();
        for (int test = 0; test < 4; ++test) {
            if (test == 1) {
                q.pose.orientation = {0, .17364818f, 0, .98480775f};
                q.pose.position.x = .2f;
            }
            if (test == 2) {
                f.eyes[0].fov = {-.6f, .8f, .7f, -.9f};
                f.eyes[1].pose.orientation = {0, 0, .08715574f, .9961947f};
            }
            if (test == 3) {
                q.pose.orientation = {0, 1, 0, 0};
            }
            require(gpu.draw(draws(q, f), 0), "DX12 draw");
            require(gpu.retired(true), "DX12 completion");
            require(allocator->Reset(), "reset allocator");
            require(list->Reset(allocator.Get(), nullptr), "reset list");
            transition(target12.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            dst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = layout;
            src = {target12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(target12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            finish();
            require(readback->Map(0, nullptr, &mapped), "DX12 map");
            verify_pixels(static_cast<uint8_t*>(mapped), layout.Footprint.RowPitch, q, f, pixels);
            readback->Unmap(0, nullptr);
        }
        expect(!gpu.draw({}, 0) && !gpu.draw(draws(q, f), 1), "invalid DX12 draw bounds");
    }
    debug12(d12.Get());
    device12 = nullptr;
    std::printf("UI composition policy, lifecycle and DX11/DX12 pixel tests: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
