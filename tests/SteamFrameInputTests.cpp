#ifdef _WIN32
#include <Windows.h>
#endif
#include "mods/vr/SteamFrameBindings.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace sf = uevr::steam_frame;
using nlohmann::json;
namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) { throw std::runtime_error{"Cannot read fixture: " + path.string()}; }
    std::string text{std::istreambuf_iterator<char>{file}, {}};
    std::erase(text, '\r');
    return text;
}
std::string definition(const std::string& source, const std::string& name) {
    const auto marker = "std::string VR::" + name + " = R\"(";
    const auto start = source.find(marker);
    if (start == std::string::npos) { throw std::runtime_error{"Missing binding definition: " + name}; }
    const auto end = source.find(")\";", start + marker.size());
    if (end == std::string::npos) { throw std::runtime_error{"Unterminated binding definition"}; }
    return source.substr(start + marker.size(), end - start - marker.size());
}
uint64_t hash(std::string_view text) {
    uint64_t value = 14695981039346656037ull;
    for (const unsigned char byte : text) { value = (value ^ byte) * 1099511628211ull; }
    return value;
}
std::string lower(std::string text) {
    for (auto& c : text) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
    return text;
}

void policies() {
    const std::vector<std::string> legacy{
        "/interaction_profiles/khr/simple_controller", "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/oculus/go_controller", "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/microsoft/motion_controller", "/interaction_profiles/htc/vive_controller"};
    expect(sf::supported_profiles(legacy, false) == legacy, "unsupported extension retains exactly the legacy profiles");
    auto with_frame = sf::supported_profiles(legacy, true);
    expect(with_frame.size() == legacy.size() + 1 && with_frame.back() == sf::interaction_profile,
        "advertised-and-enabled Frame extension adds only its native profile");
    expect(sf::supported_profiles(with_frame, true) == with_frame, "Frame profile is not registered twice");
    expect(sf::supported_profiles(with_frame, false) == legacy, "disabled extension never submits a Frame profile");
    expect(sf::is_frame_controller("frame_controller"), "controller identity uses the exact native type");
    for (const auto type : {"frame_hmd", "oculus_touch", "knuckles", "Frame_Controller", "frame_controller_legacy", ""}) {
        expect(!sf::is_frame_controller(type), "headset/emulated/unrecognized types do not enable native input");
    }
    for (bool frame : {false, true}) {
        for (bool grip : {false, true}) {
            for (bool active : {false, true}) {
                for (bool bumper : {false, true}) {
                    expect(sf::shoulder_pressed(frame, grip, {active, bumper}) == (frame && active ? bumper : grip),
                        "native bumper is independent of grip; unavailable/custom/legacy bindings preserve grip fallback");
                }
            }
        }
    }
    expect(sf::swap_face_actions(false, true) && !sf::swap_face_actions(false, false) &&
        !sf::swap_face_actions(true, true) && !sf::swap_face_actions(true, false),
        "native ABXY labels remain stable while mixed/legacy controllers retain their swap behavior");
    expect(sf::trigger_value(true, false, {true, 0.5f}) == 128 && sf::trigger_value(true, true, {true, 0.f}) == 0,
        "native analog triggers preserve partial pull and release");
    expect(sf::trigger_value(true, false, {true, -1.f}) == 0 && sf::trigger_value(true, false, {true, 2.f}) == 255,
        "native trigger range is clamped before byte conversion");
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()}) {
        expect(sf::trigger_value(true, true, {true, value}) == 255 && sf::trigger_value(true, false, {true, value}) == 0,
            "non-finite trigger data retains the safe digital fallback");
    }
    for (bool digital : {false, true}) {
        expect(sf::trigger_value(false, digital, {true, 0.5f}) == (digital ? 255 : 0) &&
            sf::trigger_value(true, digital, {false, 0.5f}) == (digital ? 255 : 0), "legacy/unbound analog paths are unchanged");
    }
    expect(sf::merge_trigger_value(200, true, false, {true, 0.25f}) == 200 &&
        sf::merge_trigger_value(0, true, false, {true, 0.5f}) == 128 &&
        sf::merge_trigger_value(100, false, true, {}) == 255 && sf::merge_trigger_value(100, false, false, {}) == 100,
        "native trigger input preserves stronger physical-gamepad input and exactly retains legacy merging");
    expect(!sf::write_default_binding(sf::binding_file, true) && sf::write_default_binding(sf::binding_file, false),
        "new Frame defaults never overwrite an existing customization");
    expect(sf::write_default_binding("actions.json", true) && sf::write_default_binding("bindings_oculus_touch.json", true),
        "existing manifest/default-file update policy remains unchanged");
}

void transactions() {
    size_t resolved{}, submitted{};
    const auto resolve = [&](const sf::Binding&) -> std::optional<size_t> { return resolved++; };
    const auto submit = [&](const std::vector<size_t>& candidate) {
        ++submitted;
        expect(candidate.size() == sf::openxr_bindings.size(), "complete native binding set is submitted in one batch");
        return true;
    };
    expect(!sf::suggest_native_bindings<size_t>(false, resolve, submit) && resolved == 0 && submitted == 0,
        "unsupported runtime never resolves or suggests Frame paths");
    expect(sf::suggest_native_bindings<size_t>(true, resolve, submit) && submitted == 1,
        "successful native registration commits exactly once");
    for (size_t reject = 0; reject < sf::openxr_bindings.size(); ++reject) {
        resolved = submitted = 0;
        const auto missing = [&](const sf::Binding&) -> std::optional<size_t> {
            const auto index = resolved++;
            return index == reject ? std::nullopt : std::optional<size_t>{index};
        };
        expect(!sf::suggest_native_bindings<size_t>(true, missing, submit) && submitted == 0,
            "any missing action/path prevents publishing a partial Frame profile");
    }
    resolved = submitted = 0;
    expect(!sf::suggest_native_bindings<size_t>(true, resolve, [&](const auto&) { ++submitted; return false; }) && submitted == 1,
        "runtime rejection is returned to the legacy-fallback path");
}

void controller_cache() {
    sf::ControllerProfileCache cache;
    auto initial = cache.begin_refresh();
    expect(initial && cache.mask() == 0, "unknown controllers start on the legacy input path");
    expect(cache.publish(*initial, 1, false) && cache.mask() == 1 && !cache.begin_refresh(),
        "one Frame hand is published without steady-state property polling");
    cache.invalidate();
    auto pending = cache.begin_refresh();
    expect(pending && cache.mask() == 0, "disconnect/role/profile events clear native identity immediately");
    cache.invalidate();
    expect(!cache.publish(*pending, 3, false) && cache.mask() == 0 && cache.begin_refresh(),
        "a profile event rejects an older in-flight controller property read");
    auto current = cache.begin_refresh();
    expect(cache.publish(*current, 2, true) && cache.mask() == 2 && cache.begin_refresh(),
        "one valid hand may be used while an unavailable other hand is retried");
    current = cache.begin_refresh();
    expect(cache.publish(*current, 3, false) && cache.mask() == 3 && !cache.begin_refresh(),
        "both hands settle on a complete native profile");
    expect(!cache.publish(*pending, 1, false) && cache.mask() == 3,
        "late older refreshes cannot overwrite a newer complete profile");
    cache.invalidate();
    current = cache.begin_refresh();
    expect(cache.publish(*current, 0, false) && cache.mask() == 0 && !cache.begin_refresh(),
        "reconnecting legacy controllers restores the unchanged mapping without repeated scans");
}

void custom_bindings() {
    json defaults{{"bindings", json::array()}};
    std::map<std::string, sf::ActionType> actions;
    for (const auto& binding : sf::openxr_bindings) {
        defaults["bindings"].push_back({{"action", binding.action}, {"path", binding.path}});
        actions[std::string{binding.action}] = binding.path.ends_with("/pose") ? sf::ActionType::Pose :
            binding.path.ends_with("/haptic") ? sf::ActionType::Haptic : binding.path.ends_with("/value") ? sf::ActionType::Float :
            binding.path.ends_with("/thumbstick") ? sf::ActionType::Vector2 : sf::ActionType::Boolean;
    }
    const auto kind = [&](const std::string& name) -> std::optional<sf::ActionType> {
        const auto it = actions.find(name);
        return it == actions.end() ? std::nullopt : std::optional<sf::ActionType>{it->second};
    };
    const auto parsed = sf::parse_custom_bindings(defaults, kind);
    expect(parsed.bindings.size() == sf::openxr_bindings.size() && parsed.associations.empty(),
        "a saved native profile retains every action/path without requiring vector associations");
    auto customized = defaults;
    customized["bindings"][0]["action"] = "backbutton";
    customized["vector2_associations"] = json::array({{
        {"path", "/user/hand/right"}, {"activator", "joystickclick"}, {"modifier", "joystick"},
        {"outputs", json::array({{{"action", "dpad_up"}, {"value", {{"x", 0.f}, {"y", 1.f}}}}})}
    }});
    const auto custom = sf::parse_custom_bindings(customized, kind);
    expect(custom.bindings[0].action == "backbutton" && custom.associations.size() == 1 && custom.associations[0].hand == 1 &&
        custom.associations[0].outputs[0].action == "dpad_up", "valid saved remapping/vector associations are retained exactly");
    const auto rejects = [&](const json& value) {
        try { sf::parse_custom_bindings(value, kind); return false; } catch (const std::exception&) { return true; }
    };
    auto bad = defaults; bad["bindings"] = nullptr;
    expect(rejects(bad), "missing/non-array custom bindings retain native defaults");
    bad["bindings"] = json::array();
    expect(rejects(bad), "empty binding replacements cannot erase the native default profile");
    bad["bindings"] = std::vector<json>(129, defaults["bindings"][0]);
    expect(rejects(bad), "custom binding count is bounded");
    bad = defaults; bad["bindings"][0]["action"] = "unknown_action";
    expect(rejects(bad), "unknown actions reject the whole candidate before any suggestion");
    for (const auto& path : {std::string{"/user/head/input/a/click"}, std::string{"/user/hand/right"},
        std::string{"/user/hand/right/input/a/click"} + '\0' + "invalid"}) {
        bad = defaults; bad["bindings"][0]["path"] = path;
        expect(rejects(bad), "invalid hand paths and embedded nulls cannot silently alter a saved binding");
    }
    for (const auto* key : {"action", "path"}) {
        bad = defaults; bad["bindings"][0].erase(key);
        expect(rejects(bad), "incomplete custom entries reject without partial publication");
    }
    bad = customized; bad["vector2_associations"] = json::object();
    expect(rejects(bad), "association schema is validated");
    bad["vector2_associations"] = std::vector<json>(65, customized["vector2_associations"][0]);
    expect(rejects(bad), "association count is bounded");
    for (const auto& [key, value] : std::array<std::pair<const char*, const char*>, 3>{{
        {"path", "/user/hand/right/input"}, {"activator", "squeeze"}, {"modifier", "backbutton"}}}) {
        bad = customized; bad["vector2_associations"][0][key] = value;
        expect(rejects(bad), "association hand and action types are validated");
    }
    bad = customized; bad["vector2_associations"][0]["outputs"] = std::vector<json>(65, customized["vector2_associations"][0]["outputs"][0]);
    expect(rejects(bad), "association output count is bounded");
    bad = customized; bad["vector2_associations"][0]["outputs"][0]["action"] = "triggeraxis";
    expect(rejects(bad), "vector activators may only force boolean actions");
    for (const float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        bad = customized; bad["vector2_associations"][0]["outputs"][0]["value"]["x"] = value;
        expect(rejects(bad), "non-finite association coordinates are rejected");
    }
    expect(sf::parse_custom_bindings(defaults, kind).bindings.size() == parsed.bindings.size(),
        "failed custom candidates never mutate native defaults");
}

void bindings(const std::filesystem::path& root, const std::filesystem::path& driver) {
    const auto source = read(root / "src/mods/vr/Bindings.cpp");
    const auto manifest = json::parse(definition(source, "actions_json"));
    std::map<std::string, std::string> actions;
    for (const auto& action : manifest["actions"]) {
        expect(actions.emplace(lower(action.at("name").get<std::string>()), action.at("type").get<std::string>()).second,
            "all manifest action names remain unique");
    }
    expect(actions.size() == 33, "only three optional boolean actions and one analog action extend the manifest");
    for (const auto& action : manifest["actions"]) {
        const auto name = action.at("name").get<std::string>();
        if (name.ends_with("Bumper") || name.ends_with("StartButton") || name.ends_with("BackButton") || name.ends_with("TriggerAxis")) {
            expect(action.at("requirement") == "optional", "additional controls are not required on older controllers");
        }
    }
    expect(manifest["default_bindings"].size() == 6 && manifest["default_bindings"].back()["controller_type"] == sf::controller_type &&
        manifest["default_bindings"].back()["binding_url"] == sf::binding_file, "OpenVR manifest selects the native controller binding");
    const std::pair<const char*, uint64_t> legacy[]{
        {"binding_rift_json", 0x60de4ae9839fcd55ull}, {"bindings_oculus_touch_json", 0x6af803bab965db2dull},
        {"binding_vive", 0xbc3b9f7e0716d67bull}, {"bindings_vive_controller", 0xf1069b7c1e1ccd8dull},
        {"bindings_knuckles", 0xa0d3018d392e3436ull}};
    for (const auto& [name, expected] : legacy) {
        const auto body = definition(source, name);
        expect(hash(body) == expected, "pre-existing OpenVR controller JSON stays byte-identical");
        expect(json::parse(body).is_object(), "legacy binding remains valid JSON");
    }
    std::set<std::string_view> components;
    for (const auto& binding : sf::openxr_bindings) {
        expect(components.insert(binding.path).second, "native physical components are not aliased to multiple actions");
        expect(binding.path.find("/input/system/") == std::string_view::npos, "Steam/dashboard buttons are not hijacked");
        const auto action_name = std::string{"/actions/default/"} + (binding.action == "haptic" ? "out/" : "in/") + std::string{binding.action};
        expect(actions.contains(action_name), "native binding refers to a real manifest action");
        const auto expected_type = binding.path.ends_with("/pose") ? "pose" : binding.path.ends_with("/haptic") ? "vibration" :
            binding.path.ends_with("/value") ? "vector1" : binding.path.ends_with("/thumbstick") ? "vector2" : "boolean";
        expect(actions.at(action_name) == expected_type, "OpenXR component and action types agree");
    }
    const std::pair<const char*, const char*> face[]{ {"a", "abuttonright"}, {"b", "abuttonleft"}, {"x", "bbuttonright"}, {"y", "bbuttonleft"} };
    for (const auto& [physical, logical] : face) {
        const auto path = std::string{"/user/hand/right/input/"} + physical + "/click";
        const auto found = std::find_if(sf::openxr_bindings.begin(), sf::openxr_bindings.end(), [&](const auto& entry) { return entry.path == path; });
        expect(found != sf::openxr_bindings.end() && found->action == logical, "each physical ABXY button maps to its matching gamepad bit");
    }
    const auto frame = sf::make_openvr_bindings();
    const auto roundtrip = json::parse(frame.dump());
    expect(frame == roundtrip && frame.at("controller_type") == sf::controller_type, "generated native OpenVR JSON roundtrips");
    const auto& set = frame.at("bindings").at("/actions/default");
    expect(set.at("poses").size() == 4 && set.at("haptics").size() == 2 && set.at("skeleton").size() == 2,
        "both hands retain aim/grip poses, haptics and skeleton bindings");
    const auto validate_output = [&](const json& item) {
        expect(actions.contains(lower(item.at("output").get<std::string>())), "OpenVR output action exists in the manifest");
    };
    for (const auto section : {"poses", "haptics", "skeleton"}) { for (const auto& item : set.at(section)) { validate_output(item); } }
    std::set<std::pair<std::string, std::string>> source_modes;
    for (const auto& item : set.at("sources")) {
        expect(source_modes.emplace(item.at("path").get<std::string>(), item.at("mode").get<std::string>()).second,
            "click/touch/position share one source of each mode, as in existing UEVR defaults");
        for (const auto& input : item.at("inputs")) { validate_output(input); }
        expect(item.at("path").get<std::string>().find("squeeze") == std::string::npos,
            "OpenVR uses native grip paths, not OpenXR squeeze paths");
    }
    if (!driver.empty()) {
        const auto profile = json::parse(read(driver));
        expect(profile.at("controller_type") == sf::controller_type, "local SteamVR driver identifies the native controller");
        for (const auto section : {"sources", "poses", "haptics", "skeleton"}) {
            for (const auto& item : set.at(section)) {
                const auto path = item.at("path").get<std::string>();
                const auto tail = path.substr(path.find('/', std::string{"/user/hand/left"}.size()));
                expect(profile.at("input_source").contains(tail), "every native OpenVR path exists in the installed Valve driver profile");
                const auto& component = profile.at("input_source").at(tail);
                if (component.contains("side")) {
                    expect(path.starts_with(std::string{"/user/hand/"} + component.at("side").get<std::string>() + '/'),
                        "each native OpenVR component uses the correct physical hand");
                }
                if (std::string_view{section} == "sources") {
                    for (const auto& input : item.at("inputs").items()) {
                        expect(input.key() == "pull" || input.key() == "position" || component.value(input.key(), false),
                            "digital/touch components are supported by the installed controller source");
                    }
                }
            }
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) { throw std::runtime_error{"Pass the UEVR source root"}; }
        policies();
        transactions();
        controller_cache();
        custom_bindings();
        bindings(argv[1], argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{});
        if (failures == 0) { std::cout << "Steam Frame input tests passed\n"; }
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures == 0 ? 0 : 1;
}
