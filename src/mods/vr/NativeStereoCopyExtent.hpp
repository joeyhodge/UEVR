#pragma once

#include <cstdint>

namespace uevr::native_stereo_copy {

enum class CopyExtentAction : uint8_t { unchanged, wait_for_scene, rebuild };

struct CopyExtent {
    bool scoped{};
    bool external_scene{};
    bool descriptor_valid{};
    uint64_t source_width{};
    uint32_t source_height{};
    uint32_t cached_width{};
    uint32_t cached_height{};
    uint32_t eye_width{};
    uint32_t eye_height{};
};

constexpr CopyExtentAction copy_extent_action(const CopyExtent& value) {
    if (!value.scoped) { return CopyExtentAction::unchanged; }
    if (!value.external_scene || !value.descriptor_valid ||
        value.eye_width == 0 || value.eye_width > 8192 ||
        value.eye_height == 0 || value.eye_height > 16384 ||
        value.source_width != uint64_t{value.eye_width} * 2 ||
        value.source_height != value.eye_height) {
        return CopyExtentAction::wait_for_scene;
    }
    return value.source_width == value.cached_width && value.source_height == value.cached_height
        ? CopyExtentAction::unchanged : CopyExtentAction::rebuild;
}

} // namespace uevr::native_stereo_copy
