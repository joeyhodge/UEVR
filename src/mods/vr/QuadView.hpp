#pragma once

// Quad view foveated rendering: the portable, engine-independent parts.
//
// Unreal renders four views per frame instead of two:
//   slot 0: left wide   (whole left-eye field of view, low resolution)
//   slot 1: right wide  (whole right-eye field of view, low resolution)
//   slot 2: left focus  (small window around the gaze, high pixel density)
//   slot 3: right focus (same, right eye)
// All four share one atlas render target and one head-pose sample per frame.
//
// This header holds only math and bookkeeping, so it compiles and is unit
// tested without Windows, D3D or the UESDK (tests/QuadViewTests.cpp).
// Angles are handled as tangents, in which image rows and columns are linear
// (left < right, down < up), matching OpenXR's XrFovf after tan().
//
// The gaze window and dead-zone filter are ported from BeamNG-VRS
// (src/triple_view.hpp, MIT, Letrange-g), where they run on a Pimax Dream Air.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace uevr::quadview {

inline constexpr uint32_t VIEW_COUNT = 4;

struct Settings {
    bool enabled{false};
    double wide_scale{0.5};     // wide view size, share of the headset's per-eye resolution (linear)
    double focus_width{0.30};   // focus window, share of the eye's full tangent width
    double focus_height{0.30};  // and height
    double focus_density{1.0};  // focus pixel density relative to the headset's native density
    double dead_zone{0.03};     // tangent units (~1.7 deg) the gaze must move before the window follows
    uint64_t gaze_return_ms{500}; // without gaze for this long, the window recentres
};

struct Tangents {
    double left{-1}, right{1}, up{1}, down{-1};
    double width() const { return right - left; }
    double height() const { return up - down; }
};

inline Tangents from_angles(double left, double right, double up, double down) {
    return {std::tan(left), std::tan(right), std::tan(up), std::tan(down)};
}

struct Slot {
    uint32_t eye{0};    // 0 = left, 1 = right
    bool focus{false};  // false = wide view
    uint32_t index() const { return eye + (focus ? 2u : 0u); }
};

// UEVR sees view indices either starting from 0 (0..3) or from 1 (1..4),
// the same ambiguity it already resolves for stereo with index_starts_from_one.
inline Slot slot_for_view(int32_t view_index, bool index_starts_from_one) {
    const int32_t n = index_starts_from_one ? view_index - 1 : view_index;
    const auto i = static_cast<uint32_t>(((n % 4) + 4) % 4);
    return Slot{i % 2u, i >= 2u};
}

// Only the first view drives the frame (shadows, exposure, history setup);
// every other view is a secondary of it.
inline bool is_primary_view(int32_t view_index) { return view_index == 0; }

struct Rect {
    uint32_t x{0}, y{0}, w{0}, h{0};
};

struct Layout {
    uint32_t atlas_w{0}, atlas_h{0};
    Rect views[VIEW_COUNT]{};  // indexed by Slot::index()
};

inline uint32_t even_size(double v, uint32_t lo = 16, uint32_t hi = 16384) {
    const auto clamped = std::clamp(v, double(lo), double(hi));
    return static_cast<uint32_t>(std::lround(clamped / 2.0) * 2);
}

// Atlas: wide pair on top, focus pair below.
//   +-----------+-----------+
//   | wide L    | wide R    |
//   +--------+--------+-----+
//   | focus L| focus R|
//   +--------+--------+
inline Layout make_layout(const Settings& s, uint32_t eye_w, uint32_t eye_h) {
    Layout l{};
    const uint32_t ww = even_size(eye_w * std::clamp(s.wide_scale, 0.1, 1.0));
    const uint32_t wh = even_size(eye_h * std::clamp(s.wide_scale, 0.1, 1.0));
    const double density = std::clamp(s.focus_density, 0.25, 2.0);
    const uint32_t fw = even_size(eye_w * std::clamp(s.focus_width, 0.05, 1.0) * density);
    const uint32_t fh = even_size(eye_h * std::clamp(s.focus_height, 0.05, 1.0) * density);

    l.views[0] = {0, 0, ww, wh};
    l.views[1] = {ww, 0, ww, wh};
    l.views[2] = {0, wh, fw, fh};
    l.views[3] = {fw, wh, fw, fh};
    l.atlas_w = std::max(2 * ww, 2 * fw);
    l.atlas_h = wh + fh;
    return l;
}

// Scales a layout down, as a whole, until it fits inside max_w x max_h (the
// render target UEVR already asks Unreal for: two eyes wide, one eye high).
// Every view keeps its aspect ratio and the views keep their relative density.
inline Layout fit_layout(Layout l, uint32_t max_w, uint32_t max_h) {
    if (l.atlas_w == 0 || l.atlas_h == 0 || (l.atlas_w <= max_w && l.atlas_h <= max_h)) {
        return l;
    }
    const double k = std::min(double(max_w) / l.atlas_w, double(max_h) / l.atlas_h);
    // even_size rounds, so shrink the scale until the rounded atlas fits.
    for (double scale = k; scale > 0.05; scale *= 0.995) {
        Layout f{};
        const auto sz = [&](uint32_t v) { return even_size(v * scale, 2); };
        const uint32_t ww = sz(l.views[0].w), wh = sz(l.views[0].h);
        const uint32_t fw = sz(l.views[2].w), fh = sz(l.views[2].h);
        f.views[0] = {0, 0, ww, wh};
        f.views[1] = {ww, 0, ww, wh};
        f.views[2] = {0, wh, fw, fh};
        f.views[3] = {fw, wh, fw, fh};
        f.atlas_w = std::max(2 * ww, 2 * fw);
        f.atlas_h = wh + fh;
        if (f.atlas_w <= max_w && f.atlas_h <= max_h) return f;
    }
    return l;
}

inline Layout make_fitted_layout(const Settings& s, uint32_t eye_w, uint32_t eye_h) {
    return fit_layout(make_layout(s, eye_w, eye_h), 2 * eye_w, eye_h);
}

// A window of the given share of `full`, centred on (cx, cy) in tangent space
// and slid back inside `full` where it would cross an edge.
inline Tangents focus_window(const Tangents& full, double share_x, double share_y, double cx, double cy) {
    share_x = std::clamp(share_x, 0.05, 1.0);
    share_y = std::clamp(share_y, 0.05, 1.0);
    const double half_w = full.width() * share_x / 2;
    const double half_h = full.height() * share_y / 2;
    if (!std::isfinite(cx)) cx = 0;
    if (!std::isfinite(cy)) cy = 0;
    cx = std::clamp(cx, full.left + half_w, full.right - half_w);
    cy = std::clamp(cy, full.down + half_h, full.up - half_h);
    return {cx - half_w, cx + half_w, cy + half_h, cy - half_h};
}

// Keeps the focus windows still against eye-tracker noise: both eyes' centres
// jump to the new gaze together once either eye's gaze has moved past the dead
// zone (saccades hide the jump). Without gaze for return_ms, they recentre.
// Centres are in each eye's tangent space; (0, 0) is straight ahead.
struct GazeFilter {
    double cx[2]{0, 0}, cy[2]{0, 0};
    bool seeded{false};
    uint64_t last_valid_ms{0};

    void update(const double gx[2], const double gy[2], uint32_t valid_mask, double dead_zone, uint64_t now_ms,
                uint64_t return_ms) {
        if ((valid_mask & 3u) == 3u) {
            last_valid_ms = now_ms;
            bool move = !seeded;
            for (int i = 0; i < 2; ++i) {
                if (std::hypot(gx[i] - cx[i], gy[i] - cy[i]) > dead_zone) move = true;
            }
            if (move) {
                for (int i = 0; i < 2; ++i) { cx[i] = gx[i]; cy[i] = gy[i]; }
                seeded = true;
            }
        } else if (seeded && now_ms - last_valid_ms > return_ms) {
            for (int i = 0; i < 2; ++i) { cx[i] = 0; cy[i] = 0; }
            seeded = false;
        }
    }
};

// One frame's view set. Every view is filled from the SAME head-pose sample
// (pose_id), and the compositor later uses exactly these tangents, so what was
// rendered and what is composed/submitted can never disagree (the BeamNG drift).
struct FrameViews {
    uint64_t pose_id{0};
    Tangents tangents[VIEW_COUNT]{};  // indexed by Slot::index()
};

inline FrameViews make_frame_views(uint64_t pose_id, const Tangents eye_full[2], const GazeFilter& gaze,
                                   const Settings& s) {
    FrameViews f{};
    f.pose_id = pose_id;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        f.tangents[eye] = eye_full[eye];
        f.tangents[eye + 2] = focus_window(eye_full[eye], s.focus_width, s.focus_height, gaze.cx[eye], gaze.cy[eye]);
    }
    return f;
}

// Unreal's reversed-Z, infinite far plane, off-axis projection for a view
// whose edges sit at the given tangents (the same form UEVR's OpenXR runtime
// builds for each eye). `out` is in memory order: Unreal reads it as rows,
// glm as columns, so it can be copied straight into either matrix type.
// For a view-space point (x right, y up, z forward): clip = [x y z 1] * M,
// and x/z == right lands on NDC x == +1, y/z == up on NDC y == +1.
inline void projection_from_tangents(const Tangents& t, float near_z, float out[16]) {
    const double inv_rl = 1.0 / (t.right - t.left);
    const double inv_tb = 1.0 / (t.up - t.down);
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = float(2.0 * inv_rl);
    out[5] = float(2.0 * inv_tb);
    out[8] = float(-(t.right + t.left) * inv_rl);
    out[9] = float(-(t.up + t.down) * inv_tb);
    out[11] = 1.0f;
    out[14] = near_z;
}

// OpenXR wants angles (XrFovf) for each submitted view.
struct Angles {
    float left, right, up, down;
};
inline Angles to_angles(const Tangents& t) {
    return {float(std::atan(t.left)), float(std::atan(t.right)), float(std::atan(t.up)), float(std::atan(t.down))};
}

// Share of the eye's pixels the four views cost, versus plain stereo at
// native resolution (for the log and the settings UI).
inline double pixel_share(const Layout& l, uint32_t eye_w, uint32_t eye_h) {
    double sum = 0;
    for (const auto& r : l.views) sum += double(r.w) * r.h;
    return sum / (2.0 * eye_w * eye_h);
}

} // namespace uevr::quadview
