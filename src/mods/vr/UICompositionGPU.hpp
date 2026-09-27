#pragma once
#include "UICompositionPolicy.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <memory>

namespace uevr::ui_composition {
class GPU11 {
public:
    GPU11();
    ~GPU11();
    bool initialize(ID3D11Device*, std::span<ID3D11Texture2D* const>, std::span<ID3D11Texture2D* const>);
    bool draw(std::span<const Draw>, uint32_t);
    bool available(uint32_t) const;
    bool retired(bool wait = false);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class GPU12 {
public:
    GPU12();
    ~GPU12();
    bool initialize(ID3D12Device*, ID3D12CommandQueue*, std::span<ID3D12Resource* const>, std::span<ID3D12Resource* const>);
    bool draw(std::span<const Draw>, uint32_t);
    bool available(uint32_t) const;
    bool retired(bool wait = false);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace uevr::ui_composition
