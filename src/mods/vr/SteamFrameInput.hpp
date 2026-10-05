#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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

    bool publish(uint64_t observed, uint8_t mask, bool retry) {
        const auto next = (observed & ~7ull) | (mask & 3u) | (retry ? 4u : 0u);
        return m_state.compare_exchange_strong(observed, next, std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    // A reconnect/profile event cannot be overwritten by an in-flight property read.
    std::atomic<uint64_t> m_state{4u}; // epoch, refresh-needed bit, physical hand mask
};

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
