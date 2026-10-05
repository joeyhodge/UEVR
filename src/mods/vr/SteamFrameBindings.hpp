#pragma once

#include <string>
#include <iterator>
#include <stdexcept>
#include <nlohmann/json.hpp>

#include "SteamFrameInput.hpp"

namespace uevr::steam_frame {
inline constexpr size_t max_custom_binding_bytes = 256 * 1024;
enum class ActionType { Boolean, Float, Vector2, Pose, Haptic };
struct CustomBinding { std::string action; std::string path; };
struct VectorOutput { std::string action; float x; float y; };
struct VectorAssociation {
    unsigned hand;
    std::string activator;
    std::string modifier;
    std::vector<VectorOutput> outputs;
};
struct CustomBindings {
    std::vector<CustomBinding> bindings;
    std::vector<VectorAssociation> associations;
};

template<class ActionKind>
CustomBindings parse_custom_bindings(const nlohmann::json& j, ActionKind&& action_kind) {
    if (!j.contains("bindings") || !j["bindings"].is_array() || j["bindings"].empty() || j["bindings"].size() > 128) {
        throw std::runtime_error{"Invalid Steam Frame bindings array"};
    }
    CustomBindings candidate;
    for (const auto& item : j["bindings"]) {
        auto action = item.at("action").template get<std::string>();
        auto path = item.at("path").template get<std::string>();
        if (!action_kind(action) || path.find('\0') != std::string::npos ||
            (!path.starts_with("/user/hand/left/") && !path.starts_with("/user/hand/right/"))) {
            throw std::runtime_error{"Invalid Steam Frame action or component path"};
        }
        candidate.bindings.push_back({std::move(action), std::move(path)});
    }
    if (!j.contains("vector2_associations")) { return candidate; }
    if (!j["vector2_associations"].is_array() || j["vector2_associations"].size() > 64) {
        throw std::runtime_error{"Invalid Steam Frame vector associations"};
    }
    for (const auto& item : j["vector2_associations"]) {
        const auto path = item.at("path").template get<std::string>();
        if (path != "/user/hand/left" && path != "/user/hand/right") {
            throw std::runtime_error{"Invalid Steam Frame vector association hand"};
        }
        VectorAssociation association{path == "/user/hand/left" ? 0u : 1u,
            item.at("activator").template get<std::string>(), item.at("modifier").template get<std::string>(), {}};
        if (action_kind(association.activator) != ActionType::Boolean || action_kind(association.modifier) != ActionType::Vector2 ||
            !item.at("outputs").is_array() || item["outputs"].size() > 64) {
            throw std::runtime_error{"Invalid Steam Frame vector association types"};
        }
        for (const auto& output : item["outputs"]) {
            auto action = output.at("action").template get<std::string>();
            const auto x = output.at("value").at("x").template get<float>();
            const auto y = output.at("value").at("y").template get<float>();
            if (action_kind(action) != ActionType::Boolean || !std::isfinite(x) || !std::isfinite(y)) {
                throw std::runtime_error{"Invalid Steam Frame vector output"};
            }
            association.outputs.push_back({std::move(action), x, y});
        }
        candidate.associations.push_back(std::move(association));
    }
    return candidate;
}

inline nlohmann::json make_openvr_bindings() {
    using nlohmann::json;
    json result{
        {"controller_type", controller_type},
        {"name", "UEVR Steam Frame"},
        {"description", "Native Steam Frame gamepad buttons, VR grips, poses and haptics"},
        {"options", json::object()},
        {"alias_info", json::object()},
        {"bindings", {{"/actions/default", {
            {"chords", json::array()}, {"haptics", json::array()},
            {"poses", json::array()}, {"skeleton", json::array()}, {"sources", json::array()}
        }}}}
    };
    auto& bindings = result["bindings"]["/actions/default"];
    for (const auto hand : {"left", "right"}) {
        const auto root = std::string{"/user/hand/"} + hand;
        bindings["haptics"].push_back({{"output", "/actions/default/out/haptic"}, {"path", root + "/output/haptic"}});
        bindings["poses"].push_back({{"output", "/actions/default/in/pose"}, {"path", root + "/pose/openxr_aim"}});
        bindings["poses"].push_back({{"output", "/actions/default/in/grippose"}, {"path", root + "/pose/openxr_grip"}});
        bindings["skeleton"].push_back({
            {"output", std::string{"/actions/default/in/skeleton"} + hand + "hand"},
            {"path", root + "/input/skeleton/" + hand}
        });
    }
    // Derive both runtime defaults from one logical mapping. OpenVR uses grip,
    // not OpenXR's squeeze, and a source mode selects click/touch/pull components.
    for (const auto& binding : openxr_bindings) {
        if (binding.action == "pose" || binding.action == "grippose" || binding.action == "haptic") {
            continue;
        }
        auto path = std::string{binding.path};
        const auto split = path.find_last_of('/');
        const auto component = path.substr(split + 1);
        std::string mode{"button"};
        std::string input{component};
        if (component == "click" || component == "touch" || component == "value") {
            path.resize(split);
            if (component == "value") { mode = "trigger"; input = "pull"; }
        } else if (component == "thumbstick") {
            mode = "joystick";
            input = "position";
        }
        const auto squeeze = path.find("/input/squeeze");
        if (squeeze != std::string::npos) { path.replace(squeeze, std::string_view{"/input/squeeze"}.size(), "/input/grip"); }
        if (path.ends_with("/input/thumbstick")) { mode = "joystick"; }
        auto& sources = bindings["sources"];
        auto source = std::find_if(sources.begin(), sources.end(), [&](const json& item) {
            return item["path"] == path && item["mode"] == mode;
        });
        if (source == sources.end()) {
            sources.push_back({{"path", path}, {"mode", mode}, {"inputs", json::object()}});
            source = std::prev(sources.end());
        }
        (*source)["inputs"][input] = {{"output", std::string{"/actions/default/in/"} + std::string{binding.action}}};
    }
    return result;
}
}
