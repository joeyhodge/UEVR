#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace uevr::controller_touch {
inline constexpr std::string_view openvr_file = "bindings_oculus_touch.json";
inline constexpr size_t max_saved_binding_bytes = 256 * 1024;

struct OpenXRBinding { std::string_view path; std::string_view action; };
// The existing OpenXR suggestion code rejects components unsupported by a profile.
// Touch/Touch Plus have no squeeze/touch component; do not synthesize one.
inline constexpr auto openxr_bindings = std::to_array<OpenXRBinding>({
    {"/user/hand/*/input/trigger/touch", "triggertouch"},
    {"/user/hand/*/input/thumbstick/touch", "joysticktouch"},
    {"/user/hand/*/input/trigger/value", "triggeraxis"},
    {"/user/hand/*/input/squeeze/value", "squeeze"},
});

template<class Binding>
std::vector<Binding> add_openxr_controls(std::vector<Binding> defaults) {
    for (const auto& binding : openxr_bindings) {
        defaults.push_back({std::string{binding.path}, std::string{binding.action}});
    }
    return defaults;
}

struct OpenVRBinding {
    std::string_view path;
    std::string_view mode;
    std::string_view input;
    std::string_view action;
};
// These components are advertised by SteamVR's oculus_touch driver profile.
// A binding is not a guarantee that a particular driver supplies touch data.
inline constexpr auto openvr_bindings = std::to_array<OpenVRBinding>({
    {"/user/hand/left/input/trigger", "button", "touch", "triggertouch"},
    {"/user/hand/right/input/trigger", "button", "touch", "triggertouch"},
    {"/user/hand/left/input/grip", "button", "touch", "griptouch"},
    {"/user/hand/right/input/grip", "button", "touch", "griptouch"},
    {"/user/hand/left/input/joystick", "joystick", "touch", "joysticktouch"},
    {"/user/hand/right/input/joystick", "joystick", "touch", "joysticktouch"},
    {"/user/hand/left/input/trigger", "trigger", "pull", "triggeraxis"},
    {"/user/hand/right/input/trigger", "trigger", "pull", "triggeraxis"},
    {"/user/hand/left/input/x", "button", "touch", "abuttontouchleft"},
    {"/user/hand/left/input/y", "button", "touch", "bbuttontouchleft"},
    {"/user/hand/right/input/a", "button", "touch", "abuttontouchright"},
    {"/user/hand/right/input/b", "button", "touch", "bbuttontouchright"},
    {"/user/hand/left/input/thumbrest", "button", "touch", "thumbresttouchleft"},
    {"/user/hand/right/input/thumbrest", "button", "touch", "thumbresttouchright"},
});

inline nlohmann::json add_openvr_controls(nlohmann::json defaults) {
    auto& sources = defaults.at("bindings").at("/actions/default").at("sources");
    for (const auto& binding : openvr_bindings) {
        auto source = std::find_if(sources.begin(), sources.end(), [&](const nlohmann::json& item) {
            return item.at("path").get_ref<const std::string&>() == binding.path &&
                item.at("mode").get_ref<const std::string&>() == binding.mode;
        });
        if (source == sources.end()) {
            sources.push_back({{"path", std::string{binding.path}}, {"mode", std::string{binding.mode}}, {"inputs", nlohmann::json::object()}});
            source = std::prev(sources.end());
        }
        source->at("inputs")[std::string{binding.input}] = {{"output", std::string{"/actions/default/in/"} + std::string{binding.action}}};
    }
    return defaults;
}

inline std::string make_openvr_defaults(std::string_view legacy) {
    return add_openvr_controls(nlohmann::json::parse(legacy)).dump(4);
}

inline nlohmann::json remove_added_controls(nlohmann::json defaults) {
    auto& sources = defaults.at("bindings").at("/actions/default").at("sources");
    for (const auto& binding : openvr_bindings) {
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const nlohmann::json& item) {
            return item.at("path").get_ref<const std::string&>() == binding.path &&
                item.at("mode").get_ref<const std::string&>() == binding.mode;
        });
        if (source == sources.end()) { continue; }
        source->at("inputs").erase(std::string{binding.input});
        if (source->at("inputs").empty()) { sources.erase(source); }
    }
    return defaults;
}

inline bool can_upgrade_saved_defaults(std::string_view saved, std::string_view current) noexcept {
    if (saved.size() > max_saved_binding_bytes || current.size() > max_saved_binding_bytes) { return false; }
    try {
        const auto bounded_parse = [](std::string_view text) {
            return nlohmann::json::parse(text, [](int depth, auto, const auto&) {
                if (depth > 16) { throw std::runtime_error{"Binding nesting exceeds limit"}; }
                return true;
            });
        };
        const auto installed = bounded_parse(saved);
        const auto defaults = bounded_parse(current);
        // Only a complete stock file (old or current) may be rewritten.
        return installed == defaults || installed == remove_added_controls(defaults);
    } catch (...) {
        return false;
    }
}

inline bool can_upgrade_saved_defaults(const std::filesystem::path& path, std::string_view current) noexcept {
    try {
        std::ifstream file{path, std::ios::binary};
        if (!file) { return false; }
        std::string saved(max_saved_binding_bytes + 1, '\0');
        file.read(saved.data(), static_cast<std::streamsize>(saved.size()));
        if (file.bad()) { return false; }
        saved.resize(static_cast<size_t>(file.gcount()));
        return can_upgrade_saved_defaults(std::string_view{saved}, current);
    } catch (...) {
        return false;
    }
}
}
