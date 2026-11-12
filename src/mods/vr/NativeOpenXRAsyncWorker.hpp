#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace uevr::native_array {
// One owned request at a time. An in-progress runtime wait is not cancellable:
// stop() joins it before the callback's owner or these synchronization objects die.
template<class Request> class AsyncWorker {
public:
    struct Cancellation {
        const std::atomic<uint64_t>& generation;
        uint64_t expected;
        std::stop_token stop;
        bool cancelled() const noexcept {
            return stop.stop_requested() || generation.load(std::memory_order_acquire) != expected;
        }
    };

    ~AsyncWorker() { shutdown(); }
    AsyncWorker() = default;
    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    template<class Callback> bool submit(Request request, Callback callback) {
        std::scoped_lock lifecycle{m_lifecycle};
        std::scoped_lock state{m_mutex};
        if (m_closed.load(std::memory_order_acquire) || m_busy.load(std::memory_order_acquire) ||
            m_failed.load(std::memory_order_acquire)) { return false; }
        if (!m_thread.joinable()) {
            m_thread = std::jthread([this, callback = std::move(callback)](std::stop_token stop) mutable {
                for (;;) {
                    std::optional<Request> request;
                    uint64_t generation{};
                    {
                        std::unique_lock lock{m_mutex};
                        m_cv.wait(lock, [&] { return stop.stop_requested() || m_pending.has_value(); });
                        if (stop.stop_requested()) { break; }
                        request = std::move(m_pending);
                        m_pending.reset();
                        generation = m_generation.load(std::memory_order_acquire);
                        m_running = true;
                    }
                    try {
                        const Cancellation cancellation{m_generation, generation, stop};
                        if (!cancellation.cancelled()) { callback(*request, cancellation); }
                    } catch (...) {
                        m_failed.store(true, std::memory_order_release);
                    }
                    // Release the request's runtime references before publishing idle.
                    request.reset();
                    {
                        std::scoped_lock lock{m_mutex};
                        m_running = false;
                        m_busy.store(false, std::memory_order_release);
                    }
                }
            });
        }
        m_pending.emplace(std::move(request));
        m_busy.store(true, std::memory_order_release);
        m_cv.notify_one();
        return true;
    }

    void cancel_pending() {
        if (!m_busy.load(std::memory_order_acquire)) { return; }
        std::scoped_lock lock{m_mutex};
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        m_pending.reset();
        if (!m_running) { m_busy.store(false, std::memory_order_release); }
    }

    // stop() drains for reinitialization; shutdown() also rejects future requests.
    // Caller must not hold a runtime/copy lock needed by its callback.
    void stop() { drain(false); }
    void shutdown() { drain(true); }
    bool busy() const noexcept { return m_busy.load(std::memory_order_acquire); }
    bool failed() const noexcept { return m_failed.load(std::memory_order_acquire); }
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

private:
    void drain(bool close) {
        std::scoped_lock lifecycle{m_lifecycle};
        {
            std::scoped_lock lock{m_mutex};
            if (close) { m_closed.store(true, std::memory_order_release); }
            m_generation.fetch_add(1, std::memory_order_acq_rel);
            m_pending.reset();
            if (m_thread.joinable()) { m_thread.request_stop(); }
            m_cv.notify_all();
        }
        if (m_thread.joinable()) { m_thread.join(); }
        m_busy.store(false, std::memory_order_release);
        m_failed.store(false, std::memory_order_release);
    }

    std::mutex m_lifecycle, m_mutex;
    std::condition_variable m_cv;
    std::atomic<uint64_t> m_generation{1};
    std::atomic_bool m_busy{false}, m_failed{false}, m_closed{false};
    std::optional<Request> m_pending;
    bool m_running{};
    std::jthread m_thread; // Destroy/join before every object the thread accesses.
};
}
