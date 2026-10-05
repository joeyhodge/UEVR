#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace uevr::prospi::roof {
inline constexpr size_t max_targets = 256;
inline constexpr size_t max_levels = 256;
inline constexpr size_t cached_targets_per_draw = 8;
inline constexpr int32_t scan_objects_per_draw = 256;
inline constexpr uintptr_t viewport_interface_offset = 0x28;
inline constexpr size_t viewport_interface_draw_slot = 4;

// Match the callback against Engine.GameViewport, never find a UObject by
// subtracting from an arbitrary interface pointer. ProSpi's native Draw uses
// the source/BN-proven FCommonViewportClient subobject at +0x28.
constexpr std::optional<uintptr_t> viewport_dispatch_offset(uintptr_t dispatch, uintptr_t viewport) {
    if (!dispatch || !viewport || dispatch % alignof(uintptr_t) || viewport % alignof(uintptr_t)) { return {}; }
    if (dispatch == viewport) { return 0; }
    if (viewport <= UINTPTR_MAX - viewport_interface_offset && dispatch == viewport + viewport_interface_offset) {
        return viewport_interface_offset;
    }
    return {};
}

struct InitializationState {
    bool attempted{}, supported{};
    void on_disabled(bool hook_installed) {
        // Explicit off/on can retry failed discovery, never replace a live hook
        // or repeatedly scan metadata on every enabled Draw.
        if (!supported && !hook_installed) { attempted = false; }
    }
};

constexpr bool native_bool_storage(uint8_t field_size, uint8_t byte_offset,
    uint8_t byte_mask, uint8_t field_mask) {
    // UE4.27 FBoolProperty::SetBoolSize: native bool uses ByteMask=true,
    // whereas IsNativeBool tests FieldMask==255. These are not interchangeable.
    return field_size == 1 && byte_offset == 0 && byte_mask == 1 && field_mask == 0xff;
}

inline std::wstring lowercase(std::wstring_view name) {
    std::wstring result{name};
    for (auto& c : result) {
        if (c >= L'A' && c <= L'Z') { c += L'a' - L'A'; }
    }
    return result;
}

inline std::wstring asset_words(std::wstring_view name) {
    std::wstring result;
    result.reserve(name.size());
    const auto lower = [](wchar_t c) { return c >= L'a' && c <= L'z'; };
    const auto upper = [](wchar_t c) { return c >= L'A' && c <= L'Z'; };
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    wchar_t previous{};
    for (const auto c : name) {
        // Keep numbered/CamelCase structural roles separate: wallGrazed02,
        // rubberFence01 and handrail05 are not arbitrary substring matches.
        if ((upper(c) && lower(previous)) || (digit(c) && (lower(previous) || upper(previous))) ||
            ((lower(c) || upper(c)) && digit(previous))) { result += L'_'; }
        result += upper(c) ? c + L'a' - L'A' : c;
        previous = c;
    }
    return result;
}

inline bool token(std::wstring_view name, std::wstring_view word) {
    const auto alnum = [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
    };
    size_t at{};
    while ((at = name.find(word, at)) != std::wstring_view::npos) {
        const auto end = at + word.size();
        if ((at == 0 || !alnum(name[at - 1])) && (end == name.size() || !alnum(name[end]))) {
            return true;
        }
        ++at;
    }
    return false;
}

inline bool excluded_geometry(std::wstring_view raw) {
    const auto name = lowercase(raw);
    const auto words = asset_words(raw);
    for (const auto role : {L"mask", L"proxy", L"collision", L"lod", L"extra", L"variant",
            L"alternate", L"wbc", L"crowd", L"spectator"}) {
        if (token(name, role) || token(words, role)) { return true; }
    }
    return name.find(L"soloplaymask") != std::wstring_view::npos ||
        name.find(L"score") != std::wstring_view::npos ||
        name.find(L"displaytemplete") != std::wstring_view::npos ||
        name.find(L"displaytemplate") != std::wstring_view::npos;
}

inline bool roof_name(std::wstring_view raw) {
    const auto name = lowercase(raw);
    return !excluded_geometry(raw) && (token(name, L"roof") || token(name, L"ceiling"));
}

inline bool stadium_roof(std::wstring_view component, std::wstring_view mesh,
    std::wstring_view mesh_package) {
    const auto package = lowercase(mesh_package);
    return roof_name(component) && roof_name(mesh) &&
        package.starts_with(L"/game/stadiums/") && package.size() > 15;
}

inline bool outfield_structure(std::wstring_view raw) {
    const auto name = lowercase(raw);
    if (excluded_geometry(raw) || !token(name, L"outfield")) { return false; }
    const auto words = asset_words(raw);
    // Only structural roles from the controlled A/B, not every outfield asset.
    // In particular, alternate displays and ads are not structural geometry.
    for (const auto role : {L"wall", L"walls", L"stand", L"stands", L"pillar", L"pillars",
            L"stair", L"stairs", L"fence", L"fences", L"handrail", L"railing", L"wiremesh", L"saku", L"room",
            L"net", L"window", L"glass"}) {
        if (token(name, role) || token(words, role)) { return true; }
    }
    return false;
}

inline bool stadium_geometry(std::wstring_view component, std::wstring_view mesh,
    std::wstring_view mesh_package) {
    if (stadium_roof(component, mesh, mesh_package)) { return true; }
    const auto package = lowercase(mesh_package);
    if (!package.starts_with(L"/game/stadiums/") || package.size() <= 15) { return false; }
    auto c = lowercase(component);
    auto m = lowercase(mesh);
    if (!outfield_structure(component) || !outfield_structure(mesh)) { return false; }
    if (const auto fbx = c.find(L".fbx("); fbx != std::wstring::npos && c.ends_with(L")")) { c.resize(fbx); }
    if (m.starts_with(L"sm_")) { m.erase(0, 3); }
    return c == m;
}

enum class Geometry : uint8_t { rejected, structural, observed_background };

inline Geometry geometry(std::wstring_view component, std::wstring_view mesh,
    std::wstring_view mesh_package) {
    if (stadium_geometry(component, mesh, mesh_package)) { return Geometry::structural; }
    const auto package = lowercase(mesh_package);
    if (!package.starts_with(L"/game/stadiums/") || excluded_geometry(component) || excluded_geometry(mesh)) {
        return Geometry::rejected;
    }
    auto c = lowercase(component);
    auto m = lowercase(mesh);
    // Only the exported stadium-background groups, with identical component,
    // asset and package leaf names. Never select arbitrary stadium UObjects.
    if (const auto fbx = c.find(L".fbx("); fbx != std::wstring::npos && c.ends_with(L")")) { c.resize(fbx); }
    const auto slash = package.rfind(L'/');
    if (slash == std::wstring::npos || package.substr(slash + 1) != m) { return Geometry::rejected; }
    if (m.starts_with(L"sm_")) { m.erase(0, 3); }
    if (c != m) { return Geometry::rejected; }
    if (c.starts_with(L"p00_soto_") || c.starts_with(L"p03_outfield_") || c.starts_with(L"p07_alpha_outfield_") ||
        c == L"p02_infield_cheering" || c.starts_with(L"p02_infield_cheering_")) {
        return Geometry::observed_background;
    }
    return Geometry::rejected;
}

struct VisibilityProof {
    Geometry kind{Geometry::rejected};
    bool observed_visible{}, inherited_override{};
    void sample_unforced(bool visible) {
        if (!visible) { inherited_override = false; }
        else if (!inherited_override) { observed_visible = true; }
    }
    void game_request(bool requested, bool readback) {
        if (!readback || requested) { inherited_override = false; }
        observed_visible |= requested && readback;
    }
    bool allows_override() const {
        return kind == Geometry::structural || (kind == Geometry::observed_background && observed_visible);
    }
};

// Snapshot keys before invoking a game setter; never keep a cache iterator
// across a potentially reentrant game call. The cursor resumes a bounded sweep.
inline size_t maintenance_index(size_t& cursor, size_t count) {
    if (!count) { cursor = 0; return 0; }
    const auto index = cursor % count;
    cursor = (index + 1) % count;
    return index;
}

struct Eligibility {
    bool enabled{}, game_thread{}, current_world{}, current_component{};
    bool current_owner{}, current_mesh{}, current_level{}, level_in_world{};
    bool owner_hidden{}, component_hidden{};

    bool accepted() const {
        return enabled && game_thread && current_world && current_component && current_owner &&
            current_mesh && current_level && level_in_world && !owner_hidden && !component_hidden;
    }
};

// Shadow the game's requests, not the forced value, so disabling never restores
// a camera state sampled before the most recent cut.
struct Lease {
    bool requested{}, forced{};
    void game_request(bool visible) { requested = visible; forced = false; }
    void override_applied(bool readback) { forced = !requested && readback; }
    bool should_restore(bool current_visible) const { return forced && current_visible; }
};

inline bool disposable_pending(const VisibilityProof& proof, const Lease& lease) {
    return proof.kind == Geometry::observed_background && !proof.allows_override() &&
        !proof.inherited_override && !lease.forced;
}
}
