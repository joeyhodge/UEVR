#pragma once

#include <cstddef>
#include <cstdint>
#include <Windows.h>

// Backport of cursey/kananlib dcb698e (praydog), under the adjacent Boost license.
#define UEVR_KANANLIB_READABLE_WINDOW_BACKPORT 1

namespace utility::uevr_kananlib_backport {
inline std::size_t page_size() {
    static const std::size_t size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    return size;
}

inline std::size_t readable_decode_window(const uint8_t* ip) {
    constexpr std::size_t window = 64;
    const auto page = page_size();
    const auto to_page_end = page - (reinterpret_cast<uintptr_t>(ip) & (page - 1));

    __try {
        volatile uint8_t probe = *ip;
        (void)probe;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (to_page_end >= window) {
        return window;
    }
    __try {
        volatile uint8_t probe = *(ip + to_page_end);
        (void)probe;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return to_page_end;
    }
    return window;
}
}
