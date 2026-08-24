#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace utility::logging::detail {

inline int64_t steady_clock_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline bool should_log_every_n_seconds(
    std::atomic<int64_t>& last_log_nanoseconds,
    std::chrono::seconds interval) noexcept {
    const auto now = steady_clock_nanoseconds();
    const auto interval_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count();
    auto last = last_log_nanoseconds.load(std::memory_order_relaxed);

    while (now - last >= interval_nanoseconds) {
        if (last_log_nanoseconds.compare_exchange_weak(
                last,
                now,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }

    return false;
}

} // namespace utility::logging::detail

#define SPDLOG_INFO_ONCE(...) do { \
    static std::atomic_flag once = ATOMIC_FLAG_INIT; \
    if (!once.test_and_set(std::memory_order_relaxed)) { SPDLOG_INFO(__VA_ARGS__); } \
} while (false)

#define SPDLOG_WARN_ONCE(...) do { \
    static std::atomic_flag once = ATOMIC_FLAG_INIT; \
    if (!once.test_and_set(std::memory_order_relaxed)) { SPDLOG_WARN(__VA_ARGS__); } \
} while (false)

#define SPDLOG_ERROR_ONCE(...) do { \
    static std::atomic_flag once = ATOMIC_FLAG_INIT; \
    if (!once.test_and_set(std::memory_order_relaxed)) { SPDLOG_ERROR(__VA_ARGS__); } \
} while (false)

#define SPDLOG_INFO_EVERY_N_SEC(n, ...) do { \
    static std::atomic<int64_t> last_log_nanoseconds{0}; \
    if (::utility::logging::detail::should_log_every_n_seconds(last_log_nanoseconds, std::chrono::seconds(n))) { \
        SPDLOG_INFO(__VA_ARGS__); \
    } \
} while (false)

#define SPDLOG_WARNING_EVERY_N_SEC(n, ...) do { \
    static std::atomic<int64_t> last_log_nanoseconds{0}; \
    if (::utility::logging::detail::should_log_every_n_seconds(last_log_nanoseconds, std::chrono::seconds(n))) { \
        SPDLOG_WARN(__VA_ARGS__); \
    } \
} while (false)

#define SPDLOG_ERROR_EVERY_N_SEC(n, ...) do { \
    static std::atomic<int64_t> last_log_nanoseconds{0}; \
    if (::utility::logging::detail::should_log_every_n_seconds(last_log_nanoseconds, std::chrono::seconds(n))) { \
        SPDLOG_ERROR(__VA_ARGS__); \
    } \
} while (false)
