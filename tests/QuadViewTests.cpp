#include "mods/vr/QuadView.hpp"

#include <cmath>
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
    q::GazeFilter g{};
    const double a[2]{0.10, 0.12}, b[2]{-0.05, -0.04};
    g.update(a, b, 3u, 0.03, 1000, 500);
    expect(g.seeded && near(g.cx[0], 0.10) && near(g.cy[1], -0.04), "first valid gaze seeds both eyes");
    const double a2[2]{0.11, 0.13};
    g.update(a2, b, 3u, 0.03, 1010, 500);
    expect(near(g.cx[0], 0.10), "movement inside the dead zone is ignored");
    const double a3[2]{0.10, 0.20};
    g.update(a3, b, 3u, 0.03, 1020, 500);
    expect(near(g.cx[0], 0.10) && near(g.cx[1], 0.20), "either eye leaving the dead zone moves both");
    g.update(a3, b, 1u, 0.03, 1400, 500);
    expect(g.seeded, "a short gaze dropout keeps the window");
    g.update(a3, b, 0u, 0.03, 1600, 500);
    expect(!g.seeded && near(g.cx[0], 0.0) && near(g.cy[1], 0.0), "a long dropout recentres");
}

static void frame_tests() {
    q::Settings s{};
    const q::Tangents eyes[2]{{-1.1, 0.9, 1.0, -1.0}, {-0.9, 1.1, 1.0, -1.0}};
    q::GazeFilter g{};
    const auto f = q::make_frame_views(42, eyes, g, s);
    expect(f.pose_id == 42, "all views carry the pose sample they were built from");
    for (uint32_t eye = 0; eye < 2; ++eye) {
        expect(near(f.tangents[eye].left, eyes[eye].left), "wide views keep the full eye field of view");
        const auto& w = f.tangents[eye + 2];
        expect(w.left >= eyes[eye].left && w.right <= eyes[eye].right, "focus view lies inside its eye");
    }
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

int main() {
    try {
        slot_tests();
        layout_tests();
        window_tests();
        gaze_tests();
        frame_tests();
        fit_tests();
        projection_tests();
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    std::cout << "quad-view-tests: all passed\n";
    return 0;
}
