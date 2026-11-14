#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace utility::validation {
// Stop only when the result is decided. Every new invocation probes afresh.
template<size_t Count, size_t Required, class Predicate>
bool at_least(Predicate&& predicate) {
    static_assert(Required > 0 && Required <= Count);
    size_t accepted{};
    for (size_t i{}; i < Count; ++i) {
        if (predicate(i) && ++accepted == Required) { return true; }
        if (accepted + (Count - i - 1) < Required) { return false; }
    }
    return false;
}

struct ExecutableRegion {
    uintptr_t begin{};
    size_t bytes{};
    bool executable{};
    bool contains(uintptr_t address, size_t size) const {
        return address != 0 && size != 0 && bytes != 0 &&
            bytes <= (std::numeric_limits<uintptr_t>::max)() - begin &&
            address >= begin && address - begin < bytes && size <= bytes - (address - begin);
    }
};

// One read-only validation operation can cover several addresses in the same
// homogeneous VirtualQuery region. Never retain this batch across calls.
template<size_t Capacity, class Query> class ExecutableRegionBatch {
public:
    explicit ExecutableRegionBatch(Query query) : m_query{std::move(query)} {}
    bool check(uintptr_t address, size_t bytes) {
        if (!address || !bytes || bytes > (std::numeric_limits<uintptr_t>::max)() - address) { return false; }
        for (size_t i{}; i < m_count; ++i) {
            if (m_regions[i].contains(address, bytes)) { return m_regions[i].executable; }
        }
        const auto region = m_query(address);
        if (!region || !region->contains(address, bytes)) { return false; }
        if (m_count < Capacity) { m_regions[m_count++] = *region; }
        return region->executable;
    }
private:
    Query m_query;
    std::array<ExecutableRegion, Capacity> m_regions{};
    size_t m_count{};
};

template<size_t Capacity, class Query> auto executable_regions(Query query) {
    return ExecutableRegionBatch<Capacity, Query>{std::move(query)};
}
}
