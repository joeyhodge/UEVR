#include <d3dcompiler.h>

#include <openvr.h>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>
#include <utility/Module.hpp>
#include <sdk/Utility.hpp>
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <DirectXMath.h>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"

#include "d3d12/DirectXTK.hpp"

#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
// OpenXR runtimes commonly hand back acquired swapchain images in COMMON.
// Treat COMMON as the baseline to avoid relying on undefined pre-state assumptions.
constexpr auto OPENXR_SWAPCHAIN_BASE_STATE = D3D12_RESOURCE_STATE_COMMON;

namespace {
const bool tq2_guard = []() {
    const auto module_path = utility::get_module_path(utility::get_executable());
    if (!module_path.has_value() || module_path->empty()) {
        return false;
    }

    auto lower_path = *module_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });

    return lower_path.find("tq2-win64-shipping.exe") != std::string::npos ||
           lower_path.find("tq2_win64_shipping.exe") != std::string::npos;
}();

const bool ue57_guard = []() {
    const auto version_info = sdk::get_file_version_info();

    auto major = HIWORD(version_info.dwFileVersionMS);
    auto minor = LOWORD(version_info.dwFileVersionMS);

    if (major == 0 && minor >= 10000) {
        major = minor / 10000;
        minor = minor % 10000;
    }

    if (major == 5 && minor == 7) {
        return true;
    }

    if (const auto version = sdk::search_for_version(utility::get_executable()); version) {
        return version->find(L"5.7") != std::wstring::npos;
    }

    return false;
}();

FRHITexture2D* g_last_good_scene_texture = nullptr;

bool is_tq2_diag_enabled() {
    static const bool enabled = []() {
        const auto env = std::getenv("UEVR_TQ2_DIAG");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();

    return enabled;
}

void log_desc_diag_every_sec(const char* tag, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) {
    if (!is_tq2_diag_enabled()) {
        return;
    }

    SPDLOG_INFO_EVERY_N_SEC(
        1,
        "[VR][TQ2_DIAG] {} ptr={:x} {}x{} fmt={} samples={} flags={:#x} state_assumed=caller",
        tag,
        (uintptr_t)resource,
        desc.Width,
        desc.Height,
        (uint32_t)desc.Format,
        desc.SampleDesc.Count,
        (uint32_t)desc.Flags);
}

bool is_d3d_module_path(const std::string& lower_module_path) {
    return lower_module_path.find("d3d12core.dll") != std::string::npos ||
           lower_module_path.find("d3d12.dll") != std::string::npos ||
           lower_module_path.find("d3d11on12.dll") != std::string::npos;
}

bool try_read_ptr_nothrow(uintptr_t address, uintptr_t& out) noexcept {
    __try {
        out = *(uintptr_t*)address;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }

    return true;
}

ID3D12Resource* validate_d3d12_resource_candidate(uintptr_t candidate_ptr);
ID3D12Resource* scan_object_for_d3d12_resource(uintptr_t base, uintptr_t max_offset);
ID3D12Resource* probe_native_d3d12_resource_from_texture_blob(FRHITexture2D* texture);
ID3D12Resource* probe_native_d3d12_resource_via_frhi_base(FRHITexture2D* texture, std::string_view source, bool log_result);

bool is_likely_valid_texture_object(FRHITexture2D* texture) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return false;
    }

    uintptr_t candidate_vtable_ptr{};
    if (!try_read_ptr_nothrow((uintptr_t)texture, candidate_vtable_ptr) || candidate_vtable_ptr == 0) {
        return false;
    }

    const auto candidate_vtable = (void*)candidate_vtable_ptr;
    if (IsBadReadPtr(candidate_vtable, sizeof(void*))) {
        return false;
    }

    uintptr_t first_vfunc_ptr{};
    if (!try_read_ptr_nothrow(candidate_vtable_ptr, first_vfunc_ptr) || first_vfunc_ptr == 0) {
        return false;
    }

    const auto first_vfunc = (void*)first_vfunc_ptr;
    if (IsBadReadPtr(first_vfunc, sizeof(void*))) {
        return false;
    }

    return utility::get_module_within(candidate_vtable).has_value() &&
           utility::get_module_within(first_vfunc).has_value();
}

FRHITexture2D* resolve_texture_object_from_blob(FRHITexture2D* texture, std::string_view source) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return texture;
    }

    if (is_likely_valid_texture_object(texture)) {
        return texture;
    }

    const auto test_candidate = [&](uintptr_t candidate_ptr) -> FRHITexture2D* {
        if (candidate_ptr < 0x10000 || IsBadReadPtr((void*)candidate_ptr, sizeof(void*))) {
            return nullptr;
        }

        auto* candidate = (FRHITexture2D*)candidate_ptr;
        if (is_likely_valid_texture_object(candidate)) {
            return candidate;
        }

        return nullptr;
    };

    // UE5.7/TQ2 frequently exposes wrapper objects that hold TextureRHI-like members
    // around these slots. Prefer these targeted members before the broad blob walk so
    // we do not reintroduce the older "unwrap to random transient object" behavior.
    static constexpr std::array<uintptr_t, 8> kLikelyWrapperTextureOffsets{
        0x48, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88
    };

    const auto blob_base = (uintptr_t)texture;

    for (const auto offset : kLikelyWrapperTextureOffsets) {
        uintptr_t candidate_ptr{};
        if (!try_read_ptr_nothrow(blob_base + offset, candidate_ptr) || candidate_ptr < 0x10000 || candidate_ptr == blob_base) {
            continue;
        }

        if (auto* direct_candidate = test_candidate(candidate_ptr); direct_candidate != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} unwrapped texture object via wrapper member (+0x{:x}) {:x} -> {:x}",
                source,
                offset,
                (uintptr_t)texture,
                (uintptr_t)direct_candidate);
            return direct_candidate;
        }

        if (validate_d3d12_resource_candidate(candidate_ptr) != nullptr || IsBadReadPtr((void*)candidate_ptr, sizeof(void*))) {
            continue;
        }

        auto* wrapper_candidate = (FRHITexture2D*)candidate_ptr;
        if (probe_native_d3d12_resource_via_frhi_base(wrapper_candidate, source, false) != nullptr ||
            scan_object_for_d3d12_resource(candidate_ptr, 0x220) != nullptr)
        {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} accepted texture-like wrapper member (+0x{:x}) {:x} -> {:x}",
                source,
                offset,
                (uintptr_t)texture,
                candidate_ptr);
            return wrapper_candidate;
        }
    }

    constexpr uintptr_t kMaxBlobScanOffset = 0x180;

    for (uintptr_t offset = 0; offset <= kMaxBlobScanOffset; offset += sizeof(void*)) {
        uintptr_t candidate_ptr{};
        if (!try_read_ptr_nothrow(blob_base + offset, candidate_ptr)) {
            continue;
        }

        if (auto* direct_candidate = test_candidate(candidate_ptr); direct_candidate != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} unwrapped texture object via blob scan (+0x{:x}) {:x} -> {:x}",
                source,
                offset,
                (uintptr_t)texture,
                (uintptr_t)direct_candidate);
            return direct_candidate;
        }

        uintptr_t indirect_ptr{};
        if (!try_read_ptr_nothrow(candidate_ptr, indirect_ptr)) {
            continue;
        }

        if (auto* indirect_candidate = test_candidate(indirect_ptr); indirect_candidate != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} unwrapped texture object via indirect blob scan (+0x{:x}) {:x} -> {:x}",
                source,
                offset,
                (uintptr_t)texture,
                (uintptr_t)indirect_candidate);
            return indirect_candidate;
        }
    }

    return texture;
}

bool has_guarded_scene_native_resource(FRHITexture2D* texture);
FRHITexture2D* normalize_guarded_scene_pointer(FRHITexture2D* texture, std::string_view source);

bool is_likely_double_wide_source(uint32_t source_width, uint32_t expected_double_width) {
    if (source_width == 0 || expected_double_width == 0) {
        return false;
    }

    // Allow small dynamic-resolution variance around the expected stereo width.
    const auto tolerance = expected_double_width / 10;
    const auto min_width = expected_double_width > tolerance ? expected_double_width - tolerance : expected_double_width;

    return source_width + tolerance >= expected_double_width && source_width >= min_width;
}

void* try_read_vtable_nothrow(void* object) noexcept {
    uintptr_t vtable_ptr{};
    if (!try_read_ptr_nothrow((uintptr_t)object, vtable_ptr)) {
        return nullptr;
    }

    return (void*)vtable_ptr;
}

std::pair<uint32_t, uint32_t> get_expected_stereo_extent(
    VR* vr,
    uint32_t fallback_double_width,
    uint32_t fallback_height)
{
    uint32_t expected_width = fallback_double_width;
    uint32_t expected_height = fallback_height;

    if (vr != nullptr) {
        if (auto* openxr = vr->get_openxr_runtime(); openxr != nullptr) {
            if (const auto it = openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE);
                it != openxr->swapchains.end() && it->second.width > 0 && it->second.height > 0)
            {
                return {it->second.width, it->second.height};
            }
        }
    }

    if (vr != nullptr) {
        const auto hmd_width = vr->get_hmd_width();
        const auto hmd_height = vr->get_hmd_height();

        if (hmd_width > 0 && hmd_height > 0) {
            const auto doubled = (uint64_t)hmd_width * 2ULL;
            expected_width = (uint32_t)std::min<uint64_t>(doubled, (uint64_t)std::numeric_limits<uint32_t>::max());
            expected_height = (uint32_t)hmd_height;
        }
    }

    if (expected_width == 0) {
        expected_width = fallback_double_width;
    }

    if (expected_height == 0) {
        expected_height = fallback_height;
    }

    return {expected_width, expected_height};
}

bool is_plausible_scene_source_desc(
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_backbuffer_desc,
    uint32_t expected_stereo_width,
    uint32_t expected_stereo_height)
{
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_desc.Width == 0 || source_desc.Height == 0 ||
        source_desc.Format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    // For TQ2/UE5.7 we frequently see tiny 512x512 resources when the resolver
    // picks an auxiliary texture. Those lead to invalid copy regions and
    // command list close failures. Keep a conservative floor.
    const auto min_width = std::max<uint32_t>(
        1024u,
        std::max<uint32_t>((uint32_t)real_backbuffer_desc.Width / 2u, expected_stereo_width / 4u));
    const auto min_height = std::max<uint32_t>(
        540u,
        std::max<uint32_t>((uint32_t)real_backbuffer_desc.Height / 2u, expected_stereo_height / 4u));

    if (source_desc.Width < min_width || source_desc.Height < min_height) {
        return false;
    }

    // Also clamp absurdly large candidates (e.g. 11k x 11k aux resources) that can
    // pass the minimum checks but break CopyTextureRegion on OpenXR command lists.
    const auto max_width = std::max<uint64_t>(
        (uint64_t)real_backbuffer_desc.Width * 3ULL,
        (uint64_t)std::max<uint32_t>(expected_stereo_width, (uint32_t)real_backbuffer_desc.Width) * 2ULL);
    const auto max_height = std::max<uint64_t>(
        (uint64_t)real_backbuffer_desc.Height * 3ULL,
        (uint64_t)std::max<uint32_t>(expected_stereo_height, (uint32_t)real_backbuffer_desc.Height) * 2ULL);

    if ((uint64_t)source_desc.Width > max_width || (uint64_t)source_desc.Height > max_height) {
        return false;
    }

    // Reject extreme aspect outliers in guarded mode selection.
    const auto width = (uint64_t)source_desc.Width;
    const auto height = (uint64_t)source_desc.Height;
    if (width > height * 6ULL || height > width * 6ULL) {
        return false;
    }

    return true;
}

bool is_probable_desktop_sized_scene_source(
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_backbuffer_desc,
    uint32_t expected_stereo_width,
    uint32_t expected_stereo_height)
{
    if (source_desc.Width == 0 || source_desc.Height == 0 ||
        real_backbuffer_desc.Width == 0 || real_backbuffer_desc.Height == 0)
    {
        return false;
    }

    const bool same_size_as_real_backbuffer =
        source_desc.Width == real_backbuffer_desc.Width &&
        source_desc.Height == real_backbuffer_desc.Height;

    if (!same_size_as_real_backbuffer) {
        return false;
    }

    const bool expected_runtime_is_much_larger =
        expected_stereo_width > ((uint32_t)real_backbuffer_desc.Width + ((uint32_t)real_backbuffer_desc.Width / 2u)) ||
        expected_stereo_height > ((uint32_t)real_backbuffer_desc.Height + ((uint32_t)real_backbuffer_desc.Height / 2u));

    return expected_runtime_is_much_larger;
}

std::optional<RECT> make_aspect_fit_rect(uint32_t src_width, uint32_t src_height, uint32_t dst_width, uint32_t dst_height) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return std::nullopt;
    }

    const auto scale_x = (double)dst_width / (double)src_width;
    const auto scale_y = (double)dst_height / (double)src_height;
    const auto scale = std::min(scale_x, scale_y);

    auto fit_width = (uint32_t)std::lround((double)src_width * scale);
    auto fit_height = (uint32_t)std::lround((double)src_height * scale);

    fit_width = std::max<uint32_t>(1, std::min<uint32_t>(fit_width, dst_width));
    fit_height = std::max<uint32_t>(1, std::min<uint32_t>(fit_height, dst_height));

    const auto offset_x = (dst_width - fit_width) / 2;
    const auto offset_y = (dst_height - fit_height) / 2;

    RECT rect{};
    rect.left = (LONG)offset_x;
    rect.top = (LONG)offset_y;
    rect.right = (LONG)(offset_x + fit_width);
    rect.bottom = (LONG)(offset_y + fit_height);
    return rect;
}

bool try_get_resource_desc_nothrow(ID3D12Resource* resource, D3D12_RESOURCE_DESC& out_desc) noexcept {
    if (resource == nullptr || IsBadReadPtr(resource, sizeof(void*))) {
        return false;
    }

    __try {
        const auto vtable = *(void***)resource;
        if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
            return false;
        }

        out_desc = resource->GetDesc();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return out_desc.Dimension != D3D12_RESOURCE_DIMENSION_UNKNOWN && out_desc.Width > 0 && out_desc.Height > 0;
}

ID3D12Resource* validate_d3d12_resource_candidate(uintptr_t candidate_ptr) {
    if (candidate_ptr == 0 || IsBadReadPtr((void*)candidate_ptr, sizeof(void*))) {
        return nullptr;
    }

    uintptr_t candidate_vtable_ptr{};
    if (!try_read_ptr_nothrow(candidate_ptr, candidate_vtable_ptr)) {
        return nullptr;
    }

    const auto candidate_vtable = (void*)candidate_vtable_ptr;
    if (candidate_vtable == nullptr || IsBadReadPtr(candidate_vtable, sizeof(void*))) {
        return nullptr;
    }

    const auto module_within = utility::get_module_within(candidate_vtable);
    if (!module_within.has_value()) {
        return nullptr;
    }

    const auto module_path = utility::get_module_path(*module_within);
    if (!module_path.has_value()) {
        return nullptr;
    }

    auto lower_path = *module_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });

    if (!is_d3d_module_path(lower_path)) {
        return nullptr;
    }

    auto* as_resource = (ID3D12Resource*)candidate_ptr;
    D3D12_RESOURCE_DESC desc{};
    if (!try_get_resource_desc_nothrow(as_resource, desc)) {
        return nullptr;
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 ||
        desc.Height == 0 ||
        desc.DepthOrArraySize == 0 ||
        desc.MipLevels == 0)
    {
        return nullptr;
    }

    return as_resource;
}

ID3D12Resource* scan_object_for_d3d12_resource(uintptr_t base, uintptr_t max_offset) {
    if (base < 0x10000 || IsBadReadPtr((void*)base, sizeof(void*))) {
        return nullptr;
    }

    for (uintptr_t offset = 0; offset <= max_offset; offset += sizeof(void*)) {
        uintptr_t candidate_ptr{};
        if (!try_read_ptr_nothrow(base + offset, candidate_ptr)) {
            continue;
        }

        if (auto* resource = validate_d3d12_resource_candidate(candidate_ptr); resource != nullptr) {
            return resource;
        }
    }

    return nullptr;
}

ID3D12Resource* probe_native_d3d12_resource_from_texture_blob(FRHITexture2D* texture) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    // Fast path: FRHITexture-like wrappers often hold the resource near +0xD0 on UE5.7.
    constexpr std::array<uintptr_t, 8> likely_offsets{
        0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8
    };

    for (const auto offset : likely_offsets) {
        uintptr_t candidate_ptr{};
        if (!try_read_ptr_nothrow((uintptr_t)texture + offset, candidate_ptr)) {
            continue;
        }

        if (auto* direct = validate_d3d12_resource_candidate(candidate_ptr); direct != nullptr) {
            return direct;
        }

        // Some titles wrap the resource in one extra layer (e.g., TextureRHI-like structs).
        if (candidate_ptr != 0 && !IsBadReadPtr((void*)candidate_ptr, sizeof(void*))) {
            if (auto* nested = scan_object_for_d3d12_resource(candidate_ptr, 0x180); nested != nullptr) {
                return nested;
            }
        }
    }

    // Slate-promoted scene targets in TQ2 are often wrappers that point at a texture-like
    // object rather than storing the native D3D12 resource directly near +0xD0.
    static constexpr std::array<uintptr_t, 8> kLikelyWrapperTextureOffsets{
        0x48, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88
    };

    for (const auto offset : kLikelyWrapperTextureOffsets) {
        uintptr_t candidate_ptr{};
        if (!try_read_ptr_nothrow((uintptr_t)texture + offset, candidate_ptr) ||
            candidate_ptr < 0x10000 ||
            candidate_ptr == (uintptr_t)texture ||
            IsBadReadPtr((void*)candidate_ptr, sizeof(void*)))
        {
            continue;
        }

        if (auto* direct = validate_d3d12_resource_candidate(candidate_ptr); direct != nullptr) {
            return direct;
        }

        auto* wrapper_candidate = (FRHITexture2D*)candidate_ptr;
        if (auto* via_frhi_base = probe_native_d3d12_resource_via_frhi_base(wrapper_candidate, "scene render target wrapper member", false);
            via_frhi_base != nullptr)
        {
            return via_frhi_base;
        }

        if (auto* nested = scan_object_for_d3d12_resource(candidate_ptr, 0x220); nested != nullptr) {
            return nested;
        }
    }

    if (auto* broad_direct = scan_object_for_d3d12_resource((uintptr_t)texture, 0x220); broad_direct != nullptr) {
        return broad_direct;
    }

    return nullptr;
}

ID3D12Resource* probe_native_d3d12_resource_via_frhi_base(FRHITexture2D* texture, std::string_view source, bool log_result = true) {
    if (!(tq2_guard || ue57_guard) || texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    void* native_resource = nullptr;
    __try {
        native_resource = reinterpret_cast<FRHITexture*>(texture)->get_native_resource();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (log_result) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] {} FRHITexture base fallback raised SEH exception", source);
        }
        return nullptr;
    }

    if (native_resource == nullptr) {
        return nullptr;
    }

    auto* validated_native_resource = validate_d3d12_resource_candidate((uintptr_t)native_resource);
    if (validated_native_resource == nullptr) {
        if (log_result) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} FRHITexture base fallback returned non-D3D12 candidate {:x}; discarding",
                source,
                (uintptr_t)native_resource);
        }
        return nullptr;
    }

    if (log_result) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} recovered native resource via FRHITexture base fallback: {:x}",
            source,
            (uintptr_t)validated_native_resource);
    }
    return validated_native_resource;
}

ID3D12Resource* safe_get_native_resource(FRHITexture2D* texture, std::string_view source) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    const bool guarded_scene_source = (tq2_guard || ue57_guard) && source == "scene render target";
    bool guarded_invalid_texture = false;

    if (guarded_scene_source) {
        if (auto* normalized = normalize_guarded_scene_pointer(texture, source);
            normalized != nullptr &&
            normalized != texture &&
            !IsBadReadPtr(normalized, sizeof(void*)))
        {
            texture = normalized;
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} guarded path: using normalized scene texture candidate {:x}",
                source,
                (uintptr_t)texture);
        }
    }

    if (guarded_scene_source && !is_likely_valid_texture_object(texture)) {
        if (auto* native_resource = probe_native_d3d12_resource_via_frhi_base(texture, source); native_resource != nullptr) {
            return native_resource;
        }

        if (auto* native_resource = probe_native_d3d12_resource_from_texture_blob(texture); native_resource != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} pointer failed initial validation in guarded UE5.7 mode; recovered native resource via pointer scan",
                source);
            return native_resource;
        }

        auto* unwrapped = resolve_texture_object_from_blob(texture, source);
        if (unwrapped != nullptr && unwrapped != texture && is_likely_valid_texture_object(unwrapped)) {
            texture = unwrapped;
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} pointer failed initial validation in guarded UE5.7 mode; recovered via blob unwrap",
                source);
        } else {
            guarded_invalid_texture = true;
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} pointer failed validation in guarded UE5.7 mode; attempting guarded fallback probes only",
                source);
        }
    }

    if (!guarded_scene_source) {
        texture = resolve_texture_object_from_blob(texture, source);
    }

    void* candidate_vtable{};
    __try {
        candidate_vtable = *(void**)texture;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] {} get_native_resource failed: texture pointer unreadable", source);
        return nullptr;
    }

    auto candidate_vtable_looks_valid = false;
    if (candidate_vtable != nullptr && !IsBadReadPtr(candidate_vtable, sizeof(void*))) {
        const auto first_candidate_vfunc = *(void**)candidate_vtable;
        if (first_candidate_vfunc != nullptr &&
            !IsBadReadPtr(first_candidate_vfunc, sizeof(void*)) &&
            utility::get_module_within((void*)candidate_vtable).has_value() &&
            utility::get_module_within(first_candidate_vfunc).has_value())
        {
            candidate_vtable_looks_valid = true;
        }
    }

    auto known_vtable = FRHITexture2D::get_vtable();
    if (known_vtable == nullptr) {
        if (candidate_vtable_looks_valid) {
            FRHITexture2D::set_vtable(candidate_vtable);
            known_vtable = candidate_vtable;
            SPDLOG_WARN_ONCE(
                "[VR] {} seeded FRHITexture2D vtable from runtime texture candidate {:x}",
                source,
                (uintptr_t)candidate_vtable);
        } else {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] {} get_native_resource skipped: FRHITexture2D vtable is not seeded yet", source);
        return nullptr;
        }
    }

    if (candidate_vtable != known_vtable && !tq2_guard && !ue57_guard) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} get_native_resource skipped: vtable mismatch (candidate {:x}, known {:x})",
            source,
            (uintptr_t)candidate_vtable,
            (uintptr_t)known_vtable);
        return nullptr;
    }

    const bool vtable_matches_known = known_vtable != nullptr && candidate_vtable == known_vtable;
    const bool guarded_mismatched_vtable = guarded_scene_source && known_vtable != nullptr && candidate_vtable != known_vtable;

    if (guarded_mismatched_vtable) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} guarded path: skipping direct get_native_resource call for mismatched vtable (candidate {:x}, known {:x}); using blob probe only",
            source,
            (uintptr_t)candidate_vtable,
            (uintptr_t)known_vtable);
    }

    ID3D12Resource* native_resource{};
    const bool can_attempt_native_resource_call =
        !guarded_invalid_texture &&
        !guarded_mismatched_vtable &&
        vtable_matches_known;

    if (can_attempt_native_resource_call) {
        __try {
            native_resource = (ID3D12Resource*)texture->get_native_resource();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] {} get_native_resource raised SEH exception", source);
            return nullptr;
        }

        if (native_resource != nullptr) {
            const auto validated_native_resource = validate_d3d12_resource_candidate((uintptr_t)native_resource);

            if (validated_native_resource == nullptr) {
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] {} get_native_resource returned non-D3D12 candidate {:x}; discarding",
                    source,
                    (uintptr_t)native_resource);
                native_resource = nullptr;
            } else {
                native_resource = validated_native_resource;
            }
        }
    } else {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} skipping get_native_resource call due invalid texture object/vtable (candidate {:x}, known {:x})",
            source,
            (uintptr_t)candidate_vtable,
            (uintptr_t)known_vtable);
    }

    const bool allow_blob_probe =
        candidate_vtable_looks_valid || candidate_vtable == known_vtable || tq2_guard || ue57_guard || guarded_invalid_texture;

    if (native_resource == nullptr && guarded_scene_source) {
        native_resource = probe_native_d3d12_resource_via_frhi_base(texture, source, false);

        if (native_resource != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} recovered native resource via guarded FRHITexture fallback before pointer scan: {:x}",
                source,
                (uintptr_t)native_resource);
        }
    }

    if (native_resource == nullptr && allow_blob_probe) {
        native_resource = probe_native_d3d12_resource_from_texture_blob(texture);

        if (native_resource != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} recovered native resource via UE5.7 pointer scan: {:x}",
                source,
                (uintptr_t)native_resource);
        }
    }

    if (native_resource == nullptr && !guarded_scene_source) {
        native_resource = probe_native_d3d12_resource_via_frhi_base(texture, source);
    }

    if (native_resource == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} get_native_resource returned null (texture {:x}, vtable {:x}, known {:x}, tq2_guard={})",
            source,
            (uintptr_t)texture,
            (uintptr_t)candidate_vtable,
            (uintptr_t)known_vtable,
            tq2_guard ? 1 : 0);
    }

    return native_resource;
}

template <typename TextureT>
ID3D12Resource* safe_get_native_resource(TextureT* texture, std::string_view source) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    ID3D12Resource* native_resource{};
    __try {
        native_resource = (ID3D12Resource*)texture->get_native_resource();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] {} get_native_resource raised SEH exception", source);
        return nullptr;
    }

    if (native_resource != nullptr) {
        const auto validated_native_resource = validate_d3d12_resource_candidate((uintptr_t)native_resource);

        if (validated_native_resource == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] {} get_native_resource returned non-D3D12 candidate {:x}; discarding",
                source,
                (uintptr_t)native_resource);
            return nullptr;
        }

        return validated_native_resource;
    }

    return nullptr;
}

FRHITexture2D* resolve_scene_render_target_for_d3d12(VR* vr) {
    if (vr == nullptr) {
        return nullptr;
    }

    auto& fake_stereo_hook = vr->get_fake_stereo_hook();
    if (fake_stereo_hook == nullptr) {
        return nullptr;
    }

    auto* rtm = fake_stereo_hook->get_render_target_manager();
    if (rtm == nullptr) {
        return nullptr;
    }

    const auto try_resolve_from_viewport_slots = [&]() -> FRHITexture2D* {
        auto* viewport = rtm->get_viewport();
        if (viewport == nullptr || IsBadReadPtr(viewport, sizeof(void*))) {
            return nullptr;
        }

        static constexpr std::array<uintptr_t, 2> kViewportTextureRefOffsets{
            0x2E0, // preferred scene texture ref path on UE5.7 SceneViewport
            0x8    // fallback texture ref path used by the same accessor
        };

        for (const auto offset : kViewportTextureRefOffsets) {
            const auto ref_addr = (uintptr_t)viewport + offset;
            uintptr_t raw_candidate{};

            if (!try_read_ptr_nothrow(ref_addr, raw_candidate) || raw_candidate < 0x10000) {
                continue;
            }

            auto* candidate = (FRHITexture2D*)raw_candidate;
            auto* resolved = candidate;
            const bool direct_ref_is_texture = is_likely_valid_texture_object(candidate);

            if (!direct_ref_is_texture) {
                resolved = resolve_texture_object_from_blob(resolved, "scene render target viewport slot");
            }

            if (resolved != nullptr && is_likely_valid_texture_object(resolved)) {
                if (direct_ref_is_texture) {
                    rtm->set_render_target_ref((FRHITexture2D**)ref_addr);
                }
                rtm->set_render_target(resolved);
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] Resolved scene render target from viewport slot +0x{:x}: viewport {:x} -> {:x} (direct_ref={})",
                    offset,
                    (uintptr_t)viewport,
                    (uintptr_t)resolved,
                    direct_ref_is_texture ? 1 : 0);

                if (!direct_ref_is_texture) {
                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Viewport slot +0x{:x} is wrapper-style storage; keeping render_target_ref from hook/provider path",
                        offset);
                }
                return resolved;
            }
        }

        return nullptr;
    };

    if (tq2_guard || ue57_guard) {
        auto* texture_ref = rtm->get_render_target_ref();
        FRHITexture2D* resolved_from_ref = nullptr;
        if (texture_ref != nullptr && !IsBadReadPtr(texture_ref, sizeof(void*))) {
            uintptr_t from_ref_raw{};
            if (try_read_ptr_nothrow((uintptr_t)texture_ref, from_ref_raw) && from_ref_raw >= 0x10000) {
                auto* from_ref = (FRHITexture2D*)from_ref_raw;
                resolved_from_ref = normalize_guarded_scene_pointer(from_ref, "scene render target ref");
                if (resolved_from_ref != nullptr && resolved_from_ref == from_ref) {
                    rtm->set_render_target(resolved_from_ref);
                }

                if (resolved_from_ref != nullptr) {
                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Resolved scene render target from viewport texture-ref: ref {:x} -> {:x}",
                        (uintptr_t)texture_ref,
                        (uintptr_t)resolved_from_ref);
                } else {
                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Scene texture-ref present but unresolved this frame: ref {:x} value {:x}",
                        (uintptr_t)texture_ref,
                        from_ref_raw);
                }
            }
        }

        // Prefer the manager's authoritative scene target first, but only if it is not
        // a weaker wrapper-style candidate than the live viewport texture-ref result.
        if (auto* manager_scene = rtm->get_render_target(); manager_scene != nullptr) {
            if (manager_scene != rtm->get_ui_target()) {
                auto* resolved_manager_scene = normalize_guarded_scene_pointer(manager_scene, "scene render target manager");
                if (resolved_manager_scene != nullptr) {
                    const auto known_vtable = FRHITexture2D::get_vtable();
                    void* manager_vtable = nullptr;
                    void* ref_vtable = nullptr;

                    if (known_vtable != nullptr) {
                        if (!IsBadReadPtr(resolved_manager_scene, sizeof(void*))) {
                            manager_vtable = *(void**)resolved_manager_scene;
                        }

                        if (resolved_from_ref != nullptr && !IsBadReadPtr(resolved_from_ref, sizeof(void*))) {
                            ref_vtable = *(void**)resolved_from_ref;
                        }
                    }

                    const bool manager_matches_known = known_vtable != nullptr && manager_vtable == known_vtable;
                    const bool ref_matches_known = known_vtable != nullptr && ref_vtable == known_vtable;

                    if (resolved_from_ref != nullptr &&
                        resolved_from_ref != resolved_manager_scene &&
                        ref_matches_known &&
                        !manager_matches_known)
                    {
                        SPDLOG_INFO_EVERY_N_SEC(1,
                            "[VR] Preferring viewport texture-ref over manager authority: manager {:x} (vt {:x}) -> ref {:x} (vt {:x})",
                            (uintptr_t)resolved_manager_scene,
                            (uintptr_t)manager_vtable,
                            (uintptr_t)resolved_from_ref,
                            (uintptr_t)ref_vtable);
                        return resolved_from_ref;
                    }

                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Resolved scene render target from manager authority: {:x} -> {:x}",
                        (uintptr_t)manager_scene,
                        (uintptr_t)resolved_manager_scene);
                    return resolved_manager_scene;
                }
            } else {
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] Ignoring manager scene target because it matches UI target: {:x}",
                    (uintptr_t)manager_scene);
            }
        }

        if (resolved_from_ref != nullptr) {
            return resolved_from_ref;
        }

        if (auto* from_viewport_slots = try_resolve_from_viewport_slots(); from_viewport_slots != nullptr) {
            return from_viewport_slots;
        }
    }

    if (!(tq2_guard || ue57_guard) && g_last_good_scene_texture != nullptr) {
        auto* cached_texture = g_last_good_scene_texture;
        if (!is_likely_valid_texture_object(cached_texture)) {
            cached_texture = resolve_texture_object_from_blob(cached_texture, "cached scene render target");
        }

        if (cached_texture != nullptr && is_likely_valid_texture_object(cached_texture)) {
            const auto known_vtable = FRHITexture2D::get_vtable();
            if (known_vtable != nullptr) {
                void* cached_vtable{};
                __try {
                    cached_vtable = *(void**)cached_texture;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    cached_vtable = nullptr;
                }

                if (cached_vtable != known_vtable) {
                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Discarding cached scene texture due vtable mismatch (cached {:x}, known {:x})",
                        (uintptr_t)cached_vtable,
                        (uintptr_t)known_vtable);
                    g_last_good_scene_texture = nullptr;
                    cached_texture = nullptr;
                }
            }
        }

        if (cached_texture != nullptr && is_likely_valid_texture_object(cached_texture)) {
            g_last_good_scene_texture = cached_texture;
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Reusing last-good cached scene render target pointer");
            return cached_texture;
        }

        g_last_good_scene_texture = nullptr;
    }

    auto* texture = rtm->get_render_target();

    if (texture == nullptr) {
        auto* relaxed_texture = rtm->get_render_target_relaxed();
        if (relaxed_texture != nullptr) {
            if (tq2_guard || ue57_guard) {
                // In guarded mode, still allow relaxed pointers when they can be
                // validated structurally or directly yield a native resource.
                auto* candidate = normalize_guarded_scene_pointer(relaxed_texture, "relaxed scene render target");
                if (candidate != nullptr) {
                    texture = candidate;
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Using validated relaxed scene render target pointer in guarded UE5.7 mode");
                } else {
                    texture = relaxed_texture;
                    SPDLOG_INFO_EVERY_N_SEC(1,
                        "[VR] Using unvalidated relaxed scene render target pointer in guarded UE5.7 mode (blob resolution path)");
                }
            } else {
                texture = relaxed_texture;
                if (is_likely_valid_texture_object(relaxed_texture)) {
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Using relaxed scene render target pointer");
                } else {
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Using relaxed scene render target pointer (validation failed)");
                }
            }
        }
    }

    if (texture != nullptr) {
        if (tq2_guard || ue57_guard) {
            // Guarded UE5.7/TQ2 path: do not aggressively unwrap scene pointers.
            // In TQ2 this often lands on tiny transient textures (e.g. 64x64) and causes
            // sustained fallback/flicker. Keep the original object when it is already sane.
            if (auto* normalized = normalize_guarded_scene_pointer(texture, "scene render target"); normalized != nullptr) {
                texture = normalized;
            }
        } else {
            texture = resolve_texture_object_from_blob(texture, "scene render target");
        }
    }

    return texture;
}

bool has_guarded_scene_native_resource(FRHITexture2D* texture) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return false;
    }

    return
        probe_native_d3d12_resource_via_frhi_base(texture, "guarded scene candidate", false) != nullptr ||
        probe_native_d3d12_resource_from_texture_blob(texture) != nullptr;
}

FRHITexture2D* normalize_guarded_scene_pointer(FRHITexture2D* texture, std::string_view source) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return nullptr;
    }

    if (is_likely_valid_texture_object(texture)) {
        return texture;
    }

    auto* unwrapped = resolve_texture_object_from_blob(texture, source);
    if (unwrapped == nullptr || IsBadReadPtr(unwrapped, sizeof(void*))) {
        return has_guarded_scene_native_resource(texture) ? texture : nullptr;
    }

    if (unwrapped != texture && is_likely_valid_texture_object(unwrapped)) {
        return unwrapped;
    }

    if (unwrapped != texture && has_guarded_scene_native_resource(unwrapped)) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] {} guarded path: preferring concrete unwrapped scene candidate {:x} -> {:x}",
            source,
            (uintptr_t)texture,
            (uintptr_t)unwrapped);
        return unwrapped;
    }

    return has_guarded_scene_native_resource(texture) ? texture : nullptr;
}

bool is_bgra8_family(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool is_rgba8_family(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool is_r10g10b10a2_family(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT canonical_typed_color_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return fmt;
    }
}

DXGI_FORMAT select_unorm8_copy_format(DXGI_FORMAT source_format) noexcept {
    const auto typed = canonical_typed_color_format(source_format);
    return is_bgra8_family(typed) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
}

DXGI_FORMAT normalize_copy_color_format(DXGI_FORMAT fmt) noexcept {
    // Keep copy intermediates in linear UNORM variants to avoid accidental
    // gamma-domain writes on runtimes that expose sRGB swapchain images.
    switch (canonical_typed_color_format(fmt)) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return canonical_typed_color_format(fmt);
    }
}

DXGI_FORMAT preferred_linear_view_format(DXGI_FORMAT fmt) noexcept {
    const auto typed = canonical_typed_color_format(fmt);

    switch (typed) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return typed;
    }
}

DXGI_FORMAT preferred_srgb_rtv_format(DXGI_FORMAT fmt) noexcept {
    const auto typed = canonical_typed_color_format(fmt);

    switch (typed) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    default:
        return typed;
    }
}

DXGI_FORMAT effective_rtv_format(const d3d12::TextureContext& texture_ctx) noexcept {
    if (texture_ctx.rtv_format != DXGI_FORMAT_UNKNOWN) {
        return canonical_typed_color_format(texture_ctx.rtv_format);
    }

    if (texture_ctx.texture != nullptr) {
        return canonical_typed_color_format(texture_ctx.texture->GetDesc().Format);
    }

    return DXGI_FORMAT_UNKNOWN;
}

bool is_copy_format_compatible(DXGI_FORMAT src, DXGI_FORMAT dst) noexcept {
    if (src == dst) {
        return true;
    }

    if (src == DXGI_FORMAT_UNKNOWN || dst == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    // Some runtimes expose typeless OpenXR images even when a typed format was requested.
    if (is_bgra8_family(src) && is_bgra8_family(dst)) {
        return true;
    }

    if (is_rgba8_family(src) && is_rgba8_family(dst)) {
        return true;
    }

    if (is_r10g10b10a2_family(src) && is_r10g10b10a2_family(dst)) {
        return true;
    }

    return false;
}

bool uses_shader_resource_state(D3D12_RESOURCE_STATES state) noexcept {
    return (state & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) != 0;
}

bool is_depth_or_stencil_like_format(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return true;
    default:
        return false;
    }
}

DirectX::DX12::SpriteBatch* ensure_openxr_blit_batch_for_format(DXGI_FORMAT output_format) {
    static std::mutex s_batch_mtx{};
    static std::unordered_map<uint32_t, std::unique_ptr<DirectX::DX12::SpriteBatch>> s_batches{};

    if (g_framework == nullptr || g_framework->get_d3d12_hook() == nullptr) {
        return nullptr;
    }

    auto* const device = g_framework->get_d3d12_hook()->get_device();
    auto* const command_queue = g_framework->get_d3d12_hook()->get_command_queue();
    if (device == nullptr || command_queue == nullptr) {
        return nullptr;
    }

    const auto typed_output = canonical_typed_color_format(output_format);
    const auto key = (uint32_t)typed_output;

    std::scoped_lock _{s_batch_mtx};
    auto it = s_batches.find(key);
    if (it != s_batches.end() && it->second != nullptr) {
        return it->second.get();
    }

    DirectX::ResourceUploadBatch upload{device};
    upload.Begin();

    DirectX::SpriteBatchPipelineStateDescription pd{
        DirectX::RenderTargetState{typed_output, DXGI_FORMAT_UNKNOWN}
    };

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);
    auto result = upload.End(command_queue);
    result.wait();

    auto* const out = batch.get();
    s_batches[key] = std::move(batch);
    return out;
}

bool try_openxr_blit_to_swapchain(
    uint32_t swapchain_idx,
    uint32_t texture_index,
    ID3D12Device* device,
    d3d12::CommandContext& command_ctx,
    ID3D12Resource* source_resource,
    d3d12::TextureContext& dst_ctx,
    const D3D12_RESOURCE_DESC& src_desc,
    const D3D12_RESOURCE_DESC& dst_desc,
    D3D12_RESOURCE_STATES src_state,
    D3D12_BOX* src_box,
    D3D12_RESOURCE_STATES dst_state)
{
    if (device == nullptr || source_resource == nullptr || dst_ctx.texture.Get() == nullptr) {
        return false;
    }

    if (!uses_shader_resource_state(src_state)) {
        return false;
    }

    if (is_depth_or_stencil_like_format(src_desc.Format) || is_depth_or_stencil_like_format(dst_desc.Format)) {
        return false;
    }

    d3d12::TextureContext src_ctx{};
    src_ctx.texture = source_resource;

    const auto typed_src_format = preferred_linear_view_format(src_desc.Format);
    if (!src_ctx.create_srv(device, typed_src_format)) {
        SPDLOG_WARNING_EVERY_N_SEC(1,
            "[VR] OpenXR blit fallback skipped: failed to create SRV (swapchain {} image {} src_fmt={})",
            swapchain_idx,
            texture_index,
            (uint32_t)typed_src_format);
        return false;
    }

    if (dst_ctx.rtv_heap == nullptr || dst_ctx.rtv_heap->Heap() == nullptr) {
        const auto typed_dst_format = preferred_srgb_rtv_format(dst_desc.Format);
        if (!dst_ctx.create_rtv(device, typed_dst_format)) {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] OpenXR blit fallback skipped: failed to create RTV (swapchain {} image {} dst_fmt={})",
                swapchain_idx,
                texture_index,
                (uint32_t)typed_dst_format);
            return false;
        }
    }

    const auto dst_view_format = effective_rtv_format(dst_ctx);
    auto* const batch = ensure_openxr_blit_batch_for_format(dst_view_format != DXGI_FORMAT_UNKNOWN ? dst_view_format : dst_desc.Format);
    if (batch == nullptr) {
        SPDLOG_WARNING_EVERY_N_SEC(1,
            "[VR] OpenXR blit fallback skipped: no SpriteBatch available (swapchain {} image {} dst_fmt={})",
            swapchain_idx,
            texture_index,
            (uint32_t)(dst_view_format != DXGI_FORMAT_UNKNOWN ? dst_view_format : dst_desc.Format));
        return false;
    }

    const float clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_ctx.clear_rtv(dst_ctx.texture.Get(), dst_ctx.get_rtv(), clear_color, dst_state);

    std::optional<RECT> src_rect{};
    if (src_box != nullptr) {
        src_rect = RECT{
            (LONG)src_box->left,
            (LONG)src_box->top,
            (LONG)src_box->right,
            (LONG)src_box->bottom
        };
    }

    d3d12::render_srv_to_rtv(
        batch,
        command_ctx.cmd_list.Get(),
        src_ctx,
        dst_ctx,
        src_rect,
        src_state,
        dst_state);

    SPDLOG_INFO_EVERY_N_SEC(1,
        "[VR] OpenXR blit fallback used (swapchain {} image {}): src {}x{} fmt {} -> dst {}x{} fmt {}",
        swapchain_idx,
        texture_index,
        src_desc.Width,
        src_desc.Height,
        (uint32_t)src_desc.Format,
        dst_desc.Width,
        dst_desc.Height,
        (uint32_t)(dst_view_format != DXGI_FORMAT_UNKNOWN ? dst_view_format : dst_desc.Format));

    return true;
}
}

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    const bool guarded_57_mode = tq2_guard || ue57_guard;

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    bool using_real_backbuffer_source = false;
    auto ue4_texture = resolve_scene_render_target_for_d3d12(vr);

    if (ue4_texture != nullptr) {
        backbuffer = safe_get_native_resource(ue4_texture, "scene render target");

        if (backbuffer == nullptr && !guarded_57_mode && g_last_good_scene_texture != nullptr && g_last_good_scene_texture != ue4_texture) {
            auto* cached_texture = g_last_good_scene_texture;
            if (cached_texture != nullptr && !IsBadReadPtr(cached_texture, sizeof(void*))) {
                auto cached_backbuffer = safe_get_native_resource(cached_texture, "cached scene render target");
                if (cached_backbuffer != nullptr) {
                    ue4_texture = cached_texture;
                    backbuffer = cached_backbuffer;
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Using last-good cached scene resource after current candidate failed");
                }
            }
        }
    } else {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Scene render target pointer is null");
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
        using_real_backbuffer_source = true;
    }

    if (backbuffer == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Scene render target unavailable; falling back to real back buffer (D3D12)");
        backbuffer = real_backbuffer;
        using_real_backbuffer_source = true;
    }

    if (backbuffer.Get() == real_backbuffer.Get()) {
        using_real_backbuffer_source = true;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    D3D12_RESOURCE_DESC real_backbuffer_desc{};
    if (!try_get_resource_desc_nothrow(real_backbuffer.Get(), real_backbuffer_desc)) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to read real back buffer desc.");
        return vr::VRCompositorError_None;
    }

    D3D12_RESOURCE_DESC backbuffer_desc{};
    if (!try_get_resource_desc_nothrow(backbuffer.Get(), backbuffer_desc)) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] [warn] Scene backbuffer desc read failed; falling back to real back buffer");
        backbuffer = real_backbuffer;
        backbuffer_desc = real_backbuffer_desc;
        using_real_backbuffer_source = true;
    }

    const auto [expected_runtime_width, expected_runtime_height] = get_expected_stereo_extent(
        vr,
        (uint32_t)real_backbuffer_desc.Width * 2u,
        (uint32_t)real_backbuffer_desc.Height);
    const auto expected_double_width_for_frame = guarded_57_mode ? expected_runtime_width : (uint32_t)m_backbuffer_size[0];

    const auto clear_guarded_scene_cache = [&]() {
        m_guarded_last_scene_source.Reset();
        m_guarded_scene_source_tex.reset();
        m_guarded_scene_source_srv_format = DXGI_FORMAT_UNKNOWN;
    };

    const auto is_uevr_managed_scene_candidate = [&](ID3D12Resource* candidate, const char* stage) -> bool {
        if (candidate == nullptr) {
            return false;
        }

        if (const auto swapchain_idx = m_openxr.find_swapchain_index(candidate); swapchain_idx.has_value()) {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases OpenXR swapchain {}",
                stage,
                (uintptr_t)candidate,
                *swapchain_idx);
            return true;
        }

        const auto framework_rt = g_framework != nullptr ? g_framework->get_rendertarget_d3d12().Get() : nullptr;
        if (candidate == framework_rt) {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases the framework render target",
                stage,
                (uintptr_t)candidate);
            return true;
        }

        if (candidate == m_game_tex.texture.Get() ||
            candidate == m_backbuffer_copy.texture.Get() ||
            candidate == m_game_ui_tex.texture.Get() ||
            candidate == m_scene_capture_tex.texture.Get() ||
            candidate == m_openvr.ui_tex.texture.Get())
        {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases a UEVR-managed texture",
                stage,
                (uintptr_t)candidate);
            return true;
        }

        for (const auto& tex : m_2d_screen_tex) {
            if (candidate == tex.texture.Get()) {
                SPDLOG_WARNING_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases a 2D-screen texture",
                    stage,
                    (uintptr_t)candidate);
                return true;
            }
        }

        for (const auto& tex : m_openvr.left_eye_tex) {
            if (candidate == tex.texture.Get()) {
                SPDLOG_WARNING_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases an OpenVR eye texture",
                    stage,
                    (uintptr_t)candidate);
                return true;
            }
        }

        for (const auto& tex : m_openvr.right_eye_tex) {
            if (candidate == tex.texture.Get()) {
                SPDLOG_WARNING_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7: rejecting {} scene candidate {:x}; it aliases an OpenVR eye texture",
                    stage,
                    (uintptr_t)candidate);
                return true;
            }
        }

        return false;
    };

    const auto try_promote_guarded_ref_scene_source = [&]() -> bool {
        if (!guarded_57_mode || using_real_backbuffer_source || backbuffer.Get() == nullptr || backbuffer.Get() == real_backbuffer.Get()) {
            return false;
        }

        if (!is_probable_desktop_sized_scene_source(
                backbuffer_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            return false;
        }

        auto* fake_stereo_hook = vr->get_fake_stereo_hook().get();
        if (fake_stereo_hook == nullptr) {
            return false;
        }

        auto* rtm = fake_stereo_hook->get_render_target_manager();
        if (rtm == nullptr) {
            return false;
        }

        auto* texture_ref = rtm->get_render_target_ref();
        if (texture_ref == nullptr || IsBadReadPtr(texture_ref, sizeof(void*))) {
            return false;
        }

        uintptr_t ref_raw{};
        if (!try_read_ptr_nothrow((uintptr_t)texture_ref, ref_raw) || ref_raw < 0x10000) {
            return false;
        }

        auto* ref_candidate = normalize_guarded_scene_pointer((FRHITexture2D*)ref_raw, "scene render target ref override");
        if (ref_candidate == nullptr || ref_candidate == ue4_texture) {
            return false;
        }

        auto* ref_native = safe_get_native_resource(ref_candidate, "scene render target ref override");
        if (ref_native == nullptr || ref_native == backbuffer.Get() || ref_native == real_backbuffer.Get()) {
            return false;
        }

        if (is_uevr_managed_scene_candidate(ref_native, "texture-ref override")) {
            return false;
        }

        D3D12_RESOURCE_DESC ref_desc{};
        if (!try_get_resource_desc_nothrow(ref_native, ref_desc)) {
            return false;
        }

        if (!is_plausible_scene_source_desc(
                ref_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height) ||
            is_probable_desktop_sized_scene_source(
                ref_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            return false;
        }

        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] Guarded UE5.7: promoting viewport texture-ref scene candidate {:x} over desktop-sized manager source {:x} ({}x{} fmt {} -> {}x{} fmt {})",
            (uintptr_t)ref_native,
            (uintptr_t)backbuffer.Get(),
            backbuffer_desc.Width,
            backbuffer_desc.Height,
            (uint32_t)backbuffer_desc.Format,
            ref_desc.Width,
            ref_desc.Height,
            (uint32_t)ref_desc.Format);

        ue4_texture = ref_candidate;
        backbuffer = ref_native;
        backbuffer_desc = ref_desc;
        using_real_backbuffer_source = false;
        rtm->set_render_target(ref_candidate);
        return true;
    };

    const auto try_reuse_guarded_scene_source = [&]() -> bool {
        if (!guarded_57_mode || m_guarded_last_scene_source.Get() == nullptr) {
            return false;
        }

        auto* const cached_scene = m_guarded_last_scene_source.Get();
        if (cached_scene == real_backbuffer.Get()) {
            clear_guarded_scene_cache();
            return false;
        }

        if (is_uevr_managed_scene_candidate(cached_scene, "cached")) {
            clear_guarded_scene_cache();
            return false;
        }

        D3D12_RESOURCE_DESC cached_desc{};
        if (!try_get_resource_desc_nothrow(cached_scene, cached_desc)) {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: discarding cached scene source {:x}; desc read failed",
                (uintptr_t)cached_scene);
            clear_guarded_scene_cache();
            return false;
        }

        if (!is_plausible_scene_source_desc(
                cached_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: discarding cached scene source {:x}; desc {}x{} fmt {} no longer plausible",
                (uintptr_t)cached_scene,
                cached_desc.Width,
                cached_desc.Height,
                (uint32_t)cached_desc.Format);
            clear_guarded_scene_cache();
            return false;
        }

        if (is_probable_desktop_sized_scene_source(
                cached_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            SPDLOG_WARNING_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: discarding cached scene source {:x}; desc {}x{} matches the desktop backbuffer while expected stereo is {}x{}",
                (uintptr_t)cached_scene,
                cached_desc.Width,
                cached_desc.Height,
                expected_runtime_width,
                expected_runtime_height);
            clear_guarded_scene_cache();
            return false;
        }

        backbuffer = cached_scene;
        backbuffer_desc = cached_desc;
        using_real_backbuffer_source = false;

        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] Guarded UE5.7: reusing cached non-real scene source {:x} ({}x{} fmt {}) instead of real backbuffer",
            (uintptr_t)cached_scene,
            cached_desc.Width,
            cached_desc.Height,
            (uint32_t)cached_desc.Format);
        return true;
    };

    const auto remember_guarded_scene_source = [&]() {
        if (!guarded_57_mode || backbuffer.Get() == nullptr || backbuffer.Get() == real_backbuffer.Get() || using_real_backbuffer_source) {
            return;
        }

        if (is_uevr_managed_scene_candidate(backbuffer.Get(), "active")) {
            return;
        }

        if (!is_plausible_scene_source_desc(
                backbuffer_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            return;
        }

        if (is_probable_desktop_sized_scene_source(
                backbuffer_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: not caching desktop-sized scene source {:x} ({}x{} fmt {}) while expected stereo is {}x{}",
                (uintptr_t)backbuffer.Get(),
                backbuffer_desc.Width,
                backbuffer_desc.Height,
                (uint32_t)backbuffer_desc.Format,
                expected_runtime_width,
                expected_runtime_height);
            return;
        }

        if (m_guarded_last_scene_source.Get() != backbuffer.Get()) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: caching non-real scene source {:x} ({}x{} fmt {})",
                (uintptr_t)backbuffer.Get(),
                backbuffer_desc.Width,
                backbuffer_desc.Height,
                (uint32_t)backbuffer_desc.Format);
            m_guarded_scene_source_tex.reset();
            m_guarded_scene_source_srv_format = DXGI_FORMAT_UNKNOWN;
        }

        m_guarded_last_scene_source = backbuffer;
    };

    if (guarded_57_mode &&
        backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get() &&
        !using_real_backbuffer_source &&
        is_uevr_managed_scene_candidate(backbuffer.Get(), "resolved"))
    {
        backbuffer = real_backbuffer;
        backbuffer_desc = real_backbuffer_desc;
        using_real_backbuffer_source = true;
    }

    if (guarded_57_mode && !using_real_backbuffer_source) {
        try_promote_guarded_ref_scene_source();
    }

    if (guarded_57_mode && using_real_backbuffer_source) {
        try_reuse_guarded_scene_source();
    }

    if (guarded_57_mode && !using_real_backbuffer_source) {
        if (!is_plausible_scene_source_desc(
                backbuffer_desc,
                real_backbuffer_desc,
                expected_runtime_width,
                expected_runtime_height))
        {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] Scene source {}x{} fmt {} rejected as implausible for UE5.7/TQ2; using real backbuffer {}x{} fmt {} (expected stereo {}x{})",
                backbuffer_desc.Width,
                backbuffer_desc.Height,
                (uint32_t)backbuffer_desc.Format,
                real_backbuffer_desc.Width,
                real_backbuffer_desc.Height,
                (uint32_t)real_backbuffer_desc.Format,
                expected_runtime_width,
                expected_runtime_height);
            backbuffer = real_backbuffer;
            backbuffer_desc = real_backbuffer_desc;
            using_real_backbuffer_source = true;
        }
    }

    const bool had_guarded_scene_candidate_pre_latch =
        guarded_57_mode &&
        backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get() &&
        !using_real_backbuffer_source &&
        is_plausible_scene_source_desc(
            backbuffer_desc,
            real_backbuffer_desc,
            expected_runtime_width,
            expected_runtime_height);

    if (guarded_57_mode) {
        // UE5.7/TQ2 can briefly oscillate between stale/invalid scene resources during startup.
        // Release the real-backbuffer latch as soon as a plausible scene source appears.
        static uint32_t s_force_real_frames = 0;
        static uint32_t s_stable_scene_frames = 0;
        constexpr uint32_t kForceRealLatchFrames = 12;
        constexpr uint32_t kMinStableSceneFrames = 3;

        if (!had_guarded_scene_candidate_pre_latch) {
            s_force_real_frames = kForceRealLatchFrames;
            s_stable_scene_frames = 0;
        } else {
            ++s_stable_scene_frames;
            if (s_stable_scene_frames >= kMinStableSceneFrames) {
                s_force_real_frames = 0;
            }
        }

        if (had_guarded_scene_candidate_pre_latch && s_force_real_frames > 0) {
            if (s_stable_scene_frames >= kMinStableSceneFrames) {
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7 source latch released after {} stable scene frames",
                    s_stable_scene_frames);
                s_force_real_frames = 0;
            } else {
                --s_force_real_frames;
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7 source latch: forcing real backbuffer for stabilization ({} frames remaining)",
                    s_force_real_frames);
                backbuffer = real_backbuffer;
                backbuffer_desc = real_backbuffer_desc;
                using_real_backbuffer_source = true;
            }
        }
    }

    if (guarded_57_mode) {
        if (!using_real_backbuffer_source) {
            remember_guarded_scene_source();
        } else if (had_guarded_scene_candidate_pre_latch) {
            SPDLOG_INFO_EVERY_N_SEC(1,
                "[VR] Guarded UE5.7: keeping cached scene candidate warm while latch still forces real backbuffer");
        } else {
            try_reuse_guarded_scene_source();
        }
    }

    if (!using_real_backbuffer_source && !guarded_57_mode && ue4_texture != nullptr && is_likely_valid_texture_object(ue4_texture)) {
        bool cache_candidate = true;
        const auto known_vtable = FRHITexture2D::get_vtable();
        if (known_vtable != nullptr) {
            const auto tex_vtable = try_read_vtable_nothrow(ue4_texture);

            if (tex_vtable != known_vtable) {
                cache_candidate = false;
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] Not caching scene texture with mismatched vtable (candidate {:x}, known {:x})",
                    (uintptr_t)tex_vtable,
                    (uintptr_t)known_vtable);
            }
        }

        if (cache_candidate) {
            g_last_good_scene_texture = ue4_texture;
        }
    }

    m_last_frame_used_real_backbuffer_source = using_real_backbuffer_source;
    if (m_last_frame_used_real_backbuffer_source) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Using real backbuffer source path this frame (scene RT unresolved or compatibility mode)");
    }

    log_desc_diag_every_sec("real_backbuffer", real_backbuffer.Get(), real_backbuffer_desc);
    log_desc_diag_every_sec("active_source_backbuffer", backbuffer.Get(), backbuffer_desc);

    if (!is_likely_double_wide_source((uint32_t)backbuffer_desc.Width, expected_double_width_for_frame)) {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] Using single-wide source texture {}x{} (expected stereo width {})",
            backbuffer_desc.Width,
            backbuffer_desc.Height,
            expected_double_width_for_frame);
    }

    const auto ui_invert_alpha = vr->get_overlay_component().get_ui_invert_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();
    if (runtime->is_openxr()) {
        m_openxr.begin_frame_submission();
    }

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    auto ui_target = ffsr->get_render_target_manager()->get_ui_target();
    const auto scene_target = ue4_texture != nullptr ? ue4_texture : resolve_scene_render_target_for_d3d12(vr);
    const bool scene_rt_unresolved = m_last_frame_used_real_backbuffer_source;
    const bool guarded_fallback_mode = (tq2_guard || ue57_guard) && scene_rt_unresolved;
    const bool tq2_scene_missing = tq2_guard && (scene_target == nullptr || scene_rt_unresolved);
    if (ui_target != nullptr && ui_target == scene_target) {
        // Avoid treating the scene/backbuffer as UI. Clearing/copying it can black out the desktop view.
        SPDLOG_INFO_ONCE("[VR] UI target matches scene render target; disabling UI copy/clear for this frame");
        ui_target = nullptr;
    }

    auto ui_target_native = safe_get_native_resource(ui_target, "ui target");
    if (ui_target_native != nullptr && (ui_target_native == backbuffer.Get() || ui_target_native == real_backbuffer.Get())) {
        // A mis-detected UI RT can alias the scene/backbuffer on UE5.7 and get cleared each frame.
        // Drop UI processing for this frame to avoid blacking out desktop/HMD output.
        SPDLOG_WARN_ONCE("[VR] UI target aliases scene/backbuffer; disabling UI copy/clear path");
        ui_target = nullptr;
        ui_target_native = nullptr;
        m_game_ui_tex.reset();
    }

    if (guarded_fallback_mode) {
        // In UE5.7 fallback mode, stale UI capture paths can present a frozen frame over the scene.
        ui_target = nullptr;
        ui_target_native = nullptr;
        m_game_ui_tex.reset();
    }

    const auto frame_count = vr->m_render_frame_count;

    const bool use_offscreen_game_copy =
        backbuffer.Get() == real_backbuffer.Get() ||
        ((tq2_guard || ue57_guard) && backbuffer.Get() != real_backbuffer.Get());

    auto source_color_format = canonical_typed_color_format(backbuffer_desc.Format);
    if (source_color_format == DXGI_FORMAT_UNKNOWN) {
        source_color_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    auto copy_output_format = select_unorm8_copy_format(source_color_format);
    auto desired_game_width = (uint32_t)backbuffer_desc.Width;
    auto desired_game_height = (uint32_t)backbuffer_desc.Height;

    // UE5.7/TQ2 guard: keep the last known-good OpenXR-compatible intermediate format/size
    // so transient session-state changes don't flip the copy path back to engine-native BGRA.
    if ((tq2_guard || ue57_guard) && runtime->is_openxr() && m_guarded_sticky_openxr_copy_format != DXGI_FORMAT_UNKNOWN) {
        copy_output_format = m_guarded_sticky_openxr_copy_format;
        if (m_guarded_sticky_openxr_copy_width > 0 && m_guarded_sticky_openxr_copy_height > 0) {
            desired_game_width = m_guarded_sticky_openxr_copy_width;
            desired_game_height = m_guarded_sticky_openxr_copy_height;
        }
    }

    // Prefer matching the active OpenXR scene swapchain format for the intermediate game copy.
    // This avoids invalid CopyTextureRegion calls when the runtime does not support the engine's
    // preferred channel order (for example BGRA source vs RGBA swapchain).
    if (runtime->is_openxr() && vr->m_openxr->ready()) {
        struct SceneSwapchainProbe {
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
            uint32_t width{};
            uint32_t height{};
        };

        SceneSwapchainProbe openxr_scene{};

        {
            std::scoped_lock _{m_openxr.mtx};

            const auto probe_swapchain = [&](uint32_t idx) -> SceneSwapchainProbe {
                const auto it = m_openxr.contexts.find(idx);
                if (it == m_openxr.contexts.end() || it->second.textures.empty() || it->second.textures[0].texture == nullptr) {
                    return {};
                }

                D3D12_RESOURCE_DESC desc{};
                if (!try_get_resource_desc_nothrow(it->second.textures[0].texture, desc)) {
                    return {};
                }

                SceneSwapchainProbe out{};
                out.format = canonical_typed_color_format(desc.Format);
                out.width = (uint32_t)desc.Width;
                out.height = (uint32_t)desc.Height;
                return out;
            };

            const auto preferred_idx = is_actually_afr
                ? (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE
                : (uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE;

            openxr_scene = probe_swapchain(preferred_idx);

            if (openxr_scene.format == DXGI_FORMAT_UNKNOWN && is_actually_afr) {
                openxr_scene = probe_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE);
            }

            if (openxr_scene.format == DXGI_FORMAT_UNKNOWN && vr->m_openxr != nullptr) {
                std::scoped_lock __{vr->m_openxr->swapchain_mtx};

                const auto from_meta = [&](uint32_t idx) -> SceneSwapchainProbe {
                    if (!vr->m_openxr->swapchains.contains(idx)) {
                        return {};
                    }

                    const auto& sc = vr->m_openxr->swapchains[idx];
                    SceneSwapchainProbe out{};
                    out.format = canonical_typed_color_format(sc.format);
                    out.width = (uint32_t)std::max<int32_t>(sc.width, 0);
                    out.height = (uint32_t)std::max<int32_t>(sc.height, 0);
                    return out;
                };

                openxr_scene = from_meta(preferred_idx);
                if (openxr_scene.format == DXGI_FORMAT_UNKNOWN && is_actually_afr) {
                    openxr_scene = from_meta((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE);
                }
            }
        }

        auto normalized_openxr_fmt = normalize_copy_color_format(openxr_scene.format);
        if (normalized_openxr_fmt == DXGI_FORMAT_UNKNOWN && (tq2_guard || ue57_guard) && m_guarded_sticky_openxr_copy_format != DXGI_FORMAT_UNKNOWN) {
            normalized_openxr_fmt = m_guarded_sticky_openxr_copy_format;
        }
        if (normalized_openxr_fmt == DXGI_FORMAT_UNKNOWN && (tq2_guard || ue57_guard)) {
            // Keep guarded fallback aligned with guarded swapchain selection (BGRA8 first).
            normalized_openxr_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        if (normalized_openxr_fmt != DXGI_FORMAT_UNKNOWN && copy_output_format != normalized_openxr_fmt) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[VR] OpenXR scene swapchain format {} differs from intermediate copy format {}; forcing exact game-copy format match",
                (uint32_t)normalized_openxr_fmt,
                (uint32_t)copy_output_format);
            copy_output_format = normalized_openxr_fmt;
        }

        if ((tq2_guard || ue57_guard) && normalized_openxr_fmt != DXGI_FORMAT_UNKNOWN) {
            m_guarded_sticky_openxr_copy_format = normalized_openxr_fmt;
        }

        // AFR scene submits per-eye images. Keep the intermediate copy sized to the eye swapchain
        // so CopyRegion does not crop into uninitialized areas.
        if (is_actually_afr && openxr_scene.width > 0 && openxr_scene.height > 0) {
            desired_game_width = openxr_scene.width;
            desired_game_height = openxr_scene.height;
        }

        if ((tq2_guard || ue57_guard) && openxr_scene.width > 0 && openxr_scene.height > 0) {
            m_guarded_sticky_openxr_copy_width = openxr_scene.width;
            m_guarded_sticky_openxr_copy_height = openxr_scene.height;
        }
    }

    if (runtime->is_openxr() && !is_actually_afr) {
        const auto hmd_width = vr->get_hmd_width();
        const auto hmd_height = vr->get_hmd_height();
        if (hmd_width > 0 && hmd_height > 0) {
            desired_game_width = (uint32_t)hmd_width;
            desired_game_height = (uint32_t)hmd_height;
        }
    }

    const auto game_copy_rtv_format = preferred_srgb_rtv_format(copy_output_format);
    const auto game_copy_srv_format = preferred_linear_view_format(copy_output_format);

    if (use_offscreen_game_copy) {
        auto reset_copy_targets = [&]() {
            m_game_tex.reset();
            m_backbuffer_copy.reset();
        };

        if (m_backbuffer_copy.texture.Get() != nullptr) {
            D3D12_RESOURCE_DESC copy_desc{};
            if (!try_get_resource_desc_nothrow(m_backbuffer_copy.texture.Get(), copy_desc)) {
                reset_copy_targets();
            } else {
                const auto copy_fmt = canonical_typed_color_format(copy_desc.Format);
                const auto src_fmt = canonical_typed_color_format(backbuffer_desc.Format);
                const bool size_mismatch =
                    copy_desc.Width != backbuffer_desc.Width ||
                    copy_desc.Height != backbuffer_desc.Height;
                const bool fmt_mismatch = !is_copy_format_compatible(copy_fmt, src_fmt);

                if (size_mismatch || fmt_mismatch) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        1,
                        "[VR] Recreating guarded copy targets due source desc change: src {}x{} fmt {} -> copy {}x{} fmt {}",
                        backbuffer_desc.Width,
                        backbuffer_desc.Height,
                        (uint32_t)src_fmt,
                        copy_desc.Width,
                        copy_desc.Height,
                        (uint32_t)copy_fmt);
                    reset_copy_targets();
                }
            }
        }

        if (m_game_tex.texture.Get() != nullptr) {
            D3D12_RESOURCE_DESC game_desc{};
            if (!try_get_resource_desc_nothrow(m_game_tex.texture.Get(), game_desc)) {
                reset_copy_targets();
            } else {
                const auto game_fmt = canonical_typed_color_format(game_desc.Format);
                const bool size_mismatch =
                    game_desc.Width != desired_game_width ||
                    game_desc.Height != desired_game_height;
                const bool fmt_mismatch = !is_copy_format_compatible(game_fmt, copy_output_format);

                if (size_mismatch || fmt_mismatch) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        1,
                        "[VR] Recreating guarded game texture due target mismatch: desired {}x{} fmt {} vs current {}x{} fmt {}",
                        desired_game_width,
                        desired_game_height,
                        (uint32_t)copy_output_format,
                        game_desc.Width,
                        game_desc.Height,
                        (uint32_t)game_fmt);
                    reset_copy_targets();
                }
            }
        }
    }

    if (m_game_tex.texture.Get() == nullptr && use_offscreen_game_copy) {
        if (backbuffer.Get() == real_backbuffer.Get()) {
            spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Guarded UE5.7 path: setting up offscreen game texture copy from resolved scene resource");
        }
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer_desc;
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), source_color_format, source_color_format, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        // Keep channel-family fidelity from source (RGBA/BGRA) while normalizing to UNORM8.
        desc.Format = copy_output_format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        auto target_game_width = desired_game_width;
        auto target_game_height = desired_game_height;

        if ((uint32_t)desc.Width != target_game_width || (uint32_t)desc.Height != target_game_height) {
            SPDLOG_INFO_EVERY_N_SEC(2,
                "[VR] Resizing fallback game texture {}x{} -> {}x{}",
                desc.Width,
                desc.Height,
                target_game_width,
                target_game_height);
        }

        desc.Width = target_game_width;
        desc.Height = target_game_height;

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[VR] Guarded color path: source_fmt={} typed_source_fmt={} copy_fmt={}",
            (uint32_t)backbuffer_desc.Format,
            (uint32_t)source_color_format,
            (uint32_t)copy_output_format
        );

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), game_copy_rtv_format, game_copy_srv_format, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && m_game_tex.texture.Get() != backbuffer.Get()) {
        if (!(tq2_guard || ue57_guard)) {
            spdlog::info("[VR] Setting up game texture as reference to original");

            if (!m_game_tex.setup(device, backbuffer.Get(), game_copy_rtv_format, game_copy_srv_format, L"Game Texture")) {
                spdlog::error("[VR] Failed to fully setup game texture.");
                m_game_tex.reset();
            }
        } else {
            // Guarded UE5.7/TQ2: keep m_game_tex as a dedicated copy to avoid aliasing
            // (we clear/draw into m_game_tex later for 2D-screen/UI paths).
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Guarded UE5.7 path: preserving offscreen game texture copy (no direct alias to scene resource)");
        }
    }

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = safe_get_native_resource(scene_capture, "scene capture render target");

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");

            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    auto ensure_game_batch_for_format = [&](DXGI_FORMAT dst_format) -> DirectX::DX12::SpriteBatch* {
        const auto typed_dst_format = canonical_typed_color_format(dst_format);
        const auto required_format = typed_dst_format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : typed_dst_format;

        if (m_game_batch == nullptr || m_game_batch_format != required_format) {
            SPDLOG_INFO_EVERY_N_SEC(2,
                "[VR] Rebuilding guarded game SpriteBatch PSO for RTV format {} (previous={})",
                (uint32_t)required_format,
                (uint32_t)m_game_batch_format);
            m_game_batch = setup_sprite_batch_pso(required_format);
            m_game_batch_format = required_format;
        }

        return m_game_batch.get();
    };

    auto get_guarded_scene_source_ctx = [&](ID3D12Resource* source_texture) -> d3d12::TextureContext* {
        if (source_texture == nullptr) {
            return nullptr;
        }

        if (m_game_tex.texture.Get() == source_texture && m_game_tex.srv_heap != nullptr) {
            return &m_game_tex;
        }

        if (!guarded_57_mode) {
            return nullptr;
        }

        const auto typed_source_format = preferred_linear_view_format(source_color_format);
        if (typed_source_format == DXGI_FORMAT_UNKNOWN) {
            return nullptr;
        }

        if (m_guarded_scene_source_tex.texture.Get() != source_texture ||
            m_guarded_scene_source_tex.srv_heap == nullptr ||
            m_guarded_scene_source_srv_format != typed_source_format)
        {
            m_guarded_scene_source_tex.reset();
            m_guarded_scene_source_tex.texture = source_texture;

            if (!m_guarded_scene_source_tex.create_srv(device, typed_source_format)) {
                SPDLOG_ERROR_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7 direct scene submit skipped: failed to create SRV for source {:x} fmt {}",
                    (uintptr_t)source_texture,
                    (uint32_t)typed_source_format);
                m_guarded_scene_source_tex.reset();
                m_guarded_scene_source_srv_format = DXGI_FORMAT_UNKNOWN;
                return nullptr;
            }

            m_guarded_scene_source_srv_format = typed_source_format;
            SPDLOG_INFO_EVERY_N_SEC(2,
                "[VR] Guarded UE5.7: using direct scene SRV path for OpenXR submit (source {:x}, fmt {})",
                (uintptr_t)source_texture,
                (uint32_t)typed_source_format);
        }

        return &m_guarded_scene_source_tex;
    };

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        auto* source_texture = backbuffer.Get();

        if (render_target == nullptr || source_texture == nullptr) {
            return;
        }

        const auto scene_desc = backbuffer_desc;
        const auto target_desc = render_target->GetDesc();
        const auto scene_width = (uint32_t)scene_desc.Width;
        const auto scene_height = (uint32_t)scene_desc.Height;
        const auto target_width = (uint32_t)target_desc.Width;
        const auto target_height = (uint32_t)target_desc.Height;

        if (scene_width == 0 || scene_height == 0 || target_width < 2 || target_height == 0) {
            return;
        }

        const auto scene_is_double_wide = is_likely_double_wide_source(scene_width, expected_double_width_for_frame);
        const auto scene_eye_width = scene_is_double_wide ? scene_width / 2 : scene_width;
        const auto target_eye_width = target_width / 2;

        // UE5.7/TQ2 often feeds a single-wide 3840x1080 scene source into a much taller
        // OpenXR double-wide target (e.g. 4944x2416). CopyRegion leaves most of the
        // target untouched (white/garbage). Use an aspect-fit blit into each eye instead.
        if (m_scene_capture_tex.texture.Get() == nullptr && !scene_is_double_wide) {
            auto* const dst_ctx = m_openxr.find_texture_context(render_target);
            const auto target_view_format = effective_rtv_format(*dst_ctx);
            auto* const batch_for_target = ensure_game_batch_for_format(target_view_format != DXGI_FORMAT_UNKNOWN ? target_view_format : target_desc.Format);
            auto* const src_ctx = get_guarded_scene_source_ctx(source_texture);
            const bool can_aspect_fit_blit =
                dst_ctx != nullptr &&
                dst_ctx->rtv_heap != nullptr &&
                batch_for_target != nullptr &&
                src_ctx != nullptr &&
                src_ctx->srv_heap != nullptr;

            if (can_aspect_fit_blit) {
                auto eye_rect = make_aspect_fit_rect(scene_width, scene_height, target_eye_width, target_height);

                if (eye_rect.has_value()) {
                    RECT left_dst = *eye_rect;
                    RECT right_dst = *eye_rect;
                    right_dst.left += (LONG)target_eye_width;
                    right_dst.right += (LONG)target_eye_width;

                    RECT src_rect{
                        0,
                        0,
                        (LONG)scene_width,
                        (LONG)scene_height
                    };

                    const float clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
                    commands.clear_rtv(render_target, dst_ctx->get_rtv(), clear_color, OPENXR_SWAPCHAIN_BASE_STATE);

                    d3d12::render_srv_to_rtv(
                        batch_for_target,
                        commands.cmd_list.Get(),
                        *src_ctx,
                        *dst_ctx,
                        src_rect,
                        left_dst,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        OPENXR_SWAPCHAIN_BASE_STATE
                    );

                    d3d12::render_srv_to_rtv(
                        batch_for_target,
                        commands.cmd_list.Get(),
                        *src_ctx,
                        *dst_ctx,
                        src_rect,
                        right_dst,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        OPENXR_SWAPCHAIN_BASE_STATE
                    );

                    SPDLOG_INFO_EVERY_N_SEC(
                        2,
                        "[VR] UE5.7 aspect-fit single-wide source {}x{} into double-wide target {}x{} (eye {}x{})",
                        scene_width,
                        scene_height,
                        target_width,
                        target_height,
                        target_eye_width,
                        target_height
                    );

                    return;
                }
            }
        }

        auto copy_width = std::min<uint32_t>(scene_eye_width, target_eye_width);
        auto copy_height = std::min<uint32_t>(scene_height, target_height);

        if (copy_width == 0 || copy_height == 0) {
            return;
        }

        D3D12_BOX left_src_box{
            .left = 0,
            .top = 0,
            .front = 0,
            .right = copy_width,
            .bottom = copy_height,
            .back = 1
        };

        if (m_scene_capture_tex.texture.Get() != nullptr) {
            const auto scene_capture_desc = m_scene_capture_tex.texture->GetDesc();
            copy_width = std::min<uint32_t>(copy_width, (uint32_t)scene_capture_desc.Width);
            copy_height = std::min<uint32_t>(copy_height, (uint32_t)scene_capture_desc.Height);

            D3D12_BOX right_src_box{
                .left = 0,
                .top = 0,
                .front = 0,
                .right = copy_width,
                .bottom = copy_height,
                .back = 1
            };

            commands.copy_region_stereo(
                source_texture, m_scene_capture_tex.texture.Get(), render_target,
                &left_src_box, &right_src_box,
                0, 0, 0, target_eye_width, 0, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COMMON
            );
        } else {
            // Single-wide fallback path: duplicate the same eye source to both halves.
            commands.copy_region(
                source_texture,
                render_target,
                &left_src_box,
                0,
                0,
                0,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COMMON
            );

            commands.copy_region(
                source_texture,
                render_target,
                &left_src_box,
                target_eye_width,
                0,
                0,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COMMON
            );
        }
    };

    const bool guarded_scene_source_is_typeless =
        guarded_57_mode &&
        (backbuffer_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
         backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         backbuffer_desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);

    const bool guarded_scene_source_is_single_wide =
        guarded_57_mode &&
        runtime->is_openxr() &&
        !is_actually_afr &&
        !is_likely_double_wide_source((uint32_t)backbuffer_desc.Width, expected_double_width_for_frame);

    const bool guarded_scene_source_requires_intermediate =
        guarded_57_mode &&
        runtime->is_openxr() &&
        !is_actually_afr &&
        backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get() &&
        (guarded_scene_source_is_typeless ||
         guarded_scene_source_is_single_wide ||
         source_color_format == DXGI_FORMAT_UNKNOWN ||
         source_color_format != copy_output_format ||
         is_r10g10b10a2_family(source_color_format) ||
         desired_game_width != (uint32_t)backbuffer_desc.Width ||
         desired_game_height != (uint32_t)backbuffer_desc.Height);

    const bool guarded_direct_scene_submit =
        guarded_57_mode &&
        runtime->is_openxr() &&
        !is_actually_afr &&
        backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get() &&
        !guarded_scene_source_requires_intermediate;

    if (guarded_57_mode && runtime->is_openxr() && !is_actually_afr &&
        backbuffer.Get() != nullptr && backbuffer.Get() != real_backbuffer.Get() &&
        !guarded_direct_scene_submit)
    {
        SPDLOG_INFO_EVERY_N_SEC(1,
            "[VR] Guarded UE5.7: forcing normalized offscreen copy for scene source raw_fmt {} typed_fmt {} size {}x{} -> game-copy {}x{} fmt {}",
            (uint32_t)backbuffer_desc.Format,
            (uint32_t)source_color_format,
            backbuffer_desc.Width,
            backbuffer_desc.Height,
            desired_game_width,
            desired_game_height,
            (uint32_t)copy_output_format);
    }

    // For copying the real backbuffer if we need to
    const bool guarded_scene_source_copy =
        guarded_57_mode &&
        backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get() &&
        backbuffer.Get() != m_game_tex.texture.Get() &&
        !guarded_direct_scene_submit;

    const bool should_copy_source_to_game_tex =
        m_game_tex.texture.Get() != nullptr &&
        ((m_backbuffer_copy.texture.Get() != nullptr && backbuffer == real_backbuffer) || guarded_scene_source_copy);

    bool copied_source_into_game_tex = false;

    if (should_copy_source_to_game_tex) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            const auto guarded_wait_ms = (tq2_guard || ue57_guard) ? 2000u : INFINITE;
            command_ctx.wait(guarded_wait_ms);
            if ((tq2_guard || ue57_guard) && command_ctx.waiting_for_fence) {
                SPDLOG_WARNING_EVERY_N_SEC(1,
                    "[VR] Guarded UE5.7: source->game command context wait timed out, recreating context '{}'.",
                    utility::narrow(command_ctx.internal_name));
                const auto name = command_ctx.internal_name;
                command_ctx.reset();
                command_ctx.setup(name.c_str());
            }
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (guarded_scene_source_copy) {
                d3d12::TextureContext guarded_scene_source{};
                guarded_scene_source.texture = backbuffer;

                if (!guarded_scene_source.create_srv(device, preferred_linear_view_format(source_color_format))) {
                    SPDLOG_ERROR_EVERY_N_SEC(1,
                        "[VR] Guarded UE5.7 scene->game blit skipped: failed to create SRV for source {:x}",
                        (uintptr_t)backbuffer.Get());
                    return vr::VRCompositorError_None;
                }

                if (is_tq2_diag_enabled()) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        1,
                        "[VR][TQ2_DIAG] guarded scene->game blit src={:x} dst={:x} src_state={:#x}",
                        (uintptr_t)backbuffer.Get(),
                        (uintptr_t)m_game_tex.texture.Get(),
                        (uint32_t)ENGINE_SRC_COLOR);
                }

                const auto src_desc = backbuffer->GetDesc();
                const auto dst_desc = m_game_tex.texture->GetDesc();

                RECT src_rect{
                    0,
                    0,
                    (LONG)src_desc.Width,
                    (LONG)src_desc.Height
                };

                const bool use_aspect_fit_for_game_copy = !(runtime->is_openxr() && is_actually_afr);
                const auto dest_rect = use_aspect_fit_for_game_copy
                    ? make_aspect_fit_rect(
                        (uint32_t)src_desc.Width,
                        (uint32_t)src_desc.Height,
                        (uint32_t)dst_desc.Width,
                        (uint32_t)dst_desc.Height
                    )
                    : std::nullopt;
                const auto dst_view_format = effective_rtv_format(m_game_tex);
                auto* const batch_for_game_tex = ensure_game_batch_for_format(dst_view_format != DXGI_FORMAT_UNKNOWN ? dst_view_format : dst_desc.Format);
                if (batch_for_game_tex == nullptr) {
                    SPDLOG_ERROR_EVERY_N_SEC(1,
                        "[VR] Guarded scene->game blit skipped: SpriteBatch PSO unavailable for format {}",
                        (uint32_t)dst_desc.Format);
                    return vr::VRCompositorError_None;
                }

                d3d12::render_srv_to_rtv(
                    batch_for_game_tex,
                    command_ctx.cmd_list.Get(),
                    guarded_scene_source,
                    m_game_tex,
                    src_rect,
                    dest_rect,
                    ENGINE_SRC_COLOR,
                    D3D12_RESOURCE_STATE_RENDER_TARGET
                );
            } else {
                const auto src_state = D3D12_RESOURCE_STATE_PRESENT;

                if (is_tq2_diag_enabled()) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        1,
                        "[VR][TQ2_DIAG] source->game copy src={:x} dst={:x} src_state={:#x} guarded={} real_src={}",
                        (uintptr_t)backbuffer.Get(),
                        (uintptr_t)m_backbuffer_copy.texture.Get(),
                        (uint32_t)src_state,
                        (tq2_guard || ue57_guard) ? 1 : 0,
                        1);
                }
                command_ctx.copy(backbuffer.Get(), m_backbuffer_copy.texture.Get(), src_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

                const auto src_desc = m_backbuffer_copy.texture->GetDesc();
                const auto dst_desc = m_game_tex.texture->GetDesc();

                RECT src_rect{
                    0,
                    0,
                    (LONG)src_desc.Width,
                    (LONG)src_desc.Height
                };

                const bool use_aspect_fit_for_game_copy = !(runtime->is_openxr() && is_actually_afr);
                const auto dest_rect = use_aspect_fit_for_game_copy
                    ? make_aspect_fit_rect(
                        (uint32_t)src_desc.Width,
                        (uint32_t)src_desc.Height,
                        (uint32_t)dst_desc.Width,
                        (uint32_t)dst_desc.Height
                    )
                    : std::nullopt;
                const auto dst_view_format = effective_rtv_format(m_game_tex);
                auto* const batch_for_game_tex = ensure_game_batch_for_format(dst_view_format != DXGI_FORMAT_UNKNOWN ? dst_view_format : dst_desc.Format);
                if (batch_for_game_tex == nullptr) {
                    SPDLOG_ERROR_EVERY_N_SEC(1,
                        "[VR] Guarded source->game blit skipped: SpriteBatch PSO unavailable for format {}",
                        (uint32_t)dst_desc.Format);
                    return vr::VRCompositorError_None;
                }

                if (use_aspect_fit_for_game_copy && dest_rect.has_value()) {
                    SPDLOG_INFO_EVERY_N_SEC(2,
                        "[VR] Real backbuffer fallback aspect-fit blit: src {}x{} -> dst {}x{} (rect {},{}-{},{} )",
                        src_desc.Width,
                        src_desc.Height,
                        dst_desc.Width,
                        dst_desc.Height,
                        dest_rect->left,
                        dest_rect->top,
                        dest_rect->right,
                        dest_rect->bottom);

                    d3d12::render_srv_to_rtv(
                        batch_for_game_tex,
                        command_ctx.cmd_list.Get(),
                        m_backbuffer_copy,
                        m_game_tex,
                        src_rect,
                        dest_rect,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        batch_for_game_tex,
                        command_ctx.cmd_list.Get(),
                        m_backbuffer_copy,
                        m_game_tex,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET
                    );
                }
            }

            command_ctx.execute();
            copied_source_into_game_tex = true;
        }

        backbuffer = m_game_tex.texture;
        if (!try_get_resource_desc_nothrow(backbuffer.Get(), backbuffer_desc)) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to read copied game texture desc.");
            return vr::VRCompositorError_None;
        }
    }

    // UE5.7/TQ2: always prefer the intermediate game-copy texture for OpenXR scene submits.
    // This keeps color/channel layout stable for runtimes exposing typeless RGBA swapchain images.
    if (runtime->is_openxr() && guarded_57_mode && m_game_tex.texture.Get() != nullptr && !guarded_direct_scene_submit) {
        if (backbuffer.Get() != m_game_tex.texture.Get()) {
            D3D12_RESOURCE_DESC game_desc{};
            if (try_get_resource_desc_nothrow(m_game_tex.texture.Get(), game_desc)) {
                backbuffer = m_game_tex.texture;
                backbuffer_desc = game_desc;

                SPDLOG_INFO_EVERY_N_SEC(
                    2,
                    "[VR] Guarded UE5.7: forcing OpenXR submit source to game-copy texture {}x{} fmt {} (fresh_copy={})",
                    game_desc.Width,
                    game_desc.Height,
                    (uint32_t)game_desc.Format,
                    copied_source_into_game_tex ? 1 : 0);
            }
        }
    }

    if (ui_target != nullptr) {
        if (ui_target_native != nullptr && m_game_ui_tex.texture.Get() != ui_target_native) {
            const auto ui_rtv_format = preferred_srgb_rtv_format(ui_target_native->GetDesc().Format);
            const auto ui_srv_format = preferred_linear_view_format(ui_target_native->GetDesc().Format);
            if (!m_game_ui_tex.setup(device,
                ui_target_native,
                ui_rtv_format, ui_srv_format,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const bool skip_dynamic_recreate = (tq2_guard || ue57_guard);
            const auto native = ui_target_native;
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            if (skip_dynamic_recreate) {
                                SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR] Guarded UE5.7: UI size changed [{}x{}]->[{}x{}], deferring recreate for stability", desc.Width, desc.Height, uisc.width, uisc.height);
                            } else {
                                SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                                ffsr->set_should_recreate_textures(true);
                            }
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        if (skip_dynamic_recreate) {
                            SPDLOG_INFO_EVERY_N_SEC(2, "[OpenVR] Guarded UE5.7: UI size changed [{}x{}]->[{}x{}], deferring recreate for stability", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        } else {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                }
            } else if (native == nullptr) {
                if (skip_dynamic_recreate) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Guarded UE5.7: UI native resource null; deferring recreate");
                } else {
                    spdlog::error("[VR] Recreating UI texture because native resource is null");
                    ffsr->set_should_recreate_textures(true);
                }
            }
        }
    } else {
        m_game_ui_tex.reset(); // Probably fixes non-resident errors.
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (ui_invert_alpha > 0.0f && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            const std::array<float, 4> blend_factor{ 1.0f, 1.0f, 1.0f, ui_invert_alpha };
            const DirectX::XMFLOAT4 invert_alpha_tint{ 1.0f, 1.0f, 1.0f, ui_invert_alpha };
            d3d12::render_srv_to_rtv(
                m_ui_batch_alpha_invert.get(),
                commands.cmd_list.Get(),
                m_game_ui_tex,
                m_game_ui_tex,
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR,
                blend_factor,
                invert_alpha_tint);
        }

        if (!(tq2_guard || ue57_guard)) {
            draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame);
        } else if (is_tq2_diag_enabled()) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR][TQ2_DIAG] guarded mode: skipping draw_spectator_view");
        }

        if (is_2d_screen && m_game_tex.texture.Get() != nullptr && m_game_tex.srv_heap != nullptr) {
            const auto game_desc = m_game_tex.texture->GetDesc();
            const auto game_width = (uint32_t)game_desc.Width;
            const auto game_height = (uint32_t)game_desc.Height;
            const auto game_is_double_wide = is_likely_double_wide_source(game_width, (uint32_t)m_backbuffer_size[0]);
            const auto game_eye_width = game_is_double_wide ? game_width / 2 : game_width;
            const auto left_screen_format = m_2d_screen_tex[0].texture != nullptr
                ? m_2d_screen_tex[0].texture->GetDesc().Format
                : DXGI_FORMAT_B8G8R8A8_UNORM;
            const auto right_screen_format = m_2d_screen_tex[1].texture != nullptr
                ? m_2d_screen_tex[1].texture->GetDesc().Format
                : left_screen_format;
            auto* const batch_for_screen_left = ensure_game_batch_for_format(left_screen_format);
            auto* const batch_for_screen_right = ensure_game_batch_for_format(right_screen_format);
            if (batch_for_screen_left == nullptr || batch_for_screen_right == nullptr) {
                SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] 2D screen blit skipped: SpriteBatch PSO unavailable");
                return;
            }

            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            // Render left side to left screen tex
            d3d12::render_srv_to_rtv(
                batch_for_screen_left,
                commands.cmd_list.Get(),
                m_game_tex,
                m_2d_screen_tex[0],
                RECT{0, 0, (LONG)game_eye_width, (LONG)game_height},
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    batch_for_screen_left,
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                // Render right side to right screen tex
                if (m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        batch_for_screen_right,
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    RECT right_src{0, 0, (LONG)game_eye_width, (LONG)game_height};
                    if (game_is_double_wide) {
                        right_src.left = (LONG)game_eye_width;
                        right_src.right = (LONG)game_width;
                    }

                    d3d12::render_srv_to_rtv(
                        batch_for_screen_right,
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_2d_screen_tex[1],
                        right_src,
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        batch_for_screen_right,
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
		if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }
		
        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_invert_alpha };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target_native != nullptr) {
                m_openvr.ui_tex.commands.copy(ui_target_native, m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (is_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        const bool allow_openxr_ui_prepass = !(tq2_guard || ue57_guard);
        const std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> ui_pre_commands =
            allow_openxr_ui_prepass ? std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>>(draw_2d_view) : std::nullopt;
        const std::optional<std::function<void(d3d12::CommandContext&)>> ui_post_commands =
            allow_openxr_ui_prepass ? std::optional<std::function<void(d3d12::CommandContext&)>>(clear_rt) : std::nullopt;

        if (!allow_openxr_ui_prepass) {
            SPDLOG_INFO_ONCE("[VR] Guarded UE5.7 mode: disabling OpenXR UI prepass to avoid invalid desktop-fix composition");
        }

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), ui_pre_commands, ui_post_commands, ENGINE_SRC_COLOR);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), ui_pre_commands, std::nullopt, ENGINE_SRC_COLOR);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, ui_post_commands, ENGINE_SRC_COLOR);
                }
            } else if (ui_target_native != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, ui_target_native, ui_pre_commands, ui_post_commands, ENGINE_SRC_COLOR);
            } else if (vr->m_desktop_fix->value() && !guarded_fallback_mode && allow_openxr_ui_prepass) {
                SPDLOG_INFO_ONCE("[VR] UI target missing; drawing spectator view via OpenXR UI swapchain");
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, nullptr, ui_pre_commands, ui_post_commands, ENGINE_SRC_COLOR);
            }

            auto fw_rt = g_framework->get_rendertarget_d3d12();

            if (fw_rt && g_framework->is_drawing_anything()) {
                if (guarded_fallback_mode || tq2_scene_missing) {
                    SPDLOG_INFO_ONCE("[VR] Guarded fallback active: still submitting OpenXR FRAMEWORK_UI layer for frontend visibility");
                }
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), ui_pre_commands, ui_post_commands, ENGINE_SRC_COLOR);
        } else if (!guarded_fallback_mode && m_game_ui_tex.commands.ready()) {
            m_game_ui_tex.commands.wait(INFINITE);
            draw_2d_view(m_game_ui_tex.commands, nullptr);
            clear_rt(m_game_ui_tex.commands);
            m_game_ui_tex.commands.execute();
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            const auto source_desc = backbuffer_desc;
            const auto source_width = (uint32_t)source_desc.Width;
            const auto source_height = (uint32_t)source_desc.Height;
            const auto source_is_double_wide = is_likely_double_wide_source(source_width, expected_double_width_for_frame);
            const auto source_eye_width = source_is_double_wide ? source_width / 2 : source_width;

            if (tq2_guard || ue57_guard) {
                SPDLOG_INFO_EVERY_N_SEC(1,
                    "[VR] OpenXR scene source candidate: {}x{} fmt {} (expected double-wide {}x{}, source_is_double_wide={}, real_fallback={})",
                    source_width,
                    source_height,
                    (uint32_t)source_desc.Format,
                    expected_double_width_for_frame,
                    expected_runtime_height,
                    source_is_double_wide ? 1 : 0,
                    using_real_backbuffer_source ? 1 : 0);
            }

            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = source_height;
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled() || !source_is_double_wide) {
                src_box.right = source_width;
            } else {
                src_box.right = source_eye_width;
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            const auto source_desc = backbuffer_desc;
            const auto source_width = (uint32_t)source_desc.Width;
            const auto source_height = (uint32_t)source_desc.Height;
            const auto source_is_double_wide = is_likely_double_wide_source(source_width, expected_double_width_for_frame);
            const auto source_eye_width = source_is_double_wide ? source_width / 2 : source_width;

            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = source_height;
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled() || !source_is_double_wide) {
                    src_box.right = source_width;
                } else {
                    src_box.right = source_eye_width;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }

            if (is_actually_afr) {
                D3D12_BOX src_box{};
                src_box.top = 0;
                src_box.bottom = source_height;
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled() || !source_is_double_wide) {
                    src_box.left = 0;
                    src_box.right = source_width;
                } else {
                    if (!is_afr) {
                        src_box.left = source_eye_width;
                        src_box.right = source_width;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = source_eye_width;
                    }
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            } else {
                // Copy over the entire double wide when the source already matches.
                // If source is single-wide, pre_render duplicates it into both halves.
                if (!guarded_57_mode && m_scene_capture_tex.texture.Get() == nullptr && source_is_double_wide) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                } else {
                    if (guarded_57_mode) {
                        SPDLOG_INFO_EVERY_N_SEC(2,
                            "[VR] Guarded UE5.7: forcing pre-render scene path for OpenXR submit (source {}x{} fmt {}, double_wide={})",
                            source_width,
                            source_height,
                            (uint32_t)source_desc.Format,
                            source_is_double_wide ? 1 : 0);
                    }
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr) {
                m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }

            if (!is_afr) {
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openvr.copy_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                m_openvr.copy_left_to_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.submitted_this_frame((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.submitted_this_frame((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.submitted_this_frame((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (m_openxr.submitted_this_frame((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame) {
    if (command_list == nullptr) {
        return;
    }

    if (m_game_tex.texture == nullptr || m_game_tex.srv_heap == nullptr || m_game_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    const bool has_ui_tex = m_game_ui_tex.texture != nullptr &&
        m_game_ui_tex.srv_heap != nullptr &&
        m_game_ui_tex.srv_heap->Heap() != nullptr;

    const auto& vr = VR::get();

    if (!vr->is_hmd_active() || !vr->m_desktop_fix->value()) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if (vr->is_using_afr() && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;
    const auto game_desc = m_game_tex.texture->GetDesc();
    const auto game_width = (uint32_t)game_desc.Width;
    const auto game_height = (uint32_t)game_desc.Height;
    const auto game_is_double_wide = is_likely_double_wide_source(game_width, (uint32_t)m_backbuffer_size[0]);

    const auto eye_width = game_is_double_wide ? ((float)game_width / 2.0f) : (float)game_width;
    const auto eye_height = (float)game_height;
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled() || !game_is_double_wide) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = (LONG)eye_width;
        source_rect.bottom = (LONG)game_height;
    } else {
        source_rect.left = (LONG)eye_width;
        source_rect.top = 0;
        source_rect.right = (LONG)game_width;
        source_rect.bottom = (LONG)game_height;
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { m_game_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(m_game_tex.get_srv_gpu(), 
        DirectX::XMUINT2{game_width, game_height},
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    //////
    // UI (optional)
    //////
    if (has_ui_tex) {
        // Set descriptor heaps
        ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
        command_list->SetDescriptorHeaps(1, ui_heaps);

        batch->Draw(m_game_ui_tex.get_srv_gpu(), 
            DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height },
            dest_rect, 
            DirectX::Colors::White);
    } else {
        SPDLOG_INFO_ONCE("[VR] No UI texture available for spectator view; drawing scene only");
    }

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    if (backbuffer_ctx.commands.waiting_for_fence) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Skipping backbuffer clear; previous clear command list still in flight");
        return;
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        if (tq2_guard || ue57_guard) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Guarded UE5.7: skipping post-present backbuffer clear");
        } else if (m_last_frame_used_real_backbuffer_source) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Skipping post-present backbuffer clear while using real backbuffer source");
        } else {
            clear_backbuffer();
        }
    }
}

void D3D12Component::on_reset(VR* vr) {
    m_force_reset = true;
    m_last_frame_used_real_backbuffer_source = false;
    m_guarded_scene_source_tex.reset();
    m_guarded_last_scene_source.Reset();
    m_guarded_scene_source_srv_format = DXGI_FORMAT_UNKNOWN;

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_game_tex.reset();
    m_scene_capture_tex.reset();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_game_batch_format = DXGI_FORMAT_UNKNOWN;
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            g_framework->get_d3d12_rt_size()[0] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            g_framework->get_d3d12_rt_size()[1] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = resolve_scene_render_target_for_d3d12(vr.get());

    if (ue4_texture != nullptr) {
        backbuffer = safe_get_native_resource(ue4_texture, "scene render target");
    } else {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Scene render target pointer is null");
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    bool using_real_backbuffer_source = false;
    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
        using_real_backbuffer_source = true;
    }

    if (backbuffer == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Scene render target unavailable; falling back to real back buffer (D3D12)");
        backbuffer = real_backbuffer;
        using_real_backbuffer_source = true;
    }

    if (backbuffer.Get() == real_backbuffer.Get()) {
        using_real_backbuffer_source = true;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();
    const auto [expected_setup_width, expected_setup_height] = get_expected_stereo_extent(
        vr.get(),
        (uint32_t)real_backbuffer_desc.Width * 2u,
        (uint32_t)real_backbuffer_desc.Height);

    if ((tq2_guard || ue57_guard) && !using_real_backbuffer_source &&
        !is_plausible_scene_source_desc(backbuffer_desc, real_backbuffer_desc, expected_setup_width, expected_setup_height))
    {
        SPDLOG_INFO(
            "[VR] Setup rejected scene source {}x{} fmt {}; using real backbuffer {}x{} fmt {} (expected stereo {}x{})",
            backbuffer_desc.Width,
            backbuffer_desc.Height,
            (uint32_t)backbuffer_desc.Format,
            real_backbuffer_desc.Width,
            real_backbuffer_desc.Height,
            (uint32_t)real_backbuffer_desc.Format,
            expected_setup_width,
            expected_setup_height);
        backbuffer = real_backbuffer;
        backbuffer_desc = real_backbuffer_desc;
        using_real_backbuffer_source = true;
    }

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled() && !using_real_backbuffer_source) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    } else if (!vr->is_extreme_compatibility_mode_enabled() && using_real_backbuffer_source) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setup using single-wide real backbuffer source; keeping full width");
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        auto screen_desc = backbuffer_desc;
        screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        for (auto& context : m_2d_screen_tex) {
            ComPtr<ID3D12Resource> screen_tex{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &screen_desc, ENGINE_SRC_COLOR, nullptr,
                    IID_PPV_ARGS(&screen_tex)))) {
                spdlog::error("[VR] Failed to create 2D screen texture.");
                continue;
            }

            screen_tex->SetName(L"2D Screen Texture");

            if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
                spdlog::error("[VR] Failed to setup 2D screen context.");
                continue;
            }
        }
    }

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture. it's the desktop resolution.
        auto ui_desc = backbuffer_desc;
        ui_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        ui_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    const auto initial_game_batch_format = preferred_srgb_rtv_format(backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(initial_game_batch_format);
    m_game_batch_format = canonical_typed_color_format(initial_game_batch_format);

    // Custom blend state to flip the alpha in-place of the UI texture without an intermediate render target
    {
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{backbuffer_desc.Format, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        bdrt.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            backbuffer_desc.Format, 
            alpha_luminance_sprite_ps_SpritePixelShader, 
            alpha_luminance_sprite_ps_SpriteVertexShader, 
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_last_frame_used_real_backbuffer_source = using_real_backbuffer_source;
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->get_fake_stereo_hook() != nullptr) {
        auto ue4_texture = resolve_scene_render_target_for_d3d12(vr.get());

        if (ue4_texture != nullptr) {
            backbuffer = safe_get_native_resource(ue4_texture, "scene render target");
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;
    const bool guarded_ue57_tq2 = (tq2_guard || ue57_guard);
    DXGI_FORMAT guarded_color_format = backbuffer_desc.Format;
    if (guarded_color_format == DXGI_FORMAT_UNKNOWN ||
        guarded_color_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        guarded_color_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        guarded_color_format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
    {
        guarded_color_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    const auto pick_supported_openxr_color_format = [&](DXGI_FORMAT requested) -> DXGI_FORMAT {
        const auto is_supported = [&](DXGI_FORMAT fmt) -> bool {
            return fmt != DXGI_FORMAT_UNKNOWN && openxr->is_supported_swapchain_format(fmt);
        };

        if (guarded_ue57_tq2) {
            // TQ2 commonly presents through a 10-bit desktop swapchain. OpenXR runtimes on this stack
            // do not advertise R10G10B10A2, so keep the VR path pinned to BGRA8. Prefer the
            // sRGB view when the runtime supports it; OpenXR D3D12 swapchain images are often
            // typeless and must be interpreted through typed SRV/RTV views to avoid washed-out UI.
            if (is_supported(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
                return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            }
            if (is_supported(DXGI_FORMAT_B8G8R8A8_UNORM)) {
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            }
        }

        if (is_supported(requested)) {
            return requested;
        }

        const auto requested_typed = canonical_typed_color_format(requested);
        const bool prefer_bgra = guarded_ue57_tq2 || is_bgra8_family(requested_typed);

        static constexpr DXGI_FORMAT preferred_formats_bgra[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R10G10B10A2_UNORM,
        };

        static constexpr DXGI_FORMAT preferred_formats_rgba[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R10G10B10A2_UNORM,
        };

        const auto& preferred_formats = prefer_bgra ? preferred_formats_bgra : preferred_formats_rgba;

        for (const auto fmt : preferred_formats) {
            if (is_supported(fmt)) {
                return fmt;
            }
        }

        const auto supported_formats = openxr->get_supported_swapchain_formats();
        if (!supported_formats.empty()) {
            return supported_formats[0];
        }

        return DXGI_FORMAT_UNKNOWN;
    };

    const auto original_guarded_color_format = guarded_color_format;
    guarded_color_format = pick_supported_openxr_color_format(guarded_color_format);
    if (guarded_color_format == DXGI_FORMAT_UNKNOWN) {
        spdlog::error("[VR] No compatible OpenXR color format available for D3D12 swapchain.");
        return "No compatible OpenXR color format available.";
    }

    if (original_guarded_color_format != guarded_color_format) {
        SPDLOG_WARN(
            "[VR] Adjusted OpenXR color format {} -> {} to match runtime-supported formats",
            (uint32_t)original_guarded_color_format,
            (uint32_t)guarded_color_format);
    }

    const uint32_t guarded_sample_count = guarded_ue57_tq2 ? 1u : std::max<uint32_t>(1u, backbuffer_desc.SampleDesc.Count);

    if (guarded_ue57_tq2) {
        SPDLOG_INFO("[VR] Guarded UE5.7/TQ2 OpenXR swapchain settings: color_fmt={} sample_count={} (backbuffer fmt={} samples={})",
            (uint32_t)guarded_color_format,
            guarded_sample_count,
            (uint32_t)backbuffer_desc.Format,
            backbuffer_desc.SampleDesc.Count);
    }

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;
        swapchain.format = canonical_typed_color_format((DXGI_FORMAT)swapchain_create_info.format);

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        if (image_count == 0) {
            spdlog::error("[VR] OpenXR swapchain {} returned zero images.", i);
            return "OpenXR swapchain returned zero images.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            if (ctx.textures[j].texture != nullptr) {
                D3D12_RESOURCE_DESC image_desc{};
                if (try_get_resource_desc_nothrow(ctx.textures[j].texture, image_desc)) {
                    SPDLOG_INFO("[VR] OpenXR swapchain {} image {} tex {:x} desc: {}x{} fmt {} samples {} flags {:#x}",
                        i,
                        j,
                        (uintptr_t)ctx.textures[j].texture,
                        image_desc.Width,
                        image_desc.Height,
                        (uint32_t)image_desc.Format,
                        image_desc.SampleDesc.Count,
                        (uint32_t)image_desc.Flags);
                }
            }

            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        const bool is_depth_swapchain = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        if (!is_depth_swapchain) {
            for (uint32_t j = 0; j < image_count; ++j) {
                auto& texture_ctx = ctx.texture_contexts[j];
                if (texture_ctx == nullptr || ctx.textures[j].texture == nullptr) {
                    continue;
                }

                texture_ctx->texture = ctx.textures[j].texture;

                if (!texture_ctx->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                    SPDLOG_WARNING_EVERY_N_SEC(2,
                        "[VR] Failed to create persistent RTV for OpenXR swapchain {} image {}",
                        i,
                        j);
                }
            }
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, OPENXR_SWAPCHAIN_BASE_STATE);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = guarded_color_format;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = guarded_sample_count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = guarded_color_format;
    hmd_desc.SampleDesc.Count = guarded_sample_count;
    hmd_desc.SampleDesc.Quality = 0;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = guarded_color_format;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = guarded_color_format;
    desktop_rt_swapchain_create_info.width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_swapchain_create_info.height = g_framework->get_d3d12_rt_size().y;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = guarded_color_format;
    desktop_rt_desc.Width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_desc.Height = g_framework->get_d3d12_rt_size().y;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

    if (this->contexts.empty()) {
        return;
    }
    
    auto& vr = VR::get();
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

d3d12::TextureContext* D3D12Component::OpenXR::find_texture_context(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return nullptr;
    }

    std::scoped_lock _{this->mtx};

    for (auto& [_, ctx] : this->contexts) {
        const auto count = std::min(ctx.textures.size(), ctx.texture_contexts.size());
        for (size_t i = 0; i < count; ++i) {
            if (ctx.textures[i].texture == resource) {
                return ctx.texture_contexts[i].get();
            }
        }
    }

    return nullptr;
}

std::optional<uint32_t> D3D12Component::OpenXR::find_swapchain_index(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return std::nullopt;
    }

    std::scoped_lock _{this->mtx};

    for (auto& [swapchain_idx, ctx] : this->contexts) {
        for (auto& texture : ctx.textures) {
            if (texture.texture == resource) {
                return swapchain_idx;
            }
        }
    }

    return std::nullopt;
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box) 
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        if (texture_index >= ctx.texture_contexts.size() || texture_index >= ctx.textures.size()) {
            spdlog::error("[VR] OpenXR acquired invalid texture index {} for swapchain {} (contexts={}, textures={})",
                texture_index,
                swapchain_idx,
                ctx.texture_contexts.size(),
                ctx.textures.size());

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain.handle, &release_info);
            ctx.num_textures_acquired--;
            return;
        }

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& command_ctx = ctx.texture_contexts[texture_index]->commands;
            const auto guarded_wait_ms = (tq2_guard || ue57_guard) ? 2000u : INFINITE;
            command_ctx.wait(guarded_wait_ms);

            if ((tq2_guard || ue57_guard) && command_ctx.waiting_for_fence) {
                SPDLOG_WARNING_EVERY_N_SEC(1,
                    "[VR] OpenXR guarded path: command context '{}' wait timed out on swapchain {} image {}; skipping frame copy and recovering context.",
                    utility::narrow(command_ctx.internal_name),
                    swapchain_idx,
                    texture_index);

                const auto name = command_ctx.internal_name;
                command_ctx.reset();
                command_ctx.setup(name.c_str());

                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                const auto release_result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
                if (release_result != XR_SUCCESS) {
                    spdlog::error("[VR] xrReleaseSwapchainImage failed after wait-timeout recovery: {}", vr->m_openxr->get_result_string(release_result));
                }

                if (ctx.num_textures_acquired > 0) {
                    ctx.num_textures_acquired--;
                }

                return;
            }

            if (pre_commands) {
                (*pre_commands)(command_ctx, ctx.textures[texture_index].texture);
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                // Treat acquired OpenXR images as COMMON by default and perform explicit
                // transitions from that baseline for copy/clear/blit work.
                constexpr auto openxr_swapchain_dst_state = OPENXR_SWAPCHAIN_BASE_STATE;
                auto* const dst_resource = ctx.textures[texture_index].texture;

                if (resource == dst_resource) {
                    SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] OpenXR copy skipped: source and destination are identical (swapchain {} image {})", swapchain_idx, texture_index);
                } else {
                    D3D12_RESOURCE_DESC src_desc{};
                    D3D12_RESOURCE_DESC dst_desc{};
                    const bool has_src_desc = try_get_resource_desc_nothrow(resource, src_desc);
                    const bool has_dst_desc = try_get_resource_desc_nothrow(dst_resource, dst_desc);
                    auto* const dst_texture_ctx = ctx.texture_contexts[texture_index].get();
                    auto* const device = g_framework != nullptr && g_framework->get_d3d12_hook() != nullptr
                        ? g_framework->get_d3d12_hook()->get_device()
                        : nullptr;

                    auto do_copy_region = [&](D3D12_BOX box) {
                        if (box.right <= box.left || box.bottom <= box.top || box.back <= box.front) {
                            SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] OpenXR copy skipped: invalid copy box (swapchain {} image {})", swapchain_idx, texture_index);
                            return;
                        }

                        command_ctx.copy_region(
                            resource,
                            dst_resource,
                            &box,
                            src_state,
                            openxr_swapchain_dst_state);
                    };

                    if (src_box == nullptr) {
                        if (is_tq2_diag_enabled() && has_src_desc && has_dst_desc) {
                            SPDLOG_INFO_EVERY_N_SEC(
                                1,
                                "[VR][TQ2_DIAG] OpenXR copy swapchain={} image={} src={:x} {}x{} fmt={} -> dst={:x} {}x{} fmt={} src_state={:#x} dst_base={:#x}",
                                swapchain_idx,
                                texture_index,
                                (uintptr_t)resource,
                                src_desc.Width,
                                src_desc.Height,
                                (uint32_t)src_desc.Format,
                                (uintptr_t)dst_resource,
                                dst_desc.Width,
                                dst_desc.Height,
                                (uint32_t)dst_desc.Format,
                                (uint32_t)src_state,
                                (uint32_t)openxr_swapchain_dst_state);
                        }

                        const bool desc_match =
                            has_src_desc &&
                            has_dst_desc &&
                            src_desc.Dimension == dst_desc.Dimension &&
                            src_desc.Width == dst_desc.Width &&
                            src_desc.Height == dst_desc.Height &&
                            src_desc.DepthOrArraySize == dst_desc.DepthOrArraySize &&
                            src_desc.MipLevels == dst_desc.MipLevels &&
                            is_copy_format_compatible(src_desc.Format, dst_desc.Format) &&
                            src_desc.SampleDesc.Count == dst_desc.SampleDesc.Count;

                        if (has_src_desc && has_dst_desc &&
                            dst_texture_ctx != nullptr &&
                            try_openxr_blit_to_swapchain(
                                swapchain_idx,
                                texture_index,
                                device,
                                command_ctx,
                                resource,
                                *dst_texture_ctx,
                                src_desc,
                                dst_desc,
                                src_state,
                                src_box,
                                openxr_swapchain_dst_state))
                        {
                            goto openxr_copy_finalize;
                        }

                        if (desc_match) {
                            command_ctx.copy(
                                resource,
                                dst_resource,
                                src_state,
                                openxr_swapchain_dst_state);
                        } else if (has_src_desc && has_dst_desc) {
                            if (!is_copy_format_compatible(src_desc.Format, dst_desc.Format)) {
                                SPDLOG_WARNING_EVERY_N_SEC(
                                    1,
                                    "[VR] OpenXR copy skipped: incompatible formats for copy-region fallback (swapchain {} image {}): src fmt {} dst fmt {}",
                                    swapchain_idx,
                                    texture_index,
                                    (uint32_t)src_desc.Format,
                                    (uint32_t)dst_desc.Format);
                                goto openxr_copy_finalize;
                            }

                            D3D12_BOX safe_box{};
                            safe_box.left = 0;
                            safe_box.top = 0;
                            safe_box.front = 0;
                            safe_box.right = (UINT)std::min<uint64_t>(src_desc.Width, dst_desc.Width);
                            safe_box.bottom = std::min<uint32_t>((uint32_t)src_desc.Height, (uint32_t)dst_desc.Height);
                            safe_box.back = 1;

                            SPDLOG_INFO_EVERY_N_SEC(1,
                                "[VR] OpenXR copy using region fallback due desc mismatch (swapchain {} image {}): src {}x{} fmt {} -> dst {}x{} fmt {}",
                                swapchain_idx,
                                texture_index,
                                src_desc.Width,
                                src_desc.Height,
                                (uint32_t)src_desc.Format,
                                dst_desc.Width,
                                dst_desc.Height,
                                (uint32_t)dst_desc.Format);

                            do_copy_region(safe_box);
                        } else {
                            SPDLOG_WARNING_EVERY_N_SEC(1,
                                "[VR] OpenXR copy skipped: unable to read resource descriptors (swapchain {} image {})",
                                swapchain_idx,
                                texture_index);
                        }
                    } else if (has_src_desc && has_dst_desc) {
                        if (dst_texture_ctx != nullptr &&
                            try_openxr_blit_to_swapchain(
                                swapchain_idx,
                                texture_index,
                                device,
                                command_ctx,
                                resource,
                                *dst_texture_ctx,
                                src_desc,
                                dst_desc,
                                src_state,
                                src_box,
                                openxr_swapchain_dst_state))
                        {
                            goto openxr_copy_finalize;
                        }

                        if (!is_copy_format_compatible(src_desc.Format, dst_desc.Format)) {
                            SPDLOG_WARNING_EVERY_N_SEC(
                                1,
                                "[VR] OpenXR copy skipped: src_box path has incompatible formats (swapchain {} image {}): src fmt {} dst fmt {}",
                                swapchain_idx,
                                texture_index,
                                (uint32_t)src_desc.Format,
                                (uint32_t)dst_desc.Format);
                            goto openxr_copy_finalize;
                        }

                        D3D12_BOX safe_box = *src_box;
                        safe_box.left = std::min<UINT>(safe_box.left, (UINT)src_desc.Width);
                        safe_box.top = std::min<UINT>(safe_box.top, (UINT)src_desc.Height);
                        safe_box.front = 0;
                        safe_box.right = std::min<UINT>(safe_box.right, (UINT)src_desc.Width);
                        safe_box.bottom = std::min<UINT>(safe_box.bottom, (UINT)src_desc.Height);
                        safe_box.back = 1;

                        const auto src_width = safe_box.right > safe_box.left ? (safe_box.right - safe_box.left) : 0u;
                        const auto src_height = safe_box.bottom > safe_box.top ? (safe_box.bottom - safe_box.top) : 0u;
                        safe_box.right = safe_box.left + std::min<UINT>(src_width, (UINT)dst_desc.Width);
                        safe_box.bottom = safe_box.top + std::min<UINT>(src_height, (UINT)dst_desc.Height);

                        do_copy_region(safe_box);
                    } else {
                        SPDLOG_WARNING_EVERY_N_SEC(1,
                            "[VR] OpenXR copy skipped: src_box provided but descriptors unavailable (swapchain {} image {})",
                            swapchain_idx,
                            texture_index);
                    }
                }
            }

openxr_copy_finalize:
            if (additional_commands) {
                (*additional_commands)(command_ctx);
            }

            command_ctx.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                const auto guarded_wait_ms = (tq2_guard || ue57_guard) ? 2000u : INFINITE;
                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(guarded_wait_ms);
                    if ((tq2_guard || ue57_guard) && texture_ctx->commands.waiting_for_fence) {
                        SPDLOG_WARNING_EVERY_N_SEC(1,
                            "[VR] OpenXR guarded release recovery: context '{}' wait timed out; recreating context.",
                            utility::narrow(texture_ctx->commands.internal_name));
                        const auto name = texture_ctx->commands.internal_name;
                        texture_ctx->commands.reset();
                        texture_ctx->commands.setup(name.c_str());
                    }
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
            ctx.submitted_this_frame = true;
        }
    }
}
} // namespace vrmod
