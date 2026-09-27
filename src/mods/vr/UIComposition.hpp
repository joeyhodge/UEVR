#pragma once
#include "UICompositionPolicy.hpp"
#include <memory>
#include <mutex>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;

namespace uevr::ui_composition {
struct Source {
    XrSession session{};
    XrSwapchain swapchain{};
    Extent extent{};
    ui_alpha::Mode alpha{};
    bool operator==(const Source&) const = default;
};
struct Frame {
    XrInstance instance{};
    XrSystemId system{};
    XrSession session{};
    XrSpace stage{}, view{};
    std::array<XrView, 2> eyes{};
    std::optional<XrPosef> view_in_stage;
    Extent eye_extent{};
    uint64_t request{};
};
struct Result {
    std::array<XrCompositionLayerBaseHeader*, 2> layers{};
    std::array<std::shared_ptr<void>, 2> leases{};
    uint32_t count{};
    explicit operator bool() const { return count > 0; }
};
class Compositor {
public:
    virtual ~Compositor() = default;
    virtual Result compose(const Frame&, std::span<XrCompositionLayerBaseHeader* const>) = 0;
    virtual Status status() const = 0;
    virtual void reject_submission() = 0;
};

#define UEVR_UI_COMPOSITOR_COMMON                                                                                         \
    void begin_frame(uint64_t request);                                                                                   \
    void invalidate(bool framework);                                                                                      \
    void bind(bool framework, const XrCompositionLayerBaseHeader*, XrSwapchain original, ui_alpha::Mode effective_alpha); \
    Result compose(const Frame&, std::span<XrCompositionLayerBaseHeader* const>) override;                                \
    Status status() const override;                                                                                       \
    void reject_submission() override;                                                                                    \
    bool reset();

class D3D11 final : public Compositor {
public:
    D3D11();
    ~D3D11();
    UEVR_UI_COMPOSITOR_COMMON
    void capture(bool framework, const Source&, ID3D11Device*, std::span<ID3D11Texture2D* const>, uint32_t);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::recursive_mutex m_mutex;
};
class D3D12 final : public Compositor {
public:
    D3D12();
    ~D3D12();
    UEVR_UI_COMPOSITOR_COMMON
    void capture(bool framework, const Source&, ID3D12Device*, ID3D12CommandQueue*, std::span<ID3D12Resource* const>, uint32_t);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::recursive_mutex m_mutex;
};
#undef UEVR_UI_COMPOSITOR_COMMON
} // namespace uevr::ui_composition
