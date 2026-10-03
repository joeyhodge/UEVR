#pragma once

#include <openxr/openxr.h>
#include <optional>
#include <cstdint>
#include "UIAlphaPolicy.hpp"

namespace uevr::ui_alpha {

// A timeout keeps the same acquired image pending. It must not be released or
// reacquired until a successful wait. Injectable calls keep this offline-testable.
class ImageLease {
public:
    template<class Acquire, class Wait>
    std::optional<uint32_t> acquire(uint32_t count, Acquire&& acquire, Wait&& wait) {
        if (m_failed || count == 0) { return {}; }
        if (!m_acquired) {
            const auto result = acquire(m_index);
            if (result != XR_SUCCESS || m_index >= count) { m_failed = true; return {}; }
            m_acquired = true;
        }
        if (!m_waited) {
            const auto result = wait();
            if (result == XR_TIMEOUT_EXPIRED) { return {}; }
            if (result != XR_SUCCESS) { m_failed = true; return {}; }
            m_waited = true;
        }
        return m_index;
    }
    template<class Release> bool release(Release&& release) {
        if (m_failed || !m_waited || release() != XR_SUCCESS) { m_failed = true; return false; }
        m_waited = m_acquired = false;
        return true;
    }
    bool failed() const { return m_failed; }
private:
    uint32_t m_index{};
    bool m_acquired{}, m_waited{}, m_failed{};
};

inline bool replace_sub_image(XrCompositionLayerBaseHeader& layer, XrSwapchain original,
    const XrSwapchainSubImage& replacement, Extent logical) {
    XrSwapchainSubImage* image{};
    XrEyeVisibility eye{};
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        auto& quad = reinterpret_cast<XrCompositionLayerQuad&>(layer);
        image = &quad.subImage; eye = quad.eyeVisibility;
    } else if (layer.type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
        auto& cylinder = reinterpret_cast<XrCompositionLayerCylinderKHR&>(layer);
        image = &cylinder.subImage; eye = cylinder.eyeVisibility;
    }
    if (!image || eye != XR_EYE_VISIBILITY_BOTH || image->swapchain != original ||
        image->imageArrayIndex != 0 || image->imageRect.offset.x || image->imageRect.offset.y ||
        image->imageRect.extent.width <= 0 || image->imageRect.extent.height <= 0 ||
        static_cast<uint32_t>(image->imageRect.extent.width) != logical.width ||
        static_cast<uint32_t>(image->imageRect.extent.height) != logical.height ||
        original == XR_NULL_HANDLE || replacement.swapchain == XR_NULL_HANDLE) { return false; }
    *image = replacement;
    return true;
}

}
