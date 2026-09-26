#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace uevr::mono {

// Serialized IDs are shared by the backend and injector, including non-DIBR builds.
inline constexpr int32_t method_id = 5;
struct Choice { int32_t id; const char* label; };
inline constexpr std::array choices {
    Choice{0, "Native Stereo"}, Choice{1, "Synchronized Sequential"}, Choice{2, "Alternating/AFR"},
    Choice{3, "Synthetic Stereo (DIBR, Experimental)"},
    Choice{4, "Synthetic Stereo (DIBR Single View, Experimental)"},
    Choice{method_id, "Mono (Experimental)"},
};
inline constexpr std::array non_dibr_choices {choices[0], choices[1], choices[2], choices[5]};

constexpr const char* label(int32_t id, std::span<const Choice> available = choices) {
    for (const auto& choice : available) { if (choice.id == id) { return choice.label; } }
    return "Unsupported rendering method (preserved)";
}
constexpr bool available(int32_t id, std::span<const Choice> options = choices) {
    for (const auto& choice : options) { if (choice.id == id) { return true; } }
    return false;
}

struct Capabilities {
    bool dx11{}, dx12{}, openxr{}, restricted_title{}, extreme{}, split_screen{}, screen_2d{},
        sceneview_compat{}, stereo_emulation{};
};
constexpr const char* unavailable_reason(const Capabilities& c) {
    if ((!c.dx11 && !c.dx12) || !c.openxr) { return "Mono requires DX11 or DX12 with OpenXR"; }
    if (c.restricted_title) { return "Mono is unavailable for this title's restricted rendering path"; }
    if (c.extreme || c.split_screen || c.screen_2d || c.sceneview_compat || c.stereo_emulation) {
        return "Mono requires Extreme Compatibility, split-screen, 2D Screen, SceneView compatibility and Stereo Emulation OFF";
    }
    return nullptr;
}

// Requests from config, Lua/API and ImGui all use this transaction. Only Mono
// transitions are deferred; existing mode-to-mode selection remains unchanged.
class Selection {
public:
    struct State { int32_t method; uint32_t generation; };
    State snapshot() const {
        const auto packed = m_active.load(std::memory_order_acquire);
        return {static_cast<int32_t>(packed), static_cast<uint32_t>(packed >> 32)};
    }
    uint64_t request_token() const { return m_requested.load(std::memory_order_acquire); }
    int32_t requested() const { return static_cast<int32_t>(request_token()); }
    int32_t active() const { return snapshot().method; }
    uint64_t generation() const { return snapshot().generation; }
    bool pending() const { return requested() != active(); }
    void initialize(int32_t value) {
        std::scoped_lock lock{m_mutex};
        if (m_initialized) { request_locked(value); return; }
        m_initialized = true;
        m_requested = pack(value, 1);
        // Even a startup profile enters through the same retired, game-frame
        // transaction. This also leaves unsupported startup requests escapable.
        m_active = pack(value == method_id ? 0 : value, 0);
    }
    void request(int32_t value) {
        std::scoped_lock lock{m_mutex};
        request_locked(value);
    }
    void consumers_retired(uint64_t token) {
        std::scoped_lock lock{m_mutex};
        if (request_token() == token && pending()) { m_retired_request = token; }
    }
    bool commit_at_game_frame_boundary() {
        std::scoped_lock lock{m_mutex};
        if (!pending() || m_retired_request != request_token()) { return false; }
        m_retired_request = 0;
        m_active.store(pack(requested(), snapshot().generation + 1), std::memory_order_release);
        return true;
    }
private:
    static uint64_t pack(int32_t method, uint32_t serial) {
        return (uint64_t{serial} << 32) | static_cast<uint32_t>(method);
    }
    void request_locked(int32_t value) {
        if (value == requested()) { return; }
        m_retired_request = 0;
        m_requested = pack(value, static_cast<uint32_t>(request_token() >> 32) + 1);
        // A later old-mode request cancels an uncommitted entry into Mono.
        if (active() != method_id && value != method_id) {
            m_active = pack(value, snapshot().generation);
        }
    }
    mutable std::mutex m_mutex;
    std::atomic<uint64_t> m_requested{}, m_active{};
    uint64_t m_retired_request{};
    bool m_initialized{};
};

struct Eye {
    std::array<float, 4> orientation{0, 0, 0, 1};
    std::array<float, 3> position{};
    // Tangents: left, right, up, down.
    std::array<float, 4> fov{};
};
struct Geometry {
    std::array<float, 4> orientation{};
    std::array<float, 3> center{};
    float horizontal{}, vertical{};
    std::array<std::array<float, 4>, 2> bounds{}; // left, right, top, bottom
};

inline std::optional<Geometry> geometry(const std::array<Eye, 2>& eyes) {
    float dot = 0;
    std::array<float, 2> norms{};
    for (size_t eye = 0; eye != 2; ++eye) {
        float norm = 0;
        for (float v : eyes[eye].orientation) { if (!std::isfinite(v)) { return {}; } norm += v * v; }
        if (std::abs(norm - 1.0f) > 0.001f) { return {}; }
        norms[eye] = norm;
        for (float v : eyes[eye].position) { if (!std::isfinite(v)) { return {}; } }
        for (float v : eyes[eye].fov) { if (!std::isfinite(v) || std::abs(v) > 100.f) { return {}; } }
        const auto& f = eyes[eye].fov;
        if (!(f[0] < -0.001f && f[1] > 0.001f && f[2] > 0.001f && f[3] < -0.001f)) { return {}; }
    }
    for (size_t i = 0; i != 4; ++i) { dot += eyes[0].orientation[i] * eyes[1].orientation[i]; }
    // Parallel optical axes only (quaternion sign is immaterial). No canted approximation.
    if (std::abs(dot) / std::sqrt(norms[0] * norms[1]) < 0.999999f) { return {}; }
    Geometry g{};
    for (size_t i = 0; i != 4; ++i) { g.orientation[i] = eyes[0].orientation[i] / std::sqrt(norms[0]); }
    for (size_t i = 0; i != 3; ++i) { g.center[i] = (eyes[0].position[i] + eyes[1].position[i]) * 0.5f; }
    for (const auto& eye : eyes) {
        g.horizontal = (std::max)({g.horizontal, -eye.fov[0], eye.fov[1]});
        g.vertical = (std::max)({g.vertical, eye.fov[2], -eye.fov[3]});
    }
    for (size_t i = 0; i != 2; ++i) {
        const auto& f = eyes[i].fov;
        g.bounds[i] = {0.5f + 0.5f * f[0] / g.horizontal, 0.5f + 0.5f * f[1] / g.horizontal,
                       0.5f - 0.5f * f[2] / g.vertical, 0.5f - 0.5f * f[3] / g.vertical};
    }
    return g;
}

constexpr bool frame_matches(uint64_t generation, uint64_t produced_generation,
    uint32_t render_frame, uint32_t pose_frame, uint32_t projection_frame, bool main_family) {
    return generation != 0 && produced_generation == generation && main_family &&
        render_frame == pose_frame && render_frame == projection_frame;
}

constexpr bool needs_frame_gate(int32_t method, uint64_t generation, uint64_t ready_generation) {
    return method == method_id || generation != ready_generation;
}

struct CopyRegion { uint32_t left{}, top{}, right{}, bottom{}, destination_x{}; };
// Keep the established double-wide allocation: one engine view in its left
// region, copied to both halves. No AFR timing, new target lifetime or shader.
inline std::optional<std::array<CopyRegion, 2>> copy_regions(
    uint64_t source_width, uint32_t source_height, uint64_t destination_width, uint32_t destination_height) {
    if (source_width == 0 || (source_width & 1) || source_width > 32768 || source_height == 0 ||
        source_width != destination_width || source_height != destination_height) { return {}; }
    const auto eye_width = static_cast<uint32_t>(source_width / 2);
    return std::array{CopyRegion{0, 0, eye_width, source_height, 0},
                      CopyRegion{0, 0, eye_width, source_height, eye_width}};
}
} // namespace uevr::mono
