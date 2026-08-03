#pragma once

#include <cstdint>

#include <sdk/EngineVersion.hpp>

namespace uevr::compat {
enum class SceneViewLayout : uint8_t {
    Legacy,
    UE56_57,
    UE58,
    UE60,
    Unsupported,
};

enum class SlateInputLayout : uint8_t {
    Legacy,
    UE55,
    UE58,
    UE60,
    Unsupported,
};

enum class CVarLayout : uint8_t {
    Legacy,
    UE57Plus,
    Unsupported,
};

enum class XRLayout : uint8_t {
    Legacy,
    UE58,
    UE60,
    Unsupported,
};

struct Profile {
    sdk::EngineVersion version{};
    bool source_validated{};
    bool ue57_or_newer{};
    bool ue58_or_newer{};
    bool uses_ue58_render_target_manager{};
    bool supports_scene_viewport_adoption{};
    bool supports_live_openxr_resize{};
    bool supports_ui_layer_pose{};
    bool supports_modern_viewport_slots{};
    bool supports_modern_slate_scan{};
    uint32_t command_list_root_offset{};
    SceneViewLayout scene_view_layout{SceneViewLayout::Unsupported};
    SlateInputLayout slate_input_layout{SlateInputLayout::Unsupported};
    CVarLayout cvar_layout{CVarLayout::Unsupported};
    XRLayout xr_layout{XRLayout::Unsupported};
};

constexpr Profile make_profile(sdk::EngineVersion version) noexcept {
    Profile result{.version = version};

    if (!version.valid()) {
        return result;
    }

    if (version.major == 4) {
        result.source_validated = true;
        result.supports_live_openxr_resize = version.minor == 27;
        result.scene_view_layout = SceneViewLayout::Legacy;
        result.slate_input_layout = SlateInputLayout::Legacy;
        result.cvar_layout = CVarLayout::Legacy;
        result.xr_layout = XRLayout::Legacy;
        return result;
    }

    if (version.major == 5) {
        // Preserve existing UE5 behavior. UE6 capabilities are deliberately
        // modeled separately so future UE6 minors cannot inherit them.
        result.source_validated = true;
        result.ue57_or_newer = version.minor >= 7;
        result.ue58_or_newer = version.minor >= 8;
        result.uses_ue58_render_target_manager = version.minor >= 8;
        result.supports_scene_viewport_adoption = version.minor >= 8;
        result.supports_live_openxr_resize = version.minor >= 3;
        result.supports_ui_layer_pose = version.minor >= 7;
        result.supports_modern_viewport_slots = version.minor >= 8;
        result.supports_modern_slate_scan = version.minor >= 8;
        result.command_list_root_offset = version.minor >= 8 ? 0x28 : 0;
        result.scene_view_layout = version.minor >= 8
            ? SceneViewLayout::UE58
            : (version.minor >= 6 ? SceneViewLayout::UE56_57 : SceneViewLayout::Legacy);
        result.slate_input_layout = version.minor >= 8
            ? SlateInputLayout::UE58
            : (version.minor >= 5 ? SlateInputLayout::UE55 : SlateInputLayout::Legacy);
        result.cvar_layout = version.minor >= 7 ? CVarLayout::UE57Plus : CVarLayout::Legacy;
        result.xr_layout = version.minor >= 8 ? XRLayout::UE58 : XRLayout::Legacy;
        return result;
    }

    if (version.is(6, 0)) {
        result.source_validated = true;
        result.ue57_or_newer = true;
        result.ue58_or_newer = true;
        result.uses_ue58_render_target_manager = true;
        result.supports_scene_viewport_adoption = true;
        result.supports_live_openxr_resize = true;
        result.supports_ui_layer_pose = true;
        result.supports_modern_viewport_slots = true;
        result.supports_modern_slate_scan = true;
        result.command_list_root_offset = 0x28;
        result.scene_view_layout = SceneViewLayout::UE60;
        result.slate_input_layout = SlateInputLayout::UE60;
        result.cvar_layout = CVarLayout::UE57Plus;
        result.xr_layout = XRLayout::UE60;
    }

    // UE6.1+ remains unsupported until a later source snapshot revalidates it.
    return result;
}

const Profile& get();
bool is_exact(uint16_t major, uint16_t minor) noexcept;
const char* scene_view_layout_name(SceneViewLayout layout) noexcept;
const char* slate_input_layout_name(SlateInputLayout layout) noexcept;
const char* cvar_layout_name(CVarLayout layout) noexcept;
const char* xr_layout_name(XRLayout layout) noexcept;

static_assert(make_profile({6, 0, 0, sdk::EngineVersionSource::Embedded}).source_validated);
static_assert(make_profile({6, 0, 0, sdk::EngineVersionSource::Embedded}).scene_view_layout == SceneViewLayout::UE60);
static_assert(make_profile({6, 0, 0, sdk::EngineVersionSource::Embedded}).slate_input_layout == SlateInputLayout::UE60);
static_assert(make_profile({6, 0, 0, sdk::EngineVersionSource::Embedded}).command_list_root_offset == 0x28);
static_assert(!make_profile({6, 1, 0, sdk::EngineVersionSource::Embedded}).source_validated);
}
