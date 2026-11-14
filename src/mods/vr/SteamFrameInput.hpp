#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uevr::steam_frame {
inline constexpr char extension[] = "XR_VALVE_frame_controller_interaction";
inline constexpr char interaction_profile[] = "/interaction_profiles/valve/frame_controller_valve";
inline constexpr char controller_type[] = "frame_controller";
inline constexpr char binding_file[] = "bindings_frame_controller.json";

inline std::vector<std::string> supported_profiles(std::span<const std::string> legacy, bool extension_enabled) {
    std::vector<std::string> profiles{legacy.begin(), legacy.end()};
    std::erase(profiles, std::string{interaction_profile});
    if (extension_enabled) { profiles.emplace_back(interaction_profile); }
    return profiles;
}

constexpr bool is_frame_controller(std::string_view type) {
    return type == controller_type;
}

inline const char* normalized_controller_type(std::string_view profile);

class ControllerProfileCache {
public:
    uint8_t mask() const { return static_cast<uint8_t>(m_state.load(std::memory_order_acquire) & 3u); }

    void invalidate() {
        auto current = m_state.load(std::memory_order_acquire);
        while (!m_state.compare_exchange_weak(current, ((current & ~7ull) + 8u) | 4u,
            std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }

    std::optional<uint64_t> begin_refresh() const {
        const auto current = m_state.load(std::memory_order_acquire);
        return (current & 4u) != 0 ? std::optional<uint64_t>{current} : std::nullopt;
    }

    bool publish(uint64_t observed, uint8_t mask, bool retry, std::array<std::string, 2> profiles = {}) {
        std::scoped_lock lock{m_identity_mutex};
        const auto next = (observed & ~7ull) | (mask & 3u) | (retry ? 4u : 0u);
        if (!m_state.compare_exchange_strong(observed, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        m_identity_epoch = next & ~7ull;
        m_profiles = std::move(profiles);
        return true;
    }

    std::string profile(unsigned hand) const {
        if (hand >= 2) { return {}; }
        std::scoped_lock lock{m_identity_mutex};
        const auto state = m_state.load(std::memory_order_acquire);
        return (state & ~7ull) == m_identity_epoch ? m_profiles[hand] : std::string{};
    }

    const char* type(unsigned hand) const {
        if (hand >= 2) { return "unknown"; }
        std::scoped_lock lock{m_identity_mutex};
        const auto state = m_state.load(std::memory_order_acquire);
        return (state & ~7ull) == m_identity_epoch ? normalized_controller_type(m_profiles[hand]) : "unknown";
    }

private:
    // A reconnect/profile event cannot be overwritten by an in-flight property read.
    std::atomic<uint64_t> m_state{4u}; // epoch, refresh-needed bit, physical hand mask
    mutable std::mutex m_identity_mutex;
    uint64_t m_identity_epoch{};
    std::array<std::string, 2> m_profiles{};
};

inline const char* normalized_controller_type(std::string_view profile) {
    if (profile == controller_type || profile == interaction_profile) { return "frame"; }
    if (profile == "oculus_touch" || profile == "/interaction_profiles/oculus/touch_controller" ||
        profile == "/interaction_profiles/meta/touch_controller_plus") { return "touch"; }
    if (profile == "knuckles" || profile == "/interaction_profiles/valve/index_controller") { return "index"; }
    if (profile == "vive_controller" || profile == "/interaction_profiles/htc/vive_controller") { return "vive"; }
    if (profile == "holographic_controller" || profile == "/interaction_profiles/microsoft/motion_controller") { return "wmr"; }
    if (profile == "/interaction_profiles/khr/simple_controller") { return "simple"; }
    return "unknown";
}

struct Binding {
    std::string_view path;
    std::string_view action;
};

// UEVR's historical face-action names describe its logical gamepad mapping:
// AButtonRight=A, AButtonLeft=B, BButtonRight=X, BButtonLeft=Y.
inline constexpr std::array openxr_bindings{
    Binding{"/user/hand/right/input/a/click", "abuttonright"},
    Binding{"/user/hand/right/input/b/click", "abuttonleft"},
    Binding{"/user/hand/right/input/x/click", "bbuttonright"},
    Binding{"/user/hand/right/input/y/click", "bbuttonleft"},
    Binding{"/user/hand/right/input/a/touch", "abuttontouchright"},
    Binding{"/user/hand/right/input/b/touch", "abuttontouchleft"},
    Binding{"/user/hand/right/input/x/touch", "bbuttontouchright"},
    Binding{"/user/hand/right/input/y/touch", "bbuttontouchleft"},
    Binding{"/user/hand/left/input/trigger/touch", "triggertouch"},
    Binding{"/user/hand/right/input/trigger/touch", "triggertouch"},
    Binding{"/user/hand/left/input/squeeze/touch", "griptouch"},
    Binding{"/user/hand/right/input/squeeze/touch", "griptouch"},
    Binding{"/user/hand/left/input/bumper/touch", "bumpertouch"},
    Binding{"/user/hand/right/input/bumper/touch", "bumpertouch"},
    Binding{"/user/hand/left/input/thumbstick/touch", "joysticktouch"},
    Binding{"/user/hand/right/input/thumbstick/touch", "joysticktouch"},
    Binding{"/user/hand/left/input/dpad_up/touch", "dpad_uptouch"},
    Binding{"/user/hand/left/input/dpad_right/touch", "dpad_righttouch"},
    Binding{"/user/hand/left/input/dpad_down/touch", "dpad_downtouch"},
    Binding{"/user/hand/left/input/dpad_left/touch", "dpad_lefttouch"},
    Binding{"/user/hand/left/input/view/touch", "backbuttontouch"},
    Binding{"/user/hand/right/input/menu/touch", "startbuttontouch"},
    Binding{"/user/hand/left/input/dpad_up/click", "dpad_up"},
    Binding{"/user/hand/left/input/dpad_right/click", "dpad_right"},
    Binding{"/user/hand/left/input/dpad_down/click", "dpad_down"},
    Binding{"/user/hand/left/input/dpad_left/click", "dpad_left"},
    Binding{"/user/hand/left/input/view/click", "backbutton"},
    Binding{"/user/hand/right/input/menu/click", "startbutton"},
    Binding{"/user/hand/left/input/bumper/click", "bumper"},
    Binding{"/user/hand/right/input/bumper/click", "bumper"},
    Binding{"/user/hand/left/input/trigger/click", "trigger"},
    Binding{"/user/hand/right/input/trigger/click", "trigger"},
    Binding{"/user/hand/left/input/trigger/value", "triggeraxis"},
    Binding{"/user/hand/right/input/trigger/value", "triggeraxis"},
    Binding{"/user/hand/left/input/squeeze/click", "grip"},
    Binding{"/user/hand/right/input/squeeze/click", "grip"},
    Binding{"/user/hand/left/input/squeeze/value", "squeeze"},
    Binding{"/user/hand/right/input/squeeze/value", "squeeze"},
    Binding{"/user/hand/left/input/thumbstick", "joystick"},
    Binding{"/user/hand/right/input/thumbstick", "joystick"},
    Binding{"/user/hand/left/input/thumbstick/click", "joystickclick"},
    Binding{"/user/hand/right/input/thumbstick/click", "joystickclick"},
    Binding{"/user/hand/left/input/grip/pose", "grippose"},
    Binding{"/user/hand/right/input/grip/pose", "grippose"},
    Binding{"/user/hand/left/input/aim/pose", "pose"},
    Binding{"/user/hand/right/input/aim/pose", "pose"},
    Binding{"/user/hand/left/output/haptic", "haptic"},
    Binding{"/user/hand/right/output/haptic", "haptic"},
};

template<class Value, class Resolve, class Submit>
bool suggest_native_bindings(bool extension_enabled, Resolve&& resolve, Submit&& submit) {
    if (!extension_enabled) { return false; }
    std::vector<Value> candidate;
    candidate.reserve(openxr_bindings.size());
    for (const auto& binding : openxr_bindings) {
        const auto value = resolve(binding);
        if (!value) { return false; }
        candidate.push_back(*value);
    }
    return submit(candidate);
}

struct DigitalInput {
    bool active{};
    bool pressed{};
};

struct AnalogInput {
    bool active{};
    float value{};
};

inline DigitalInput validated_digital(DigitalInput input) {
    return input.active ? input : DigitalInput{};
}

inline AnalogInput validated_axis(AnalogInput input) {
    return input.active && std::isfinite(input.value)
        ? AnalogInput{true, std::clamp(input.value, 0.0f, 1.0f)} : AnalogInput{};
}

constexpr bool passive_input_action(std::string_view action) {
    return action == "/actions/default/in/Squeeze" || action.ends_with("Touch");
}

constexpr bool write_default_binding(std::string_view filename, bool exists) {
    return filename != binding_file || !exists;
}

constexpr bool swap_face_actions(bool frame, bool swapped) {
    return !frame && swapped;
}

constexpr bool shoulder_pressed(bool frame, bool grip_pressed, DigitalInput bumper) {
    // A custom/unavailable native bumper binding retains the historical grip fallback.
    return frame && bumper.active ? bumper.pressed : grip_pressed;
}

inline uint8_t trigger_value(bool frame, bool digital_pressed, AnalogInput analog) {
    if (frame && analog.active && std::isfinite(analog.value)) {
        return static_cast<uint8_t>(std::lround(std::clamp(analog.value, 0.0f, 1.0f) * 255.0f));
    }
    return digital_pressed ? 255 : 0;
}

inline uint8_t merge_trigger_value(uint8_t existing, bool frame, bool digital_pressed, AnalogInput analog) {
    return (std::max)(existing, trigger_value(frame, digital_pressed, analog));
}
}
