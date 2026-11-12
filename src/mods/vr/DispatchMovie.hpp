#pragma once

#include "DispatchMoviePolicy.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <optional>

namespace sdk { class UObject; }
namespace uevr::dispatch_movie {
struct Frame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
    uint64_t epoch{}, sampled_ms{};
    uint32_t width{}, height{};
    uintptr_t rhi{}, resource12{};
};
void update(Scope scope, sdk::UObject* world);
std::optional<Frame> read();
bool current(const Frame& frame);
// Only succeeds on the engine's post-flush Present thread, on its own queue.
std::optional<D3D12_RESOURCE_STATES> source_state(const Frame&, ID3D12Device*, ID3D12CommandQueue*, IDXGISwapChain*);
void note_submission(bool requested, bool presented);
void shutdown();
const char* status();
}
