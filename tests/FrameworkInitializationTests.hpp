#include "utility/FrameworkInitialization.hpp"

void test_framework_initialization() {
    using namespace std::chrono_literals;
    using uevr::initialization::D3D12Retry;
    using uevr::initialization::Result;
    using uevr::initialization::warming_up;
    D3D12Retry retry;
    auto now = D3D12Retry::Clock::time_point{1s};
    uint32_t frames{};
    bool first = true;
    int calls{};
    const auto initialize = [&]() {
        ++calls;
        if (first) { frames = 0; first = false; }
        if (frames < uevr::initialization::warmup_frames) { ++frames; return false; }
        return true;
    };
    for (int i = 0; i < 60; ++i) {
        expect(retry.attempt(now, warming_up(first, frames), initialize) == Result::warming_up,
            "each startup warm-up frame advances without backoff");
        expect(retry.retry_delay() == 0ms, "warm-up is not counted as initialization failure");
        now += 1ms;
    }
    expect(calls == 60 && frames == 60 && !first, "all 60 shared warm-up frames are preserved");
    expect(retry.attempt(now, warming_up(first, frames), initialize) == Result::ready && calls == 61,
        "initialization is attempted on the very next frame after warm-up");

    const auto fail = [&]() { ++calls; return false; };
    for (auto delay : {50ms, 100ms, 200ms, 200ms, 200ms}) {
        expect(retry.attempt(now, false, fail) == Result::failed && retry.retry_delay() == delay,
            "genuine initialization failures retain the existing bounded backoff");
        const auto before = calls;
        expect(retry.attempt(now + delay - 1ms, true, initialize) == Result::throttled && calls == before,
            "throttled attempts do not execute initialization or consume warm-up frames");
        now += delay;
    }
    first = true;
    expect(retry.attempt(now, warming_up(first, frames), initialize) == Result::warming_up && frames == 1,
        "renewed warm-up after a renderer retry advances once the real failure deadline expires");
    expect(retry.attempt(now, true, fail) == Result::warming_up,
        "renewed warm-up adds no artificial inter-frame deadline");
    expect(retry.attempt(now, false, fail) == Result::failed && retry.retry_delay() == 200ms,
        "pending warm-up does not erase the real failure backoff tier");
    retry.reset();
    expect(retry.retry_delay() == 0ms && retry.attempt(now, false, fail) == Result::failed && retry.retry_delay() == 50ms,
        "device/reset path clears both the deadline and failure tier");
    now += 50ms;
    expect(retry.attempt(now, false, []() { return true; }) == Result::ready && retry.retry_delay() == 0ms,
        "successful initialization clears failure state");
    expect(retry.attempt(now, false, fail) == Result::failed && retry.retry_delay() == 50ms,
        "success leaves no stale failure deadline");
    expect(warming_up(true, 60) && !warming_up(false, 60) && warming_up(false, 59),
        "classification uses the pre-call shared warm-up state, not reset-induced real failures");
}
