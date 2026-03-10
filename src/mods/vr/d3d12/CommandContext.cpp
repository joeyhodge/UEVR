#include <spdlog/spdlog.h>
#include <utility/String.hpp>
#include <utility/Module.hpp>
#include <cstdlib>
#include <algorithm>
#include <cctype>

#include "Framework.hpp"
#include "../../../utility/Logging.hpp"

#include "TextureContext.hpp"
#include "CommandContext.hpp"

namespace d3d12 {
static bool is_tq2_exe() {
    static const bool is_tq2 = []() {
        const auto module_path = utility::get_module_path(utility::get_executable());
        if (!module_path.has_value()) {
            return false;
        }

        auto lower_path = *module_path;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });

        return lower_path.find("tq2-win64-shipping.exe") != std::string::npos ||
               lower_path.find("tq2_win64_shipping.exe") != std::string::npos;
    }();

    return is_tq2;
}

static bool should_skip_transition_barriers(const std::wstring& ctx_name) {
    // Keep this opt-in for diagnostics/recovery only.
    // Default behavior keeps explicit transitions enabled for correctness.
    static const bool force_skip = []() {
        const auto env = std::getenv("UEVR_TQ2_SKIP_OXR_BARRIERS");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();

    if (!force_skip) {
        return false;
    }

    if (!is_tq2_exe()) {
        return false;
    }

    return ctx_name.find(L"OpenXR commands") != std::wstring::npos;
}

static bool is_trace_enabled(const std::wstring& ctx_name) {
    static const bool env_enabled = []() {
        const auto env = std::getenv("UEVR_TRACE_D3D12_CONTEXT");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();

    return env_enabled;
}

static std::string to_narrow_name(const std::wstring& value) {
    return utility::narrow(value);
}

static void log_device_removed_reason(const wchar_t* where_tag) {
    if (g_framework == nullptr || g_framework->get_d3d12_hook() == nullptr) {
        return;
    }

    auto* const device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return;
    }

    const auto reason = device->GetDeviceRemovedReason();
    if (FAILED(reason)) {
        spdlog::error("[VR] {} device removed reason {:#x}", utility::narrow(where_tag), (uint32_t)reason);
    }
}

static bool try_get_desc_nothrow(ID3D12Resource* resource, D3D12_RESOURCE_DESC& out_desc) noexcept {
    if (resource == nullptr) {
        return false;
    }

    __try {
        out_desc = resource->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
        log_device_removed_reason(L"CreateCommandAllocator");
        return false;
    }

    this->cmd_allocator->SetName(name);

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, this->cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&this->cmd_list)))) {
        spdlog::error("[VR] Failed to create command list for {}", utility::narrow(name));
        log_device_removed_reason(L"CreateCommandList");
        return false;
    }
    
    this->cmd_list->SetName(name);

    if (FAILED(device->CreateFence(this->fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&this->fence)))) {
        spdlog::error("[VR] Failed to create fence for {}", utility::narrow(name));
        log_device_removed_reason(L"CreateFence");
        return false;
    }

    this->fence->SetName(name);
    this->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

void CommandContext::reset() {
    std::scoped_lock _{this->mtx};
    const auto trace_enabled = is_trace_enabled(this->internal_name);

    if (trace_enabled) {
        SPDLOG_INFO("[VR][CtxTrace] reset begin '{}' waiting={} has_commands={} fence={} submits={} waits={}",
            to_narrow_name(this->internal_name), this->waiting_for_fence, this->has_commands, this->fence_value, this->submit_count, this->wait_count);
    }

    this->wait(2000);
    //this->on_post_present(VR::get().get());

    this->cmd_allocator.Reset();
    this->cmd_list.Reset();
    this->fence.Reset();
    this->fence_value = 0;
    CloseHandle(this->fence_event);
    this->fence_event = 0;
    this->waiting_for_fence = false;

    if (trace_enabled) {
        SPDLOG_INFO("[VR][CtxTrace] reset end '{}' close_failures={} recover_failures={} wait_timeouts={}",
            to_narrow_name(this->internal_name), this->close_failure_count, this->recover_failure_count, this->wait_timeout_count);
    }
}

void CommandContext::wait(uint32_t ms) {
    std::scoped_lock _{this->mtx};
    const auto trace_enabled = is_trace_enabled(this->internal_name);

    if (this->fence_event && this->waiting_for_fence) {
        ++this->wait_count;

        if (trace_enabled && ((this->wait_count % 120) == 1 || ms >= 1000)) {
            SPDLOG_INFO("[VR][CtxTrace] wait begin '{}' count={} fence={} ms={}",
                to_narrow_name(this->internal_name), this->wait_count, this->fence_value, ms);
        }

        const auto wait_result = WaitForSingleObject(this->fence_event, ms);
        this->last_wait_result = wait_result;

        if (wait_result == WAIT_TIMEOUT) {
            ++this->wait_timeout_count;
            if (trace_enabled) {
                SPDLOG_WARN("[VR][CtxTrace] wait timeout '{}' count={} timeouts={} fence={}",
                    to_narrow_name(this->internal_name), this->wait_count, this->wait_timeout_count, this->fence_value);
            }
            return;
        }

        if (wait_result != WAIT_OBJECT_0) {
            spdlog::error("[VR] WaitForSingleObject failed for {} ({:#x})", utility::narrow(this->internal_name), (uint32_t)wait_result);
            return;
        }

        ResetEvent(this->fence_event);
        this->waiting_for_fence = false;

        if (FAILED(this->cmd_allocator->Reset())) {
            spdlog::error("[VR] Failed to reset command allocator for {}", utility::narrow(this->internal_name));
            return;
        }

        if (FAILED(this->cmd_list->Reset(this->cmd_allocator.Get(), nullptr))) {
            spdlog::error("[VR] Failed to reset command list for {}", utility::narrow(this->internal_name));
            return;
        }

        this->has_commands = false;

        if (trace_enabled && ((this->wait_count % 120) == 1 || ms >= 1000)) {
            SPDLOG_INFO("[VR][CtxTrace] wait complete '{}' count={} fence={}",
                to_narrow_name(this->internal_name), this->wait_count, this->fence_value);
        }
    }
}

void CommandContext::copy(ID3D12Resource* src, ID3D12Resource* dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};
    const auto skip_barriers = should_skip_transition_barriers(this->internal_name);

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy");
        return;
    }

    if (src == dst) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] copy skipped: source and destination are identical ({})", utility::narrow(this->internal_name));
        return;
    }

    D3D12_RESOURCE_DESC src_desc{};
    D3D12_RESOURCE_DESC dst_desc{};

    if (!try_get_desc_nothrow(src, src_desc) || !try_get_desc_nothrow(dst, dst_desc)) {
        SPDLOG_ERROR_EVERY_N_SEC(1,
            "[VR] copy skipped: failed to query resource desc safely ({}) src={:x} dst={:x}",
            utility::narrow(this->internal_name),
            (uintptr_t)src,
            (uintptr_t)dst);
        return;
    }

    const bool desc_mismatch =
        src_desc.Dimension != dst_desc.Dimension ||
        src_desc.Width != dst_desc.Width ||
        src_desc.Height != dst_desc.Height ||
        src_desc.DepthOrArraySize != dst_desc.DepthOrArraySize ||
        src_desc.MipLevels != dst_desc.MipLevels ||
        src_desc.Format != dst_desc.Format ||
        src_desc.SampleDesc.Count != dst_desc.SampleDesc.Count ||
        src_desc.SampleDesc.Quality != dst_desc.SampleDesc.Quality;

    if (desc_mismatch) {
        SPDLOG_ERROR_EVERY_N_SEC(1,
            "[VR] copy skipped: incompatible resources ({}) src {}x{} fmt {} sample {} -> dst {}x{} fmt {} sample {}",
            utility::narrow(this->internal_name),
            src_desc.Width,
            src_desc.Height,
            (uint32_t)src_desc.Format,
            src_desc.SampleDesc.Count,
            dst_desc.Width,
            dst_desc.Height,
            (uint32_t)dst_desc.Format,
            dst_desc.SampleDesc.Count);
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

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    this->cmd_list->CopyResource(dst, src);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};
    const auto skip_barriers = should_skip_transition_barriers(this->internal_name);

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region");
        return;
    }

    if (src == dst) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] copy_region skipped: source and destination are identical ({})", utility::narrow(this->internal_name));
        return;
    }

    D3D12_RESOURCE_DESC src_desc{};
    D3D12_RESOURCE_DESC dst_desc{};
    if (!try_get_desc_nothrow(src, src_desc) || !try_get_desc_nothrow(dst, dst_desc)) {
        SPDLOG_ERROR_EVERY_N_SEC(1,
            "[VR] copy_region skipped: failed to query resource desc safely ({}) src={:x} dst={:x}",
            utility::narrow(this->internal_name),
            (uintptr_t)src,
            (uintptr_t)dst);
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

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
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

    this->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, src_box);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, UINT dst_x, UINT dst_y, UINT dst_z, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};
    const auto skip_barriers = should_skip_transition_barriers(this->internal_name);

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region");
        return;
    }

    if (src == dst) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] copy_region skipped: source and destination are identical ({})", utility::narrow(this->internal_name));
        return;
    }

    D3D12_RESOURCE_DESC src_desc{};
    D3D12_RESOURCE_DESC dst_desc{};
    if (!try_get_desc_nothrow(src, src_desc) || !try_get_desc_nothrow(dst, dst_desc)) {
        SPDLOG_ERROR_EVERY_N_SEC(1,
            "[VR] copy_region skipped: failed to query resource desc safely ({}) src={:x} dst={:x}",
            utility::narrow(this->internal_name),
            (uintptr_t)src,
            (uintptr_t)dst);
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

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
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

    if (!skip_barriers) {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
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
    const auto skip_barriers = should_skip_transition_barriers(this->internal_name);

    if (srcleft == nullptr || srcright == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region_stereo");
        return;
    }

    if (srcleft == dst || srcright == dst) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] copy_region_stereo skipped: source aliases destination ({})", utility::narrow(this->internal_name));
        return;
    }

    // Transition states to copy source / dest.
    D3D12_RESOURCE_BARRIER barriers[3]
    {
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcleft, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcright, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, dst_state, D3D12_RESOURCE_STATE_COPY_DEST} }
    };
    
    if (!skip_barriers) {
        this->cmd_list->ResourceBarrier(3, barriers);
    }

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

    if (!skip_barriers) {
        this->cmd_list->ResourceBarrier(3, barriers);
    }

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
        this->cmd_list->ResourceBarrier(1, barriers);
    }

    // Clear the resource.
    this->cmd_list->ClearRenderTargetView(rtv, color, 0, nullptr);

    // Switch back to present.
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dst_barrier.Transition.StateAfter = dst_state;

    if (dst_state != dst_barrier.Transition.StateBefore) {
        D3D12_RESOURCE_BARRIER barriers[1]{dst_barrier};
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
    const auto trace_enabled = is_trace_enabled(this->internal_name);

    if (this->has_commands) {
        if (trace_enabled) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR][CtxTrace] execute begin '{}' submits={} waiting={} has_commands={} fence={}",
                to_narrow_name(this->internal_name), this->submit_count, this->waiting_for_fence, this->has_commands, this->fence_value);
        }

        const auto close_result = this->cmd_list->Close();

        if (FAILED(close_result)) {
            ++this->close_failure_count;
            spdlog::error("[VR] Failed to close command list. ({} {:#x})", utility::narrow(this->internal_name), (uint32_t)close_result);
            log_device_removed_reason(L"CommandContext::execute Close");

            // Recover this context so one failed close doesn't permanently stall all subsequent frames.
            // When Close fails, the list may still be in recording state. Release/recreate list+allocator.
            auto* device = g_framework->get_d3d12_hook()->get_device();
            this->cmd_list.Reset();

            bool recovered = true;
            if (this->cmd_allocator != nullptr && FAILED(this->cmd_allocator->Reset())) {
                this->cmd_allocator.Reset();
            }

            if (this->cmd_allocator == nullptr &&
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&this->cmd_allocator))))
            {
                recovered = false;
            }

            if (recovered &&
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, this->cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&this->cmd_list))))
            {
                recovered = false;
            }

            if (recovered && this->cmd_allocator != nullptr) {
                this->cmd_allocator->SetName(this->internal_name.c_str());
            }

            if (recovered && this->cmd_list != nullptr) {
                this->cmd_list->SetName(this->internal_name.c_str());
            }

            if (!recovered) {
                ++this->recover_failure_count;
                spdlog::error("[VR] Failed to recreate command context resources for {}", utility::narrow(this->internal_name));
            }

            if (trace_enabled) {
                SPDLOG_ERROR("[VR][CtxTrace] execute close-failure '{}' close_failures={} recover_failures={} waits={} timeouts={} fence={} waiting={}",
                    to_narrow_name(this->internal_name), this->close_failure_count, this->recover_failure_count, this->wait_count, this->wait_timeout_count, this->fence_value, this->waiting_for_fence);
            }

            this->waiting_for_fence = false;
            this->has_commands = false;
            return;
        }

        auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
        ID3D12CommandList* const cmd_lists[] = {this->cmd_list.Get()};
        command_queue->ExecuteCommandLists(1, cmd_lists);
        if (FAILED(command_queue->Signal(this->fence.Get(), ++this->fence_value))) {
            spdlog::error("[VR] Failed to signal command queue fence for {}", utility::narrow(this->internal_name));
            log_device_removed_reason(L"CommandContext::execute Signal");
            this->waiting_for_fence = false;
            this->has_commands = false;
            return;
        }

        if (FAILED(this->fence->SetEventOnCompletion(this->fence_value, this->fence_event))) {
            spdlog::error("[VR] Failed to set fence completion event for {}", utility::narrow(this->internal_name));
            log_device_removed_reason(L"CommandContext::execute SetEventOnCompletion");
            this->waiting_for_fence = false;
            this->has_commands = false;
            return;
        }

        this->last_signaled_fence = this->fence_value;
        this->waiting_for_fence = true;
        this->has_commands = false;
        ++this->submit_count;

        if (trace_enabled) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR][CtxTrace] execute submit '{}' submits={} fence={} waiting={} has_commands={}",
                to_narrow_name(this->internal_name), this->submit_count, this->fence_value, this->waiting_for_fence, this->has_commands);
        }
    }
}
}
