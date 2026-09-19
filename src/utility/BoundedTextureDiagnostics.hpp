#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace utility::diagnostics {

struct TextureObservation {
    uintptr_t key{};
    bool first_seen{};
    uint64_t seen{};
    size_t tracked_keys{};
    uint64_t duplicate_suppressed{};
    uint64_t overflow_suppressed{};
};

// Keep the first keys for the injection session. Overflow counts observations,
// not unique resources: tracking exact uniqueness would require unbounded memory.
template <size_t Capacity = 64>
class BoundedTextureObservations {
public:
    static_assert(Capacity > 0);

    template <typename KeyProvider>
    std::optional<TextureObservation> observe(bool enabled, KeyProvider&& key_provider) {
        if (!enabled) {
            return std::nullopt;
        }

        // Resource queries must neither run with diagnostics off nor under the lock.
        const uintptr_t key = std::forward<KeyProvider>(key_provider)();
        std::scoped_lock lock{m_mutex};
        ++m_seen;
        for (size_t i = 0; i < m_size; ++i) {
            if (m_keys[i] == key) {
                ++m_duplicate_suppressed;
                return snapshot(key, false);
            }
        }

        if (m_size == Capacity) {
            ++m_overflow_suppressed;
            return snapshot(key, false);
        }

        m_keys[m_size++] = key;
        return snapshot(key, true);
    }

private:
    TextureObservation snapshot(uintptr_t key, bool first_seen) const {
        return {key, first_seen, m_seen, m_size, m_duplicate_suppressed, m_overflow_suppressed};
    }

    std::mutex m_mutex{};
    std::array<uintptr_t, Capacity> m_keys{};
    size_t m_size{};
    uint64_t m_seen{};
    uint64_t m_duplicate_suppressed{};
    uint64_t m_overflow_suppressed{};
};

// Bound optional memory probes across all viewport identities, including churn.
template <size_t Limit = 64>
class TextureProbeBudget {
public:
    using Clock = std::chrono::steady_clock;

    bool try_acquire(bool enabled, Clock::time_point now) {
        if (!enabled) {
            return false;
        }

        std::scoped_lock lock{m_mutex};
        if (m_count == Limit || (m_count != 0 && now - m_last < std::chrono::seconds(2))) {
            return false;
        }

        m_last = now;
        ++m_count;
        return true;
    }

private:
    std::mutex m_mutex{};
    Clock::time_point m_last{};
    size_t m_count{};
};

} // namespace utility::diagnostics
