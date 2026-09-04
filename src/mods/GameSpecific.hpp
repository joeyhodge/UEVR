#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace uevr::games {

// The legacy Dune rendering experiment depends on binary-specific D3D12
// descriptor and render-target hooks. Current game builds no longer match
// those guards, so keep every Dune-only runtime path retired and use UEVR's
// generic rendering paths instead.
inline constexpr bool dune_experimental_rendering_enabled = false;

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

inline bool is_stalker2_legacy_ue51_runtime(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms) {
    if (!is_stalker2_executable_path(path)) {
        return false;
    }

    if (!detected_version.empty() && detected_version != L"0.00" && detected_version != L"unknown") {
        return detected_version == L"5.1" || detected_version.starts_with(L"5.1.");
    }

    return file_version_ms == 0x00050001;
}

inline bool is_stalker2_ue55_runtime(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms) {
    if (!is_stalker2_executable_path(path)) {
        return false;
    }

    if (!detected_version.empty() && detected_version != L"0.00" && detected_version != L"unknown") {
        return detected_version == L"5.5" || detected_version.starts_with(L"5.5.");
    }

    return file_version_ms == 0x00050005;
}

inline bool should_use_stalker2_ue55_native_fix_capture_layout(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms,
    bool dx12,
    bool native_stereo_fix_active) {
    return dx12 && native_stereo_fix_active &&
           is_stalker2_ue55_runtime(path, detected_version, file_version_ms);
}

inline bool should_use_stalker2_ue55_synced_scene_target(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms,
    bool dx12,
    bool synchronized_sequential,
    bool completed_game_viewport_draw) {
    return dx12 && synchronized_sequential && completed_game_viewport_draw &&
           is_stalker2_ue55_runtime(path, detected_version, file_version_ms);
}

inline bool stalker2_native_fix_requires_same_pass(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms) {
    return is_stalker2_legacy_ue51_runtime(path, detected_version, file_version_ms) ||
           is_stalker2_ue55_runtime(path, detected_version, file_version_ms);
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

inline bool is_the_sinking_city_2_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\thesinkingcity2.exe") ||
           lowered.ends_with(L"/thesinkingcity2.exe") ||
           lowered == L"thesinkingcity2.exe";
}

inline bool is_sw_zero_company_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\swzerocompany.exe") ||
           lowered.ends_with(L"/swzerocompany.exe") ||
           lowered == L"swzerocompany.exe";
}

inline bool is_bodycam_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\bodycam-win64-shipping.exe") ||
           lowered.ends_with(L"/bodycam-win64-shipping.exe") ||
           lowered == L"bodycam-win64-shipping.exe";
}

inline bool is_bodycam_ue554_runtime(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms,
    uint32_t file_version_ls) {
    if (!is_bodycam_executable_path(path)) {
        return false;
    }

    if (!detected_version.empty() && detected_version != L"0.00" && detected_version != L"unknown") {
        return detected_version == L"5.5.4";
    }

    return file_version_ms == 0x00050005 && file_version_ls == 0x00040000;
}

inline bool should_use_bodycam_ue554_dx12_texture_layout(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms,
    uint32_t file_version_ls,
    bool dx12) {
    return dx12 && is_bodycam_ue554_runtime(
        path,
        detected_version,
        file_version_ms,
        file_version_ls);
}

inline bool is_sw_zero_company_ue56_runtime(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms) {
    if (!is_sw_zero_company_executable_path(path)) {
        return false;
    }

    if (!detected_version.empty() && detected_version != L"0.00" && detected_version != L"unknown") {
        return detected_version == L"5.6" || detected_version.starts_with(L"5.6.");
    }

    return file_version_ms == 0x00050006;
}

inline bool is_storm_escape_executable_path(std::wstring_view path) {
    const auto lowered = lowercase_path(path);
    return lowered.ends_with(L"\\stormescape-win64-shipping.exe") ||
           lowered.ends_with(L"/stormescape-win64-shipping.exe") ||
           lowered == L"stormescape-win64-shipping.exe";
}

inline bool should_use_storm_escape_ue561_native_fix_capture_layout(
    std::wstring_view path,
    std::wstring_view detected_version,
    uint32_t file_version_ms,
    uint32_t file_version_ls,
    bool dx12,
    bool native_stereo_fix_active) {
    if (!dx12 || !native_stereo_fix_active || !is_storm_escape_executable_path(path)) {
        return false;
    }

    const bool exact_detected_version = detected_version == L"5.6.1";
    const bool exact_file_version =
        file_version_ms == 0x00050006 && file_version_ls == 0x00010000;
    return exact_detected_version || exact_file_version;
}

inline bool is_controller_camera_guard_candidate_path(std::wstring_view path) {
    return is_stalker2_executable_path(path) || is_mechwarrior_clans_executable_path(path);
}

}
