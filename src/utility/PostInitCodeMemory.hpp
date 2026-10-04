#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace uevr::post_init {
// Bounds come from a validated loaded module, not from the candidate's operands.
// A window may cross readable pages but never guards, image bounds or code/data
// protection boundaries. Partial windows let the decoder reject truncation.
inline std::span<const uint8_t> module_window(uintptr_t address, uintptr_t base,
                                            uintptr_t end, size_t maximum,
                                            bool executable) {
    if (base == 0 || end <= base || address < base || address >= end) { return {}; }
    const auto limit = address + (std::min)(maximum, end - address);
    auto cursor = address;
    while (cursor < limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            break;
        }
        const auto protection = mbi.Protect & 0xff;
        const auto readable_code = protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const auto readable_data = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY;
        if (!readable_code && (executable || !readable_data)) { break; }
        const auto region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.RegionSize > (std::numeric_limits<uintptr_t>::max)() - region_base ||
            region_base + mbi.RegionSize <= cursor) { break; }
        cursor = (std::min)(limit, region_base + mbi.RegionSize);
    }
    return {reinterpret_cast<const uint8_t*>(address), cursor - address};
}
}
