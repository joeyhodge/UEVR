#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

namespace uevr::initialization {
inline constexpr uint32_t warmup_frames = 60;

inline bool warming_up(bool first_initialize, uint32_t frames) {
    return first_initialize || frames < warmup_frames;
}

enum class Result { throttled, warming_up, failed, ready };

class D3D12Retry {
public:
    using Clock = std::chrono::steady_clock;

    template<class Initialize>
    Result attempt(Clock::time_point now, bool warmup_pending, Initialize&& initialize) {
        if (m_next_attempt != Clock::time_point{} && now < m_next_attempt) {
            return Result::throttled;
        }
        if (std::forward<Initialize>(initialize)()) {
            reset();
            return Result::ready;
        }
        // initialize() deliberately returns false during the shared 60-frame
        // warm-up. These frames must not accumulate failure backoff or warnings.
        if (warmup_pending) {
            m_next_attempt = {};
            return Result::warming_up;
        }
        if (m_failure_tier < 3) { ++m_failure_tier; }
        m_next_attempt = now + retry_delay();
        return Result::failed;
    }

    std::chrono::milliseconds retry_delay() const {
        // Preserve the existing genuine-failure schedule: 50, 100, then 200 ms.
        return std::chrono::milliseconds{m_failure_tier == 0 ? 0 : 50 << (m_failure_tier - 1)};
    }

    void reset() {
        m_next_attempt = {};
        m_failure_tier = 0;
    }

private:
    Clock::time_point m_next_attempt{};
    uint32_t m_failure_tier{};
};
}
