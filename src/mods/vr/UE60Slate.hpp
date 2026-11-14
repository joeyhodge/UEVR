#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace uevr::ue60 {
// Stock Win64 prefixes from UE6 main 6e797985 / 0a2ad85b. Never pass this
// normalized prefix back to Unreal: the full input and its tail remain owned by UE.
struct alignas(16) ColorSpace {
    double chromaticities[8];
    double rgb_to_xyz[16];
    double xyz_to_rgb[16];
    uint8_t is_srgb;
};
struct HDRMetadata {
    int32_t output_format;
    int32_t color_gamut;
    float minimum_nits;
    float maximum_nits;
    float full_frame_nits;
    float paper_white_nits;
    ColorSpace limiting_color_space;
    uint8_t hdr_supported;
};
struct IntPoint { int32_t x, y; };
struct IntRect { IntPoint min, max; };
struct SlateInputPrefixOctober {
    void* renderer;
    void* window_element_list;
    void* window;
    void* window_viewport;
    void* viewport_info;
    HDRMetadata hdr;
    IntPoint cursor;
    IntRect scene_rect;
    float ui_scale;
};
struct Screenshot {
    IntRect rect;
    void* viewport;
    void* colors;
    void* hdr_colors;
};
struct SlateInputPrefix {
    void* renderer;
    void* window_element_list;
    void* window;
    void* window_viewport;
    void* viewport_info;
    HDRMetadata hdr;
    Screenshot screenshot;
    IntPoint cursor;
    IntRect scene_rect;
    float ui_scale;
};

template<class Input>
inline bool valid_sdr_input(const Input& input, const void* renderer) noexcept {
    const auto& hdr = input.hdr;
    const auto valid_nits = [](float value) { return std::isfinite(value) && value >= 0 && value <= 100000; };
    const auto width = static_cast<int64_t>(input.scene_rect.max.x) - input.scene_rect.min.x;
    const auto height = static_cast<int64_t>(input.scene_rect.max.y) - input.scene_rect.min.y;
    return renderer != nullptr && input.renderer == renderer && input.window != nullptr &&
        input.window_element_list != nullptr && input.viewport_info != nullptr &&
        hdr.output_format >= 0 && hdr.output_format <= 2 && hdr.color_gamut >= 0 && hdr.color_gamut < 5 &&
        hdr.hdr_supported == 0 && valid_nits(hdr.minimum_nits) && valid_nits(hdr.maximum_nits) &&
        valid_nits(hdr.full_frame_nits) && valid_nits(hdr.paper_white_nits) &&
        hdr.maximum_nits >= hdr.minimum_nits && std::isfinite(input.ui_scale) &&
        (input.ui_scale == -1 || (input.ui_scale > 0 && input.ui_scale <= 16)) &&
        width > 0 && width <= 32768 && height > 0 && height <= 32768;
}

enum class InputLayout : uint8_t { october, november };
struct DecodedInput {
    SlateInputPrefixOctober prefix;
    InputLayout layout;
};
// Both snapshots report 6.0.0. The version alone must not choose offsets.
// Screenshot state is copied as opaque bytes, never dereferenced or modified.
inline std::optional<DecodedInput> decode_sdr_input(std::span<const uint8_t> bytes, const void* renderer) noexcept {
    std::optional<DecodedInput> decoded;
    SlateInputPrefixOctober october{};
    if (bytes.size() >= sizeof(october)) {
        std::memcpy(&october, bytes.data(), sizeof(october));
        if (valid_sdr_input(october, renderer)) { decoded = {october, InputLayout::october}; }
    }
    SlateInputPrefix november{};
    if (bytes.size() >= sizeof(november)) {
        std::memcpy(&november, bytes.data(), sizeof(november));
        if (valid_sdr_input(november, renderer)) {
            if (decoded) { return {}; }
            decoded = {{november.renderer, november.window_element_list, november.window, november.window_viewport,
                november.viewport_info, november.hdr, november.cursor, november.scene_rect, november.ui_scale}, InputLayout::november};
        }
    }
    return decoded;
}

static_assert(sizeof(void*) == 8);
static_assert(sizeof(ColorSpace) == 0x150 && alignof(ColorSpace) == 16);
static_assert(sizeof(HDRMetadata) == 0x180);
static_assert(offsetof(HDRMetadata, limiting_color_space) == 0x20);
static_assert(offsetof(HDRMetadata, hdr_supported) == 0x170);
static_assert(offsetof(SlateInputPrefix, viewport_info) == 0x20);
static_assert(offsetof(SlateInputPrefix, hdr) == 0x30);
static_assert(sizeof(Screenshot) == 0x28 && offsetof(SlateInputPrefix, screenshot) == 0x1b0);
static_assert(offsetof(SlateInputPrefix, cursor) == 0x1d8);
static_assert(offsetof(SlateInputPrefix, scene_rect) == 0x1e0);
static_assert(offsetof(SlateInputPrefix, ui_scale) == 0x1f0);
static_assert(sizeof(SlateInputPrefix) == 0x200);
static_assert(offsetof(SlateInputPrefixOctober, cursor) == 0x1b0);
static_assert(offsetof(SlateInputPrefixOctober, scene_rect) == 0x1b8);
static_assert(offsetof(SlateInputPrefixOctober, ui_scale) == 0x1c8);
static_assert(std::is_trivially_copyable_v<SlateInputPrefix>);
}
