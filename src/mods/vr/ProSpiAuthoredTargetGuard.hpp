#pragma once

#include "ProSpiNativeFocusGuard.hpp"

namespace uevr::prospi::focus_guard {

enum class AuthoredScope : uint8_t {
    presentation, near_home_crossing, interview_repair, dugout_repair, home_pan_repair,
    dugout_entry_repair, upward_home_repair, podium_closeup_repair, low_infield_closeup_repair,
    first_base_corner_repair, first_base_dugout_repair, automatic_window, outward_outfield
};

inline bool live_stereo_window_scope(AuthoredScope scope) noexcept {
    return scope == AuthoredScope::automatic_window || scope == AuthoredScope::outward_outfield;
}

inline bool recorded_first_base_scope(AuthoredScope scope) noexcept {
    return scope == AuthoredScope::first_base_corner_repair || scope == AuthoredScope::first_base_dugout_repair;
}

inline bool recorded_pan_scope(AuthoredScope scope) noexcept {
    return scope == AuthoredScope::dugout_entry_repair || scope == AuthoredScope::upward_home_repair ||
        scope == AuthoredScope::podium_closeup_repair || scope == AuthoredScope::low_infield_closeup_repair ||
        recorded_first_base_scope(scope);
}

inline bool repair_scope(AuthoredScope scope) noexcept {
    return scope == AuthoredScope::interview_repair || scope == AuthoredScope::dugout_repair ||
        scope == AuthoredScope::home_pan_repair || live_stereo_window_scope(scope) || recorded_pan_scope(scope);
}

inline bool dugout_scope(AuthoredScope scope) noexcept {
    return scope == AuthoredScope::dugout_repair;
}

inline bool dugout_repair_rig(const trace::Pose& p) noexcept {
    if (!trace::valid_pose(p) || std::abs(std::remainder(p.rotation[2], 360.0f)) > 25.0f) { return false; }
    // Reuse the evidenced low-rig envelope; only admission ignores roll. All
    // endpoint/source checks below retain the actual full camera basis.
    auto envelope = framing::Pose{p.location, p.rotation, p.fov};
    envelope.rotation[2] = 0;
    return celebration::low_rig(envelope);
}

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

// Admission is not proof of a bad shot. The current native lens/target and
// an already-crossed or offscreen endpoint are still required below.
inline bool home_pan_repair_rig(const trace::Pose& p) noexcept {
    if (!trace::valid_pose(p)) { return false; }
    // Keep the existing interview/dugout rigs and authored behind-backstop
    // cameras outside this new scope; removing their lift needs other proof.
    return !near_home_crossing_rig(p) && std::abs(p.location[0]) <= 900 &&
        p.location[1] >= -1000 && p.location[1] <= 1800 &&
        p.location[2] >= -100 && p.location[2] <= 600 &&
        std::abs(std::remainder(p.rotation[1] + 90, 360.0f)) <= 40 &&
        std::remainder(p.rotation[0], 360.0f) >= -25 && std::remainder(p.rotation[0], 360.0f) <= 15 &&
        std::abs(std::remainder(p.rotation[2], 360.0f)) <= 25 && p.fov >= 5 && p.fov <= 60;
}

// Recorded presentation rigs. The pose envelope alone never authorizes a correction.
inline bool recorded_pan_rig(const trace::Pose& p, AuthoredScope scope) noexcept {
    if (!trace::valid_pose(p) || std::abs(std::remainder(p.rotation[2], 360.0f)) > 0.01f) { return false; }
    const auto [x, y, z] = p.location;
    const auto pitch = std::remainder(p.rotation[0], 360.0f), yaw = std::remainder(p.rotation[1], 360.0f);
    switch (scope) {
    case AuthoredScope::dugout_entry_repair:
        return x >= -3150 && x <= -2250 && y >= 200 && y <= 750 && z >= 40 && z <= 190 &&
            pitch >= 2 && pitch <= 15 && yaw >= -110 && yaw <= -70 && p.fov >= 12 && p.fov <= 30;
    case AuthoredScope::upward_home_repair:
        return std::abs(x) <= 150 && y >= -350 && y <= -75 && z >= 50 && z <= 190 &&
            pitch >= 2 && pitch <= 22 && yaw >= 30 && yaw <= 75 && p.fov >= 25 && p.fov <= 39;
    case AuthoredScope::low_infield_closeup_repair:
        return std::abs(x) <= 75 && y >= -410 && y <= -340 && z >= 20 && z <= 80 &&
            pitch >= 4 && pitch <= 11 && yaw >= 47 && yaw <= 66 && p.fov >= 9 && p.fov <= 16;
    case AuthoredScope::first_base_corner_repair:
        return x >= 2860 && x <= 2930 && y >= -300 && y <= -235 && z >= 112 && z <= 145 &&
            pitch >= 0.2f && pitch <= 2.2f && yaw >= 134 && yaw <= 140 && p.fov >= 10 && p.fov <= 21;
    case AuthoredScope::first_base_dugout_repair: {
        // Entry through the crowd label and the rising line pan are one rig.
        const bool entry = x >= 2270 && x <= 2690 && y >= -755 && y <= -345 && z >= 132 && z <= 158 &&
            yaw >= 58 && yaw <= 81 && pitch >= 0.2f && pitch <= 2.5f;
        const bool tracking = x >= 2575 && x <= 2975 && y >= -945 && y <= -660 && z >= 86 && z <= 158 &&
            yaw >= 72 && yaw <= 111 && pitch >= 0.2f && pitch <= 11.5f;
        return p.fov >= 10.5f && p.fov <= 16.5f && (entry || tracking);
    }
    case AuthoredScope::podium_closeup_repair:
        return x >= 1150 && x <= 1450 && y >= -550 && y <= -320 && z >= 100 && z <= 230 &&
            pitch >= 1 && pitch <= 10 && yaw >= -165 && yaw <= -125 && p.fov >= 25 && p.fov <= 39;
    default: return false;
    }
}

inline bool recorded_pan_target(const Request& r, const trace::NativeSource& source, AuthoredScope scope) noexcept {
    const auto [x, y, z] = source.look_at;
    const auto focus = source.focus_distance;
    switch (scope) {
    case AuthoredScope::dugout_entry_repair:
        return std::abs(x - r.camera.location[0]) <= 300 && y >= -525 && y <= 300 &&
            z >= 75 && z <= 240 && focus >= 300 && focus <= 950;
    case AuthoredScope::upward_home_repair:
        return std::abs(x) <= 250 && y >= -250 && y <= 150 && z >= 95 && z <= 240 &&
            focus >= 125 && focus <= 450;
    case AuthoredScope::low_infield_closeup_repair:
        return x >= 175 && x <= 750 && y >= 30 && y <= 550 &&
            z >= 100 && z <= 180 && focus >= 450 && focus <= 1200;
    case AuthoredScope::first_base_corner_repair:
        return x >= 2110 && x <= 2570 && y >= 55 && y <= 475 && z >= 110 && z <= 170 &&
            focus >= 475 && focus <= 1075;
    case AuthoredScope::first_base_dugout_repair:
        return x >= 2440 && x <= 2880 && y >= -465 && y <= 30 && z >= 130 && z <= 205 &&
            focus >= 350 && focus <= 820;
    case AuthoredScope::podium_closeup_repair:
        return x >= 875 && x <= 1350 && y >= -825 && y <= -450 && z >= 115 && z <= 250 &&
            focus >= 170 && focus <= 400;
    default: return false;
    }
}

inline bool authored_offsets_supported(const Request& r, AuthoredScope scope) noexcept {
    if (!recorded_pan_scope(scope) && !live_stereo_window_scope(scope)) {
        return r.forward_offset == 0.0f && r.right_offset == 0.0f && r.up_offset == 0.0f;
    }
    return r.forward_offset * r.forward_offset + r.right_offset * r.right_offset +
        r.up_offset * r.up_offset <= 30.0f * 30.0f;
}

inline bool outward_outfield_rig(const trace::Pose& p) noexcept {
    // In this field coordinate system home is toward +Y from the outfield.
    // Use the direction along the field, not a radial stadium boundary: an
    // oblique pan can turn toward center field while still moving deeper out.
    return trace::valid_pose(p) && p.location[1] <= -4500 && p.location[1] >= -13000 &&
        std::abs(p.location[0]) <= 10000 && p.location[2] >= -100 && p.location[2] <= 4200 &&
        trace::angle_delta(p.rotation[0], 0) <= 35 && std::sin(p.rotation[1] * 0.017453292519943295f) <= -0.35f;
}

inline bool crossing_candidate(const Request& r, bool presentation_camera,
    AuthoredScope scope = AuthoredScope::presentation) noexcept {
    const bool outward = scope == AuthoredScope::outward_outfield;
    return presentation_camera && r.enabled && r.prospi && r.mode == 3 && r.dolly_enabled &&
        r.sequencer_enabled && r.safety_enabled && r.automatic_source && !r.protected_camera &&
        (outward ? r.zone == 4 : (r.zone == 1 || r.zone == 2)) && trace::valid_pose(r.camera) && r.camera.fov >= 0.5f &&
        r.camera.fov <= (scope == AuthoredScope::near_home_crossing || scope == AuthoredScope::interview_repair ||
            scope == AuthoredScope::home_pan_repair || live_stereo_window_scope(scope) ? 60.0f : 40.0f) &&
        (!outward || (r.dolly >= 2000 && outward_outfield_rig(r.camera))) &&
        ((scope != AuthoredScope::near_home_crossing && scope != AuthoredScope::interview_repair) || near_home_crossing_rig(r.camera)) &&
        (!dugout_scope(scope) || dugout_repair_rig(r.camera)) &&
        (scope != AuthoredScope::home_pan_repair || home_pan_repair_rig(r.camera)) &&
        (!recorded_pan_scope(scope) || recorded_pan_rig(r.camera, scope)) &&
        std::abs(std::remainder(r.camera.rotation[2], 360.0f)) <= (repair_scope(scope) ? 25.0f : 0.01f) &&
        std::isfinite(r.dolly) && (scope == AuthoredScope::automatic_window ? r.dolly >= 0.0f : r.dolly > 0.0f) &&
        r.dolly <= (outward ? 10000.0f : 8000.0f) &&
        std::isfinite(r.lift) && r.lift >= 0.0f && r.lift <= 600.0f &&
        std::isfinite(r.max_lift) && r.max_lift >= r.lift && r.max_lift <= 600.0f &&
        std::isfinite(r.floor) && r.floor >= -50.0f && r.floor <= 350.0f &&
        std::isfinite(r.base_fov) && r.base_fov >= 45.0f && r.base_fov <= 120.0f &&
        authored_offsets_supported(r, scope);
}

struct AuthoredProjection {
    double depth{}, x{}, y{}, camera_z{}, forward_z{}, up_z{}, cos_pitch{};
    double half_width{}, half_height{}, authored_width{}, authored_height{};

    double dolly_z(double dolly) const noexcept { return camera_z + forward_z * dolly; }
    double end_z(double dolly, double lift) const noexcept { return dolly_z(dolly) + up_z * lift; }
};

struct StereoWindow {
    float half_width{}, half_height{}, rotation_slack{};

    bool valid() const noexcept {
        return half_width >= 0.2f && half_width <= 5.0f && half_height >= 0.2f && half_height <= 5.0f &&
            rotation_slack >= 0 && rotation_slack <= 2;
    }
};

inline StereoWindow common_openxr_window(const std::array<std::array<float, 4>, 2>& eyes,
    float rotation_slack = 0) noexcept {
    StereoWindow out{5, 5, 0};
    for (const auto& eye : eyes) {
        // OpenXR tangents: left and down are negative. Use the window common to both eyes.
        const auto [left, right, up, down] = eye;
        if (!(-left >= 0.2f && -left <= 5 && right >= 0.2f && right <= 5 &&
            up >= 0.2f && up <= 5 && -down >= 0.2f && -down <= 5)) { return {}; }
        out.half_width = (std::min)({out.half_width, -left, right});
        out.half_height = (std::min)({out.half_height, up, -down});
    }
    // ||eye rotation - I|| bounds error in each coordinate, including depth.
    const float delta = rotation_slack * std::hypot(1.0f, out.half_width, out.half_height);
    out.half_width -= delta * (1 + out.half_width);
    out.half_height -= delta * (1 + out.half_height);
    out.rotation_slack = rotation_slack;
    return out;
}

// Called only after crossing_candidate validates the request geometry.
inline std::optional<AuthoredProjection> project_authored_target(const Request& r,
    const trace::NativeSource& source, AuthoredScope scope, StereoWindow stereo = {}) noexcept {
    const bool live_stereo = live_stereo_window_scope(scope);
    const bool outward = scope == AuthoredScope::outward_outfield;
    if (!trace::valid_source(source) || !source.object || !source.camera_actor || !r.camera_manager ||
        (live_stereo && (!stereo.valid() || source.focus_distance < 150.0f)) ||
        (scope == AuthoredScope::automatic_window && source.focus_distance > 4500.0f) ||
        !source.pose.aspect_valid || !std::isfinite(source.pose.aspect) || source.pose.aspect < 0.5f || source.pose.aspect > 3.0f ||
        source.focus_distance < 100.0f || source.focus_distance >
            (dugout_scope(scope) ? 1250.0f : scope == AuthoredScope::home_pan_repair ? 6000.0f :
                scope == AuthoredScope::near_home_crossing || scope == AuthoredScope::interview_repair ? 1000.0f :
                outward ? 10000.0f : 8000.0f) ||
        !trace::source_matches_input(source, r.camera, r.camera_manager, r.now_ns) ||
        r.now_ns < source.time_ns || r.now_ns - source.time_ns > 50'000'000 ||
        trace::distance(source.pose.location, r.camera.location) > 1.0f ||
        trace::angle_delta(source.pose.rotation[0], r.camera.rotation[0]) > 0.1f ||
        trace::angle_delta(source.pose.rotation[1], r.camera.rotation[1]) > 0.1f ||
        trace::angle_delta(source.pose.rotation[2], r.camera.rotation[2]) > 0.01f ||
        std::abs(source.pose.fov - r.camera.fov) > 0.1f ||
        (recorded_pan_scope(scope) && !recorded_pan_target(r, source, scope))) { return {}; }
    if (scope == AuthoredScope::home_pan_repair &&
        (std::abs(source.look_at[0]) > 1500 || source.look_at[1] < -2000 || source.look_at[1] > 750 ||
            source.look_at[2] < -50 || source.look_at[2] > 400)) { return {}; }
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
        std::hypot(lateral, vertical) > depth * std::tan(0.15 * radians)) { return {}; }

    // Keep the original authored window, then translate into UEVR's saved
    // camera-relative offsets before solving for the additional dolly/lift.
    const auto width = depth * std::tan(source.pose.fov * radians * 0.5);
    const auto half_width = std::tan(r.base_fov * radians * 0.5);
    const auto half_height = half_width / source.pose.aspect;
    return AuthoredProjection{
        depth - r.forward_offset, lateral - r.right_offset, vertical - r.up_offset,
        r.camera.location[2] + s * r.forward_offset + right[2] * r.right_offset + up[2] * r.up_offset,
        s, up[2], c, live_stereo ? (std::min)(half_width, double(stereo.half_width)) : half_width,
        live_stereo ? (std::min)(half_height, double(stereo.half_height)) : half_height, width, width / source.pose.aspect};
}

inline bool authored_window_contains(const AuthoredProjection& p, float dolly, float lift,
    double floor, float eye_margin) noexcept {
    const auto remaining = p.depth - dolly;
    return p.end_z(dolly, lift) + 0.001 >= floor && remaining >= eye_margin + 2.0 &&
        std::abs(p.x) + p.authored_width + eye_margin <= (remaining - eye_margin) * (p.half_width * 0.95) + 0.0001 &&
        std::abs(p.y - lift) + p.authored_height + eye_margin <= (remaining - eye_margin) * (p.half_height * 0.95) + 0.0001;
}

// A last-resort rail for presentation rigs outside the calibrated envelopes.
// Change only a proved crossing of the game's current look-at point, not zoom
// preferences, camera aim, lift, or any already-in-front endpoint.
inline Decision bound_crossing(const Request& r, const trace::NativeSource& source, bool presentation_camera,
    AuthoredScope scope = AuthoredScope::presentation) noexcept {
    Decision out{.dolly = r.dolly, .lift = r.lift, .predicted_z = r.predicted_z};
    if (!crossing_candidate(r, presentation_camera, scope)) { return out; }
    const auto projection = project_authored_target(r, source, scope);
    if (!projection) { out.status = Status::source_mismatch; return out; }
    const auto& p = *projection;
    out.status = Status::unchanged;
    if (r.dolly < p.depth) { return out; }
    const auto reserve = (std::max)({2.0, p.authored_width / p.half_width,
        std::abs(p.x) / (p.half_width * 0.5), std::abs(p.y - r.lift) / (p.half_height * 0.5)});
    const auto cap = (float)(p.depth - reserve - 0.01);
    const auto old_z = p.end_z(r.dolly, r.lift);
    const auto end_z = p.end_z(cap, r.lift);
    const auto required_z = (std::min)((std::max)((double)r.floor, old_z), (double)r.floor + 2.0);
    const auto remaining = p.depth - cap;
    // Match the existing float endpoint tolerance: 0.001 cm, not a lower floor.
    if (!std::isfinite(cap) || cap < 0.0f || cap >= r.dolly || !std::isfinite(end_z) ||
        end_z + 0.001 < required_z || remaining < reserve ||
        std::abs(p.x) > remaining * p.half_width * 0.5 || std::abs(p.y - r.lift) > remaining * p.half_height * 0.5) {
        out.status = Status::no_safe_endpoint; return out;
    }
    out.dolly = cap; out.predicted_z = (float)p.dolly_z(cap);
    out.end_z = (float)end_z; out.target_depth = (float)remaining;
    out.ndc_x = (float)(p.x / (remaining * p.half_width));
    out.ndc_y = (float)((p.y - r.lift) / (remaining * p.half_height));
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
    const std::array<double,3> right{-std::sin(yaw)*std::cos(roll)+std::sin(pitch)*std::cos(yaw)*std::sin(roll),
        std::cos(yaw)*std::cos(roll)+std::sin(pitch)*std::sin(yaw)*std::sin(roll), -std::cos(pitch)*std::sin(roll)};
    const std::array<double,3> up{-std::sin(pitch)*std::cos(yaw)*std::cos(roll)-std::sin(yaw)*std::sin(roll),
        -std::sin(pitch)*std::sin(yaw)*std::cos(roll)+std::cos(yaw)*std::sin(roll), std::cos(pitch)*std::cos(roll)};
    for (size_t i = 0; i < 3; ++i) {
        pose.location[i] += float(f[i] * (r.dolly + r.forward_offset) + right[i] * r.right_offset +
            up[i] * (r.lift + r.up_offset));
    }
    const auto framed = trace::target_framing(pose, source.look_at);
    if (!framed.valid) { return {}; }
    return !framed.behind && framed.clip_estimate_valid && !framed.offscreen_estimate;
}

// Preserve the authored framing window at the focus plane, not an inferred player
// bounding box. Enter only after the assist demonstrably crossed or clipped its
// current authored target; an already-safe close/action shot is an exact no-op.
inline Decision bound_authored_window(const Request& r, const trace::NativeSource& source,
    bool presentation_camera, float eye_margin = 0.0f, AuthoredScope scope = AuthoredScope::presentation,
    StereoWindow stereo = {}) noexcept {
    Decision out{.dolly = r.dolly, .lift = r.lift, .predicted_z = r.predicted_z};
    if (!crossing_candidate(r, presentation_camera, scope)) { return out; }
    const auto projection = project_authored_target(r, source, scope, stereo);
    if (!projection) { out.status = Status::source_mismatch; return out; }
    const auto& p = *projection;
    out.status = Status::unchanged;
    if (!std::isfinite(eye_margin) || eye_margin < 0 || eye_margin > 100) { return out; }
    if (p.cos_pitch <= 0.5) { return out; }
    const double remaining = p.depth - r.dolly;
    const bool crossed = remaining <= 0.0;
    const bool low_infield = scope == AuthoredScope::low_infield_closeup_repair;
    // This cut loses the focus below the view before dolly crosses it. Require
    // a clear vertical miss from the lifted endpoint, not just a tight crop.
    const bool clipped_below = low_infield && r.lift > 0 &&
        p.y - r.lift < -(std::max)(remaining, 2.0) * p.half_height * 1.05;
    if ((scope == AuthoredScope::near_home_crossing || recorded_pan_scope(scope)) &&
        remaining > -2.0 && !clipped_below) { return out; }
    const bool clipped = clipped_below || (remaining > 0.0 &&
        (std::abs(p.x) > remaining * p.half_width * 1.05 || std::abs(p.y - r.lift) > remaining * p.half_height * 1.05));
    const bool automatic = scope == AuthoredScope::automatic_window;
    const bool outward = scope == AuthoredScope::outward_outfield;
    if (live_stereo_window_scope(scope)) {
        // Ignore tight but still visible cuts and misses already present in the saved offsets alone.
        const bool saved_view_visible = p.depth > eye_margin + 2.0 &&
            std::abs(p.x) + eye_margin <= (p.depth - eye_margin) * p.half_width &&
            std::abs(p.y) + eye_margin <= (p.depth - eye_margin) * p.half_height;
        if (!saved_view_visible) { return out; }
        if (outward) {
            // The ball-follow label alone does not identify the subject. Only a
            // proven crossing permits a change; a low but in-front focus does not.
            if (remaining > -double(eye_margin) - 2.0) { return out; }
        } else {
            // A wide headset can still display a subject below the broadcast framing window.
            const double y = p.y - r.lift, bottom = (std::max)(p.half_height * 1.05, 0.57735);
            const double slack = eye_margin + stereo.rotation_slack * std::hypot(p.x, y, remaining);
            const bool lost_below = y + (std::max)(remaining, 2.0) * bottom + slack * std::hypot(1.0, bottom) < 0.0;
            if (remaining > -2.0 && !lost_below) { return out; }
        }
    } else if (!crossed && !clipped) { return out; }
    const double old_z = p.end_z(r.dolly, r.lift);
    // A level first-base shot can cross its target without excessive lift.
    if (recorded_pan_scope(scope) && !recorded_first_base_scope(scope) && old_z <= double(r.floor) + 2.0) { return out; }
    const double required_z = live_stereo_window_scope(scope) ? double(r.floor) + 2.0 :
        (std::min)((std::max)(double(r.floor), old_z), double(r.floor) + 2.0);
    const double a = (required_z - p.camera_z) / p.up_z, b = -p.forward_z / p.up_z;
    const auto lift_limit = low_infield || outward ? r.lift : r.max_lift;
    const double width = p.half_width * 0.95, height = p.half_height * 0.95;
    struct Endpoint { float dolly{}, lift{}; };
    const auto endpoint = [&](double dolly) {
        const auto d = float(dolly), lift = float((std::max)(0.0, a + b * d));
        return Endpoint{d, lift};
    };
    const auto safe = [&](const Endpoint& e) {
        const bool improves = e.dolly < r.dolly || (automatic && e.dolly == r.dolly && e.lift < r.lift - 0.01f);
        return e.dolly >= 0 && improves && e.lift >= 0 && e.lift <= lift_limit &&
            authored_window_contains(p, e.dolly, e.lift, required_z, eye_margin);
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
            !constrain(slope, lift_limit - intercept) || !constrain(1.0, p.depth - eye_margin - 2.0) ||
            !constrain(width, (p.depth - eye_margin) * width - std::abs(p.x) - p.authored_width - eye_margin) ||
            !constrain(height - slope, (p.depth - eye_margin) * height - p.y + intercept - p.authored_height - eye_margin) ||
            !constrain(height + slope, (p.depth - eye_margin) * height + p.y - intercept - p.authored_height - eye_margin)) { return; }
        const auto d = (std::max)(lo, hi - 0.01);
        if (d >= 0 && d <= r.dolly && safe(endpoint(d))) { best = (std::max)(best, d); }
    };
    solve(0, 0, false); solve(a, b, true);
    if (best < 0) { out.status = Status::no_safe_endpoint; return out; }
    const auto e = endpoint(best);
    const auto depth = p.depth - e.dolly;
    out.status = Status::target_capped; out.dolly = e.dolly; out.lift = e.lift;
    out.predicted_z = float(p.dolly_z(e.dolly));
    out.end_z = float(p.end_z(e.dolly, e.lift)); out.target_depth = float(depth);
    out.ndc_x = float(p.x / (depth * p.half_width)); out.ndc_y = float((p.y - e.lift) / (depth * p.half_height));
    return out;
}

} // namespace uevr::prospi::focus_guard
