#pragma once

#include "ProSpiCameraTrace.hpp"
#include "ProSpiNativeFocusGuard.hpp"
#include "ProSpiNativeCameraSource.hpp"
#include "ProSpiShotIdentity.hpp"
#include "ProSpiAuthoredTargetGuard.hpp"
#include "EBaseballFramingRepair.hpp"
#include <atomic>
#include <mutex>
#include <optional>

namespace uevr::prospi::cut_guard {

struct Snapshot {
    trace::Pose camera{};
    float dolly{}, lift{};
    uint64_t time_ns{};
    bool automatic{};
    focus_guard::ViewCorrection view_focus{};
    shot::Identity identity{};
    trace::native::Layout native_layout{};
    focus_guard::Request presentation_request{};
    uint64_t epoch{};
    uint32_t publisher_thread{};
    bool presentation{};
    focus_guard::AuthoredScope scope{};
    repair::Request framing_repair{};
};

enum class Status : uint8_t { unchanged, unavailable, stale, discontinuity, authored_focus, identity_changed, authored_window, revalidated };
inline const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::unavailable: return "No current packet";
    case Status::stale: return "Expired camera packet";
    case Status::discontinuity: return "Camera-cut fallback";
    case Status::authored_focus: return "Existing authored-focus correction";
    case Status::identity_changed: return "Retired shot identity";
    case Status::authored_window: return "Authored framing recovered";
    case Status::revalidated: return "New shot revalidated; offsets unchanged";
    default: return "Existing camera assist";
    }
}
struct Decision {
    Status status{};
    float dolly{}, lift{};
    trace::NativeSource current_source{};
    repair::Reason repair_reason{};
};

inline Decision evaluate(const Snapshot& s, const trace::Pose& input, uint64_t now, const shot::Identity* current = nullptr) noexcept {
    if (!s.automatic) { return {Status::unchanged, s.dolly, s.lift}; }
    if (!trace::valid_pose(s.camera) || !trace::finite(input.location) || !trace::finite(input.rotation) ||
        !std::isfinite(s.dolly) || !std::isfinite(s.lift) || !s.time_ns) { return {Status::unavailable}; }
    if (now < s.time_ns || now - s.time_ns > 250'000'000) { return {Status::stale}; }
    if (current && shot::changed(shot::compare(s.identity, *current))) { return {Status::identity_changed}; }
    // Permit normal authored pans/ball follow; reject only a cut-sized pose jump.
    if (trace::distance(s.camera.location, input.location) > 750.0f ||
        trace::angle_delta(s.camera.rotation[0], input.rotation[0]) > 20.0f ||
        trace::angle_delta(s.camera.rotation[1], input.rotation[1]) > 25.0f ||
        trace::angle_delta(s.camera.rotation[2], input.rotation[2]) > 25.0f) { return {Status::discontinuity}; }
    if (focus_guard::matches_view(s.view_focus, input) && s.view_focus.dolly <= s.dolly) {
        return {Status::authored_focus, s.view_focus.dolly, s.view_focus.lift};
    }
    return {Status::unchanged, s.dolly, s.lift};
}

inline Decision evaluate_current(const Snapshot& s, const trace::Pose& input, uint64_t now,
    const shot::Identity& current, const trace::NativeSource& native, float eye_margin = 0.0f) noexcept {
    const auto baseline = evaluate(s, input, now);
    auto decision = evaluate(s, input, now, &current);
    if (!s.automatic || !s.identity.valid || !current.valid ||
        (decision.status != Status::unchanged && decision.status != Status::identity_changed)) { return decision; }
    const auto change = shot::compare(s.identity, current);
    if (decision.status == Status::identity_changed && change == shot::Change::owner) { return decision; }
    if (!trace::source_matches_input(native, input, s.identity.manager.address, now)) { return baseline; }
    const auto with_source = [&](Decision result) { result.current_source = native; return result; };
    auto request = s.presentation_request;
    request.camera = input; request.camera.fov = native.pose.fov;
    request.camera.aspect = native.pose.aspect; request.camera.aspect_valid = native.pose.aspect_valid;
    request.now_ns = now; request.dolly = baseline.dolly; request.lift = baseline.lift;
    if (s.scope == focus_guard::AuthoredScope::near_home_crossing) {
        const auto correction = focus_guard::bound_authored_window(request, native, s.presentation, eye_margin, s.scope);
        return with_source(correction.applied() ? Decision{Status::authored_window, correction.dolly, correction.lift} : baseline);
    }
    // A changed camera does not by itself prove the old numerical offsets are bad.
    // Reauthorize an already-visible endpoint against the new native source instead of introducing a reset jump.
    if (decision.status == Status::identity_changed) {
        const auto visible = focus_guard::authored_endpoint_visible(request, native);
        if (!visible) { return baseline; }
        if (*visible) { return with_source({Status::revalidated, baseline.dolly, baseline.lift}); }
    }
    if (s.presentation) {
        const auto correction = focus_guard::bound_authored_window(request, native, true, eye_margin);
        if (correction.applied()) { return with_source({Status::authored_window, correction.dolly, correction.lift}); }
    }
    return with_source(decision);
}

inline Decision repair_current(const Snapshot& s, const trace::Pose& input, uint64_t now,
    const shot::Identity& current, const trace::NativeSource& native, float eye_margin,
    Decision baseline, repair::Continuity& continuity) noexcept {
    if (!repair::candidate(s.framing_repair)) { continuity.reset(); return baseline; }
    const auto change = shot::compare(s.identity, current);
    if (!s.identity.valid || !current.valid || !s.identity.view_target.valid() || !current.view_target.valid() ||
        s.framing_repair.camera.camera_manager != s.identity.manager.address || change == shot::Change::unavailable ||
        change == shot::Change::owner || change == shot::Change::target ||
        !s.time_ns || now < s.time_ns || now - s.time_ns > 250'000'000 ||
        !trace::valid_pose(s.camera) || !input.valid || !trace::finite(input.location) || !trace::finite(input.rotation) ||
        trace::distance(s.camera.location, input.location) > 750 ||
        trace::angle_delta(s.camera.rotation[0], input.rotation[0]) > 20 ||
        trace::angle_delta(s.camera.rotation[1], input.rotation[1]) > 25 ||
        trace::angle_delta(s.camera.rotation[2], input.rotation[2]) > 25) {
        continuity.reset(); return baseline;
    }
    auto request = s.framing_repair;
    request.camera.camera = input;
    // CalculateStereoViewOffset has no lens argument. Only the independently
    // validated, current native source may supply it, not the pre-tick cache.
    request.camera.camera.fov = native.pose.fov;
    request.camera.camera.aspect = native.pose.aspect; request.camera.camera.aspect_valid = native.pose.aspect_valid;
    request.camera.now_ns = now;
    request.camera.dolly = baseline.dolly; request.camera.lift = baseline.lift;
    const auto correction = continuity.update(request, native, current, eye_margin);
    baseline.repair_reason = correction.reason;
    if (correction.reason == repair::Reason::unchanged || correction.reason == repair::Reason::no_safe_endpoint) {
        baseline.current_source = native;
    }
    if (correction.applied) {
        baseline.dolly = correction.dolly; baseline.lift = correction.lift;
        baseline.status = Status::authored_window; baseline.current_source = native;
    }
    return baseline;
}

// Fixed-size publication; neither game nor view callbacks wait on a reader/writer.
class Publication {
public:
    uint64_t invalidate() noexcept { return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool publish(Snapshot s, uint64_t epoch) noexcept {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock || epoch != m_epoch.load(std::memory_order_acquire)) { return false; }
        s.epoch = epoch;
        m_snapshot = s; m_published_epoch = epoch; m_have = true;
        return true;
    }
    std::optional<Snapshot> read() const noexcept {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock || !m_have || m_published_epoch != m_epoch.load(std::memory_order_acquire)) { return {}; }
        return m_snapshot;
    }
private:
    mutable std::mutex m_mutex;
    std::atomic<uint64_t> m_epoch{};
    uint64_t m_published_epoch{};
    bool m_have{};
    Snapshot m_snapshot{};
};

// Publisher-thread owned. Reuse one complete decision for matching eye calls;
// never pair a fresh source for one eye with a previous-cut decision for the other.
class ViewCache {
public:
    std::optional<Decision> read(uint64_t epoch, uint64_t frame, const trace::Pose& input, uint64_t now,
        bool strict = false, float eye_margin = 0) const noexcept {
        if (!m_valid || strict != m_strict || epoch != m_epoch || frame != m_frame || now < m_time || now - m_time > 250'000'000 ||
            input.location != m_input.location || input.rotation != m_input.rotation || !input.valid ||
            !trace::finite(input.location) || !trace::finite(input.rotation)) { return {}; }
        if (strict && (!m_strict || !std::isfinite(input.fov) || input.fov != m_input.fov ||
            input.aspect_valid != m_input.aspect_valid || (input.aspect_valid && input.aspect != m_input.aspect) ||
            !std::isfinite(eye_margin) || eye_margin != m_eye_margin || now - m_time > 50'000'000 ||
            (trace::valid_source(m_decision.current_source) &&
                (now < m_decision.current_source.time_ns || now - m_decision.current_source.time_ns > 50'000'000)))) { return {}; }
        return m_decision;
    }
    void publish(uint64_t epoch, uint64_t frame, const trace::Pose& input, uint64_t now, Decision decision,
        bool strict = false, float eye_margin = 0) noexcept {
        m_epoch = epoch; m_frame = frame; m_input = input; m_time = now; m_decision = decision;
        m_strict = strict; m_eye_margin = eye_margin;
        m_valid = input.valid && trace::finite(input.location) && trace::finite(input.rotation);
    }
private:
    uint64_t m_epoch{}, m_frame{}, m_time{};
    trace::Pose m_input{};
    Decision m_decision{};
    float m_eye_margin{};
    bool m_valid{}, m_strict{};
};

} // namespace uevr::prospi::cut_guard
