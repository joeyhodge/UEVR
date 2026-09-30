#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace utility {
// Legacy clients have no release callback. Keep handle addresses unique for the
// session, but never retain a detached state's payload through its public handle.
template<class T, class Owner>
class OpaqueStateRegistry {
public:
    struct Entry { std::weak_ptr<T> state; Owner owner{}; };
    void* publish(const std::shared_ptr<T>& state, Owner owner) {
        if (!state) { return nullptr; }
        std::scoped_lock lock{m_mutex};
        if (const auto it = m_by_state.find(state.get()); it != m_by_state.end()) {
            if (it->second->state.lock() == state && it->second->owner == owner) { return it->second; }
        }
        auto entry = std::make_unique<Entry>(Entry{state, owner});
        auto result = entry.get();
        m_entries.emplace(result, std::move(entry));
        m_by_state[state.get()] = result;
        return result;
    }
    std::pair<std::shared_ptr<T>, Owner> resolve(void* handle) {
        std::scoped_lock lock{m_mutex};
        const auto it = m_entries.find(handle);
        if (it == m_entries.end()) { return {}; }
        return {it->second->state.lock(), it->second->owner};
    }
private:
    std::mutex m_mutex;
    std::unordered_map<void*, std::unique_ptr<Entry>> m_entries;
    std::unordered_map<T*, Entry*> m_by_state;
};
}
