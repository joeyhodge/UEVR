#pragma once

#include "UIAlphaPolicy.hpp"
#include <openxr/openxr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <optional>
#include <span>

namespace uevr::ui_composition {

using Extent = ui_alpha::Extent;
enum class Status : uint8_t { off, waiting, active, unsupported, busy, failed };
inline const char* status_text(Status s) {
    switch (s) {
    case Status::off:
        return "Original runtime UI layers";
    case Status::active:
        return "Per-eye UI active (after scene processing)";
    case Status::waiting:
        return "Original layers: waiting for fresh UI and valid eye poses";
    case Status::unsupported:
        return "Original layers: unsupported layer, space, format, or extent";
    case Status::busy:
        return "Original layers: optional GPU/XR resources busy";
    default:
        return "Original layers: optional composition failed; toggle to retry";
    }
}
inline bool enabled_config(std::string_view value) {
    return value == "1";
}
inline bool eligible(bool openxr, bool mono, bool dibr, bool transition, bool screen2d) {
    return openxr && (mono || dibr) && !transition && !screen2d;
}
inline bool budget(Extent eye, uint32_t count) {
    return eye.width && eye.width <= 4096 && eye.height && eye.height <= 8192 && count && count <= 16 &&
           uint64_t{eye.width} * eye.height * 2 * count * 4 <= 256ull * 1024 * 1024;
}
inline bool valid_pose(const XrPosef& p) {
    const auto& q = p.orientation;
    const auto& v = p.position;
    const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(norm) && std::abs(norm - 1.0f) < 0.002f && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::abs(v.x) < 1e6f && std::abs(v.y) < 1e6f && std::abs(v.z) < 1e6f;
}
inline glm::quat rotation(const XrPosef& p) {
    return glm::normalize(glm::quat{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z});
}
inline glm::vec3 position(const XrPosef& p) {
    return {p.position.x, p.position.y, p.position.z};
}
inline XrPosef relative_pose(const XrPosef& base, const XrPosef& world) {
    const auto q = glm::inverse(rotation(base));
    const auto r = q * rotation(world);
    const auto p = q * (position(world) - position(base));
    return {{r.x, r.y, r.z, r.w}, {p.x, p.y, p.z}};
}
inline bool valid_fov(const XrFovf& f) {
    const std::array a{f.angleLeft, f.angleRight, f.angleDown, f.angleUp};
    for (float v : a) {
        if (!std::isfinite(v) || std::abs(v) >= 1.56f) {
            return false;
        }
    }
    return f.angleRight - f.angleLeft > 0.001f && f.angleUp - f.angleDown > 0.001f;
}

struct Draw {
    std::array<std::array<float, 4>, 4> corners{};
    uint32_t input{}, eye{};
};
// Quad corners are projected in the ORIGINAL layer space, using actual XR eyes,
// never Mono's centered scene pose or DIBR scene/depth coordinates.
inline std::optional<Draw> project_quad(const XrCompositionLayerQuad& q, const XrView& eye, uint32_t index) {
    if (!valid_pose(q.pose) || !valid_pose(eye.pose) || !valid_fov(eye.fov) || !std::isfinite(q.size.width) ||
        !std::isfinite(q.size.height) || q.size.width <= 0 || q.size.height <= 0 || q.size.width > 100 || q.size.height > 100 ||
        index > 1) {
        return {};
    }
    const auto inverse_eye = glm::inverse(rotation(eye.pose));
    const auto local_eye = glm::inverse(rotation(q.pose)) * (position(eye.pose) - position(q.pose));
    Draw draw{};
    draw.eye = index;
    // OpenXR quads are front-facing only. Degenerate triangles draw no pixels.
    if (local_eye.z <= 0) {
        return draw;
    }
    const float l = std::tan(eye.fov.angleLeft), r = std::tan(eye.fov.angleRight);
    const float b = std::tan(eye.fov.angleDown), t = std::tan(eye.fov.angleUp);
    for (size_t i = 0; i < 4; ++i) {
        const glm::vec3 local{(i & 1 ? 0.5f : -0.5f) * q.size.width, (i & 2 ? -0.5f : 0.5f) * q.size.height, 0};
        const auto v = inverse_eye * (rotation(q.pose) * local + position(q.pose) - position(eye.pose));
        const float d = -v.z;
        draw.corners[i] = {(2 * v.x - (r + l) * d) / (r - l), (2 * v.y - (t + b) * d) / (t - b), 0.5f * d, d};
        for (float c : draw.corners[i]) {
            if (!std::isfinite(c)) {
                return {};
            }
        }
    }
    return draw;
}

inline bool supported_quad(const XrCompositionLayerBaseHeader& layer, XrSwapchain handle, Extent extent) {
    if (layer.type != XR_TYPE_COMPOSITION_LAYER_QUAD || layer.next || !handle ||
        layer.layerFlags != XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) {
        return false;
    }
    const auto& q = reinterpret_cast<const XrCompositionLayerQuad&>(layer);
    return q.eyeVisibility == XR_EYE_VISIBILITY_BOTH && q.subImage.swapchain == handle && q.subImage.imageArrayIndex == 0 &&
           q.subImage.imageRect.offset.x == 0 && q.subImage.imageRect.offset.y == 0 && q.subImage.imageRect.extent.width > 0 &&
           q.subImage.imageRect.extent.height > 0 && static_cast<uint32_t>(q.subImage.imageRect.extent.width) == extent.width &&
           static_cast<uint32_t>(q.subImage.imageRect.extent.height) == extent.height;
}
} // namespace uevr::ui_composition
