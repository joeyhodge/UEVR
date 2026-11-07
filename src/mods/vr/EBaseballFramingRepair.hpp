#pragma once

#include "ProSpiAuthoredTargetGuard.hpp"
#include "ProSpiShotIdentity.hpp"
#include "ProSpiCameraReturnSmoothing.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace uevr::prospi::repair {

enum class Reason : uint8_t {
    off, unchanged, protected_controls, unsupported_rig, source_unavailable,
    source_mismatch, no_safe_endpoint, interview_focus, dugout_focus, dugout_height, release_hold
};
inline const char* reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::unchanged: return "Existing framing is safe";
    case Reason::protected_controls: return "Explicit controls / protected camera";
    case Reason::unsupported_rig: return "Outside evidenced interview/dugout rigs";
    case Reason::source_unavailable: return "No verified current native source";
    case Reason::source_mismatch: return "Current native source does not match";
    case Reason::no_safe_endpoint: return "No validated pullback endpoint";
    case Reason::interview_focus: return "Interview authored window recovered";
    case Reason::dugout_focus: return "Dugout authored window recovered";
    case Reason::dugout_height: return "Dugout field-side height recovered";
    case Reason::release_hold: return "Safe pullback retained during recovery";
    default: return "Off / unsupported build";
    }
}

struct Request {
    focus_guard::Request camera{};
    float field_floor{}, legacy_floor{}, smoothing{0.25f};
    int32_t rendering_method{};
    bool ebaseball{}, manual_controls{}, protected_camera{};
};
struct Decision {
    Reason reason{};
    float dolly{}, lift{};
    bool applied{};
};

inline focus_guard::AuthoredScope scope(const Request& r) noexcept {
    if (focus_guard::near_home_crossing_rig(r.camera.camera)) { return focus_guard::AuthoredScope::interview_repair; }
    return focus_guard::AuthoredScope::dugout_repair;
}

inline focus_guard::Request geometry(const Request& r, const trace::Pose& input, uint64_t now,
    float dolly, float lift) noexcept {
    auto c = r.camera;
    c.camera = input; c.now_ns = now; c.dolly = dolly; c.lift = lift;
    c.floor = r.field_floor; c.zone = 1;
    return c;
}

inline bool candidate(const Request& r) noexcept {
    if (!r.ebaseball || r.manual_controls || r.protected_camera ||
        // FieldFloor / BaselineDugout / a misclassified low StandCrowd rig.
        (r.camera.zone != 1 && r.camera.zone != 2 && r.camera.zone != 3) ||
        !std::isfinite(r.field_floor) || r.field_floor < -50 || r.field_floor > 350 ||
        !std::isfinite(r.legacy_floor) || !std::isfinite(r.smoothing)) { return false; }
    const auto c = geometry(r, r.camera.camera, r.camera.now_ns, r.camera.dolly, r.camera.lift);
    return focus_guard::crossing_candidate(c, true, scope(r));
}

inline bool window_contains(const Request& r, const trace::NativeSource& native, float dolly, float lift,
    float eye_margin) noexcept {
    if (!candidate(r)) { return false; }
    const auto c = geometry(r, r.camera.camera, r.camera.now_ns, r.camera.dolly, r.camera.lift);
    const auto checked = focus_guard::bound_crossing(c, native, true, scope(r));
    if (checked.status == focus_guard::Status::off || checked.status == focus_guard::Status::source_mismatch ||
        !std::isfinite(dolly) || !std::isfinite(lift) || dolly < 0 || dolly > c.dolly || lift < 0 || lift > c.max_lift ||
        !std::isfinite(eye_margin) || eye_margin < 0 || eye_margin > 100) { return false; }
    constexpr double rad = std::numbers::pi_v<double> / 180;
    const auto pitch = c.camera.rotation[0] * rad, yaw = c.camera.rotation[1] * rad, roll = c.camera.rotation[2] * rad;
    const auto s = std::sin(pitch), cp = std::cos(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const std::array<double,3> forward{cp * std::cos(yaw), cp * std::sin(yaw), s};
    const std::array<double,3> right{-std::sin(yaw)*cr+s*std::cos(yaw)*sr, std::cos(yaw)*cr+s*std::sin(yaw)*sr, -cp*sr};
    const std::array<double,3> up{-s*std::cos(yaw)*cr-std::sin(yaw)*sr, -s*std::sin(yaw)*cr+std::cos(yaw)*sr, cp*cr};
    double depth{}, x{}, y{};
    for (size_t i = 0; i < 3; ++i) {
        const auto delta = double(native.look_at[i]) - c.camera.location[i];
        depth += delta * forward[i]; x += delta * right[i]; y += delta * up[i];
    }
    const auto half_width = std::tan(c.base_fov * rad * 0.5), half_height = half_width / native.pose.aspect;
    const auto authored_width = depth * std::tan(native.pose.fov * rad * 0.5);
    const auto remaining = depth - dolly;
    const auto end_z = c.camera.location[2] + s * dolly + up[2] * lift;
    return end_z + 0.001 >= r.field_floor && remaining >= eye_margin + 2 &&
        std::abs(x) + authored_width + eye_margin <= (remaining - eye_margin) * half_width * 0.95 + 0.0001 &&
        std::abs(y - lift) + authored_width / native.pose.aspect + eye_margin <=
            (remaining - eye_margin) * half_height * 0.95 + 0.0001;
}

inline Decision evaluate(const Request& r, const trace::NativeSource& native, float eye_margin = 0) noexcept {
    Decision out{Reason::off, r.camera.dolly, r.camera.lift};
    if (!r.ebaseball || !r.camera.enabled) { return out; }
    if (r.manual_controls || r.protected_camera) { out.reason = Reason::protected_controls; return out; }
    if (!candidate(r)) { out.reason = Reason::unsupported_rig; return out; }
    if (!trace::valid_source(native)) { out.reason = Reason::source_unavailable; return out; }
    const auto c = geometry(r, r.camera.camera, r.camera.now_ns, out.dolly, out.lift);
    const auto selected = scope(r);
    const auto correction = focus_guard::bound_authored_window(c, native, true, eye_margin, selected);
    switch (correction.status) {
    case focus_guard::Status::source_mismatch: out.reason = Reason::source_mismatch; return out;
    case focus_guard::Status::off: out.reason = Reason::unsupported_rig; return out;
    case focus_guard::Status::no_safe_endpoint: out.reason = Reason::no_safe_endpoint; return out;
    default: break;
    }
    if (!correction.applied()) { out.reason = Reason::unchanged; return out; }
    out.dolly = correction.dolly; out.lift = correction.lift; out.applied = true;
    out.reason = selected == focus_guard::AuthoredScope::interview_repair ? Reason::interview_focus :
        r.legacy_floor > 350 && out.lift < r.camera.lift - 1 ? Reason::dugout_height : Reason::dugout_focus;
    return out;
}

// Game/publisher-thread owned. Only numerical offsets survive, never native
// pointers or permission to reuse a source. Every held endpoint is revalidated.
class Continuity {
public:
    void reset() noexcept { m_valid = false; }
    Decision update(const Request& r, const trace::NativeSource& native, const shot::Identity& identity,
        float eye_margin) noexcept {
        auto result = evaluate(r, native, eye_margin);
        const auto now = r.camera.now_ns;
        const auto change = shot::compare(m_identity, identity);
        const Context context{r.rendering_method, r.field_floor, r.camera.max_lift, r.camera.base_fov, r.smoothing};
        const bool continuous = m_valid && identity.valid && change == shot::Change::unchanged &&
            context == m_context &&
            now > m_time && now - m_time <= 250'000'000 &&
            !trace::inferred_cut(m_pose, r.camera.camera) &&
            trace::angle_delta(m_pose.rotation[2], r.camera.camera.rotation[2]) <= 25;
        if (!identity.valid || !identity.world.valid() || !identity.manager.valid() || !identity.view_target.valid() ||
            identity.manager.address != r.camera.camera_manager || (trace::valid_source(native) &&
                (identity.native_camera != native.object || identity.camera_actor != native.camera_actor))) {
            reset(); return {Reason::source_mismatch, r.camera.dolly, r.camera.lift};
        }
        if (!candidate(r) || !trace::source_matches_input(native, r.camera.camera,
                r.camera.camera_manager, now) || result.reason == Reason::source_mismatch ||
            result.reason == Reason::source_unavailable || result.reason == Reason::no_safe_endpoint ||
            result.reason == Reason::unsupported_rig) {
            reset(); return result;
        }
        // A remembered failure is not permission to retune a currently good
        // shot. Constant native camera pointers are not unique cut identifiers.
        if (continuous && result.applied && result.dolly > m_dolly + 0.001f) {
            const auto elapsed = now - m_last_tighten;
            float allowed = m_dolly;
            if (elapsed >= 250'000'000) {
                const auto release_start = (std::max)(m_time, m_last_tighten + 250'000'000);
                const auto dt = double(now - release_start) * 1e-9;
                if (dt > 0) {
                    const auto retention = float(std::pow(std::clamp(r.smoothing, 0.0f, 0.95f), dt * return_smoothing::reference_hz));
                    allowed = return_smoothing::apply(result.dolly, m_dolly, retention);
                }
            }
            auto held_request = geometry(r, r.camera.camera, now, allowed, 0);
            // Recompute signed clearance for the held dolly on the current pose.
            const auto f = trace::forward(held_request.camera);
            const auto pitch = held_request.camera.rotation[0] * std::numbers::pi_v<double> / 180;
            const auto roll = held_request.camera.rotation[2] * std::numbers::pi_v<double> / 180;
            const auto up_z = std::cos(pitch) * std::cos(roll);
            const auto z = held_request.camera.location[2] + f[2] * allowed;
            held_request.lift = up_z > 0 ? float((std::max)(0.0, (double(r.field_floor) + 2 - z) / up_z)) : r.camera.max_lift + 1;
            if (window_contains(r, native, allowed, held_request.lift, eye_margin)) {
                result.dolly = allowed; result.lift = held_request.lift;
                result.applied = true; result.reason = Reason::release_hold;
            }
        }
        if (!continuous || result.dolly < m_dolly - 0.001f) { m_last_tighten = now; }
        m_valid = result.applied;
        m_context = context;
        m_identity = identity; m_pose = r.camera.camera; m_time = now;
        m_dolly = result.dolly;
        return result;
    }
private:
    struct Context {
        int32_t rendering_method{};
        float floor{}, max_lift{}, base_fov{}, smoothing{};
        bool operator==(const Context&) const = default;
    };
    bool m_valid{};
    Context m_context{};
    shot::Identity m_identity{};
    trace::Pose m_pose{};
    uint64_t m_time{}, m_last_tighten{};
    float m_dolly{};
};
}
