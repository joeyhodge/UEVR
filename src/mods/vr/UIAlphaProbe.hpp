#pragma once

#include "UIAlphaGPU.hpp"
#include "UIAlphaPolicy.hpp"
#include <optional>

namespace uevr::ui_alpha {

// Bounded 64x64 point samples. Polling never waits for the GPU on the frame path.
class Probe11 {
public:
    Probe11();
    ~Probe11();
    bool initialize(ID3D11Device*, std::span<ID3D11Texture2D* const>);
    bool enqueue(uint32_t source);
    std::optional<Sample> poll();
    bool retired(bool wait = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class Probe12 {
public:
    Probe12();
    ~Probe12();
    bool initialize(ID3D12Device*, ID3D12CommandQueue*, std::span<ID3D12Resource* const>);
    bool enqueue(uint32_t source);
    std::optional<Sample> poll();
    bool retired(bool wait = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
