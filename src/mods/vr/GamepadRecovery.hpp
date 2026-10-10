#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace uevr::input_recovery {
constexpr bool input_focus_allows_recovery(bool ready, bool restarting, bool focused) {
    return ready && !restarting && focused;
}

// The caller serializes this policy with VR's recovery mutex. These
// deadlines bound only recovery work; normal engine-tick input stays unchanged.
class GamepadRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto xinput_stale_after = std::chrono::seconds{2};
    static constexpr auto engine_stale_after = std::chrono::seconds{1};
    static constexpr auto fallback_interval = std::chrono::milliseconds{8};
    static constexpr auto focus_probe_interval = std::chrono::milliseconds{250};

    void reset() { *this = {}; }
    void observe_xinput_poll() {
        m_next_retry = {};
        m_retry_step = 0;
    }

    template<class Focus>
    bool request_retry(TimePoint now, bool stale_xinput, bool input_intent, Focus&& focused) {
        if (!stale_xinput || !input_intent || now < m_next_retry) { return false; }
        if (!focused()) {
            m_next_retry = now + focus_probe_interval;
            return false;
        }
        m_next_retry = now + retry_delays[m_retry_step];
        if (m_retry_step + 1 < retry_delays.size()) { ++m_retry_step; }
        return true;
    }

    template<class Focus>
    bool request_stalled_poll(TimePoint now, TimePoint last_engine_tick, Focus&& focused) {
        if (now - last_engine_tick <= engine_stale_after || now < m_next_stalled_poll) { return false; }
        const bool available = focused();
        m_next_stalled_poll = now + (available ? fallback_interval : focus_probe_interval);
        return available;
    }

private:
    static constexpr std::array retry_delays{
        std::chrono::seconds{2}, std::chrono::seconds{4}, std::chrono::seconds{8},
        std::chrono::seconds{16}, std::chrono::seconds{30}};
    TimePoint m_next_retry{};
    TimePoint m_next_stalled_poll{};
    size_t m_retry_step{};
};
}
