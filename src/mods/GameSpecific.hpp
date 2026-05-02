#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace uevr::games {

inline std::wstring lowercase_path(std::wstring_view path) {
    std::wstring lowered{path};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

inline bool is_avowed_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.find(L"avowed-win64-shipping") != std::wstring::npos ||
           lowered.find(L"avowed-wingdk-shipping") != std::wstring::npos;
}

}
