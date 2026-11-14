#pragma once

#include "Pre57SlateDiscovery.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <d3d12.h>
#include <optional>
#include <string_view>

namespace uevr::pre57_slate {
inline bool pointer_value(uintptr_t value) {
    return value >= 0x10000 && value < 0x0000800000000000ull && (value & 7) == 0;
}

struct FunctionRange {
    uintptr_t begin{}, end{}, image_base{};
    bool operator==(const FunctionRange&) const = default;
};

template <class Read, class Executable, class OwnerAt>
ScanResult discover_raw_output(uintptr_t draw, ImageRange image, std::span<const uint8_t> original_entry, CodeContract contract,
    Read&& read, Executable&& executable, OwnerAt&& owner_at) {
    const auto owner = owner_at(draw);
    if (!owner || owner->begin != draw || owner->image_base != image.base || owner->end <= draw || owner->end - draw > 0x10000 ||
        !image.contains(draw, owner->end - draw) || !executable(draw, owner->end - draw)) {
        return {ScanState::InvalidRange, {}};
    }
    std::vector<uint8_t> code(owner->end - draw);
    if (!read(draw, code.data(), code.size())) {
        return {ScanState::InvalidCode, {}};
    }
    auto result = find_raw_registration(code, draw, image, original_entry, contract, read, executable);
    if (result.match) {
        const auto callee = owner_at(result.match->target);
        if (!callee || callee->begin != result.match->target || callee->image_base != image.base || callee->end <= callee->begin ||
            callee->end - callee->begin != contract.code.size()) {
            return {ScanState::InvalidRange, {}};
        }
    }
    return result;
}

enum class EngineContract { Unsupported, Immediate54, RDG55_56 };
inline EngineContract engine_contract(std::wstring_view version, uint32_t file_ms) {
    unsigned major{}, minor{};
    if (version.empty() || version == L"0.00") {
        major = file_ms >> 16;
        minor = file_ms & 0xffff;
    } else {
        size_t i{};
        for (; i < version.size() && version[i] >= L'0' && version[i] <= L'9'; ++i) {
            major = major * 10 + version[i] - L'0';
            if (major > 100) {
                return EngineContract::Unsupported;
            }
        }
        if (!i || i == version.size() || version[i++] != L'.') {
            return EngineContract::Unsupported;
        }
        const auto start = i;
        for (; i < version.size() && version[i] >= L'0' && version[i] <= L'9'; ++i) {
            minor = minor * 10 + version[i] - L'0';
            if (minor > 100) {
                return EngineContract::Unsupported;
            }
        }
        if (i == start || (i < version.size() && version[i] != L'.' && version[i] != L'-' && version[i] != L'+')) {
            return EngineContract::Unsupported;
        }
    }
    if (major != 5) {
        return EngineContract::Unsupported;
    }
    if (minor == 4) {
        return EngineContract::Immediate54;
    }
    return minor == 5 || minor == 6 ? EngineContract::RDG55_56 : EngineContract::Unsupported;
}

enum class ObservationState {
    Off,
    Unsupported,
    Legacy54,
    WaitingDraw,
    Discovered,
    Observing,
    EngineOwnedCandidate,
    SyntheticRequired,
    Rejected
};
inline const char* name(ObservationState state) {
    switch (state) {
    case ObservationState::Off:
        return "off";
    case ObservationState::Unsupported:
        return "unsupported engine/RHI";
    case ObservationState::Legacy54:
        return "UE5.4 immediate path preserved";
    case ObservationState::WaitingDraw:
        return "waiting for DrawWindow";
    case ObservationState::Discovered:
        return "raw registration discovered; not authorized to redirect";
    case ObservationState::Observing:
        return "observing; window/ownership not established";
    case ObservationState::EngineOwnedCandidate:
        return "distinct engine texture; ownership/copy proof still required";
    case ObservationState::SyntheticRequired:
        return "scene alias; a separate owned-target adapter is required";
    default:
        return "rejected; original UI routing preserved";
    }
}

struct TextureFacts {
    uintptr_t texture{}, native{}, device{}, expected_device{}, scene_texture{}, scene_native{};
    D3D12_RESOURCE_DESC desc{};
};
inline bool supported_color(const D3D12_RESOURCE_DESC& desc) {
    // Existing pre-5.7 UI copies use a BGRA view, not UE5.8's conversion ring.
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width > 0 && desc.Width <= 16384 && desc.Height > 0 &&
           desc.Height <= 16384 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
           desc.SampleDesc.Quality == 0 && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
           (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
           !(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) &&
           (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}
inline bool same_descriptor(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout &&
           a.Flags == b.Flags;
}
inline ObservationState classify(const TextureFacts& facts) {
    if (!pointer_value(facts.texture) || !pointer_value(facts.native) || !pointer_value(facts.device) ||
        !pointer_value(facts.expected_device) || facts.device != facts.expected_device || !supported_color(facts.desc)) {
        return ObservationState::Rejected;
    }
    if (!facts.scene_texture || !facts.scene_native) {
        return ObservationState::Observing;
    }
    if (!pointer_value(facts.scene_texture) || !pointer_value(facts.scene_native)) {
        return ObservationState::Rejected;
    }
    if (facts.texture == facts.scene_texture || facts.native == facts.scene_native) {
        return ObservationState::SyntheticRequired;
    }
    return ObservationState::EngineOwnedCandidate;
}

struct Epoch {
    uintptr_t module{}, draw{}, renderer{}, window{}, viewport{}, scene{}, device{};
    Extent extent{};
    uint32_t rendering_method{};
    bool operator==(const Epoch&) const = default;
};

// Observations contain values only, never a reusable engine or native pointer.
// A ref to FRHITexture alone cannot pin an unknown FRenderTargetPool allocation's
// role/content. This classifier therefore has no Ready state or publishing API.
struct Observation {
    Epoch epoch{};
    TextureFacts facts{};
    ObservationState state{ObservationState::Off};
    uint64_t observed_ms{};
    uint32_t stable{};
    bool known_adapter{};
    void reset(ObservationState next = ObservationState::Off) {
        *this = {};
        state = next;
    }
    void observe(Epoch current, TextureFacts values, uint64_t now) {
        const auto next = classify(values);
        const bool same = epoch == current && facts.texture == values.texture && facts.native == values.native &&
                          facts.scene_native == values.scene_native && same_descriptor(facts.desc, values.desc) && state == next &&
                          now >= observed_ms && now - observed_ms <= 2500;
        stable = same && stable < 1000000 ? stable + 1 : 1;
        epoch = current;
        facts = values;
        observed_ms = now;
        state = next;
        if (next == ObservationState::Rejected) {
            stable = 0;
        }
    }
};

struct InputPrefix {
    uintptr_t renderer{}, element_list{}, window{}, viewport{};
    uintptr_t post_process_data{}, post_process_count{};
    int32_t cursor_x{}, cursor_y{}, min_x{}, min_y{}, max_x{}, max_y{};
    float ui_scale{};
};
static_assert(sizeof(InputPrefix) == 0x50 && offsetof(InputPrefix, min_x) == 0x38);

struct MainWindow {
    uintptr_t window{}, viewport{};
    bool retained{};
};
struct WindowInput {
    uintptr_t address{};
    InputPrefix input{};
    Extent extent{};
};

// The caller must prove the FULL element stride and pin the main SWindow and
// viewport, not infer them from this prefix or select the first readable window.
template <class Read>
std::optional<WindowInput> select_main_window(
    uintptr_t data, int32_t count, size_t compiled_stride, MainWindow main, uintptr_t renderer, Read&& read) {
    if (!main.retained || !pointer_value(main.window) || !pointer_value(main.viewport) || !pointer_value(renderer) ||
        !pointer_value(data) || count < 1 || count > 16 || compiled_stride < sizeof(InputPrefix) || compiled_stride > 0x400 ||
        (compiled_stride & 7) || data >= 0x0000800000000000ull - size_t(count) * compiled_stride) {
        return {};
    }
    std::optional<WindowInput> result;
    for (int32_t i = 0; i < count; ++i) {
        const auto address = data + size_t(i) * compiled_stride;
        InputPrefix input{};
        if (!read(address, &input, sizeof(input))) {
            return {};
        }
        if (input.window != main.window || input.viewport != main.viewport) {
            continue;
        }
        const auto extent = checked_extent(input.min_x, input.min_y, input.max_x, input.max_y);
        if (input.renderer != renderer || !input.element_list || !extent || !std::isfinite(input.ui_scale) ||
            input.post_process_count > 256 || (input.post_process_count && !input.post_process_data) || result) {
            return {};
        }
        result = WindowInput{address, input, *extent};
    }
    return result;
}
} // namespace uevr::pre57_slate
