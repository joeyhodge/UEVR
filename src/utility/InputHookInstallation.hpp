#pragma once

#include <cstddef>
#include <mutex>
#include <string_view>

namespace uevr::input_hooks {

inline bool requires_shf_installation_lock(std::wstring_view path) {
    if (path.find(L'\0') != std::wstring_view::npos) {
        return false;
    }

    const auto separator = path.find_last_of(L"/\\");
    const auto filename = path.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
    constexpr std::wstring_view expected = L"shf-win64-shipping.exe";
    if (filename.size() != expected.size()) {
        return false;
    }

    for (std::size_t i = 0; i < filename.size(); ++i) {
        const auto ch = filename[i] >= L'A' && filename[i] <= L'Z'
            ? filename[i] + (L'a' - L'A') : filename[i];
        if (ch != expected[i]) {
            return false;
        }
    }
    return true;
}

inline std::unique_lock<std::recursive_mutex> acquire_shf_installation_lock(
    std::wstring_view path, std::recursive_mutex& hook_monitor_mutex) {
    std::unique_lock lock{hook_monitor_mutex, std::defer_lock};
    if (requires_shf_installation_lock(path)) {
        lock.lock();
    }
    return lock;
}

} // namespace uevr::input_hooks
