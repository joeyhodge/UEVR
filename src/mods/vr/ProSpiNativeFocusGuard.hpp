#pragma once

#include "ProSpiCameraTrace.hpp"
#include "ProSpiCelebrationFraming.hpp"

namespace uevr::prospi::focus_guard {

enum class Status : int32_t {
    off, protected_camera, outside_envelope, invalid_input, source_unavailable,
    source_mismatch, no_safe_endpoint, unchanged, signed_height, target_capped,
};

inline const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::protected_camera: return "Existing assist (protected settings/camera)";
    case Status::outside_envelope: return "Existing assist (other camera rig)";
    case Status::invalid_input: return "Existing assist (invalid geometry/budget)";
    case Status::source_unavailable: return "Existing assist (native source unavailable)";
    case Status::source_mismatch: return "Existing assist (stale/mismatched source)";
    case Status::no_safe_endpoint: return "Existing assist (no validated endpoint)";
    case Status::unchanged: return "Existing assist already within bounds";
    case Status::signed_height: return "Signed low-camera height applied";
    case Status::target_capped: return "Dolly bounded before authored focus";
    default: return "Off / mode not selected";
    }
}

struct Request {
    trace::Pose camera{};
    uintptr_t camera_manager{};
    uint64_t now_ns{};
    int32_t mode{}, zone{};
    bool enabled{}, prospi{}, dolly_enabled{}, sequencer_enabled{}, safety_enabled{};
    bool protected_camera{}, automatic_source{}, close_camera{}, baseline_camera{};
    float dolly{}, lift{}, predicted_z{}, floor{}, max_lift{}, base_fov{};
    float forward_offset{}, right_offset{}, up_offset{};
};

inline bool keep_dugout_field_floor(bool enabled, int32_t mode, bool field_rule, bool authored_dugout) noexcept {
    return enabled && mode == 3 && field_rule && authored_dugout;
}

// These are low authored rigs, not inferred player/ball identities.
inline bool close_rig(const Request& r) noexcept {
    if (!r.close_camera || r.zone != 1 || !trace::valid_pose(r.camera)) { return false; }
    const auto& p = r.camera;
    const auto pitch = std::remainder(p.rotation[0], 360.0f);
    const auto yaw = std::remainder(p.rotation[1], 360.0f);
    if (std::abs(std::remainder(p.rotation[2], 360.0f)) > 0.01f || p.fov < 0.5f || p.fov > 36.0f) { return false; }
    const bool walk_up = p.fov >= 8.0f && std::abs(p.location[0]) <= 350.0f && p.location[1] >= -750.0f && p.location[1] <= 100.0f &&
        p.location[2] >= -100.0f && p.location[2] <= 150.0f && pitch >= 0.25f && pitch <= 12.0f &&
        yaw >= 25.0f && yaw <= 75.0f;
    const bool baseline = std::abs(p.location[0]) >= 1200.0f && std::abs(p.location[0]) <= 4200.0f &&
        p.location[1] >= -2500.0f && p.location[1] <= 1800.0f && p.location[2] >= -100.0f && p.location[2] <= 600.0f &&
        pitch >= -18.0f && pitch <= 16.0f && p.fov <= 24.0f &&
        (p.location[0] > 0.0f ? (yaw >= 125.0f || yaw <= -135.0f) : (yaw >= -45.0f && yaw <= 75.0f));
    return walk_up || baseline || (r.baseline_camera && celebration::baseline_rig(
        {p.location, p.rotation, p.fov}));
}

// Restrict the experiment to the two low, upward-pitched families evidenced in the trace.
inline bool low_rig(const Request& r) noexcept {
    if (!trace::valid_pose(r.camera)) { return false; }
    const auto& p = r.camera;
    const auto pitch = std::remainder(p.rotation[0], 360.0f);
    if (p.location[2] < -100.0f || p.location[2] > 150.0f || pitch < 0.25f || pitch > 8.0f ||
        std::abs(std::remainder(p.rotation[2], 360.0f)) > 0.01f) { return false; }
    const auto x = std::abs(p.location[0]), y = p.location[1];
    const auto yaw = std::remainder(p.rotation[1], 360.0f);
    // The Oct 9 marked-bad near-home cut traverses 7-10.5 degrees FOV.
    // Extend only that low/upward camera envelope, not the outfield/ball-follow path.
    return (r.zone == 1 && x <= 900.0f && y >= -4500.0f && y < -2800.0f &&
            (p.fov <= 8.0f || (p.fov <= 12.0f && yaw >= 75.0f && yaw <= 125.0f))) ||
        (p.fov <= 8.0f && r.zone == 4 && x >= 2500.0f && x <= 6000.0f && y >= -8000.0f && y <= -4500.0f &&
            ((p.location[0] < 0.0f && yaw >= 15.0f && yaw <= 65.0f) ||
             (p.location[0] > 0.0f && (yaw >= 115.0f || yaw <= -150.0f))));
}

inline bool valid_geometry(const Request& r) noexcept {
    const bool close = close_rig(r);
    return std::isfinite(r.dolly) && r.dolly >= 0.0f && r.dolly <= (close ? 2000.0f : 8000.0f) &&
        std::isfinite(r.lift) && (close ? r.lift >= 0.0f : r.lift > 0.0f) && r.lift <= 600.0f &&
        std::isfinite(r.predicted_z) && std::isfinite(r.floor) && r.floor >= -50.0f && r.floor <= 350.0f &&
        std::isfinite(r.max_lift) && r.max_lift >= 0.0f && r.max_lift <= 600.0f && r.lift <= r.max_lift &&
        std::isfinite(r.base_fov) && r.base_fov >= 45.0f && r.base_fov <= 120.0f &&
        r.forward_offset == 0.0f && r.right_offset == 0.0f && r.up_offset == 0.0f;
}

inline bool eligible(const Request& r) noexcept {
    return r.enabled && r.prospi && r.mode == 3 && r.dolly_enabled && r.sequencer_enabled &&
        r.safety_enabled && r.automatic_source && !r.protected_camera && (low_rig(r) || close_rig(r)) && valid_geometry(r);
}

struct Decision {
    Status status{Status::off};
    float dolly{}, lift{}, predicted_z{}, end_z{}, target_depth{}, ndc_x{}, ndc_y{};
    bool applied() const noexcept { return status == Status::signed_height || status == Status::target_capped; }
};

inline Decision evaluate(const Request& r, const trace::NativeSource& source) noexcept {
    Decision out{.dolly = r.dolly, .lift = r.lift, .predicted_z = r.predicted_z};
    if (!r.enabled || !r.prospi || r.mode != 3 || !r.dolly_enabled || !r.sequencer_enabled) { return out; }
    if (!r.safety_enabled || !r.automatic_source || r.protected_camera) {
        out.status = Status::protected_camera; return out;
    }
    const bool close = close_rig(r);
    if (!low_rig(r) && !close) { out.status = Status::outside_envelope; return out; }
    if (!valid_geometry(r)) {
        out.status = Status::invalid_input; return out;
    }
    if (!trace::valid_source(source) || !source.pose.aspect_valid ||
        !std::isfinite(source.pose.aspect) || source.pose.aspect < 0.5f || source.pose.aspect > 3.0f ||
        source.focus_distance < (close ? 100.0f : 500.0f) ||
        source.focus_distance > (close ? 5000.0f : 20000.0f) || source.pose.fov > 40.0f) {
        out.status = Status::source_unavailable; return out;
    }
    if (!r.camera_manager || !source.object || !source.camera_actor ||
        !trace::source_matches_input(source, r.camera, r.camera_manager, r.now_ns) ||
        r.now_ns - source.time_ns > 50'000'000 ||
        trace::distance(source.pose.location, r.camera.location) > 1.0f ||
        trace::angle_delta(source.pose.rotation[0], r.camera.rotation[0]) > 0.1f ||
        trace::angle_delta(source.pose.rotation[1], r.camera.rotation[1]) > (close ? 0.25f : 0.1f) ||
        trace::angle_delta(source.pose.rotation[2], r.camera.rotation[2]) > 0.01f ||
        (close && std::abs(source.pose.fov - r.camera.fov) > 0.1f)) {
        out.status = Status::source_mismatch; return out;
    }

    constexpr double radians = 0.017453292519943295;
    const auto pitch = r.camera.rotation[0] * radians, yaw = r.camera.rotation[1] * radians;
    const auto roll = r.camera.rotation[2] * radians;
    const auto s = std::sin(pitch), c = std::cos(pitch);
    const std::array<double, 3> forward{c * std::cos(yaw), c * std::sin(yaw), s};
    const std::array<double, 3> right{-std::sin(yaw) * std::cos(roll) + s * std::cos(yaw) * std::sin(roll),
        std::cos(yaw) * std::cos(roll) + s * std::sin(yaw) * std::sin(roll), -c * std::sin(roll)};
    const std::array<double, 3> up{-s * std::cos(yaw) * std::cos(roll) - std::sin(yaw) * std::sin(roll),
        -s * std::sin(yaw) * std::cos(roll) + std::cos(yaw) * std::sin(roll), c * std::cos(roll)};
    double depth{}, lateral{}, vertical{};
    for (size_t i = 0; i < 3; ++i) {
        const double delta = (double)source.look_at[i] - r.camera.location[i];
        depth += delta * forward[i]; lateral += delta * right[i]; vertical += delta * up[i];
    }
    if (!std::isfinite(depth) || !std::isfinite(lateral) || !std::isfinite(vertical) ||
        depth <= 0.0 || std::abs(depth - source.focus_distance) > 2.0 ||
        std::hypot(lateral, vertical) > depth * std::tan((close ? 0.30 : 0.15) * radians)) {
        out.status = Status::source_mismatch; return out;
    }

    const auto half_width = std::tan(r.base_fov * radians * 0.5);
    const auto half_height = half_width / source.pose.aspect;
    // Retain at least the depth implied by the original game FOV, not a guessed stadium zoom.
    const auto reserve = (std::max)(2.0, source.focus_distance *
        std::tan(source.pose.fov * radians * 0.5) / half_width);
    if (close) {
        // Already-safe close shots keep their exact dolly/lift, including action zoom.
        const auto signed_z = r.camera.location[2] + s * r.dolly;
        const auto z = signed_z + up[2] * r.lift;
        const auto minimum_lift = (std::max)(0.0, (r.floor + 2.0 - signed_z) / up[2]);
        // An unsigned downward estimate must not preserve a spurious lift on an upward baseline dolly.
        const bool excess_unsigned_lift = r.baseline_camera && signed_z > r.predicted_z + 1.0 &&
            r.lift > minimum_lift + 1.0;
        const auto remaining = depth - r.dolly;
        if (!excess_unsigned_lift && z >= r.floor && remaining >= reserve &&
            std::abs(lateral) <= remaining * half_width * 0.95 &&
            std::abs(vertical - r.lift) <= remaining * half_height * 0.95) {
            out.status = Status::unchanged; return out;
        }
    }
    const auto floor = (double)r.floor + 2.0;
    // Cover the accepted 1 cm / 0.1 degree source-pose agreement tolerance without lowering the floor.
    const auto minimum_s = std::sin(pitch - 0.1 * radians);
    const auto minimum_up_z = std::cos(std::abs(pitch) + 0.1 * radians) * std::cos(std::abs(roll) + 0.01 * radians);
    const auto k = (floor - (r.camera.location[2] - 1.0)) / minimum_up_z, a = minimum_s / minimum_up_z;
    const auto budget = (double)(close ? r.max_lift : (std::min)(r.max_lift, r.lift));
    // Corrected close shots need framing margin, not a subject on the viewport edge.
    const auto clip_fraction = close ? 0.5 : 0.95;
    struct Endpoint { double dolly{}, lift{}, z{}, depth{}, x{}, y{}; };
    const auto endpoint = [&](double dolly) {
        const auto lift = (std::max)(0.0, k - a * dolly);
        return Endpoint{dolly, lift, r.camera.location[2] + s * dolly + up[2] * lift,
            depth - dolly, lateral, vertical - lift};
    };
    const auto safe = [&](const Endpoint& e) {
        return std::isfinite(e.z) && e.dolly >= 0.0 && e.dolly <= r.dolly &&
            e.lift <= budget + 0.0001 && e.z >= floor - 0.001 && e.depth >= reserve - 0.001 &&
            r.camera.location[2] - 1.0 + minimum_s * e.dolly + minimum_up_z * e.lift >= floor - 0.001 &&
            std::abs(e.x) <= e.depth * half_width * clip_fraction + 0.0001 &&
            std::abs(e.y) <= e.depth * half_height * clip_fraction + 0.0001;
    };
    auto selected = endpoint(r.dolly);
    if (!safe(selected)) {
        // Two linear lift regimes give bounded feasible intervals; no iterative game calls or path scan.
        double best = -1.0;
        const auto solve = [&](double lo, double hi, double lift_intercept, double lift_slope) {
            const auto constrain = [&](double coefficient, double bound) {
                if (std::abs(coefficient) < 1e-12) { return bound >= 0.0; }
                if (coefficient > 0.0) { hi = (std::min)(hi, bound / coefficient); }
                else { lo = (std::max)(lo, bound / coefficient); }
                return lo <= hi;
            };
            if (lo > hi || !constrain(lift_slope, budget - lift_intercept) ||
                !constrain(1.0, depth - reserve) ||
                !constrain(half_width * clip_fraction, depth * half_width * clip_fraction - std::abs(lateral)) ||
                !constrain(half_height * clip_fraction - lift_slope,
                    depth * half_height * clip_fraction - vertical + lift_intercept) ||
                !constrain(half_height * clip_fraction + lift_slope,
                    depth * half_height * clip_fraction + vertical - lift_intercept)) { return; }
            // Tiny inward margin avoids rounding a float endpoint across its limiting plane.
            const auto d = (std::max)(lo, hi - 0.01);
            if (d >= 0.0 && d <= r.dolly && safe(endpoint(d))) { best = (std::max)(best, d); }
        };
        if (a > 1e-12) {
            const auto transition = k / a;
            solve(0.0, (std::min)((double)r.dolly, transition), k, -a);
            solve((std::max)(0.0, transition), r.dolly, 0.0, 0.0);
        } else if (a < -1e-12) {
            const auto transition = k / a;
            solve(0.0, (std::min)((double)r.dolly, transition), 0.0, 0.0);
            solve((std::max)(0.0, transition), r.dolly, k, -a);
        } else {
            solve(0.0, r.dolly, (std::max)(0.0, k), 0.0);
        }
        if (best < 0.0) { out.status = Status::no_safe_endpoint; return out; }
        selected = endpoint(best);
    }
    const auto final = endpoint((float)selected.dolly);
    if (!safe(final) || (!close && (float)final.lift > r.lift) || (float)final.lift > r.max_lift) {
        out.status = Status::no_safe_endpoint; return out;
    }
    out.dolly = (float)final.dolly; out.lift = (float)final.lift;
    out.predicted_z = (float)(r.camera.location[2] + s * final.dolly);
    out.end_z = (float)final.z; out.target_depth = (float)final.depth;
    out.ndc_x = (float)(final.x / (final.depth * half_width));
    out.ndc_y = (float)(final.y / (final.depth * half_height));
    out.status = r.dolly - out.dolly > 0.001f ? Status::target_capped :
        std::abs(r.lift - out.lift) > 0.001f ? Status::signed_height : Status::unchanged;
    return out;
}

} // namespace uevr::prospi::focus_guard
