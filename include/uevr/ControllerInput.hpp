// MIT licensed, like API.h and API.hpp.
#pragma once

#include "API.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <string>

namespace uevr::input {
inline const UEVR_VRData* functions(const UEVR_PluginInitializeParam* param) {
    // Check the version before reading any appended function pointer.
    return param && param->version && param->version->major == 2 && param->version->minor >= 41
        ? param->vr : nullptr;
}

inline std::string controller_type(const UEVR_PluginInitializeParam* param, UEVR_InputSourceHandle source) {
    const auto api = functions(param);
    const auto value = api && api->get_controller_type ? api->get_controller_type(source) : nullptr;
    return value ? value : "unknown";
}

inline std::string controller_profile(const UEVR_PluginInitializeParam* param, UEVR_InputSourceHandle source) {
    const auto api = functions(param);
    std::array<char, 512> buffer{};
    if (!api || !api->get_controller_profile) { return {}; }
    const auto length = api->get_controller_profile(source, buffer.data(), static_cast<unsigned int>(buffer.size()));
    return length < buffer.size() && buffer[length] == '\0' ? std::string{buffer.data(), length} : std::string{};
}

inline UEVR_DigitalInputState action_state(const UEVR_PluginInitializeParam* param,
    UEVR_ActionHandle action, UEVR_InputSourceHandle source)
{
    const auto api = functions(param);
    UEVR_DigitalInputState state{};
    if (api && api->get_action_state) { api->get_action_state(action, source, &state); }
    return state.active ? state : UEVR_DigitalInputState{};
}

inline UEVR_AnalogInputState action_axis(const UEVR_PluginInitializeParam* param,
    UEVR_ActionHandle action, UEVR_InputSourceHandle source)
{
    const auto api = functions(param);
    UEVR_AnalogInputState state{};
    if (api && api->get_action_axis) { api->get_action_axis(action, source, &state); }
    if (!state.active || !std::isfinite(state.value)) { return {}; }
    return {true, std::clamp(state.value, 0.0f, 1.0f)};
}
}
