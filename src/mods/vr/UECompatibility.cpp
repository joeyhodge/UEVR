#include "UECompatibility.hpp"

#include <spdlog/spdlog.h>

namespace uevr::compat {
const Profile& get() {
    static const Profile profile = make_profile(sdk::get_engine_version());
    static const bool logged = [&]() {
        SPDLOG_INFO(
            "[UECompat] version={}.{}.{} source={} validated={} scene={} slate={} cvar={} xr={} rt_manager_ue58={} command_root=0x{:X}",
            profile.version.major,
            profile.version.minor,
            profile.version.patch,
            sdk::engine_version_source_name(profile.version.source),
            profile.source_validated,
            scene_view_layout_name(profile.scene_view_layout),
            slate_input_layout_name(profile.slate_input_layout),
            cvar_layout_name(profile.cvar_layout),
            xr_layout_name(profile.xr_layout),
            profile.uses_ue58_render_target_manager,
            profile.command_list_root_offset);

        if (profile.version.major == 6 && !profile.source_validated) {
            SPDLOG_ERROR(
                "[UECompat] UE {}.{} is not source-validated; UE6 ABI-sensitive paths will fail closed",
                profile.version.major,
                profile.version.minor);
        }

        return true;
    }();
    (void)logged;
    return profile;
}

bool is_exact(uint16_t major, uint16_t minor) noexcept {
    return get().version.is(major, minor);
}

const char* scene_view_layout_name(SceneViewLayout layout) noexcept {
    switch (layout) {
    case SceneViewLayout::Legacy: return "legacy";
    case SceneViewLayout::UE56_57: return "UE56_57";
    case SceneViewLayout::UE58: return "UE58";
    case SceneViewLayout::UE60: return "UE60";
    default: return "unsupported";
    }
}

const char* slate_input_layout_name(SlateInputLayout layout) noexcept {
    switch (layout) {
    case SlateInputLayout::Legacy: return "legacy";
    case SlateInputLayout::UE55: return "UE55";
    case SlateInputLayout::UE58: return "UE58";
    case SlateInputLayout::UE60: return "UE60";
    default: return "unsupported";
    }
}

const char* cvar_layout_name(CVarLayout layout) noexcept {
    switch (layout) {
    case CVarLayout::Legacy: return "legacy";
    case CVarLayout::UE57Plus: return "UE57+";
    default: return "unsupported";
    }
}

const char* xr_layout_name(XRLayout layout) noexcept {
    switch (layout) {
    case XRLayout::Legacy: return "legacy";
    case XRLayout::UE58: return "UE58";
    case XRLayout::UE60: return "UE60";
    default: return "unsupported";
    }
}
}
