#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uevr::prospi::return_smoothing {

inline constexpr double reference_hz = 30.0;
inline constexpr double max_gap_seconds = 0.25;

struct Context {
    uintptr_t world{}, camera_manager{};
    int32_t rendering_method{}, sequencer_mode{}, play_mode{}, zone{};
    bool exact_override{}, cinematic_assist{};
    bool operator==(const Context&) const = default;
};

class Timeline {
public:
    void reset() noexcept { m_valid = false; }

    float update(const Context& context, double now, bool cut, float smoothing) noexcept {
        if (!context.world || !context.camera_manager || !std::isfinite(now) || now <= 0 ||
            !std::isfinite(smoothing)) {
            reset();
            return 0;
        }
        const auto elapsed = now - m_time;
        const auto continuous = m_valid && context == m_context && !cut &&
            elapsed > 0 && elapsed <= max_gap_seconds;
        m_context = context;
        m_time = now;
        m_valid = true;
        if (!continuous) { return 0; }

        // Preserve the old 30 Hz response; other update rates use the same wall-clock decay.
        return static_cast<float>(std::pow(std::clamp(smoothing, 0.0f, 0.95f), elapsed * reference_hz));
    }

private:
    Context m_context{};
    double m_time{};
    bool m_valid{};
};

inline float apply(float target, float previous, float retention) noexcept {
    if (!std::isfinite(target) || !std::isfinite(previous) || !std::isfinite(retention) ||
        retention <= 0 || retention > 1 || target <= previous) {
        return target;
    }
    return target + (previous - target) * retention;
}

} // namespace uevr::prospi::return_smoothing
