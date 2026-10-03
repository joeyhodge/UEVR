#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace uevr::ue60 {
// Stock Win64 ABI from UE6 main 6e797985, 2026-10-03. Never pass this
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
struct SlateInputPrefix {
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

inline bool valid_sdr_input(const SlateInputPrefix& input, const void* renderer) noexcept {
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
        input.ui_scale > 0 && input.ui_scale <= 16 && width > 0 && width <= 32768 && height > 0 && height <= 32768;
}

static_assert(sizeof(void*) == 8);
static_assert(sizeof(ColorSpace) == 0x150 && alignof(ColorSpace) == 16);
static_assert(sizeof(HDRMetadata) == 0x180);
static_assert(offsetof(HDRMetadata, limiting_color_space) == 0x20);
static_assert(offsetof(HDRMetadata, hdr_supported) == 0x170);
static_assert(offsetof(SlateInputPrefix, viewport_info) == 0x20);
static_assert(offsetof(SlateInputPrefix, hdr) == 0x30);
static_assert(offsetof(SlateInputPrefix, cursor) == 0x1b0);
static_assert(offsetof(SlateInputPrefix, scene_rect) == 0x1b8);
static_assert(offsetof(SlateInputPrefix, ui_scale) == 0x1c8);
static_assert(std::is_trivially_copyable_v<SlateInputPrefix>);
}
