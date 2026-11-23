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
    source_mismatch, no_safe_endpoint, interview_focus, dugout_focus, dugout_height, release_hold,
    home_pan_focus, home_pan_height, dugout_entry, upward_home, podium_closeup, low_infield_closeup,
    first_base_corner, first_base_dugout
};
inline const char* reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::unchanged: return "Existing framing is safe";
    case Reason::protected_controls: return "Explicit controls / protected camera";
    case Reason::unsupported_rig: return "Outside supported player-camera rigs";
    case Reason::source_unavailable: return "No verified current native source";
    case Reason::source_mismatch: return "Current native source does not match";
    case Reason::no_safe_endpoint: return "No validated pullback endpoint";
    case Reason::interview_focus: return "Interview authored window recovered";
    case Reason::dugout_focus: return "Dugout authored window recovered";
    case Reason::dugout_height: return "Dugout field-side height recovered";
    case Reason::release_hold: return "Safe pullback retained during recovery";
    case Reason::home_pan_focus: return "Home/plate authored window recovered";
    case Reason::home_pan_height: return "Home/plate excess lift recovered";
    case Reason::dugout_entry: return "Dugout entry framing recovered";
    case Reason::upward_home: return "Near-home close-up framing recovered";
    case Reason::podium_closeup: return "Interview close-up framing recovered";
    case Reason::low_infield_closeup: return "Low infield close-up framing recovered";
    case Reason::first_base_corner: return "First-base corner framing recovered";
    case Reason::first_base_dugout: return "First-base dugout pan framing recovered";
    default: return "Off / unsupported build";
    }
}

struct Request {
    focus_guard::Request camera{};
    float field_floor{}, legacy_floor{}, smoothing{0.25f};
    int32_t rendering_method{};
    bool ebaseball{}, manual_controls{}, protected_camera{}, home_pan{};
    focus_guard::AuthoredScope recorded_pan{};
};
struct Decision {
    Reason reason{};
    float dolly{}, lift{};
    bool applied{};
};

struct HomePanGates {
    bool ebaseball{}, presentation{}, pitch_label{}, protected_preset{}, celebration{};
};
inline bool admit_home_pan(const trace::Pose& camera, HomePanGates gates) noexcept {
    return gates.ebaseball && (gates.presentation || gates.pitch_label) &&
        !gates.protected_preset && !gates.celebration && focus_guard::home_pan_repair_rig(camera);
}

struct RecordedPanGates {
    bool dugout_entry{}, upward_home{}, podium_closeup{}, first_base_corner{}, first_base_dugout{};
};
inline focus_guard::AuthoredScope admit_recorded_pan(const trace::Pose& camera, RecordedPanGates gates) noexcept {
    using Scope = focus_guard::AuthoredScope;
    if (gates.dugout_entry && focus_guard::recorded_pan_rig(camera, Scope::dugout_entry_repair)) {
        return Scope::dugout_entry_repair;
    }
    if (gates.upward_home && focus_guard::recorded_pan_rig(camera, Scope::upward_home_repair)) {
        return Scope::upward_home_repair;
    }
    if (gates.upward_home && focus_guard::recorded_pan_rig(camera, Scope::low_infield_closeup_repair)) {
        return Scope::low_infield_closeup_repair;
    }
    if (gates.podium_closeup && focus_guard::recorded_pan_rig(camera, Scope::podium_closeup_repair)) {
        return Scope::podium_closeup_repair;
    }
    if (gates.first_base_corner && focus_guard::recorded_pan_rig(camera, Scope::first_base_corner_repair)) {
        return Scope::first_base_corner_repair;
    }
    if (gates.first_base_dugout && focus_guard::recorded_pan_rig(camera, Scope::first_base_dugout_repair)) {
        return Scope::first_base_dugout_repair;
    }
    return Scope::presentation;
}

inline focus_guard::AuthoredScope scope(const Request& r) noexcept {
    if (focus_guard::recorded_pan_scope(r.recorded_pan)) { return r.recorded_pan; }
    if (r.home_pan) { return focus_guard::AuthoredScope::home_pan_repair; }
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
        (r.camera.zone != 1 && r.camera.zone != 2 && r.camera.zone != 3 &&
            !(r.home_pan && r.camera.zone == 0)) ||
        !std::isfinite(r.field_floor) || r.field_floor < -50 || r.field_floor > 350 ||
        !std::isfinite(r.legacy_floor) || !std::isfinite(r.smoothing)) { return false; }
    using Scope = focus_guard::AuthoredScope;
    if (r.recorded_pan == Scope::dugout_entry_repair &&
        (r.camera.zone != 3 || r.legacy_floor <= r.field_floor + 350)) { return false; }
    if ((r.recorded_pan == Scope::upward_home_repair || r.recorded_pan == Scope::podium_closeup_repair) &&
        r.camera.zone != 1) { return false; }
    if ((r.recorded_pan == Scope::low_infield_closeup_repair || r.recorded_pan == Scope::first_base_corner_repair) &&
        (r.camera.zone != 1 || r.legacy_floor != r.field_floor)) { return false; }
    if (r.recorded_pan == Scope::first_base_dugout_repair) {
        const bool field = r.camera.zone == 1 && r.legacy_floor == r.field_floor;
        const bool false_crowd = r.camera.zone == 3 && r.legacy_floor > r.field_floor + 350;
        if (!field && !false_crowd) { return false; }
    }
    const auto c = geometry(r, r.camera.camera, r.camera.now_ns, r.camera.dolly, r.camera.lift);
    return focus_guard::crossing_candidate(c, true, scope(r));
}

inline bool matches_offsets(const Request& r, float forward, float right, float up) noexcept {
    return forward == r.camera.forward_offset && right == r.camera.right_offset && up == r.camera.up_offset;
}

inline bool window_contains(const focus_guard::Request& c, const focus_guard::AuthoredProjection& p, float dolly, float lift,
    float eye_margin) noexcept {
    return dolly >= 0 && dolly <= c.dolly && lift >= 0 && lift <= c.max_lift &&
        eye_margin >= 0 && eye_margin <= 100 && focus_guard::authored_window_contains(p, dolly, lift, c.floor, eye_margin);
}

inline bool window_contains(const Request& r, const trace::NativeSource& native, float dolly, float lift,
    float eye_margin) noexcept {
    if (!candidate(r)) { return false; }
    const auto c = geometry(r, r.camera.camera, r.camera.now_ns, r.camera.dolly, r.camera.lift);
    const auto projection = focus_guard::project_authored_target(c, native, scope(r));
    return projection && window_contains(c, *projection, dolly, lift, eye_margin);
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
    switch (selected) {
    case focus_guard::AuthoredScope::dugout_entry_repair: out.reason = Reason::dugout_entry; return out;
    case focus_guard::AuthoredScope::upward_home_repair: out.reason = Reason::upward_home; return out;
    case focus_guard::AuthoredScope::podium_closeup_repair: out.reason = Reason::podium_closeup; return out;
    case focus_guard::AuthoredScope::low_infield_closeup_repair: out.reason = Reason::low_infield_closeup; return out;
    case focus_guard::AuthoredScope::first_base_corner_repair: out.reason = Reason::first_base_corner; return out;
    case focus_guard::AuthoredScope::first_base_dugout_repair: out.reason = Reason::first_base_dugout; return out;
    default: break;
    }
    out.reason = selected == focus_guard::AuthoredScope::home_pan_repair ?
        (r.legacy_floor > 350 && out.lift < r.camera.lift - 1 ? Reason::home_pan_height : Reason::home_pan_focus) :
        selected == focus_guard::AuthoredScope::interview_repair ? Reason::interview_focus :
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
        const Context context{r.rendering_method, r.field_floor, r.camera.max_lift, r.camera.base_fov, r.smoothing, r.home_pan,
            r.recorded_pan, r.camera.forward_offset, r.camera.right_offset, r.camera.up_offset};
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
            const auto c = geometry(r, r.camera.camera, now, r.camera.dolly, r.camera.lift);
            const auto selected = scope(r);
            if (const auto p = focus_guard::project_authored_target(c, native, selected); p && p->up_z > 0) {
                const auto lift = float((std::max)(0.0, (double(c.floor) + 2 - p->dolly_z(allowed)) / p->up_z));
                if ((selected != focus_guard::AuthoredScope::low_infield_closeup_repair || lift <= c.lift) &&
                    window_contains(c, *p, allowed, lift, eye_margin)) {
                    result.dolly = allowed; result.lift = lift;
                    result.applied = true; result.reason = Reason::release_hold;
                }
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
        bool home_pan{};
        focus_guard::AuthoredScope recorded_pan{};
        float forward_offset{}, right_offset{}, up_offset{};
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
