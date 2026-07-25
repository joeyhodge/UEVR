#include <spdlog/spdlog.h>
#include <utility/String.hpp>
#include <atomic>
#include <chrono>
#include <string_view>

#include "Framework.hpp"
#include "render/D3D12Diagnostics.hpp"

#include "TextureContext.hpp"
#include "CommandContext.hpp"

namespace {
constexpr auto FENCE_PROFILER_LOG_INTERVAL = std::chrono::seconds(5);
std::atomic_bool g_fence_profiler_enabled{false};

struct FenceTimingStats {
    uint64_t count{};
    double total_ms{};
    double max_ms{};

    void add(std::chrono::steady_clock::duration duration) {
        const auto ms = std::chrono::duration<double, std::milli>{duration}.count();
        ++count;
        total_ms += ms;
        if (ms > max_ms) {
            max_ms = ms;
        }
    }

    double avg() const {
        return count == 0 ? 0.0 : total_ms / (double)count;
    }

    void reset() {
        count = 0;
        total_ms = 0.0;
        max_ms = 0.0;
    }
};

struct FenceProfilerState {
    std::mutex mtx{};
    std::chrono::steady_clock::time_point last_log{};
    FenceTimingStats wait{};
    FenceTimingStats execute_signal{};
    uint64_t wait_zero_timeout{};
    uint64_t wait_nonzero_timeout{};
    uint64_t wait_infinite{};
    uint64_t long_wait{};
    uint64_t pending_after_execute{};

    void reset() {
        wait.reset();
        execute_signal.reset();
        wait_zero_timeout = 0;
        wait_nonzero_timeout = 0;
        wait_infinite = 0;
        long_wait = 0;
        pending_after_execute = 0;
    }
};

FenceProfilerState& fence_profiler() {
    static FenceProfilerState state{};
    return state;
}

void maybe_log_fence_profiler_locked(FenceProfilerState& state, std::chrono::steady_clock::time_point now) {
    if (state.last_log.time_since_epoch().count() == 0) {
        state.last_log = now;
        return;
    }

    if (now - state.last_log < FENCE_PROFILER_LOG_INTERVAL) {
        return;
    }

    if (state.wait.count == 0 && state.execute_signal.count == 0) {
        state.last_log = now;
        return;
    }

    spdlog::info(
        "[D3D12][fence-profiler] wait avg={:.2f}ms max={:.2f}ms n={} execute_signal avg={:.2f}ms max={:.2f}ms n={} wait_zero_timeout={} wait_nonzero_timeout={} wait_infinite={} long_waits={} pending_after_execute={}",
        state.wait.avg(),
        state.wait.max_ms,
        state.wait.count,
        state.execute_signal.avg(),
        state.execute_signal.max_ms,
        state.execute_signal.count,
        state.wait_zero_timeout,
        state.wait_nonzero_timeout,
        state.wait_infinite,
        state.long_wait,
        state.pending_after_execute
    );

    state.last_log = now;
    state.reset();
}

void record_fence_wait(
    std::chrono::steady_clock::duration duration,
    uint32_t requested_ms,
    DWORD wait_result,
    UINT64 fence_value,
    UINT64 completed_before,
    UINT64 completed_after,
    std::wstring_view name)
{
    const auto now = std::chrono::steady_clock::now();
    const auto duration_ms = std::chrono::duration<double, std::milli>{duration}.count();

    auto& state = fence_profiler();
    std::scoped_lock _{state.mtx};

    state.wait.add(duration);

    if (requested_ms == INFINITE) {
        ++state.wait_infinite;
    }

    if (wait_result == WAIT_TIMEOUT) {
        if (requested_ms == 0) {
            ++state.wait_zero_timeout;
        } else {
            ++state.wait_nonzero_timeout;
        }
    }

    if (duration_ms >= 10.0) {
        ++state.long_wait;
        const auto context_name = utility::narrow(std::wstring{name.begin(), name.end()});
        spdlog::warn(
            "[D3D12][fence-profiler] WaitForSingleObject took {:.2f}ms context={} requested_ms={} result={} fence={} completed_before={} completed_after={}",
            duration_ms,
            context_name,
            requested_ms,
            wait_result,
            fence_value,
            completed_before,
            completed_after
        );
    }

    maybe_log_fence_profiler_locked(state, now);
}

void record_fence_execute_signal(
    std::chrono::steady_clock::duration duration,
    UINT64 fence_value,
    UINT64 completed_after_signal,
    std::wstring_view name)
{
    const auto now = std::chrono::steady_clock::now();
    const auto duration_ms = std::chrono::duration<double, std::milli>{duration}.count();

    auto& state = fence_profiler();
    std::scoped_lock _{state.mtx};

    state.execute_signal.add(duration);

    if (completed_after_signal < fence_value) {
        ++state.pending_after_execute;
    }

    if (duration_ms >= 10.0) {
        const auto context_name = utility::narrow(std::wstring{name.begin(), name.end()});
        spdlog::warn(
            "[D3D12][fence-profiler] ExecuteCommandLists/Signal took {:.2f}ms context={} fence={} completed_after_signal={}",
            duration_ms,
            context_name,
            fence_value,
            completed_after_signal
        );
    }

    maybe_log_fence_profiler_locked(state, now);
}

void record_barriers(std::string_view source, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    render::D3D12Diagnostics::get().record_resource_barriers(source, count, barriers);
}
}

namespace d3d12 {
void set_fence_profiler_enabled(bool enabled) {
    if (g_fence_profiler_enabled.load(std::memory_order_relaxed) == enabled) {
        return;
    }

    if (g_fence_profiler_enabled.exchange(enabled, std::memory_order_relaxed) == enabled) {
        return;
    }

    auto& state = fence_profiler();
    std::scoped_lock _{state.mtx};
    state.last_log = {};
    state.reset();
}

bool is_fence_profiler_enabled() {
    return g_fence_profiler_enabled.load(std::memory_order_relaxed);
}

bool CommandContext::setup(const wchar_t* name) {
    std::scoped_lock _{this->mtx};

    this->internal_name = name;

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    this->cmd_allocator.Reset();
    this->cmd_list.Reset();
    this->fence.Reset();

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&this->cmd_allocator)))) {
        spdlog::error("[VR] Failed to create command allocator for {}", utility::narrow(name));
        return false;
    }

    this->cmd_allocator->SetName(name);

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, this->cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&this->cmd_list)))) {
        spdlog::error("[VR] Failed to create command list for {}", utility::narrow(name));
        return false;
    }
    
    this->cmd_list->SetName(name);

    if (FAILED(device->CreateFence(this->fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&this->fence)))) {
        spdlog::error("[VR] Failed to create fence for {}", utility::narrow(name));
        return false;
    }

    this->fence->SetName(name);
    this->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

void CommandContext::reset() {
    std::scoped_lock _{this->mtx};
    this->wait(2000);
    //this->on_post_present(VR::get().get());

    this->cmd_allocator.Reset();
    this->cmd_list.Reset();
    this->fence.Reset();
    this->fence_value = 0;
    CloseHandle(this->fence_event);
    this->fence_event = 0;
    this->waiting_for_fence = false;
}

void CommandContext::wait(uint32_t ms) {
    std::scoped_lock _{this->mtx};

	if (this->fence_event && this->waiting_for_fence) {
        if (is_fence_profiler_enabled()) {
            const auto completed_before = this->fence != nullptr ? this->fence->GetCompletedValue() : 0;
            const auto wait_start = std::chrono::steady_clock::now();
            const auto wait_result = WaitForSingleObject(this->fence_event, ms);
            const auto wait_duration = std::chrono::steady_clock::now() - wait_start;
            const auto completed_after = this->fence != nullptr ? this->fence->GetCompletedValue() : 0;
            record_fence_wait(wait_duration, ms, wait_result, this->fence_value, completed_before, completed_after, this->internal_name);
        } else {
            WaitForSingleObject(this->fence_event, ms);
        }

        ResetEvent(this->fence_event);
        this->waiting_for_fence = false;
        if (FAILED(this->cmd_allocator->Reset())) {
            spdlog::error("[VR] Failed to reset command allocator for {}", utility::narrow(this->internal_name));
        }

        if (FAILED(this->cmd_list->Reset(this->cmd_allocator.Get(), nullptr))) {
            spdlog::error("[VR] Failed to reset command list for {}", utility::narrow(this->internal_name));
        }
        this->has_commands = false;
    }
}

bool CommandContext::try_wait() {
    std::scoped_lock _{this->mtx};

    if (!this->waiting_for_fence) {
        return this->ready();
    }

    if (this->fence == nullptr || this->cmd_allocator == nullptr || this->cmd_list == nullptr ||
        this->fence->GetCompletedValue() < this->fence_value) {
        return false;
    }

    if (this->fence_event != nullptr) {
        ResetEvent(this->fence_event);
    }

    if (FAILED(this->cmd_allocator->Reset())) {
        spdlog::error("[VR] Failed to reset completed command allocator for {}", utility::narrow(this->internal_name));
        return false;
    }

    if (FAILED(this->cmd_list->Reset(this->cmd_allocator.Get(), nullptr))) {
        spdlog::error("[VR] Failed to reset completed command list for {}", utility::narrow(this->internal_name));
        return false;
    }

    this->waiting_for_fence = false;
    this->has_commands = false;
    return true;
}
void CommandContext::copy(ID3D12Resource* src, ID3D12Resource* dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy/ToCopy", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    this->cmd_list->CopyResource(dst, src);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy/Restore", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    copy_region_to_subresource(src, dst, src_box, 0, src_state, dst_state);
}

void CommandContext::copy_region_to_subresource(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, UINT dst_subresource, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region_to_subresource");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy_region/ToCopy", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = dst_subresource;

    this->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, src_box);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy_region/Restore", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, UINT dst_x, UINT dst_y, UINT dst_z, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy_region_offset/ToCopy", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    this->cmd_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &src_loc, src_box);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        record_barriers("VR::CommandContext::copy_region_offset/Restore", 2, barriers);
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

// More optimal than two copy_region calls.
void CommandContext::copy_region_stereo(ID3D12Resource* srcleft, ID3D12Resource* srcright, ID3D12Resource* dst, D3D12_BOX* srcleft_box, D3D12_BOX* srcright_box,
    UINT dstleft_x, UINT dstleft_y, UINT dstleft_z,
    UINT dstright_x, UINT dstright_y, UINT dstright_z,
    D3D12_RESOURCE_STATES src_state,
    D3D12_RESOURCE_STATES dst_state)
{
    if (srcleft == nullptr || srcright == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region_stereo");
        return;
    }

    // Transition states to copy source / dest.
    D3D12_RESOURCE_BARRIER barriers[3]
    {
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcleft, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcright, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, dst_state, D3D12_RESOURCE_STATE_COPY_DEST} }
    };
    
    record_barriers("VR::CommandContext::copy_region_stereo/ToCopy", 3, barriers);
    this->cmd_list->ResourceBarrier(3, barriers);

    // Copy left half
    D3D12_TEXTURE_COPY_LOCATION src_loc_left = { srcleft, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    D3D12_TEXTURE_COPY_LOCATION dst_loc = { dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };

    this->cmd_list->CopyTextureRegion(&dst_loc, dstleft_x, dstleft_y, dstleft_z, &src_loc_left, srcleft_box);

    // Copy right half
    D3D12_TEXTURE_COPY_LOCATION src_loc_right = { srcright, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };

    this->cmd_list->CopyTextureRegion(&dst_loc, dstright_x, dstright_y, dstright_z, &src_loc_right, srcright_box);

    // Transition states back to original.
    barriers[0] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcleft, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state} };
    barriers[1] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcright, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state} };
    barriers[2] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, dst_state} };

    record_barriers("VR::CommandContext::copy_region_stereo/Restore", 3, barriers);
    this->cmd_list->ResourceBarrier(3, barriers);

    this->has_commands = true;
}

void CommandContext::clear_rtv(ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float* color, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (dst == nullptr) {
        spdlog::error("[VR] nullptr passed to clear_rtv");
        return;
    }

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // No need to switch if we're already in the right state.
    if (dst_state != dst_barrier.Transition.StateAfter) {
        D3D12_RESOURCE_BARRIER barriers[1]{dst_barrier};
        record_barriers("VR::CommandContext::clear_rtv/ToRT", 1, barriers);
        this->cmd_list->ResourceBarrier(1, barriers);
    }

    // Clear the resource.
    this->cmd_list->ClearRenderTargetView(rtv, color, 0, nullptr);

    // Switch back to present.
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dst_barrier.Transition.StateAfter = dst_state;

    if (dst_state != dst_barrier.Transition.StateBefore) {
        D3D12_RESOURCE_BARRIER barriers[1]{dst_barrier};
        record_barriers("VR::CommandContext::clear_rtv/Restore", 1, barriers);
        this->cmd_list->ResourceBarrier(1, barriers);
    }

    this->has_commands = true;
}

void CommandContext::clear_rtv(d3d12::TextureContext& tex, const float* color, D3D12_RESOURCE_STATES dst_state) {
    if (tex.texture == nullptr || tex.rtv_heap == nullptr) {
        return;
    }

    this->clear_rtv(tex.texture.Get(), tex.get_rtv(), color, dst_state);
}

void CommandContext::execute() {
    std::scoped_lock _{this->mtx};
    
    if (this->has_commands) {
        if (FAILED(this->cmd_list->Close())) {
            spdlog::error("[VR] Failed to close command list. ({})", utility::narrow(this->internal_name));
            return;
        }
        
        auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
        ID3D12CommandList* const cmd_lists[] = {this->cmd_list.Get()};
        const auto profile_fence = is_fence_profiler_enabled();
        const auto execute_start = profile_fence
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        command_queue->ExecuteCommandLists(1, cmd_lists);
        command_queue->Signal(this->fence.Get(), ++this->fence_value);
        this->fence->SetEventOnCompletion(this->fence_value, this->fence_event);
        if (profile_fence) {
            record_fence_execute_signal(
                std::chrono::steady_clock::now() - execute_start,
                this->fence_value,
                this->fence != nullptr ? this->fence->GetCompletedValue() : 0,
                this->internal_name
            );
        }
        this->waiting_for_fence = true;
        this->has_commands = false;
    }
}
}
