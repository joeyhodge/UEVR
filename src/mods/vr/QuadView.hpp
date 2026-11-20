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
//
// Contents, in order:
//   Settings and view slots   Settings, Tangents, Slot, slot_for_view, CallOrder
//   Atlas layout              Rect, Layout, make_layout, fit_layout
//   Sharp window              focus_window, focus_window_for
//   Gaze filter               median3, GazeFilter
//   Eye tracking              Quat, gaze_to_eye_tangent
//   Wide-view LOD             find_lod_factor_offset, LodOffsetLatch
//   Per-frame state           FrameSnapshot, projection_from_tangents, to_angles
//   Composition               focus_dest_rect, shapes, SpriteSettings packing
//
// Where the rest lives: docs/quad-view-integration.md.
// (std::min)/(std::max) are parenthesised so Windows.h min/max macros cannot break them.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace uevr::quadview {

inline constexpr uint32_t VIEW_COUNT = 4;

// --- Settings and view slots -------------------------------------------------

struct Settings {
    bool enabled{false};
    // Defaults match the UEVR settings (VR.hpp).
    double wide_scale{0.2};     // wide view size, share of the headset's per-eye resolution (linear)
    double focus_width{0.35};   // focus window, share of the eye's full tangent width
    double focus_height{0.35};  // and height
    double focus_density{1.0};  // focus pixel density relative to the headset's native density
    double dead_zone{0.0};      // tangent units (~1.7 deg) the gaze must move before the window follows
    double gaze_smoothing_ms{0.15};  // low-pass time constant that averages eye-tracker noise
    uint64_t gaze_return_ms{500}; // without gaze for this long, the window recentres
    double offset_x{0.0};       // sharp window shift, share of the eye's tangent width (+ = right)
    double offset_y{0.0};       // and height (+ = up), for trackers that sit slightly off
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
// Render order: wide L, wide R, sharp L, sharp R by default. With sharp_first
// the sharp pair is rendered first (sharp L, sharp R, wide L, wide R), for
// games whose effects only draw in the first two views (Groom hair).
inline Slot slot_for_view(int32_t view_index, bool index_starts_from_one, bool sharp_first = false) {
    const int32_t n = index_starts_from_one ? view_index - 1 : view_index;
    const auto i = static_cast<uint32_t>(((n % 4) + 4) % 4);
    const bool second_pair = i >= 2u;
    return Slot{i % 2u, sharp_first ? !second_pair : second_pair};
}

// UE4 never hands the stereo hooks a view index: the value is the stereo pass
// (0 = full/mono, then 1..4 as GetViewPassForIndex hands them out), and the
// engine's own pass for views 1..3 cannot tell them apart. What does hold is
// order: within one viewport draw the engine builds the views in request order
// (0, 1, 2, 3), calling each hook once per view. So the position of a call
// within the draw is the view index. Each hook keeps its own counter, since
// they are separate call sequences. The epoch changes at the start of every
// viewport draw, so a stereo-pass call made earlier in the frame (outside the
// draw) cannot shift the views; calls beyond the fourth are not views.
struct CallOrder {
    uint64_t epoch{~0ull};
    uint32_t calls{0};

    int32_t next(int32_t pass, uint64_t current_epoch) {
        if (pass < 1 || pass > (int32_t)VIEW_COUNT) {
            return -1;
        }
        if (epoch != current_epoch) {
            epoch = current_epoch;
            calls = 0;
        }
        if (calls >= VIEW_COUNT) {
            return -1;
        }
        return (int32_t)calls++;
    }
};

// Only the first view drives the frame (shadows, exposure, history setup);
// every other view is a secondary of it.
inline bool is_primary_view(int32_t view_index) { return view_index == 0; }

// --- Atlas layout ------------------------------------------------------------

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
    l.atlas_w = (std::max)(2 * ww, 2 * fw);
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
    const double k = (std::min)(double(max_w) / l.atlas_w, double(max_h) / l.atlas_h);
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
        f.atlas_w = (std::max)(2 * ww, 2 * fw);
        f.atlas_h = wh + fh;
        if (f.atlas_w <= max_w && f.atlas_h <= max_h) return f;
    }
    return l;
}

inline Layout make_fitted_layout(const Settings& s, uint32_t eye_w, uint32_t eye_h) {
    return fit_layout(make_layout(s, eye_w, eye_h), 2 * eye_w, eye_h);
}

// --- Sharp window ------------------------------------------------------------

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

// The focus window for one eye: the gaze centre (tangents) moved by the
// user's offset, then placed with focus_window.
inline Tangents focus_window_for(const Tangents& full, const Settings& s, double cx, double cy) {
    if (!std::isfinite(cx)) cx = 0;
    if (!std::isfinite(cy)) cy = 0;
    return focus_window(full, s.focus_width, s.focus_height,
        cx + s.offset_x * full.width(), cy + s.offset_y * full.height());
}

// --- Gaze filter -------------------------------------------------------------

// Median of three, used to reject single-frame eye-tracker spikes.
inline double median3(double a, double b, double c) {
    return (std::max)((std::min)(a, b), (std::min)((std::max)(a, b), c));
}

// Keeps the focus windows together with the gaze. The raw gaze is
// median-filtered (one-frame spikes never pass) and then low-passed with the
// user's smoothing time, which averages the remaining tracker noise over a few
// milliseconds. The window follows the filtered gaze directly, so it matches
// what the eyes are looking at; an optional dead zone holds it for small
// movements. When the gaze is lost (a blink, a tracking dropout) the window
// holds its place for return_ms, then eases back to the centre over EASE_MS
// instead of jumping there. Centres are in each eye's tangent space; (0, 0)
// is straight ahead.
struct GazeFilter {
    // Once the filtered gaze has settled this close to the median raw sample,
    // stop following; the window stays centred on the fixation.
    static constexpr double SETTLED = 0.004;
    // After the hold, the window slides to the centre over this long.
    static constexpr uint64_t EASE_MS = 150;

    double cx[2]{0, 0}, cy[2]{0, 0}; // window centre
    double fx[2]{0, 0}, fy[2]{0, 0}; // filtered gaze target
    double rx[2][3]{}, ry[2][3]{};    // last three raw samples per eye
    uint32_t ri[2]{0, 0};             // next raw-sample slot
    double lx[2]{0, 0}, ly[2]{0, 0}; // window centre when the gaze was lost
    bool seeded{false};
    bool following{false};
    bool easing{false};               // sliding back to the centre after a long loss
    uint64_t last_valid_ms{0};
    uint64_t last_update_ms{0};

    // smoothing_ms is the low-pass time constant: how many milliseconds of gaze
    // are averaged. 0 follows the median sample immediately. The frame time is
    // used as dt, so the feel does not change with the frame rate.
    void update(const double gx[2], const double gy[2], uint32_t valid_mask, double dead_zone,
                double smoothing_ms, uint64_t now_ms, uint64_t return_ms) {
        if ((valid_mask & 3u) == 3u) {
            if (easing) {
                // The gaze came back after a long loss: start again from where
                // the eyes are now rather than from the stale filter state.
                seeded = false;
                easing = false;
            }

            last_valid_ms = now_ms;

            const uint64_t dt_ms = last_update_ms != 0 && now_ms > last_update_ms
                ? std::clamp<uint64_t>(now_ms - last_update_ms, 1, 100)
                : 11;
            last_update_ms = now_ms;
            const double alpha = smoothing_ms <= 0.0
                ? 1.0
                : 1.0 - std::exp(-double(dt_ms) / smoothing_ms);

            if (!seeded) {
                for (int i = 0; i < 2; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        rx[i][j] = gx[i];
                        ry[i][j] = gy[i];
                    }
                    ri[i] = 0;
                    fx[i] = gx[i];
                    fy[i] = gy[i];
                    cx[i] = gx[i];
                    cy[i] = gy[i];
                }
                seeded = true;
                following = false;
                return;
            }

            double mx[2]{}, my[2]{};
            for (int i = 0; i < 2; ++i) {
                rx[i][ri[i]] = gx[i];
                ry[i][ri[i]] = gy[i];
                ri[i] = (ri[i] + 1) % 3;

                mx[i] = median3(rx[i][0], rx[i][1], rx[i][2]);
                my[i] = median3(ry[i][0], ry[i][1], ry[i][2]);
                fx[i] += alpha * (mx[i] - fx[i]);
                fy[i] += alpha * (my[i] - fy[i]);
            }

            bool move = false;
            for (int i = 0; i < 2; ++i) {
                if (std::hypot(fx[i] - cx[i], fy[i] - cy[i]) > dead_zone) {
                    move = true;
                }
            }

            if (move) {
                following = true;
            }

            if (following) {
                bool settled = true;
                for (int i = 0; i < 2; ++i) {
                    // The window goes straight to the filtered gaze, which is
                    // already smooth: it never trails behind a saccade.
                    cx[i] = fx[i];
                    cy[i] = fy[i];
                    if (std::hypot(mx[i] - fx[i], my[i] - fy[i]) > SETTLED) {
                        settled = false;
                    }
                }
                if (settled) {
                    following = false;
                }
            }
        } else if (seeded && now_ms - last_valid_ms > return_ms) {
            // Held long enough: ease from where the window was to the centre.
            if (!easing) {
                for (int i = 0; i < 2; ++i) {
                    lx[i] = cx[i];
                    ly[i] = cy[i];
                }
                easing = true;
                following = false;
            }

            const double t = std::clamp(double(now_ms - last_valid_ms - return_ms) / double(EASE_MS), 0.0, 1.0);
            const double keep = 1.0 - t * t * (3.0 - 2.0 * t);
            for (int i = 0; i < 2; ++i) {
                cx[i] = lx[i] * keep;
                cy[i] = ly[i] * keep;
            }

            if (t >= 1.0) {
                for (int i = 0; i < 2; ++i) {
                    cx[i] = 0;
                    cy[i] = 0;
                    fx[i] = 0;
                    fy[i] = 0;
                    ri[i] = 0;
                }
                seeded = false;
                easing = false;
            }
        }
    }
};

// --- Eye tracking ------------------------------------------------------------
// XR_EXT_eye_gaze_interaction gives one gaze pose (in the head's view space)
// whose -Z axis is the gaze direction. Each eye's view is rotated a little
// against the head (canting), so the direction is turned into that eye's
// frame before it becomes a tangent-space point in the eye's image.

struct Quat {
    double x{0}, y{0}, z{0}, w{1};
};

inline void rotate(const Quat& q, const double v[3], double out[3]) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    const double tx = 2 * (q.y * v[2] - q.z * v[1]);
    const double ty = 2 * (q.z * v[0] - q.x * v[2]);
    const double tz = 2 * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

inline Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Gaze direction, seen from one eye, as a point in that eye's tangent space
// (x right, y up, (0, 0) straight ahead). False when the gaze points sideways
// or backwards relative to the eye (more than ~84 degrees off axis).
inline bool gaze_to_eye_tangent(const Quat& gaze_in_head, const Quat& eye_in_head, double& tx, double& ty) {
    const double fwd[3]{0, 0, -1};
    double d_head[3], d_eye[3];
    rotate(gaze_in_head, fwd, d_head);
    rotate(conjugate(eye_in_head), d_head, d_eye);
    const double depth = -d_eye[2];
    if (!(depth > 0.1) || !std::isfinite(d_eye[0]) || !std::isfinite(d_eye[1])) return false;
    tx = d_eye[0] / depth;
    ty = d_eye[1] / depth;
    return true;
}

// --- Wide-view LOD ------------------------------------------------------------
// Unreal picks mesh LODs per view from FSceneViewInitOptions::LODDistanceFactor
// (1.0 by default; larger = treated as farther = simpler meshes). Its offset
// moves between engine versions, so it is found at runtime from the fields
// around it, which have fixed defaults:
//   FIntPoint CursorPos;                     // (-1, -1) or the mouse position
//   float     LODDistanceFactor;             // ~1.0
//   float     OverrideFarClippingPlaneDistance; // -1.0 unless a camera overrides it
// The match must be unique in the scanned range, and the offset is only
// trusted after it has been the same for many views in a row.

inline bool lod_candidate_at(const uint8_t* p, size_t size, size_t o) {
    if (o < 8 || o + 8 > size) return false;
    int32_t cx, cy;
    float lod, far_plane;
    std::memcpy(&cx, p + o - 8, 4);
    std::memcpy(&cy, p + o - 4, 4);
    std::memcpy(&lod, p + o, 4);
    std::memcpy(&far_plane, p + o + 4, 4);
    const bool cursor_ok = cx >= -1 && cy >= -1 && cx <= 32768 && cy <= 32768;
    return cursor_ok && std::isfinite(lod) && lod >= 0.01f && lod <= 100.0f && far_plane == -1.0f;
}

// The cursor is (-1, -1) whenever the view has no mouse over it, which is the
// normal case in VR.
inline bool lod_cursor_unset_at(const uint8_t* p, size_t o) {
    int32_t cx, cy;
    std::memcpy(&cx, p + o - 8, 4);
    std::memcpy(&cy, p + o - 4, 4);
    return cx == -1 && cy == -1;
}

// One match wins. Several matches (the scan runs past the end of the smaller
// UE4 struct into unrelated memory, where zeros followed by -1.0 also fit)
// are settled only by a stricter test: exactly one of them has the unset
// cursor in front of it. Anything else is refused rather than guessed.
inline std::optional<size_t> find_lod_factor_offset(const uint8_t* p, size_t size, size_t begin = 0x40) {
    std::optional<size_t> loose{}, strict{};
    uint32_t loose_count = 0, strict_count = 0;
    for (size_t o = (begin + 3) & ~size_t(3); o + 8 <= size; o += 4) {
        if (!lod_candidate_at(p, size, o)) continue;
        if (loose_count++ == 0) loose = o;
        if (lod_cursor_unset_at(p, o) && strict_count++ == 0) strict = o;
    }
    if (loose_count == 1) return loose;
    if (strict_count == 1) return strict;
    return std::nullopt;
}

// Diagnostics: every offset in the range that looks like the LOD factor
// (up to `max`), for the log when the unique match fails.
inline size_t list_lod_candidates(const uint8_t* p, size_t size, size_t* out, size_t max, size_t begin = 0x40) {
    size_t n = 0;
    for (size_t o = (begin + 3) & ~size_t(3); o + 8 <= size; o += 4) {
        if (!lod_candidate_at(p, size, o)) continue;
        if (n < max) out[n] = o;
        ++n;
    }
    return n;
}

struct LodOffsetLatch {
    static constexpr uint32_t NEEDED = 60;
    std::optional<size_t> offset{};
    size_t candidate{0};
    uint32_t streak{0};
    uint32_t misses{0};
    bool failed{false};

    // Feed one view's init options; returns the offset once it is trusted.
    std::optional<size_t> observe(const uint8_t* p, size_t size) {
        if (failed) return std::nullopt;
        if (offset) {
            if (lod_candidate_at(p, size, *offset)) return offset;
            if (++misses > 30) { offset.reset(); failed = true; }  // layout changed under us
            return std::nullopt;
        }
        const auto o = find_lod_factor_offset(p, size);
        if (!o) { streak = 0; if (++misses > 600) failed = true; return std::nullopt; }
        if (*o != candidate) { candidate = *o; streak = 0; }
        if (++streak >= NEEDED) { offset = candidate; misses = 0; }
        return offset;
    }
};

// --- Per-frame state and projection ------------------------------------------

// Everything that must agree between the game thread (view rects and
// projections) and the render thread (compose and submit) for one frame: the
// atlas layout and the two sharp windows. It is captured once, with the frame's
// pose sample, and both threads look it up by that frame's number, so a slider
// drag or a gaze update between render and compose can never split them.
struct FrameSnapshot {
    Layout layout{};
    Tangents focus[2]{};
};

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

// --- Composition --------------------------------------------------------------
// The compositor turns the atlas back into a plain stereo image: each eye's
// wide view stretched over the whole eye image, its sharp view on top where the
// focus window lies. Rows go down while tangents go up, hence the flip in y.
struct RectF {
    double x{0}, y{0}, w{0}, h{0};
};

inline RectF focus_dest_rect(const Tangents& rendered, const Tangents& focus, double eye_x0, double eye_w, double eye_h) {
    const double sx = eye_w / rendered.width();
    const double sy = eye_h / rendered.height();
    return {eye_x0 + (focus.left - rendered.left) * sx, (rendered.up - focus.up) * sy, focus.width() * sx, focus.height() * sy};
}

// Alpha of the sharp view at a point of its own image (u, v in 0..1): 1 inside,
// easing to 0 over the outer `blend` share of each side (smoothstep). This is
// what the compose pixel shader computes; kept here so it is tested.
inline double edge_alpha(double u, double v, double blend) {
    const double d = (std::min)((std::min)(u, 1.0 - u), (std::min)(v, 1.0 - v));
    if (d <= 0) return 0.0;
    if (blend <= 0) return 1.0;
    const double t = std::clamp(d / blend, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Size in wide-view pixels of an object with bounding radius `radius` seen
// from `distance` (same units): it spans about 2r/d tangent units. Inside the
// bounds counts as huge, so nothing around the camera is ever culled.
inline double wide_pixel_size(double radius, double distance, double px_per_tangent) {
    if (!(distance > radius)) return 1e9;
    return 2.0 * radius / distance * px_per_tangent;
}

// Shape of the sharp area as the compositor blends it (the sharp view itself
// is always rendered as a rectangle).
enum class FocusShape : int { RECTANGLE = 0, ROUNDED = 1, ELLIPSE = 2 };

// Distance from (u, v) (0..1 across the sharp sprite) to the shape's edge, as
// a share of the sprite size: 0 on the edge, 0.5 in the centre, negative
// outside. Mirrors quad_view_compose.fx.
inline double shape_edge_share(FocusShape shape, double u, double v) {
    const double px = std::abs(u - 0.5) * 2.0; // 0 centre .. 1 sprite edge
    const double py = std::abs(v - 0.5) * 2.0;
    switch (shape) {
    case FocusShape::ELLIPSE:
        return (1.0 - std::sqrt(px * px + py * py)) * 0.5;
    case FocusShape::ROUNDED: {
        constexpr double r = 0.5; // corner radius, share of the half size
        const double qx = px - (1.0 - r);
        const double qy = py - (1.0 - r);
        const double ox = (std::max)(qx, 0.0);
        const double oy = (std::max)(qy, 0.0);
        const double d = std::sqrt(ox * ox + oy * oy) + (std::min)((std::max)(qx, qy), 0.0) - r;
        return -d * 0.5;
    }
    default:
        return (std::min)(1.0 - px, 1.0 - py) * 0.5;
    }
}

inline double shape_alpha(FocusShape shape, double u, double v, double blend) {
    const double d = shape_edge_share(shape, u, v);
    if (d <= 0) return 0.0;
    if (blend <= 0) return 1.0;
    const double t = std::clamp(d / blend, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// The compositors pass each sprite's settings through SpriteBatch's layer
// depth (see quad_view_compose.hlsli):
//   (code + blend * 2) / 128, code = base + 8 * sharpen level
//   base 0..5 = shape * 2 + outline (sharp sprite), 6 = wide sprite (bicubic)
//   sharpen level 0..10 in tenths, blend share 0..0.5
inline constexpr float SPRITE_PACK_SCALE = 128.0f;
inline constexpr int SPRITE_WIDE_BASE = 6;

struct SpriteSettings {
    FocusShape shape{FocusShape::RECTANGLE};
    bool outline{false};
    float blend{0.0f};
    bool wide{false};     // wide view: bicubic upscale, opaque
    float sharpen{0.0f};  // 0..1, in steps of 0.1
};

inline float pack_sprite_settings(const SpriteSettings& s) {
    const int level = (int)std::lround(std::clamp(s.sharpen, 0.0f, 1.0f) * 10.0f);
    const int base = s.wide ? SPRITE_WIDE_BASE : (int)s.shape * 2 + (s.outline ? 1 : 0);
    const float b = s.wide ? 0.0f : std::clamp(s.blend, 0.0f, 0.5f) * 2.0f * 0.998f;
    return ((float)(base + 8 * level) + b) / SPRITE_PACK_SCALE;
}

inline float pack_sprite_settings(FocusShape shape, bool outline, float blend) {
    return pack_sprite_settings(SpriteSettings{shape, outline, blend, false, 0.0f});
}

inline SpriteSettings unpack_sprite_settings(float packed) {
    const float v = packed * SPRITE_PACK_SCALE;
    const float code = std::floor(v + 0.0005f);
    SpriteSettings s{};
    s.blend = std::clamp(v - code, 0.0f, 1.0f) * 0.5f;
    const int c = (int)code;
    const int level = c / 8;
    const int base = c % 8;
    s.sharpen = (float)level * 0.1f;
    s.wide = base == SPRITE_WIDE_BASE;
    s.outline = !s.wide && (base % 2) != 0;
    s.shape = s.wide ? FocusShape::RECTANGLE : (FocusShape)(base / 2);
    return s;
}

// Share of the eye's pixels the four views cost, versus plain stereo at
// native resolution (for the log and the settings UI).
inline double pixel_share(const Layout& l, uint32_t eye_w, uint32_t eye_h) {
    double sum = 0;
    for (const auto& r : l.views) sum += double(r.w) * r.h;
    return sum / (2.0 * eye_w * eye_h);
}

} // namespace uevr::quadview
