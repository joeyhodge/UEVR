#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include "ProSpiPlayerVisibilityCode.hpp"

namespace uevr::prospi::players {

using Vector = std::array<float, 3>;
using Matrix = std::array<float, 16>;
inline bool pointer(uintptr_t value) {
    return value >= 0x10000 && value < 0x0000800000000000ull && !(value & 7);
}
inline bool finite(const Vector& v) {
    return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}
inline float dot(const Vector& a, const Vector& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
struct Sphere { Vector center{}; float radius{}; };

// The native player updater supplies these same local bounds to its culling
// object. Its row-vector transform and X/Z/Y * 10 bridge are verified in both editions.
inline std::optional<Sphere> player_bounds(const std::array<float, 6>& bounds, const Matrix& matrix) {
    if (!std::all_of(bounds.begin(), bounds.end(), [](float v) { return std::isfinite(v); }) ||
        !std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); }) ||
        std::abs(matrix[3]) > 0.001f || std::abs(matrix[7]) > 0.001f ||
        std::abs(matrix[11]) > 0.001f || std::abs(matrix[15] - 1.f) > 0.001f) { return {}; }
    Vector local{}, extent{}, center{}, world_extent{};
    for (size_t i{}; i < 3; ++i) {
        if (bounds[i] > bounds[i+3] || bounds[i+3] - bounds[i] > 1000.f) { return {}; }
        local[i] = (bounds[i] + bounds[i+3]) * 0.5f;
        extent[i] = (bounds[i+3] - bounds[i]) * 0.5f;
        const auto scale2 = matrix[i*4]*matrix[i*4] + matrix[i*4+1]*matrix[i*4+1] + matrix[i*4+2]*matrix[i*4+2];
        if (scale2 < 0.01f || scale2 > 100.f || std::abs(local[i]) > 1000.f) { return {}; }
    }
    for (size_t j{}; j < 3; ++j) {
        center[j] = matrix[12+j];
        for (size_t i{}; i < 3; ++i) {
            center[j] += local[i] * matrix[i*4+j];
            world_extent[j] += extent[i] * std::abs(matrix[i*4+j]);
        }
        if (std::abs(center[j]) > 1000000.f) { return {}; }
    }
    Sphere result{{center[0]*10.f, center[2]*10.f, center[1]*10.f}, std::sqrt(dot(world_extent, world_extent))*10.f};
    return finite(result.center) && result.radius > 0 && result.radius <= 5000.f ? std::optional{result} : std::nullopt;
}

struct View {
    Vector origin{}, forward{}, right{}, up{};
    float horizontal{}, vertical{};
    int64_t time_ms{};
    uint64_t generation{};
};
inline bool packet_current(int64_t time_ms, uint64_t generation, int64_t now_ms, uint64_t expected) {
    return time_ms > 0 && generation && generation == expected && now_ms >= time_ms && now_ms-time_ms <= 250;
}
inline bool uses_headset_projection(bool match_game_fov, bool dolly) { return !match_game_fov || dolly; }

// Use a conservative union of the actual eye projections, not the telephoto
// broadcast FOV. The final per-eye view rotation also covers canted headsets.
inline std::optional<std::array<float, 2>> projection_extent(const Matrix& p) {
    if (!std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); }) ||
        p[0] < 0.05f || p[0] > 100.f || p[5] < 0.05f || p[5] > 100.f ||
        std::abs(p[11] - 1.f) > 0.001f || std::abs(p[15]) > 0.001f ||
        std::abs(p[1]) > 0.001f || std::abs(p[4]) > 0.001f) { return {}; }
    return std::array{(1.f + std::abs(p[8])) / p[0], (1.f + std::abs(p[9])) / p[5]};
}

inline std::optional<View> make_view(const Vector& origin, const Vector& rotation,
    const Matrix& left, const Matrix& right, int64_t time_ms, uint64_t generation) {
    const auto l = projection_extent(left), r = projection_extent(right);
    if (!finite(origin) || !finite(rotation) || !l || !r || time_ms <= 0 || !generation) { return {}; }
    constexpr float radians = 0.017453292519943295f;
    const auto cp = std::cos(rotation[0]*radians), sp = std::sin(rotation[0]*radians);
    const auto cy = std::cos(rotation[1]*radians), sy = std::sin(rotation[1]*radians);
    const auto cr = std::cos(rotation[2]*radians), sr = std::sin(rotation[2]*radians);
    return View{origin, {cp*cy, cp*sy, sp}, {sr*sp*cy-cr*sy, sr*sp*sy+cr*cy, -sr*cp},
        {-cr*sp*cy-sr*sy, -cr*sp*sy+sr*cy, cr*cp},
        (std::max)((*l)[0], (*r)[0])*1.1f + 0.15f,
        (std::max)((*l)[1], (*r)[1])*1.1f + 0.15f, time_ms, generation};
}
inline bool intersects(const View& view, const Sphere& sphere) {
    Vector relative{};
    for (size_t i{}; i < 3; ++i) { relative[i] = sphere.center[i] - view.origin[i]; }
    const auto depth = dot(relative, view.forward);
    const auto radius = sphere.radius + 150.f; // IPD, head motion and one-frame pose age.
    if (depth + radius < 0) { return false; }
    return std::abs(dot(relative, view.right)) <= (std::max)(depth, 0.f)*view.horizontal + radius*std::sqrt(1.f+view.horizontal*view.horizontal) &&
        std::abs(dot(relative, view.up)) <= (std::max)(depth, 0.f)*view.vertical + radius*std::sqrt(1.f+view.vertical*view.vertical);
}
inline std::optional<bool> relevance(const std::array<View, 2>& views, const Sphere& sphere,
    int64_t now_ms, uint64_t generation) {
    bool valid{};
    for (const auto& view : views) {
        if (!packet_current(view.time_ms, view.generation, now_ms, generation)) { continue; }
        valid = true;
        if (intersects(view, sphere)) { return true; }
    }
    return valid ? std::optional{false} : std::nullopt;
}

struct Identity {
    uintptr_t state{}, vtable{}, transform{};
    uint64_t model{};
    bool operator==(const Identity&) const = default;
    bool valid() const { return pointer(state) && pointer(vtable) && pointer(transform) && model != UINT64_MAX; }
};
struct State { Identity identity{}; uint8_t enabled{}; bool operator==(const State&) const = default; };
template<class Read> std::optional<State> read_state(Read read, const Layout& layout, uintptr_t address) {
    State state{{address}};
    std::array<uint64_t, 2> header{};
    if (!pointer(address) || !read(address, header.data(), sizeof(header)) ||
        !read(address+layout.enabled, &state.enabled, sizeof(state.enabled)) ||
        !read(address+layout.transform, &state.identity.transform, sizeof(state.identity.transform))) { return {}; }
    state.identity.vtable = header[0]; state.identity.model = header[1];
    if (!state.identity.valid() || state.enabled > 1 || state.identity.vtable < layout.base ||
        layout.image_size < 8 || state.identity.vtable-layout.base > layout.image_size-8) { return {}; }
    return state;
}
template<class Context> void restore_visibility(Context& ctx, bool r15) {
    auto& value = r15 ? ctx.r15 : ctx.r12;
    value = (value & ~uint64_t{0xff}) | uint64_t{1};
}
enum class Decision { unchanged, natural, restore, intentional_off, rejected };

// Four-way associative, fixed-size and thread-local in the native callback.
// No UObject walks, names, allocations or per-player VirtualQuery calls.
class Cache {
public:
    static constexpr size_t capacity = 256;
    void clear() { m_entries = {}; }
    Decision consider(const Identity& id, uint8_t enabled, uint8_t original, bool heavy,
        std::optional<bool> in_view, int64_t now_ms, uint64_t generation, std::optional<uint32_t> tick = {}) {
        if (!id.valid() || enabled > 1 || original > 1 || now_ms <= 0 || !generation) { return Decision::rejected; }
        auto& entry = find(id.state, now_ms);
        if (entry.id != id) { entry = {}; entry.id = id; }
        entry.touched = now_ms;
        if (!enabled) { entry = {}; return Decision::intentional_off; }
        if (in_view) {
            if (*in_view && tick) { entry.hmd = {now_ms, generation, *tick, true}; }
            else if (!*in_view) { entry.hmd = {}; }
        }
        if (original) {
            entry.visible = now_ms; entry.generation = generation;
            return Decision::natural;
        }
        if (heavy || (in_view && *in_view)) { return Decision::restore; }
        if (in_view) { return Decision::unchanged; }
        // A shot change precedes publication of its final HMD view. Keep an
        // independently geometry-proven body through that bounded gap, rather
        // than cycling its actor/mesh off then on. Forced readback never renews it.
        const auto& hmd = entry.hmd;
        if (tick && hmd.valid && generation >= hmd.generation && generation-hmd.generation <= 1 &&
            now_ms >= hmd.time_ms && now_ms-hmd.time_ms <= 100 && uint32_t(*tick-hmd.tick) <= 2) {
            return Decision::restore;
        }
        // During a cut's first view or a temporarily unavailable pose, retain
        // only genuine game-visible history. Never teach from our forced bit.
        const auto age = now_ms-entry.visible;
        const auto current = entry.generation == generation && age <= 3000;
        const auto handoff = entry.generation && entry.generation+1 == generation && age <= 500;
        return entry.visible > 0 && age >= 0 && (current || handoff) ? Decision::restore : Decision::unchanged;
    }
    size_t count(int64_t now_ms) const {
        return std::count_if(m_entries.begin(), m_entries.end(), [&](const auto& e) {
            return e.id.state && now_ms >= e.touched && now_ms-e.touched <= 3000;
        });
    }
private:
    struct HmdProof { int64_t time_ms{}; uint64_t generation{}; uint32_t tick{}; bool valid{}; };
    struct Entry { Identity id{}; int64_t touched{}, visible{}; uint64_t generation{}; HmdProof hmd{}; };
    Entry& find(uintptr_t state, int64_t now_ms) {
        const auto bucket = ((state >> 4) ^ (state >> 17)) & 63;
        auto* oldest = &m_entries[bucket*4];
        Entry* reusable{};
        for (size_t i{}; i < 4; ++i) {
            auto& e = m_entries[bucket*4+i];
            if (e.id.state == state) { return e; }
            if (!e.id.state || now_ms < e.touched || now_ms-e.touched > 8000) { reusable = &e; }
            if (e.touched < oldest->touched) { oldest = &e; }
        }
        if (reusable) { *reusable = {}; return *reusable; }
        *oldest = {};
        return *oldest;
    }
    std::array<Entry, capacity> m_entries{};
};
}
