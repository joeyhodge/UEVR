#pragma once

#include "UIAlphaPolicy.hpp"
#include <openxr/openxr.h>
#include <memory>
#include <mutex>
#include <span>
#include <optional>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;

namespace uevr::ui_alpha {

struct Request {
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSwapchain source{XR_NULL_HANDLE};
    Extent extent{};
    Mode mode{Mode::unchanged};
    bool operator==(const Request&) const = default;
};

// Publish pixels and flags together, only for the exact full-image UI layer.
bool replace_layer(XrCompositionLayerBaseHeader&, const Request&, XrSwapchain converted);

class D3D11 {
public:
    D3D11();
    ~D3D11();
    void begin_frame();
    Status copy(const Request&, ID3D11Device*, std::span<ID3D11Texture2D* const>, uint32_t);
    std::shared_ptr<void> apply(XrCompositionLayerBaseHeader&, Mode expected);
    std::optional<Sample> sample();
    bool reset();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::recursive_mutex m_mutex;
};

class D3D12 {
public:
    D3D12();
    ~D3D12();
    void begin_frame();
    Status copy(const Request&, ID3D12Device*, ID3D12CommandQueue*, std::span<ID3D12Resource* const>, uint32_t);
    std::shared_ptr<void> apply(XrCompositionLayerBaseHeader&, Mode expected);
    std::optional<Sample> sample();
    bool reset();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::recursive_mutex m_mutex;
};

}
