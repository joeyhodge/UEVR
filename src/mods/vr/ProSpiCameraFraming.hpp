#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

namespace uevr::prospi::framing {

inline constexpr int32_t calibrated_mode = 3;
using Vector = std::array<float, 3>;

struct Pose {
    Vector location{}, rotation{};
    float fov{};
};

struct Calibration {
    Pose pose{};
    float focus{}, min_fov{}, multiplier{1.0f}, confidence{};
    bool center_field_family{};
};

enum class Status : int32_t {
    not_selected, accepted, exact_override, protected_camera, no_support,
    invalid_input, weak_match, conflicting_focus, outside_envelope, celebration_framing,
};

inline const char* status_name(Status s) noexcept {
    switch (s) {
    case Status::accepted: return "Validated center-field calibration";
    case Status::exact_override: return "Exact saved override retained";
    case Status::protected_camera: return "Existing assist (protected/other camera)";
    case Status::no_support: return "Existing assist (needs two pose calibrations)";
    case Status::invalid_input: return "Existing assist (invalid evidence)";
    case Status::weak_match: return "Existing assist (weak/distant match)";
    case Status::conflicting_focus: return "Existing assist (conflicting focus)";
    case Status::outside_envelope: return "Existing assist (framing envelope)";
    case Status::celebration_framing: return "Calibrated low-rig focus / signed height";
    default: return "Not selected / waiting for camera";
    }
}

inline bool finite(const Vector& v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline bool valid_pose(const Pose& p) noexcept {
    return finite(p.location) && finite(p.rotation) &&
        std::isfinite(p.fov) && p.fov > 0.01f && p.fov < 179.0f;
}

inline bool valid_calibration(const Calibration& c) noexcept {
    return valid_pose(c.pose) && std::isfinite(c.focus) && c.focus >= 10.0f && c.focus <= 50000.0f &&
        std::isfinite(c.min_fov) && c.min_fov >= 5.0f && c.min_fov <= 175.0f &&
        std::isfinite(c.multiplier) && c.multiplier >= 0.1f && c.multiplier <= 3.0f;
}

// This is a bounded camera-rig envelope, not a gameplay/ball classification or collision query.
inline bool center_field_rig(const Pose& p) noexcept {
    if (!valid_pose(p)) { return false; }
    const auto pitch = std::remainder(p.rotation[0], 360.0f);
    const auto yaw = std::remainder(p.rotation[1], 360.0f);
    return std::abs(p.location[0]) <= 2500.0f && p.location[1] >= -14000.0f && p.location[1] <= -8000.0f &&
        p.location[2] >= 200.0f && p.location[2] <= 3000.0f &&
        pitch >= -20.0f && pitch <= 5.0f && yaw >= 45.0f && yaw <= 135.0f &&
        std::abs(std::remainder(p.rotation[2], 360.0f)) <= 5.0f && p.fov <= 12.0f;
}

struct Request {
    Pose camera{};
    int32_t mode{};
    bool prospi{}, dolly_enabled{}, sequencer_enabled{}, safety_enabled{};
    bool exact_override{}, learned{}, eligible_family{}, protected_risk{};
    float focus{}, legacy_ceiling{}, effective_fov{}, base_fov{90.0f};
    float forward_offset{}, right_offset{}, up_offset{}, max_safety_up{}, required_floor{};
};

struct Decision {
    Status status{Status::not_selected};
    float learned_focus{}, proposed_dolly{};
    bool accepted() const noexcept { return status == Status::accepted; }
};

inline Decision evaluate(const Request& r, std::span<const Calibration> matches) noexcept {
    Decision out{};
    if (r.mode != calibrated_mode || !r.prospi || !r.dolly_enabled || !r.sequencer_enabled) { return out; }
    if (r.exact_override) { out.status = Status::exact_override; return out; }
    if (!r.safety_enabled || !r.eligible_family || r.protected_risk || !center_field_rig(r.camera)) {
        out.status = Status::protected_camera; return out;
    }
    if (!r.learned || matches.size() < 2) { out.status = Status::no_support; return out; }
    if (matches.size() > 3 || !std::isfinite(r.focus) || !std::isfinite(r.legacy_ceiling) ||
        r.legacy_ceiling < 10.0f || r.legacy_ceiling > 15000.0f ||
        !std::isfinite(r.effective_fov) || r.effective_fov < 5.0f || r.effective_fov >= 175.0f ||
        !std::isfinite(r.base_fov) || r.base_fov < 5.0f || r.base_fov >= 175.0f ||
        !std::isfinite(r.forward_offset) || !std::isfinite(r.right_offset) || !std::isfinite(r.up_offset) ||
        !std::isfinite(r.max_safety_up) || r.max_safety_up < 0.0f || r.max_safety_up > 4000.0f ||
        !std::isfinite(r.required_floor) || r.required_floor < -500.0f || r.required_floor > 3000.0f) {
        out.status = Status::invalid_input; return out;
    }

    float total_weight{}, focus_sum{}, min_focus{50000.0f}, max_focus{};
    for (const auto& c : matches) {
        if (!valid_calibration(c) || !std::isfinite(c.confidence) || c.confidence > 1.0f ||
            !c.center_field_family || !center_field_rig(c.pose)) {
            out.status = Status::invalid_input; return out;
        }
        const auto dx = c.pose.location[0] - r.camera.location[0];
        const auto dy = c.pose.location[1] - r.camera.location[1];
        const auto dz = c.pose.location[2] - r.camera.location[2];
        if (c.confidence < 0.8f || dx * dx + dy * dy + dz * dz > 2500.0f * 2500.0f ||
            std::abs(std::remainder(c.pose.rotation[1] - r.camera.rotation[1], 360.0f)) > 28.0f ||
            std::abs(std::remainder(c.pose.rotation[0] - r.camera.rotation[0], 360.0f)) > 22.0f) {
            out.status = Status::weak_match; return out;
        }
        const auto weight = c.confidence * c.confidence;
        total_weight += weight;
        focus_sum += c.focus * weight;
        min_focus = (std::min)(min_focus, c.focus);
        max_focus = (std::max)(max_focus, c.focus);
    }
    out.learned_focus = focus_sum / total_weight;
    if (max_focus > min_focus * 1.25f || std::abs(out.learned_focus - r.focus) > 1.0f) {
        out.status = Status::conflicting_focus; return out;
    }
    if (r.focus <= r.legacy_ceiling || r.focus > 15000.0f || r.focus > r.legacy_ceiling * 1.4f) {
        out.status = Status::outside_envelope; return out;
    }

    const auto radians = std::numbers::pi_v<float> / 180.0f;
    const auto scale = std::tan(r.effective_fov * radians * 0.5f) / std::tan(r.base_fov * radians * 0.5f);
    out.proposed_dolly = r.focus * (1.0f - scale);
    if (!std::isfinite(out.proposed_dolly) || out.proposed_dolly <= 0.0f) {
        out.status = Status::outside_envelope; return out;
    }
    const auto pitch = std::remainder(r.camera.rotation[0], 360.0f) * radians;
    const auto yaw = std::remainder(r.camera.rotation[1], 360.0f) * radians;
    const auto forward = out.proposed_dolly + r.forward_offset;
    const auto end_x = r.camera.location[0] + forward * std::cos(pitch) * std::cos(yaw);
    const auto end_y = r.camera.location[1] + forward * std::cos(pitch) * std::sin(yaw);
    const auto end_z = r.camera.location[2] + forward * std::sin(pitch);
    // Bound unknown right/up contributions, including the existing camera-basis safety lift.
    const auto margin = std::abs(r.right_offset) + std::abs(r.up_offset) + r.max_safety_up;
    if (std::abs(end_x) + margin > 2500.0f || end_y - margin < -6500.0f || end_y + margin > -500.0f) {
        out.status = Status::outside_envelope; return out;
    }
    const auto roll = std::remainder(r.camera.rotation[2], 360.0f) * radians;
    const auto max_world_lift = r.max_safety_up * std::cos(pitch) * std::cos(roll);
    if (end_z - std::abs(r.right_offset) - std::abs(r.up_offset) + max_world_lift < r.required_floor) {
        out.status = Status::outside_envelope; return out;
    }
    out.status = Status::accepted;
    return out;
}

} // namespace uevr::prospi::framing
