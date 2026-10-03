#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uevr::ui_alpha {

struct Extent {
    uint32_t width{}, height{};
    bool operator==(const Extent&) const = default;
};
inline constexpr bool image_budget(Extent extent, uint32_t count) {
    return count > 0 && count <= 16 && extent.width > 0 && extent.width <= 8192 &&
        extent.height > 0 && extent.height <= 8192 &&
        uint64_t{extent.width} * extent.height * count * 4 <= 256ull * 1024 * 1024;
}

enum class Mode : uint32_t { unchanged, inspect, straight_to_premultiplied, encoded_premultiplied_to_linear };
enum class Status : uint8_t { off, waiting, inspecting, ready, active, unsupported, invalid_source, failed, gpu_busy };
enum class LayerAlpha : uint8_t { unknown, opaque, premultiplied, straight };

inline constexpr LayerAlpha layer_alpha(bool blends, bool unpremultiplied) {
    return !blends ? LayerAlpha::opaque : unpremultiplied ? LayerAlpha::straight : LayerAlpha::premultiplied;
}
inline constexpr const char* layer_alpha_text(LayerAlpha value) {
    switch (value) {
    case LayerAlpha::opaque: return "Opaque (source alpha ignored)";
    case LayerAlpha::premultiplied: return "Premultiplied alpha";
    case LayerAlpha::straight: return "Straight/unpremultiplied alpha";
    default: return "No layer observed in this mode yet";
    }
}
// One atomic observation keeps the original/submitted conventions coherent.
inline constexpr uint8_t layer_observation(LayerAlpha original, LayerAlpha submitted) {
    return static_cast<uint8_t>(original) | (static_cast<uint8_t>(submitted) << 4);
}
inline constexpr LayerAlpha original_alpha(uint8_t observation) { return static_cast<LayerAlpha>(observation & 15); }
inline constexpr LayerAlpha submitted_alpha(uint8_t observation) { return static_cast<LayerAlpha>(observation >> 4); }

inline constexpr Mode mode_from_config(std::string_view value) {
    return value.size() == 1 && value[0] >= '0' && value[0] <= '3'
        ? static_cast<Mode>(value[0] - '0') : Mode::unchanged;
}
inline constexpr bool valid(Mode mode) { return static_cast<uint32_t>(mode) <= 3; }
inline constexpr bool converts(Mode mode) {
    return mode == Mode::straight_to_premultiplied || mode == Mode::encoded_premultiplied_to_linear;
}
inline constexpr bool eligible(bool openxr, bool mono, bool dibr, bool transition, bool screen_2d) {
    return openxr && (mono || dibr) && !transition && !screen_2d;
}
inline constexpr const char* status_text(Status status) {
    switch (status) {
    case Status::off: return "Unchanged (no alpha processing)";
    case Status::waiting: return "Waiting for a fresh, supported UI image";
    case Status::inspecting: return "Inspecting source pixels; appearance unchanged";
    case Status::ready: return "Converted image ready; awaiting a matching alpha-blended layer";
    case Status::active: return "Converted image submitted with premultiplied-alpha flags";
    case Status::unsupported: return "Unchanged: requires OpenXR Mono/DIBR, without 2D/transition mode";
    case Status::invalid_source: return "Unchanged: source format, extent, or ownership did not validate";
    case Status::gpu_busy: return "Unchanged: prior GPU work is still in flight";
    default: return "Unchanged: optional alpha processing failed; change selection to retry";
    }
}

inline float decode_srgb(float x) {
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
inline float encode_srgb(float x) {
    return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

struct Sample {
    uint32_t pixels{}, transparent{}, translucent{}, opaque{};
    uint32_t transparent_rgb{}, exceeds_linear_alpha{}, exceeds_encoded_alpha{};
    uint64_t sequence{};
};

// Evidence only: additive colors and dark straight-alpha colors make automatic
// classification unsound. Never choose a conversion from these statistics.
inline Sample inspect_bgra(const void* pixels, size_t pitch, uint32_t width, uint32_t height) {
    Sample result{};
    if (!pixels || !width || !height || width > 64 || height > 64 || pitch < size_t{width} * 4) { return result; }
    const auto* data = static_cast<const uint8_t*>(pixels);
    constexpr float tolerance = 2.0f / 255.0f;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const auto* p = data + y * pitch + x * 4;
        const float a = p[3] / 255.0f;
        const float rgb = (std::max)({p[0], p[1], p[2]}) / 255.0f;
        ++result.pixels;
        if (p[3] == 0) { ++result.transparent; if (rgb > tolerance) { ++result.transparent_rgb; } }
        else if (p[3] == 255) { ++result.opaque; }
        else { ++result.translucent; }
        if (rgb > a + tolerance) { ++result.exceeds_encoded_alpha; }
        if (decode_srgb(rgb) > a + tolerance) { ++result.exceeds_linear_alpha; }
    }
    return result;
}

}
