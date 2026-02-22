#pragma once

#include <mutex>
#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
struct TextureContext;

struct CommandContext {
    CommandContext() = default;
    virtual ~CommandContext() { this->reset(); }

    bool setup(const wchar_t* name = L"CommandContext object");
    void reset();
    void wait(uint32_t ms);
    void copy(ID3D12Resource* src, ID3D12Resource* dst, 
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, 
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void copy_region(ID3D12Resource* src, ID3D12Resource* dst, 
        D3D12_BOX* src_box, UINT dst_x, UINT dst_y, UINT dst_z,
        D3D12_RESOURCE_STATES src_state, 
        D3D12_RESOURCE_STATES dst_state);
    void copy_region_stereo(ID3D12Resource* srcleft, ID3D12Resource* srcright, ID3D12Resource* dst, D3D12_BOX* srcleft_box, D3D12_BOX* srcright_box,
        UINT dstleft_x, UINT dstleft_y, UINT dstleft_z,
        UINT dstright_x, UINT dstright_y, UINT dstright_z,
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void clear_rtv(ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float* color, 
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void clear_rtv(TextureContext& tex, const float* color, D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void execute();

	bool ready() const {
        return this->cmd_list != nullptr && this->cmd_allocator != nullptr && this->fence != nullptr;
    }

    ComPtr<ID3D12CommandAllocator> cmd_allocator{};
    ComPtr<ID3D12GraphicsCommandList> cmd_list{};
    ComPtr<ID3D12Fence> fence{};
    UINT64 fence_value{};
    HANDLE fence_event{};

    std::recursive_mutex mtx{};

    bool waiting_for_fence{false};
    bool has_commands{false};

    // Runtime state-machine instrumentation counters (useful for UE5.7/TQ2 triage).
    uint64_t submit_count{0};
    uint64_t wait_count{0};
    uint64_t wait_timeout_count{0};
    uint64_t close_failure_count{0};
    uint64_t recover_failure_count{0};
    UINT64 last_signaled_fence{0};
    DWORD last_wait_result{WAIT_OBJECT_0};

    std::wstring internal_name{L"CommandContext object"};
};
}
