#pragma once

#include "ProSpiNativeFocusGuard.hpp"

namespace uevr::prospi::focus_guard {

enum class AuthoredScope : uint8_t { presentation, near_home_crossing };

// The recorded interview rigs overlap the broad Pitch/Batter preset classifier.
// This only admits observation; their current authored target must actually be crossed.
inline bool near_home_crossing_rig(const trace::Pose& p) noexcept {
    if (!trace::valid_pose(p)) { return false; }
    const auto x = std::abs(p.location[0]);
    const auto yaw = std::remainder(p.location[0] < 0 ? 180.0f - p.rotation[1] : p.rotation[1], 360.0f);
    const auto pitch = std::remainder(p.rotation[0], 360.0f);
    return x >= 900 && x <= 2000 && p.location[1] >= -1400 && p.location[1] <= 500 &&
        p.location[2] >= 50 && p.location[2] <= 350 && pitch >= -5 && pitch <= 12 &&
        yaw >= -175 && yaw <= -65 && p.fov >= 25 && p.fov <= 60;
}

inline bool crossing_candidate(const Request& r, bool presentation_camera,
    AuthoredScope scope = AuthoredScope::presentation) noexcept {
    return presentation_camera && r.enabled && r.prospi && r.mode == 3 && r.dolly_enabled &&
        r.sequencer_enabled && r.safety_enabled && r.automatic_source && !r.protected_camera &&
        (r.zone == 1 || r.zone == 2) && trace::valid_pose(r.camera) && r.camera.fov >= 0.5f &&
        r.camera.fov <= (scope == AuthoredScope::near_home_crossing ? 60.0f : 40.0f) &&
        (scope != AuthoredScope::near_home_crossing || near_home_crossing_rig(r.camera)) &&
        std::abs(std::remainder(r.camera.rotation[2], 360.0f)) <= 0.01f &&
        std::isfinite(r.dolly) && r.dolly > 0.0f && r.dolly <= 8000.0f &&
        std::isfinite(r.lift) && r.lift >= 0.0f && r.lift <= 600.0f &&
        std::isfinite(r.max_lift) && r.max_lift >= r.lift && r.max_lift <= 600.0f &&
        std::isfinite(r.floor) && r.floor >= -50.0f && r.floor <= 350.0f &&
        std::isfinite(r.base_fov) && r.base_fov >= 45.0f && r.base_fov <= 120.0f &&
        r.forward_offset == 0.0f && r.right_offset == 0.0f && r.up_offset == 0.0f;
}

// A last-resort rail for presentation rigs outside the calibrated envelopes.
// Change only a proved crossing of the game's current look-at point, not zoom
// preferences, camera aim, lift, or any already-in-front endpoint.
inline Decision bound_crossing(const Request& r, const trace::NativeSource& source, bool presentation_camera,
    AuthoredScope scope = AuthoredScope::presentation) noexcept {
    Decision out{.dolly = r.dolly, .lift = r.lift, .predicted_z = r.predicted_z};
    if (!crossing_candidate(r, presentation_camera, scope)) { return out; }
    out.status = Status::source_mismatch;
    if (!trace::valid_source(source) || !source.object || !source.camera_actor || !r.camera_manager ||
        !source.pose.aspect_valid || !std::isfinite(source.pose.aspect) || source.pose.aspect < 0.5f || source.pose.aspect > 3.0f ||
        source.focus_distance < 100.0f || source.focus_distance > (scope == AuthoredScope::near_home_crossing ? 1000.0f : 8000.0f) ||
        !trace::source_matches_input(source, r.camera, r.camera_manager, r.now_ns) ||
        r.now_ns < source.time_ns || r.now_ns - source.time_ns > 50'000'000 ||
        trace::distance(source.pose.location, r.camera.location) > 1.0f ||
        trace::angle_delta(source.pose.rotation[0], r.camera.rotation[0]) > 0.1f ||
        trace::angle_delta(source.pose.rotation[1], r.camera.rotation[1]) > 0.1f ||
        trace::angle_delta(source.pose.rotation[2], r.camera.rotation[2]) > 0.01f ||
        std::abs(source.pose.fov - r.camera.fov) > 0.1f) { return out; }
    constexpr double radians = 0.017453292519943295;
    const auto pitch = r.camera.rotation[0] * radians, yaw = r.camera.rotation[1] * radians;
    const auto roll = r.camera.rotation[2] * radians;
    const auto s = std::sin(pitch), c = std::cos(pitch);
    const std::array<double,3> forward{c * std::cos(yaw), c * std::sin(yaw), s};
    const std::array<double,3> right{-std::sin(yaw) * std::cos(roll) + s * std::cos(yaw) * std::sin(roll),
        std::cos(yaw) * std::cos(roll) + s * std::sin(yaw) * std::sin(roll), -c * std::sin(roll)};
    const std::array<double,3> up{-s * std::cos(yaw) * std::cos(roll) - std::sin(yaw) * std::sin(roll),
        -s * std::sin(yaw) * std::cos(roll) + std::cos(yaw) * std::sin(roll), c * std::cos(roll)};
    double depth{}, lateral{}, vertical{};
    for (size_t i = 0; i < 3; ++i) {
        const double delta = (double)source.look_at[i] - r.camera.location[i];
        depth += delta * forward[i]; lateral += delta * right[i]; vertical += delta * up[i];
    }
    if (!std::isfinite(depth) || !std::isfinite(lateral) || !std::isfinite(vertical) ||
        depth <= 0.0 || std::abs(depth - source.focus_distance) > 2.0 ||
        std::hypot(lateral, vertical) > depth * std::tan(0.15 * radians)) { return out; }
    out.status = Status::unchanged;
    if (r.dolly < depth) { return out; }
    const auto half_width = std::tan(r.base_fov * radians * 0.5), half_height = half_width / source.pose.aspect;
    const auto reserve = (std::max)({2.0, depth * std::tan(source.pose.fov * radians * 0.5) / half_width,
        std::abs(lateral) / (half_width * 0.5), std::abs(vertical - r.lift) / (half_height * 0.5)});
    const auto cap = (float)(depth - reserve - 0.01);
    const auto old_z = r.camera.location[2] + s * r.dolly + up[2] * r.lift;
    const auto end_z = r.camera.location[2] + s * cap + up[2] * r.lift;
    const auto required_z = (std::min)((std::max)((double)r.floor, old_z), (double)r.floor + 2.0);
    const auto remaining = depth - cap;
    // Match the existing float endpoint tolerance: 0.001 cm, not a lower floor.
    if (!std::isfinite(cap) || cap < 0.0f || cap >= r.dolly || !std::isfinite(end_z) ||
        end_z + 0.001 < required_z || remaining < reserve ||
        std::abs(lateral) > remaining * half_width * 0.5 || std::abs(vertical - r.lift) > remaining * half_height * 0.5) {
        out.status = Status::no_safe_endpoint; return out;
    }
    out.dolly = cap; out.predicted_z = (float)(r.camera.location[2] + s * cap);
    out.end_z = (float)end_z; out.target_depth = (float)remaining;
    out.ndc_x = (float)(lateral / (remaining * half_width));
    out.ndc_y = (float)((vertical - r.lift) / (remaining * half_height));
    out.status = Status::target_capped;
    return out;
}

inline std::optional<bool> authored_endpoint_visible(const Request& r, const trace::NativeSource& source,
    AuthoredScope scope = AuthoredScope::presentation) noexcept {
    const auto checked = bound_crossing(r, source, true, scope);
    if (checked.status == Status::off || checked.status == Status::source_mismatch) { return {}; }
    auto pose = r.camera;
    pose.fov = r.base_fov; pose.aspect = source.pose.aspect; pose.aspect_valid = true;
    constexpr double rad = 0.017453292519943295;
    const double pitch = pose.rotation[0]*rad, yaw = pose.rotation[1]*rad, roll = pose.rotation[2]*rad;
    const auto f = trace::forward(pose);
    const std::array<double,3> up{-std::sin(pitch)*std::cos(yaw)*std::cos(roll)-std::sin(yaw)*std::sin(roll),
        -std::sin(pitch)*std::sin(yaw)*std::cos(roll)+std::cos(yaw)*std::sin(roll), std::cos(pitch)*std::cos(roll)};
    for (size_t i=0;i<3;++i) { pose.location[i] += float(f[i]*r.dolly + up[i]*r.lift); }
    const auto framed = trace::target_framing(pose, source.look_at);
    if (!framed.valid) { return {}; }
    return !framed.behind && framed.clip_estimate_valid && !framed.offscreen_estimate;
}

// Preserve the authored framing window at the focus plane, not an inferred player
// bounding box. Enter only after the assist demonstrably crossed or clipped its
// current authored target; an already-safe close/action shot is an exact no-op.
inline Decision bound_authored_window(const Request& r, const trace::NativeSource& source,
    bool presentation_camera, float eye_margin = 0.0f, AuthoredScope scope = AuthoredScope::presentation) noexcept {
    auto out = bound_crossing(r, source, presentation_camera, scope);
    if (out.status == Status::off || out.status == Status::source_mismatch) { return out; }
    out = {.status = Status::unchanged, .dolly = r.dolly, .lift = r.lift, .predicted_z = r.predicted_z};
    if (!std::isfinite(eye_margin) || eye_margin < 0 || eye_margin > 100) { return out; }
    constexpr double radians = 0.017453292519943295;
    const double pitch = r.camera.rotation[0] * radians, yaw = r.camera.rotation[1] * radians;
    const double roll = r.camera.rotation[2] * radians;
    const double s = std::sin(pitch), c = std::cos(pitch);
    if (c <= 0.5) { return out; }
    const std::array<double,3> forward{c * std::cos(yaw), c * std::sin(yaw), s};
    const std::array<double,3> right{-std::sin(yaw) * std::cos(roll) + s * std::cos(yaw) * std::sin(roll),
        std::cos(yaw) * std::cos(roll) + s * std::sin(yaw) * std::sin(roll), -c * std::sin(roll)};
    const std::array<double,3> up{-s * std::cos(yaw) * std::cos(roll) - std::sin(yaw) * std::sin(roll),
        -s * std::sin(yaw) * std::cos(roll) + std::cos(yaw) * std::sin(roll), c * std::cos(roll)};
    double depth{}, x{}, y{};
    for (size_t i = 0; i < 3; ++i) {
        const double delta = double(source.look_at[i]) - r.camera.location[i];
        depth += delta * forward[i]; x += delta * right[i]; y += delta * up[i];
    }
    const double half_width = std::tan(r.base_fov * radians * 0.5), half_height = half_width / source.pose.aspect;
    const double remaining = depth - r.dolly;
    const bool crossed = remaining <= 0.0;
    if (scope == AuthoredScope::near_home_crossing && remaining > -2.0) { return out; }
    const bool clipped = remaining > 0.0 &&
        (std::abs(x) > remaining * half_width * 1.05 || std::abs(y - r.lift) > remaining * half_height * 1.05);
    if (!crossed && !clipped) { return out; }
    const double authored_width = depth * std::tan(source.pose.fov * radians * 0.5);
    const double authored_height = authored_width / source.pose.aspect;
    const double old_z = r.camera.location[2] + s * r.dolly + up[2] * r.lift;
    const double required_z = (std::min)((std::max)(double(r.floor), old_z), double(r.floor) + 2.0);
    const double a = (required_z - r.camera.location[2]) / up[2], b = -s / up[2];
    const double width = half_width * 0.95, height = half_height * 0.95;
    struct Endpoint { float dolly{}, lift{}; double z{}, depth{}, x{}, y{}; };
    const auto endpoint = [&](double dolly) {
        const auto d = float(dolly), lift = float((std::max)(0.0, a + b * d));
        return Endpoint{d, lift, r.camera.location[2] + s * d + up[2] * lift, depth - d, x, y - lift};
    };
    const auto safe = [&](const Endpoint& e) {
        return std::isfinite(e.z) && e.dolly >= 0 && e.dolly < r.dolly && e.lift >= 0 && e.lift <= r.max_lift &&
            e.z + 0.001 >= required_z && e.depth >= eye_margin + 2.0 &&
            std::abs(e.x) + authored_width + eye_margin <= (e.depth - eye_margin) * width + 0.0001 &&
            std::abs(e.y) + authored_height + eye_margin <= (e.depth - eye_margin) * height + 0.0001;
    };
    double best = -1.0;
    // Two linear lift regimes suffice; no game trace, actor search or iterative camera movement.
    const auto solve = [&](double intercept, double slope, bool lifted) {
        double lo = 0.0, hi = r.dolly;
        const auto constrain = [&](double coefficient, double limit) {
            if (std::abs(coefficient) < 1e-12) { return limit >= 0.0; }
            if (coefficient > 0) { hi = (std::min)(hi, limit / coefficient); }
            else { lo = (std::max)(lo, limit / coefficient); }
            return lo <= hi;
        };
        if (!constrain(lifted ? -b : b, lifted ? a : -a) ||
            !constrain(slope, r.max_lift - intercept) || !constrain(1.0, depth - eye_margin - 2.0) ||
            !constrain(width, (depth - eye_margin) * width - std::abs(x) - authored_width - eye_margin) ||
            !constrain(height - slope, (depth - eye_margin) * height - y + intercept - authored_height - eye_margin) ||
            !constrain(height + slope, (depth - eye_margin) * height + y - intercept - authored_height - eye_margin)) { return; }
        const auto d = (std::max)(lo, hi - 0.01);
        if (d >= 0 && d <= r.dolly && safe(endpoint(d))) { best = (std::max)(best, d); }
    };
    solve(0, 0, false); solve(a, b, true);
    if (best < 0) { out.status = Status::no_safe_endpoint; return out; }
    const auto e = endpoint(best);
    out.status = Status::target_capped; out.dolly = e.dolly; out.lift = e.lift;
    out.predicted_z = float(r.camera.location[2] + s * e.dolly);
    out.end_z = float(e.z); out.target_depth = float(e.depth);
    out.ndc_x = float(e.x / (e.depth * half_width)); out.ndc_y = float(e.y / (e.depth * half_height));
    return out;
}

} // namespace uevr::prospi::focus_guard
