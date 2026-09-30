#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace utility::uobject {
struct ScanSlice {
    int32_t next{};
    uint32_t visited{};
    bool yielded{};
};

// A soft budget: finish the current slot, then resume at the first unvisited
// slot next tick. Empty/tracked slots count too, so gathering cannot run unbounded.
template<class Now, class Visit>
ScanSlice scan_with_budget(int32_t start, int32_t count, uint32_t max_slots,
    std::chrono::steady_clock::duration budget, Now now, Visit visit) {
    count = (std::max)(0, count);
    if (start < 0 || start > count) { start = 0; }
    ScanSlice result{start};
    const auto limit = (std::min)(max_slots, static_cast<uint32_t>(count - start));
    if (limit == 0) { return result; }
    const auto began = now();
    while (result.visited < limit) {
        // Guarantee progress even if one object takes longer than the budget.
        if (result.visited != 0 && now() - began >= budget) {
            result.yielded = true;
            break;
        }
        visit(result.next);
        ++result.next;
        ++result.visited;
    }
    return result;
}
}
