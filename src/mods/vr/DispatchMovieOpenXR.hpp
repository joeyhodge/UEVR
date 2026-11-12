#pragma once
#include "DispatchMoviePolicy.hpp"
#include <openxr/openxr.h>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uevr::dispatch_movie {
// An XR timeout retains the SAME acquisition. No writes/releases until a
// successful wait; no new acquisition until the prior one is released.
struct ImageAccess {
    uint32_t index{};
    bool acquired{}, writable{}, release_pending{}, failed{};
    XrResult error{XR_SUCCESS};
    bool wait(XrSwapchain handle) {
        if (failed || !acquired) { return false; }
        if (writable) { return true; }
        XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        info.timeout=0;
        error=xrWaitSwapchainImage(handle,&info);
        if (error == XR_TIMEOUT_EXPIRED) { return false; }
        writable=error == XR_SUCCESS;
        failed=!writable;
        return writable;
    }
    bool acquire(XrSwapchain handle) {
        if (failed || release_pending) { return false; }
        if (!acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            error=xrAcquireSwapchainImage(handle,&info,&index);
            if (error != XR_SUCCESS) { failed=true; return false; }
            acquired=true;
        }
        return wait(handle);
    }
    bool release(XrSwapchain handle) {
        if (failed || !acquired || !writable) { return false; }
        release_pending=true;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        error=xrReleaseSwapchainImage(handle,&info);
        if (error != XR_SUCCESS) { return false; }
        acquired=writable=release_pending=false;
        return true;
    }
    template<class Retired> bool retire(XrSwapchain handle, Retired&& retired) {
        if (!acquired) { return true; }
        return wait(handle) && retired() && release(handle);
    }
};
template<class Image> struct Images {
    XrSwapchain handle{XR_NULL_HANDLE};
    std::vector<Image> images;
    Images()=default;
    Images(const Images&)=delete;
    Images(Images&& other) noexcept : handle{std::exchange(other.handle,XR_NULL_HANDLE)}, images{std::move(other.images)} {}
    ~Images() { if (handle != XR_NULL_HANDLE) { xrDestroySwapchain(handle); } }
};
template<class Image> std::optional<Images<Image>> create_images(XrSession session, uint32_t width, uint32_t height,
    XrStructureType image_type, int64_t format) {
    if (session == XR_NULL_HANDLE || !extent(width,height)) { return {}; }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags=XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format=format; info.width=width; info.height=height;
    info.sampleCount=info.faceCount=info.arraySize=info.mipCount=1;
    Images<Image> result;
    if (XR_FAILED(xrCreateSwapchain(session,&info,&result.handle))) { return {}; }
    uint32_t count{};
    if (XR_FAILED(xrEnumerateSwapchainImages(result.handle,0,&count,nullptr)) || !count || count > 64) { return {}; }
    result.images.resize(count);
    for (auto& image:result.images) { image={image_type}; }
    const auto capacity=count;
    if (XR_FAILED(xrEnumerateSwapchainImages(result.handle,capacity,&count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(result.images.data())))) { return {}; }
    if (!count || count > capacity) { return {}; }
    result.images.resize(count);
    return result;
}
inline bool opaque_layer(std::span<XrCompositionLayerBaseHeader* const> layers, XrSpace stage,
    XrSwapchain swapchain, uint32_t width, uint32_t height) noexcept {
    if (stage == XR_NULL_HANDLE || swapchain == XR_NULL_HANDLE || !extent(width,height)) { return false; }
    for (const auto* base:layers) {
        if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_QUAD) { continue; }
        const auto& layer=*reinterpret_cast<const XrCompositionLayerQuad*>(base);
        if (layer.subImage.swapchain != swapchain) { continue; }
        return layer.space == stage && !layer.next && !layer.layerFlags && layer.eyeVisibility == XR_EYE_VISIBILITY_BOTH &&
            layer.subImage.imageArrayIndex == 0 && layer.subImage.imageRect.offset.x == 0 && layer.subImage.imageRect.offset.y == 0 &&
            layer.subImage.imageRect.extent.width == width && layer.subImage.imageRect.extent.height == height &&
            std::isfinite(layer.size.width) && std::isfinite(layer.size.height) && layer.size.width > 0 && layer.size.height > 0 &&
            std::isfinite(layer.pose.position.x) && std::isfinite(layer.pose.position.y) && std::isfinite(layer.pose.position.z) &&
            std::isfinite(layer.pose.orientation.x) && std::isfinite(layer.pose.orientation.y) &&
            std::isfinite(layer.pose.orientation.z) && std::isfinite(layer.pose.orientation.w) &&
            std::abs(layer.pose.orientation.x*layer.pose.orientation.x+layer.pose.orientation.y*layer.pose.orientation.y+
                layer.pose.orientation.z*layer.pose.orientation.z+layer.pose.orientation.w*layer.pose.orientation.w-1.0f) < 0.01f;
    }
    return false;
}
}
