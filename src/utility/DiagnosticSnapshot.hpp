#pragma once
#include <mutex>
#include <optional>
#include <type_traits>

namespace utility::diagnostics {
template<class T> struct SnapshotRead {
    std::optional<T> value;
    bool busy{};
};

// Optional telemetry must never hold up its producer or its consumer.
template<class T, class Mutex = std::mutex> class Snapshot {
    static_assert(std::is_trivially_copyable_v<T>);
public:
    bool try_publish(const T& value) {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock.owns_lock()) { return false; }
        m_value = value;
        return true;
    }
    SnapshotRead<T> try_read() const {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock.owns_lock()) { return {{}, true}; }
        return {m_value, false};
    }
    bool try_clear() {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock.owns_lock()) { return false; }
        m_value.reset();
        return true;
    }
private:
    mutable Mutex m_mutex;
    std::optional<T> m_value;
};
}
