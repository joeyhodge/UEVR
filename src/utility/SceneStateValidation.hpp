#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace utility::scene_state_validation {
// Stack-local to one read-only eye-pair check, never across a native call.
// Every state and table slot is read afresh; only the common type proof is shared.
template<size_t Capacity, class Read, class Probe> class Batch {
public:
    Batch(Read read, Probe probe) : m_read{std::move(read)}, m_probe{std::move(probe)} {}
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    bool check(uintptr_t state) {
        uintptr_t table{}, first{};
        if (!state || !m_read(state, table) || !table || !m_read(table, first) || !first) {
            m_count = 0;
            return false;
        }
        for (size_t i{}; i < m_count; ++i) {
            if (m_types[i].table != table) { continue; }
            if (m_types[i].first == first) { return true; }
            m_count = 0;
            break;
        }
        if (!m_probe(table, first)) { m_count = 0; return false; }
        if (m_count < Capacity) { m_types[m_count++] = {table, first}; }
        return true;
    }

private:
    struct Type { uintptr_t table{}, first{}; };
    Read m_read;
    Probe m_probe;
    std::array<Type, Capacity> m_types{};
    size_t m_count{};
};

template<size_t Capacity, class Read, class Probe> auto batch(Read read, Probe probe) {
    return Batch<Capacity, Read, Probe>{std::move(read), std::move(probe)};
}
}
