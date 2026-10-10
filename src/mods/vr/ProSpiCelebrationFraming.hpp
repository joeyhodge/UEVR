#pragma once

#include "ProSpiCameraFraming.hpp"
#include <initializer_list>
#include <optional>

namespace uevr::prospi::celebration {

using framing::Pose;
using framing::Calibration;

// Ground-level baseline pans can change yaw without becoming stand cameras.
// This is a height-policy envelope, not proof of a player, celebration or collision.
inline bool baseline_rig(const Pose& p) noexcept {
    return framing::valid_pose(p) && std::abs(p.location[0]) >= 1200.0f && std::abs(p.location[0]) <= 4200.0f &&
        p.location[1] >= -2500.0f && p.location[1] <= 1800.0f &&
        p.location[2] >= -100.0f && p.location[2] <= 600.0f &&
        p.rotation[0] >= -18.0f && p.rotation[0] <= 16.0f &&
        std::abs(p.rotation[2]) <= 0.01f && p.fov >= 0.5f && p.fov <= 24.0f;
}

// A low baseline rig, not proof of a celebration, subject or stadium collision.
inline bool low_rig(const Pose& p) noexcept {
    if (!framing::valid_pose(p)) { return false; }
    const auto x = p.location[0];
    const auto yaw = std::remainder(p.rotation[1], 360.0f);
    return std::abs(x) >= 1200.0f && std::abs(x) <= 4200.0f &&
        p.location[1] >= -2500.0f && p.location[1] <= 1800.0f &&
        p.location[2] >= -100.0f && p.location[2] <= 600.0f &&
        p.rotation[0] >= -18.0f && p.rotation[0] <= 16.0f &&
        std::abs(p.rotation[2]) <= 0.01f && p.fov >= 5.0f && p.fov <= 40.0f &&
        (x > 0 ? (yaw >= 55.0f || yaw <= -120.0f) : (yaw >= -115.0f && yaw <= 75.0f));
}

struct Gates {
    int32_t mode{};
    bool prospi{}, dolly{}, sequencer{}, safety{}, field_floor{}, legacy_cinematic{}, decoupled_pitch{};
    bool enabled() const noexcept {
        return mode == framing::calibrated_mode && prospi && dolly && sequencer && safety &&
            field_floor && !legacy_cinematic && !decoupled_pitch;
    }
};

inline std::optional<float> short_focus(const Pose& camera, std::span<const Calibration> matches) noexcept {
    if (!low_rig(camera) || matches.empty() || matches.size() > 3) { return {}; }
    float total{}, sum{}, low{1250.0f}, high{};
    for (const auto& c : matches) {
        const auto dx = camera.location[0] - c.pose.location[0];
        const auto dy = camera.location[1] - c.pose.location[1];
        const auto dz = camera.location[2] - c.pose.location[2];
        if (!framing::valid_calibration(c) || !low_rig(c.pose) ||
            camera.location[0] * c.pose.location[0] <= 0.0f || c.focus > 1250.0f ||
            !std::isfinite(c.confidence) || c.confidence < 0.828f || c.confidence > 1.0f ||
            dx * dx + dy * dy + dz * dz > 500.0f * 500.0f ||
            std::abs(std::remainder(camera.rotation[1] - c.pose.rotation[1], 360.0f)) > 30.0f ||
            std::abs(camera.rotation[0] - c.pose.rotation[0]) > 12.0f ||
            std::abs(camera.fov - c.pose.fov) > 16.0f) { return {}; }
        const auto w = c.confidence * c.confidence;
        total += w; sum += c.focus * w;
        low = (std::min)(low, c.focus); high = (std::max)(high, c.focus);
    }
    if (high > low * 1.25f) { return {}; }
    return sum / total;
}

struct Identity {
    uintptr_t world{}, camera_manager{}, view_target{};
    int32_t rendering_method{};
    int32_t target_index{-1}, target_serial{-1};
    bool operator==(const Identity&) const = default;
    bool valid() const noexcept { return world && camera_manager && view_target; }
};

// Game-tick owned, pointer-free continuity. Never keeps a target beyond a brief gap.
class FocusContinuity {
public:
    void reset() noexcept { m_valid = false; }
    float update(bool enabled, bool exact, bool cut, const Identity& identity, const Pose& pose,
        double now, float target, bool learned) noexcept {
        if (!enabled || exact || !identity.valid() || !low_rig(pose) || !std::isfinite(now) ||
            !std::isfinite(target) || target < 10.0f || target > 1250.0f) {
            reset(); return target;
        }
        const auto dt = now - m_time;
        const auto dx = pose.location[0] - m_pose.location[0];
        const auto dy = pose.location[1] - m_pose.location[1];
        const auto dz = pose.location[2] - m_pose.location[2];
        const auto discontinuity = cut || !m_valid || identity != m_identity || dt <= 0 || dt > 0.25 ||
            dx * dx + dy * dy + dz * dz > 750.0f * 750.0f ||
            std::abs(std::remainder(pose.rotation[1] - m_pose.rotation[1], 360.0f)) > 25.0f ||
            std::abs(pose.rotation[0] - m_pose.rotation[0]) > 20.0f || std::abs(pose.fov - m_pose.fov) > 10.0f;
        if (discontinuity) {
            m_focus = target; m_last_learned = learned ? now : -1.0;
        } else {
            if (learned) { m_last_learned = now; }
            if (!learned && m_last_learned >= 0 && now - m_last_learned <= 0.25) { target = m_focus; }
            const auto step = static_cast<float>(600.0 * dt);
            m_focus += std::clamp(target - m_focus, -step, step);
        }
        m_valid = true; m_identity = identity; m_pose = pose; m_time = now;
        return m_focus;
    }
private:
    bool m_valid{};
    Identity m_identity{};
    Pose m_pose{};
    double m_time{}, m_last_learned{-1.0};
    float m_focus{};
};

struct SafetyRequest {
    Gates gates{};
    Pose camera{};
    float dolly{}, forward{}, right{}, up{}, floor{}, max_lift{};
    bool field_zone{};
    bool baseline_camera{};
};
struct SafetyDecision {
    bool accepted{};
    float dolly{}, lift{}, predicted_z{}, end_z{};
};

inline SafetyDecision signed_safety(const SafetyRequest& r) noexcept {
    SafetyDecision out{};
    if (!r.gates.enabled() || !r.field_zone ||
        !(low_rig(r.camera) || (r.baseline_camera && baseline_rig(r.camera)))) { return out; }
    for (const auto x : {r.dolly, r.forward, r.right, r.up, r.floor, r.max_lift}) {
        if (!std::isfinite(x)) { return out; }
    }
    if (r.dolly < 0 || r.dolly > 1250 || std::abs(r.forward) > 100 || std::abs(r.right) > 100 ||
        std::abs(r.up) > 100 || r.floor < -50 || r.floor > 350 || r.max_lift < 0 || r.max_lift > 600) { return out; }
    const auto pitch = r.camera.rotation[0] * std::numbers::pi_v<float> / 180.0f;
    const auto s = std::sin(pitch), c = std::cos(pitch);
    const auto base = r.camera.location[2] + r.forward * s + r.up * c;
    // Roll is bounded above; clearance also covers its small right/up contribution.
    const auto floor = r.floor + 2.0f;
    auto dolly = r.dolly;
    if (base + dolly * s + r.max_lift * c < floor) {
        if (s >= -0.001f) { return out; }
        dolly = (std::min)(dolly, (base + r.max_lift * c - floor) / -s);
        if (dolly < 0) { return out; }
    }
    out.predicted_z = base + dolly * s;
    out.lift = (std::max)(0.0f, (floor - out.predicted_z) / c);
    if (!std::isfinite(out.lift) || out.lift > r.max_lift + 0.01f) { return {}; }
    out.lift = (std::min)(out.lift, r.max_lift);
    out.end_z = out.predicted_z + out.lift * c;
    if (!std::isfinite(out.end_z) || out.end_z < r.floor + 1.0f) { return {}; }
    out.dolly = dolly; out.accepted = true;
    return out;
}

inline bool baseline_field_floor(SafetyRequest r, bool focus_guard, bool automatic, bool protected_camera) noexcept {
    if (!focus_guard || !automatic || protected_camera || !baseline_rig(r.camera) ||
        !std::isfinite(r.dolly) || r.dolly < 0 || r.dolly > 2000 ||
        r.forward != 0 || r.right != 0 || r.up != 0) { return false; }
    // Establish that the configured field floor is reachable before overriding the stand band.
    r.dolly = 0;
    r.baseline_camera = true;
    return signed_safety(r).accepted;
}
}
