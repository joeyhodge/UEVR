#pragma once

#include "ProSpiCameraTrace.hpp"
#include <atomic>
#include <mutex>
#include <optional>

namespace uevr::prospi::cut_guard {

struct Snapshot {
    trace::Pose camera{};
    float dolly{}, lift{};
    uint64_t time_ns{};
    bool automatic{};
};

enum class Status : uint8_t { unchanged, unavailable, stale, discontinuity };
struct Decision {
    Status status{};
    float dolly{}, lift{};
};

inline Decision evaluate(const Snapshot& s, const trace::Pose& input, uint64_t now) noexcept {
    if (!s.automatic) { return {Status::unchanged, s.dolly, s.lift}; }
    if (!trace::valid_pose(s.camera) || !trace::finite(input.location) || !trace::finite(input.rotation) ||
        !std::isfinite(s.dolly) || !std::isfinite(s.lift) || !s.time_ns) { return {Status::unavailable}; }
    if (now < s.time_ns || now - s.time_ns > 250'000'000) { return {Status::stale}; }
    // Permit normal authored pans/ball follow; reject only a cut-sized pose jump.
    if (trace::distance(s.camera.location, input.location) > 750.0f ||
        trace::angle_delta(s.camera.rotation[0], input.rotation[0]) > 20.0f ||
        trace::angle_delta(s.camera.rotation[1], input.rotation[1]) > 25.0f ||
        trace::angle_delta(s.camera.rotation[2], input.rotation[2]) > 25.0f) { return {Status::discontinuity}; }
    return {Status::unchanged, s.dolly, s.lift};
}

// Fixed-size publication; neither game nor view callbacks wait on a reader/writer.
class Publication {
public:
    uint64_t invalidate() noexcept { return m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool publish(Snapshot s, uint64_t epoch) noexcept {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock || epoch != m_epoch.load(std::memory_order_acquire)) { return false; }
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

} // namespace uevr::prospi::cut_guard
