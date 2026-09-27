#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <span>

namespace uevr::ui_alpha {

enum class PixelOperation : uint8_t { point_copy = 1, straight_to_premultiplied, encoded_premultiplied_to_linear };

// Dedicated resources, never engine textures or the scene compositor's resources.
class GPU11 {
public:
    GPU11();
    ~GPU11();
    bool initialize(ID3D11Device*, std::span<ID3D11Texture2D* const>, std::span<ID3D11Texture2D* const>,
        PixelOperation);
    bool draw(uint32_t source, uint32_t target);
    bool available(uint32_t target) const;
    bool retired(bool wait = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class GPU12 {
public:
    GPU12();
    ~GPU12();
    bool initialize(ID3D12Device*, ID3D12CommandQueue*, std::span<ID3D12Resource* const>, std::span<ID3D12Resource* const>,
        PixelOperation);
    bool draw(uint32_t source, uint32_t target);
    bool available(uint32_t target) const;
    bool retired(bool wait = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
