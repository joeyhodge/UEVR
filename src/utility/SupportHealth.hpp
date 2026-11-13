#pragma once

#include <nlohmann/json.hpp>

namespace utility::support {
// Pure presentation of observations, never a policy that changes game settings.
inline nlohmann::json health_findings(const nlohmann::json& observations) {
    auto findings = nlohmann::json::array();
    const auto features = observations.value("features", nlohmann::json::object());
    const auto on = [&](const char* key) {
        const auto i = features.find(key);
        return i != features.end() && i->is_boolean() && i->get<bool>();
    };
    const auto add = [&](const char* level, const char* code, const char* message) {
        findings.push_back({{"level", level}, {"code", code}, {"message", message}});
    };
    if (on("wait_only_requested") && (on("array_requested") || on("preacquire_requested"))) {
        add("notice", "wait_options_conflict", "Wait-only is blocked by the selected array/pre-acquire option; settings were not changed.");
    }
    if (on("array_requested") && on("same_pass")) {
        add("notice", "array_same_pass", "Texture-array submission requires Same Pass off. Wait-only is a separate experiment that supports Same Pass.");
    }
    if (on("native_fix_requested") && !on("native_stereo")) {
        add("notice", "native_fix_mode", "Native Fix is requested but exact Native Stereo is not selected.");
    }
    if (on("hitch_diagnostics") || on("camera_trace") || on("pacing_recorder")) {
        add("notice", "optional_diagnostics", "Optional diagnostics are enabled. Compare performance with the same instrumentation in both runs.");
    }
    if (observations.contains("openxr") && observations["openxr"].is_object() &&
        observations["openxr"].value("sampled", false) == false) {
        add("information", "xr_snapshot_unavailable", "XR state was not sampled; busy or unavailable telemetry is not a rendering failure.");
    }
    add("information", "best_effort", "Read-only observations are not a coherent engine snapshot or proof of HMD image correctness. Zero/absent measurements are not a diagnosis.");
    return findings;
}
}
