#include "mods/vr/QuadView.hpp"

#include <cmath>
#include <cstring>
#include <vector>
#include <iostream>
#include <stdexcept>

namespace q = uevr::quadview;

static void expect(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

static void slot_tests() {
    // Zero-based: 0 wide L, 1 wide R, 2 focus L, 3 focus R.
    for (int32_t i = 0; i < 4; ++i) {
        const auto s = q::slot_for_view(i, false);
        expect(s.index() == uint32_t(i), "zero-based view index must map straight to its slot");
    }
    // One-based (UE passes 1..4): same slots shifted by one.
    for (int32_t i = 1; i <= 4; ++i) {
        expect(q::slot_for_view(i, true).index() == uint32_t(i - 1), "one-based view index must shift by one");
    }
    expect(q::slot_for_view(2, false).eye == 0 && q::slot_for_view(2, false).focus, "view 2 is the left focus view");
    expect(q::slot_for_view(-1, false).index() == 3, "negative indices must wrap, never index out of range");
    expect(q::is_primary_view(0) && !q::is_primary_view(1) && !q::is_primary_view(2), "only view 0 is primary");
}

static void layout_tests() {
    q::Settings s{};
    s.wide_scale = 0.5; s.focus_width = 0.3; s.focus_height = 0.3; s.focus_density = 1.0;
    const auto l = q::make_layout(s, 3840, 3552);
    expect(l.views[0].w == 1920 && l.views[0].h == 1776, "wide view is wide_scale of the eye");
    expect(l.views[2].w == 1152 && l.views[2].h == 1066, "focus view is its share of the eye at density 1");
    expect(l.views[1].x == l.views[0].w && l.views[3].x == l.views[2].w, "pairs sit side by side");
    expect(l.views[2].y == l.views[0].h, "focus pair sits under the wide pair");
    expect(l.atlas_w == 3840 && l.atlas_h == 1776 + 1066, "atlas holds both rows");
    for (const auto& r : l.views) {
        expect(r.x + r.w <= l.atlas_w && r.y + r.h <= l.atlas_h, "every view fits in the atlas");
        expect(r.w % 2 == 0 && r.h % 2 == 0, "sizes are even");
    }
    const double share = q::pixel_share(l, 3840, 3552);
    expect(share > 0.33 && share < 0.35, "default quad view costs about a third of native stereo pixels");

    s.focus_density = 2.0; s.focus_width = 1.0; s.wide_scale = 0.1;
    const auto wide_focus = q::make_layout(s, 1000, 1000);
    expect(wide_focus.atlas_w == 2 * wide_focus.views[2].w, "atlas widens when the focus row is the wider one");
}

static void window_tests() {
    const q::Tangents full{-1.0, 1.0, 1.0, -1.0};
    auto w = q::focus_window(full, 0.3, 0.3, 0.0, 0.0);
    expect(near(w.left, -0.3) && near(w.right, 0.3) && near(w.up, 0.3) && near(w.down, -0.3), "centred window");
    w = q::focus_window(full, 0.3, 0.3, 0.95, -0.95);
    expect(near(w.right, 1.0) && near(w.down, -1.0), "window slides back inside at the edge");
    expect(near(w.width(), 0.6) && near(w.height(), 0.6), "sliding keeps the window size");
    w = q::focus_window(full, 0.3, 0.3, NAN, INFINITY);
    expect(near(w.left, -0.3) && near(w.up, 0.3), "non-finite gaze falls back to centre");
    const q::Tangents asym{-1.2, 0.8, 1.0, -1.1};
    w = q::focus_window(asym, 0.25, 0.25, 0.0, 0.0);
    expect(w.left >= asym.left && w.right <= asym.right && w.down >= asym.down && w.up <= asym.up,
           "window stays inside an asymmetric eye field of view");
}

static void gaze_tests() {
    // 15 ms smoothing at a 90 Hz frame rate (11 ms between updates).
    q::GazeFilter g{};
    const double a[2]{0.10, 0.12}, b[2]{-0.05, -0.04};
    g.update(a, b, 3u, 0.0, 15.0, 1000, 500);
    expect(g.seeded && near(g.cx[0], 0.10) && near(g.cy[1], -0.04), "first valid gaze seeds both eyes");

    // Alternating tracker noise is averaged, so the window stays on the fixation.
    for (int i = 0; i < 30; ++i) {
        const double n[2]{0.10 + (i % 2 ? 0.012 : -0.012), 0.12 + (i % 2 ? -0.010 : 0.010)};
        const double m[2]{-0.05 + (i % 2 ? 0.010 : -0.010), -0.04 + (i % 2 ? -0.012 : 0.012)};
        g.update(n, m, 3u, 0.0, 15.0, 1010 + (uint64_t)i * 10, 500);
    }
    expect(near(g.cx[0], 0.10, 0.006) && near(g.cy[1], -0.04, 0.006),
        "smoothing averages tracker noise and the window stays on the fixation");

    // A real change is median-filtered for one sample, then arrives within a
    // few frames instead of trailing the gaze.
    const double c[2]{0.20, 0.22}, d[2]{-0.15, -0.14};
    g.update(c, d, 3u, 0.0, 15.0, 1400, 500);
    expect(near(g.cx[0], 0.10, 0.02), "the first step sample is filtered");
    for (int i = 1; i <= 4; ++i) {
        g.update(c, d, 3u, 0.0, 15.0, 1400 + (uint64_t)i * 10, 500);
    }
    expect(g.cx[0] > 0.18 && g.cx[0] <= 0.20, "a real gaze change reaches the window within a few frames");
    expect(g.cx[1] > 0.20 && g.cx[1] <= 0.22, "the other eye follows too");
    expect(g.cy[1] < -0.13 && g.cy[1] >= -0.14, "and the vertical axis follows");

    // The smoothing time is the user-facing knob: 0 ms follows at once, a
    // longer time arrives later.
    q::GazeFilter fast{}, slow{};
    const double e[2]{0.30, 0.30}, f2[2]{0.30, 0.30};
    fast.update(a, b, 3u, 0.0, 0.0, 0, 500);
    slow.update(a, b, 3u, 0.0, 50.0, 0, 500);
    fast.update(e, f2, 3u, 0.0, 0.0, 11, 500);
    slow.update(e, f2, 3u, 0.0, 50.0, 11, 500);
    fast.update(e, f2, 3u, 0.0, 0.0, 22, 500);
    slow.update(e, f2, 3u, 0.0, 50.0, 22, 500);
    expect(fast.cx[0] > slow.cx[0] + 0.05, "a shorter smoothing time follows sooner");

    // An optional dead zone holds the window until the gaze has moved far enough.
    q::GazeFilter hold{};
    hold.update(a, b, 3u, 0.05, 0.0, 0, 500);
    const double small[2]{0.13, 0.14}, small_v[2]{-0.05, -0.04};
    hold.update(small, small_v, 3u, 0.05, 0.0, 11, 500);
    hold.update(small, small_v, 3u, 0.05, 0.0, 22, 500);
    expect(near(hold.cx[0], 0.10, 0.001), "a dead zone holds small movements");
    const double wide[2]{0.30, 0.30}, wide_v[2]{0.10, 0.10};
    hold.update(wide, wide_v, 3u, 0.05, 0.0, 33, 500);
    hold.update(wide, wide_v, 3u, 0.05, 0.0, 44, 500);
    expect(hold.cx[0] > 0.25, "beyond the dead zone the window follows");

    // A one-frame spike never reaches the filter, whatever the smoothing.
    q::GazeFilter spike{};
    spike.update(a, b, 3u, 0.0, 15.0, 0, 500);
    for (int i = 0; i < 20; ++i) {
        const double sx[2]{(i == 10) ? 0.4 : 0.10, 0.12};
        spike.update(sx, b, 3u, 0.0, 15.0, 11 + (uint64_t)i * 11, 500);
    }
    expect(near(spike.cx[0], 0.10, 0.005), "a one-frame gaze spike is rejected");

    g.update(c, d, 1u, 0.0, 15.0, 1700, 500);
    expect(g.seeded, "a short gaze dropout keeps the window");
    g.update(c, d, 0u, 0.0, 15.0, 2300, 500);
    expect(!g.seeded && near(g.cx[0], 0.0) && near(g.cy[1], 0.0), "a long dropout recentres");

    // Losing the gaze holds the window, then eases it to the centre instead of
    // jumping there.
    q::GazeFilter ease{};
    const double far_x[2]{0.30, 0.32}, far_y[2]{0.20, 0.20};
    ease.update(far_x, far_y, 3u, 0.0, 0.0, 1000, 500);
    ease.update(far_x, far_y, 0u, 0.0, 0.0, 1400, 500);
    expect(near(ease.cx[0], 0.30), "a short loss (a blink) holds the window where it was");
    ease.update(far_x, far_y, 0u, 0.0, 0.0, 1575, 500); // half way through the ease
    expect(ease.cx[0] > 0.05 && ease.cx[0] < 0.25 && ease.easing, "after the hold the window eases toward the centre");
    const double back_x[2]{-0.10, -0.08}, back_y[2]{0.0, 0.0};
    ease.update(back_x, back_y, 3u, 0.0, 0.0, 1580, 500);
    expect(near(ease.cx[0], -0.10) && !ease.easing, "gaze returning mid-ease starts again where the eyes are");
    ease.update(back_x, back_y, 0u, 0.0, 0.0, 2081, 500);
    ease.update(back_x, back_y, 0u, 0.0, 0.0, 2300, 500);
    expect(!ease.seeded && near(ease.cx[0], 0.0), "the ease ends exactly at the centre");
}

static void fit_tests() {
    q::Settings s{};
    s.wide_scale = 1.0; s.focus_width = 0.4; s.focus_height = 0.4; s.focus_density = 2.0;
    const auto raw = q::make_layout(s, 2000, 2000);
    expect(raw.atlas_h > 2000, "this layout overflows one eye height");
    const auto fit = q::make_fitted_layout(s, 2000, 2000);
    expect(fit.atlas_w <= 4000 && fit.atlas_h <= 2000, "fitted layout fits the two-eye render target");
    for (const auto& r : fit.views) {
        expect(r.x + r.w <= fit.atlas_w && r.y + r.h <= fit.atlas_h, "fitted views stay in the atlas");
        expect(r.w % 2 == 0 && r.h % 2 == 0, "fitted sizes are even");
    }
    const double before = double(raw.views[2].w) / raw.views[0].w;
    const double after = double(fit.views[2].w) / fit.views[0].w;
    expect(std::fabs(before - after) < 0.02, "fitting keeps the focus/wide density ratio");
    s = q::Settings{};
    const auto small = q::make_fitted_layout(s, 3840, 3552);
    expect(small.atlas_w == q::make_layout(s, 3840, 3552).atlas_w, "a layout that fits is left alone");
}

static void projection_tests() {
    const q::Tangents t{-0.4, 0.2, 0.3, -0.1};
    float m[16];
    q::projection_from_tangents(t, 10.0f, m);
    // clip = [x y z 1] * M with rows read from memory order.
    auto ndc = [&](double x, double y, double z, double& nx, double& ny, double& nz) {
        const double cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const double cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const double cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        const double cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        nx = cx / cw; ny = cy / cw; nz = cz / cw;
    };
    double nx, ny, nz;
    ndc(t.right * 5, t.up * 5, 5, nx, ny, nz);
    expect(near(nx, 1.0, 1e-6) && near(ny, 1.0, 1e-6), "right/up tangent edge lands on +1 NDC");
    ndc(t.left * 5, t.down * 5, 5, nx, ny, nz);
    expect(near(nx, -1.0, 1e-6) && near(ny, -1.0, 1e-6), "left/down tangent edge lands on -1 NDC");
    ndc(0, 0, 10, nx, ny, nz);
    expect(near(nz, 1.0, 1e-6), "reversed Z: the near plane maps to depth 1");
    const auto a = q::to_angles(t);
    expect(near(std::tan(a.left), t.left, 1e-6) && near(std::tan(a.down), t.down, 1e-6), "angles round-trip");
}

static void gaze_direction_tests() {
    double tx = 9, ty = 9;
    expect(q::gaze_to_eye_tangent({}, {}, tx, ty) && near(tx, 0) && near(ty, 0), "straight gaze is the centre");
    // Gaze turned 20 degrees right: rotation of -20 degrees about +Y (OpenXR: +Y up, -Z forward).
    const double a = -20.0 * 3.14159265358979 / 180.0;
    const q::Quat right{0, std::sin(a / 2), 0, std::cos(a / 2)};
    expect(q::gaze_to_eye_tangent(right, {}, tx, ty) && near(tx, std::tan(20.0 * 3.14159265358979 / 180.0), 1e-9) && near(ty, 0),
           "gaze 20 degrees right is tan(20) right");
    // An eye canted 20 degrees right sees that same gaze straight ahead.
    expect(q::gaze_to_eye_tangent(right, right, tx, ty) && near(tx, 0, 1e-9), "canted eye removes its own rotation");
    // Gaze up 10 degrees: rotation of +10 degrees about +X.
    const double b = 10.0 * 3.14159265358979 / 180.0;
    const q::Quat up{std::sin(b / 2), 0, 0, std::cos(b / 2)};
    expect(q::gaze_to_eye_tangent(up, {}, tx, ty) && near(ty, std::tan(b), 1e-9), "gaze up is positive y");
    const q::Quat back{0, 1, 0, 0};
    expect(!q::gaze_to_eye_tangent(back, {}, tx, ty), "gaze pointing backwards is rejected");
}

static void put_i(std::vector<uint8_t>& b, size_t o, int32_t v) { std::memcpy(b.data() + o, &v, 4); }
static void put_f(std::vector<uint8_t>& b, size_t o, float v) { std::memcpy(b.data() + o, &v, 4); }

static void lod_offset_tests() {
    std::vector<uint8_t> b(0x400, 0);
    put_i(b, 0x280, -1); put_i(b, 0x284, -1); put_f(b, 0x288, 1.0f); put_f(b, 0x28C, -1.0f);
    auto o = q::find_lod_factor_offset(b.data(), b.size());
    expect(o && *o == 0x288, "LODDistanceFactor found after the cursor and before the far-plane override");
    put_i(b, 0x280, 812); put_i(b, 0x284, 455);
    o = q::find_lod_factor_offset(b.data(), b.size());
    expect(o && *o == 0x288, "a real mouse position still matches");
    put_f(b, 0x30C, 1.0f); put_f(b, 0x310, -1.0f);
    expect(!q::find_lod_factor_offset(b.data(), b.size()), "two matches are refused");
    put_f(b, 0x310, 0.0f);
    put_f(b, 0x28C, 5000.0f);
    expect(!q::find_lod_factor_offset(b.data(), b.size()), "an overridden far plane gives no match");
    put_f(b, 0x28C, -1.0f);

    q::LodOffsetLatch latch{};
    std::optional<size_t> got{};
    for (uint32_t i = 0; i < q::LodOffsetLatch::NEEDED - 1; ++i) got = latch.observe(b.data(), b.size());
    expect(!got, "not trusted before the streak completes");
    got = latch.observe(b.data(), b.size());
    expect(got && *got == 0x288, "trusted after a full streak");
    put_f(b, 0x288, 2.0f);  // we scaled it on an earlier view: still the same field
    expect(latch.observe(b.data(), b.size()).has_value(), "a scaled factor is still recognised");
}

static void compose_tests() {
    const q::Tangents rendered{-1.0, 1.0, 1.0, -1.0};
    const q::Tangents centre{-0.3, 0.3, 0.3, -0.3};
    auto r = q::focus_dest_rect(rendered, centre, 0, 2000, 2000);
    expect(near(r.x, 700) && near(r.y, 700) && near(r.w, 600) && near(r.h, 600), "centred window lands in the middle");
    const q::Tangents upper_right{0.4, 1.0, 1.0, 0.4};
    r = q::focus_dest_rect(rendered, upper_right, 2000, 2000, 2000);
    expect(near(r.x, 3400) && near(r.y, 0), "up-right window lands top-right of the right eye image");
    expect(near(q::edge_alpha(0.5, 0.5, 0.2), 1.0), "the middle is fully sharp");
    expect(near(q::edge_alpha(0.0, 0.5, 0.2), 0.0), "the edge is fully wide");
    expect(near(q::edge_alpha(0.1, 0.5, 0.2), 0.5), "half way through the blend band is half");
    expect(near(q::edge_alpha(0.01, 0.5, 0.0), 1.0), "no blend gives a hard edge");
}

static void order_tests() {
    const auto a = q::slot_for_view(0, false, true);
    const auto b = q::slot_for_view(1, false, true);
    const auto c = q::slot_for_view(2, false, true);
    const auto d = q::slot_for_view(3, false, true);
    expect(a.focus && a.eye == 0 && b.focus && b.eye == 1, "sharp first: views 0/1 are the sharp pair");
    expect(!c.focus && c.eye == 0 && !d.focus && d.eye == 1, "sharp first: views 2/3 are the wide pair");
    expect(c.index() == 0 && a.index() == 2, "slot indices stay semantic (atlas, LOD, culling)");
}

static void offset_tests() {
    const q::Tangents full{-1.0, 1.0, 1.0, -1.0};
    q::Settings s{};
    s.focus_width = 0.3; s.focus_height = 0.3;
    auto w = q::focus_window_for(full, s, 0.0, 0.0);
    expect(near(w.left, -0.3) && near(w.right, 0.3), "no offset keeps the window centred");
    s.offset_x = 0.1; s.offset_y = -0.05;
    w = q::focus_window_for(full, s, 0.0, 0.0);
    expect(near((w.left + w.right) / 2, 0.2) && near((w.up + w.down) / 2, -0.1), "offset moves the window right and down");
    s.offset_x = 0.25;
    w = q::focus_window_for(full, s, 0.9, 0.0);
    expect(near(w.right, 1.0), "an offset window still stays inside the view");
}

static void call_order_tests() {
    q::CallOrder o{};
    // A stereo-pass call before the viewport draw (epoch 1) ...
    expect(o.next(1, 1) == 0, "an early call is counted in its own epoch");
    // ... must not shift the views of the draw (epoch 2).
    for (int32_t i = 0; i < 4; ++i) {
        expect(o.next(1 + (i % 2), 2) == i, "views are numbered by call order within the draw");
    }
    expect(o.next(1, 2) == -1, "calls after the fourth view are not views (HUD projections)");
    expect(o.next(0, 3) == -1, "pass 0 (full/mono) is never a view");
    expect(o.next(5, 3) == -1, "a pass beyond four is never a view");
    expect(o.next(2, 3) == 0, "the next draw starts again at view 0");
}

static void culling_tests() {
    // 10 cm object, 50 m away, 1000 px per tangent unit -> 2 px
    expect(near(q::wide_pixel_size(5.0, 5000.0, 1000.0), 2.0), "pixel size of a small far object");
    expect(q::wide_pixel_size(50.0, 20.0, 1000.0) > 1e6, "standing inside the bounds never culls");
}

static void shape_tests() {
    using S = q::FocusShape;
    for (double u : {0.05, 0.1, 0.3, 0.5}) {
        expect(near(q::shape_alpha(S::RECTANGLE, u, 0.5, 0.2), q::edge_alpha(u, 0.5, 0.2)), "rectangle matches the old edge fade");
    }
    expect(near(q::shape_alpha(S::ELLIPSE, 0.5, 0.5, 0.2), 1.0), "ellipse centre is sharp");
    expect(near(q::shape_alpha(S::ELLIPSE, 0.05, 0.05, 0.2), 0.0), "ellipse corner is wide");
    expect(q::shape_alpha(S::ELLIPSE, 0.05, 0.5, 0.2) > 0.0, "ellipse side is inside the shape");
    expect(near(q::shape_alpha(S::ROUNDED, 0.02, 0.02, 0.2), 0.0), "rounded corner is cut");
    expect(near(q::shape_alpha(S::ROUNDED, 0.5, 0.5, 0.2), 1.0), "rounded centre is sharp");
    expect(near(q::shape_alpha(S::ROUNDED, 0.1, 0.5, 0.2), q::edge_alpha(0.1, 0.5, 0.2)), "rounded sides fade like the rectangle");

    for (int shape = 0; shape < 3; ++shape) {
        for (bool outline : {false, true}) {
            for (float blend : {0.0f, 0.1f, 0.25f, 0.5f}) {
                const auto u = q::unpack_sprite_settings(q::pack_sprite_settings((S)shape, outline, blend));
                expect((int)u.shape == shape && u.outline == outline && std::abs(u.blend - blend) < 0.002f,
                    "sprite settings survive the layer depth packing");
                const float packed = q::pack_sprite_settings((S)shape, outline, blend);
                expect(packed >= 0.0f && packed < 1.0f, "packed settings stay in the depth range");

                for (float sharpen : {0.0f, 0.3f, 0.7f, 1.0f}) {
                    const auto s = q::unpack_sprite_settings(q::pack_sprite_settings(
                        q::SpriteSettings{(S)shape, outline, blend, false, sharpen}));
                    expect((int)s.shape == shape && s.outline == outline && !s.wide &&
                        std::abs(s.blend - blend) < 0.002f && std::abs(s.sharpen - sharpen) < 0.01f,
                        "sharpening survives the packing with the other settings");
                    const float p = q::pack_sprite_settings(q::SpriteSettings{(S)shape, outline, blend, false, sharpen});
                    expect(p >= 0.0f && p < 1.0f, "packed settings with sharpening stay in the depth range");
                }
            }
        }
    }

    const auto wide = q::unpack_sprite_settings(q::pack_sprite_settings(q::SpriteSettings{S::ELLIPSE, true, 0.3f, true, 0.5f}));
    expect(wide.wide && !wide.outline && wide.shape == S::RECTANGLE && wide.blend == 0.0f,
        "a wide sprite ignores the sharp-area shape, outline and blend");
}

int main() {
    try {
        slot_tests();
        layout_tests();
        window_tests();
        gaze_tests();
        fit_tests();
        projection_tests();
        gaze_direction_tests();
        lod_offset_tests();
        compose_tests();
        shape_tests();
        offset_tests();
        order_tests();
        call_order_tests();
        culling_tests();
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    std::cout << "quad-view-tests: all passed\n";
    return 0;
}
