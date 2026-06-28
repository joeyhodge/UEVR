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

inline bool is_stalker2_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.find(L"stalker2-win64-shipping") != std::wstring::npos;
}

inline bool is_prospi_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.find(L"prospi-win64-shipping") != std::wstring::npos ||
           lowered.find(L"prospi24-win64-shipping") != std::wstring::npos;
}

inline bool is_dune_awakening_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.find(L"dunesandbox-win64-shipping") != std::wstring::npos ||
           lowered.find(L"duneawakening") != std::wstring::npos;
}

inline bool is_mechwarrior_clans_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.find(L"mechwarrior-win64-shipping") != std::wstring::npos ||
           lowered.find(L"mw5clans") != std::wstring::npos;
}

inline bool is_daysgone_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\daysgone.exe") ||
           lowered.ends_with(L"/daysgone.exe") ||
           lowered == L"daysgone.exe";
}

inline bool is_everspace2_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\es2-win64-shipping.exe") ||
           lowered.ends_with(L"/es2-win64-shipping.exe") ||
           lowered == L"es2-win64-shipping.exe";
}

inline bool is_controller_camera_guard_candidate_path(std::wstring_view path) {
    return is_stalker2_executable_path(path) || is_mechwarrior_clans_executable_path(path);
}

}
