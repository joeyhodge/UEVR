#pragma once

#include "mods/vr/NativeOpenXRAsyncWorker.hpp"
#include "mods/vr/NativeOpenXRArrayPolicy.hpp"
#include "utility/GpuRetirement.hpp"
#include <chrono>
#include <array>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace native_async_tests {
using namespace std::chrono_literals;
namespace p = uevr::native_array;
inline void require(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
template<class Predicate> void until(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    require(done(), "worker did not complete within the bounded test deadline");
}

inline void worker_tests() {
    p::AsyncWorker<std::shared_ptr<int>> worker;
    require(!worker.busy() && !worker.failed(), "a fresh worker is not idle");
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future().share();
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> witness = owner;
    std::atomic_uint pre_acquired{};
    require(worker.submit(owner, [&](const auto& pinned, const auto& cancellation) {
        require(*pinned == 1, "request lost its runtime owner");
        entered.set_value();
        require(release_future.wait_for(2s) == std::future_status::ready, "fake runtime wait timed out");
        if (!cancellation.cancelled()) { ++pre_acquired; }
    }), "first worker request was rejected");
    require(entered_future.wait_for(1s) == std::future_status::ready, "worker did not enter its runtime wait");
    owner.reset();
    require(worker.busy() && !witness.expired(), "in-flight request did not pin its owner");
    require(!worker.submit(nullptr, [](const auto&, const auto&) {}), "worker accepted two simultaneous waits");
    worker.cancel_pending();
    require(worker.busy() && !witness.expired(), "cancellation falsely retired a blocked runtime call");
    auto stopped = std::async(std::launch::async, [&] { worker.stop(); });
    require(stopped.wait_for(20ms) == std::future_status::timeout, "stop returned before the in-flight callback drained");
    release.set_value();
    require(stopped.wait_for(1s) == std::future_status::ready, "stop failed to join after the runtime returned");
    stopped.get();
    require(!worker.busy() && witness.expired() && pre_acquired == 0, "a cancelled wait pre-acquired or retained a stale runtime");

    std::atomic_uint completed{};
    require(worker.submit(std::make_shared<int>(2), [&](const auto& request, const auto&) {
        require(*request == 2, "worker reused an old request after restart"); ++completed;
    }), "worker could not restart after session drain");
    until([&] { return !worker.busy(); });
    require(completed == 1 && !worker.failed(), "restart did not complete exactly once");
    worker.stop();

    require(worker.submit(nullptr, [](const auto&, const auto&) { throw std::runtime_error("fake XR exception"); }),
        "exception test request was rejected");
    until([&] { return !worker.busy(); });
    require(worker.failed() && !worker.submit(nullptr, [](const auto&, const auto&) {}), "worker exception did not fail closed");
    worker.stop();

    for (unsigned i = 0; i < 100; ++i) {
        p::AsyncWorker<std::shared_ptr<int>> transient;
        auto request = std::make_shared<int>(3);
        std::weak_ptr<int> pending = request;
        require(transient.submit(request, [](const auto&, const auto&) {}), "rapid request rejected");
        request.reset();
        transient.cancel_pending();
        transient.stop();
        require(!transient.busy() && !transient.failed() && pending.expired(), "rapid cancel/stop leaked a pending request");
    }
    // Destruction, not a manual stop(), must also join a running callback.
    std::future<void> destructor_entered;
    std::atomic_bool finished{};
    {
        p::AsyncWorker<int> destructed;
        std::promise<void> started;
        destructor_entered = started.get_future();
        require(destructed.submit(0, [started = std::move(started), &finished](int, const auto& cancellation) mutable {
            started.set_value();
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (!cancellation.cancelled() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
            finished = true;
        }), "destruction request rejected");
        require(destructor_entered.wait_for(1s) == std::future_status::ready, "destruction callback did not start");
    }
    require(finished, "worker destructor returned while its callback could access destroyed state");

    p::AsyncWorker<int> contested;
    std::atomic_uint active{}, calls{};
    const auto callback = [&](int, const auto&) {
        require(active.fetch_add(1) == 0, "two callbacks entered the runtime simultaneously");
        ++calls;
        std::this_thread::yield();
        --active;
    };
    std::array<std::jthread, 4> publishers;
    for (auto& publisher : publishers) {
        publisher = std::jthread([&] {
            for (int i = 0; i < 200; ++i) {
                contested.submit(i, callback);
                if (i % 3 == 0) { contested.cancel_pending(); }
                std::this_thread::yield();
            }
        });
    }
    std::jthread drainer{[&] { for (int i = 0; i < 20; ++i) { contested.stop(); std::this_thread::yield(); } }};
    for (auto& publisher : publishers) { publisher.join(); }
    drainer.join();
    until([&] { return !contested.busy(); });
    require(!contested.failed() && active == 0, "concurrent request/cancel/drain did not serialize runtime ownership");
    contested.stop();

    p::AsyncWorker<int> closed;
    closed.shutdown();
    require(closed.closed() && !closed.submit(0, [](int, const auto&) {}), "shutdown allowed a late present to recreate the worker");
    closed.stop();
    require(closed.closed() && !closed.submit(0, [](int, const auto&) {}), "a drain reopened a permanently shut down worker");
}

inline void frame_tests() {
    const p::FrameKey frame{1, 2, 3, 40};
    for (unsigned bits = 0; bits < 8; ++bits) {
        require(p::can_async_wait(frame, frame, bits & 1, bits & 2, bits & 4) == (bits == 1), "async frame eligibility changed");
    }
    for (unsigned bits = 0; bits < 16; ++bits) {
        require(p::can_pre_acquire(frame, frame, bits & 1, bits & 2, bits & 4, bits & 8) == (bits == 11),
            "pre-acquire accepted a begun/unsynced/non-rendering frame");
    }
    for (const auto stale : {p::FrameKey{}, p::FrameKey{2,2,3,40}, p::FrameKey{1,3,3,40},
        p::FrameKey{1,2,4,40}, p::FrameKey{1,2,3,41}, p::FrameKey{1,2,3,0}}) {
        require(!p::can_async_wait(stale, frame, true, false, false), "stale request could issue another frame wait");
        require(!p::can_pre_acquire(stale, frame, true, true, false, true), "stale session/frame could pre-acquire");
    }
}

struct Commands {
    bool recorded{}, submitted{true}, poisoned{}, fence{true}, reusable{true};
    uint64_t value{10}, completed{9};
    unsigned resets{};
    bool references_retired() const {
        return utility::gpu::references_retired(recorded, submitted, poisoned, fence, value, [&] { return completed; });
    }
    bool try_wait() { require(references_retired(), "attempted allocator reset before retirement"); ++resets; return reusable; }
    bool ready() const { return reusable && !poisoned; }
};

inline void retirement_tests() {
    Commands commands;
    auto source = std::make_shared<int>(1);
    std::weak_ptr<int> witness = source;
    auto replacement = std::make_shared<int>(2);
    for (unsigned i = 0; i < 1000; ++i) {
        if (p::reclaim_array_commands(commands)) { source = replacement; }
        require(!witness.expired() && commands.resets == 0, "pending array source/allocator was recycled");
    }
    commands.completed = commands.value;
    require(p::reclaim_array_commands(commands) && commands.resets == 1, "completed array slot could not be reclaimed");
    source = replacement;
    require(witness.expired(), "retired source was retained unnecessarily");
    for (unsigned bad = 0; bad < 5; ++bad) {
        Commands unsafe; unsafe.completed = 10;
        switch (bad) {
        case 0: unsafe.poisoned = true; break;
        case 1: unsafe.fence = false; break;
        case 2: unsafe.recorded = true; break;
        case 3: unsafe.value = 0; break;
        case 4: unsafe.completed = (std::numeric_limits<uint64_t>::max)(); break;
        }
        require(!p::reclaim_array_commands(unsafe) && unsafe.resets == 0, "unknown GPU retirement authorized allocator reuse");
    }

    p::ImageWait image;
    unsigned waits{}, releases{};
    bool xr_ready = false, gpu_retired = false, release_ok = false;
    const auto poll = [&] {
        return p::poll_array_release(image, [&] { ++waits; return image.waited(xr_ready); },
            [&] { return gpu_retired; }, [&] { ++releases; return release_ok; });
    };
    require(!poll() && waits == 0 && releases == 0, "cleanup called XR on an unacquired image");
    p::ImageWait timed_out;
    timed_out.acquired();
    require(!p::poll_array_release(timed_out, [] { return true; }, [] { return true; }, [&] { ++releases; return true; }) &&
        timed_out.pending() && releases == 0, "a positive timeout-like result bypassed actual image-wait ownership");
    image.acquired();
    for (unsigned i = 0; i < 1000; ++i) { require(!poll() && image.pending(), "XR timeout lost its acquisition"); }
    require(releases == 0, "cleanup released an image before a successful XR wait");
    xr_ready = true;
    require(!poll() && image.writable() && releases == 0, "cleanup released an image with pending GPU references");
    const auto waited_once = waits;
    for (unsigned i = 0; i < 1000; ++i) { require(!poll(), "cleanup ignored the GPU backlog"); }
    require(waits == waited_once, "cleanup repeated an already successful XR wait");
    gpu_retired = true;
    require(!poll() && image.writable() && releases == 1, "release failure lost its owned image");
    release_ok = true;
    require(poll() && !image.pending() && !image.writable() && releases == 2, "retired image did not release exactly once");
    require(!poll() && releases == 2, "cleanup released the same image twice");
}

inline void run() { frame_tests(); worker_tests(); retirement_tests(); }
}
