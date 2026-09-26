#include "mods/vr/MonoRenderingPolicy.hpp"
#include <limits>
#include <thread>

void test_mono_rendering() {
    namespace m = uevr::mono;
    namespace p = uevr::vr_compatibility;
    static_assert(static_cast<int>(p::RenderingMethod::Mono) == m::method_id);
    for (int id = 0; id != 6; ++id) {
        expect(m::available(id), "DIBR build retains every serialized rendering ID");
        expect(m::available(id, m::non_dibr_choices) == (id < 3 || id == 5),
            "non-DIBR Mono remains ID 5; gaps are not exposed or renumbered");
    }
    expect(!m::available(-1) && !m::available(6), "unsupported IDs are identified, never clamped into Mono");
    for (int id = 0; id != 5; ++id) {
        for (bool extreme : {false, true}) {
            expect(p::is_using_afr(static_cast<p::RenderingMethod>(id), extreme) == (id == 1 || id == 2 || extreme),
                "every historical rendering ID keeps its AFR truth table");
        }
        m::Selection s;
        s.initialize(id);
        expect(s.active() == id && s.generation() == 0, "old profile initializes without Mono machinery");
        s.request(5);
        expect(s.requested() == 5 && s.active() == id && !s.commit_at_game_frame_boundary(),
            "live Mono request keeps the previous renderer until GPU retirement");
        s.consumers_retired(s.request_token());
        expect(s.commit_at_game_frame_boundary() && s.active() == 5 && s.generation() == 1,
            "Mono transaction commits only after retirement and game-frame boundary");
        s.request(id);
        expect(s.active() == 5 && !s.commit_at_game_frame_boundary(), "exit also requires retirement");
        s.consumers_retired(s.request_token());
        expect(s.commit_at_game_frame_boundary() && s.active() == id && s.generation() == 2,
            "live exit restores previous mode ID with a new frame generation");
        for (int next = 0; next != 5; ++next) {
            s.request(next);
            expect(s.active() == next && !s.pending(), "historical live mode changes are not deferred");
        }
    }
    m::Selection s;
    s.initialize(5);
    expect(s.active() == 0 && s.requested() == 5 && s.pending(), "startup Mono uses the same safe entry transaction");
    s.consumers_retired(s.request_token());
    expect(s.commit_at_game_frame_boundary() && s.active() == 5 && s.generation() == 1, "injection into Mono commits at a game-frame boundary");
    s.initialize(1); // config reload, not a second injection
    expect(s.active() == 5 && s.requested() == 1, "config reload uses the same live-exit transaction");
    s.consumers_retired(s.request_token());
    s.request(2);
    expect(!s.commit_at_game_frame_boundary(), "a changed request invalidates a stale retirement acknowledgement");
    s.consumers_retired(s.request_token());
    s.commit_at_game_frame_boundary();
    s.request(5);
    s.request(0);
    expect(s.active() == 0 && !s.pending(), "cancelling Mono entry preserves normal rendering");
    s.request(5);
    const auto stale_token = s.request_token();
    s.request(0);
    s.request(5);
    s.consumers_retired(stale_token);
    expect(!s.commit_at_game_frame_boundary(), "cancel/re-request cannot reuse an old GPU acknowledgement (ABA)");
    s.request(99);
    expect(s.requested() == 99, "future unsupported values survive a profile roundtrip");

    m::Selection concurrent;
    concurrent.initialize(0);
    std::atomic<bool> finished{}, torn{};
    std::thread reader([&] {
        while (!finished.load(std::memory_order_acquire)) {
            const auto state = concurrent.snapshot();
            if (state.method != ((state.generation & 1) ? 5 : 0)) { torn = true; }
        }
    });
    for (int i = 1; i <= 4000; ++i) {
        concurrent.request(i % 2 ? 5 : 0);
        concurrent.consumers_retired(concurrent.request_token());
        expect(concurrent.commit_at_game_frame_boundary(), "each acknowledged concurrent request commits once");
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    expect(!torn, "concurrent readers never see a torn method/generation publication");

    for (bool extreme : {false, true}) {
        const auto matrix = p::evaluate_mode_matrix({.rendering_method=p::RenderingMethod::Mono,
            .extreme_compatibility=extreme, .native_stereo_fix_requested=true, .ghosting_fix_requested=true,
            .dibr_engine_supported=true, .dx12=true, .openxr=true});
        expect(!matrix.using_afr && !matrix.using_native_stereo && !matrix.native_stereo_fix_active &&
            !matrix.ghosting_remap_active && !matrix.dibr_selected && !matrix.dibr_preview_active,
            "Mono cannot activate AFR, Native Fix, Ghost remap or DIBR even with saved toggles");
    }
    m::Capabilities caps{.dx12=true, .openxr=true};
    expect(m::unavailable_reason(caps) == nullptr, "DX12/OpenXR capability is eligible for geometry validation");
    for (auto member : {&m::Capabilities::restricted_title, &m::Capabilities::extreme,
             &m::Capabilities::split_screen, &m::Capabilities::screen_2d, &m::Capabilities::sceneview_compat,
             &m::Capabilities::stereo_emulation}) {
        caps.*member = true;
        expect(m::unavailable_reason(caps) != nullptr, "unsupported Mono combination is explicitly rejected");
        caps.*member = false;
    }
    caps.dx12 = false;
    expect(m::unavailable_reason(caps) != nullptr, "unknown render backend is rejected");
    caps.dx11 = true;
    expect(m::unavailable_reason(caps) == nullptr, "DX11 has a separately validated copy and retirement path");
    caps.dx11 = false;
    caps.dx12 = true; caps.openxr = false;
    expect(m::unavailable_reason(caps) != nullptr, "OpenVR is not silently accepted");

    std::array<m::Eye, 2> eyes{};
    eyes[0].position = {-0.032f, 0.002f, 0.005f};
    eyes[1].position = {0.032f, 0.002f, 0.005f};
    eyes[0].fov = {-1.1f, 0.9f, 1.2f, -0.8f};
    eyes[1].fov = {-0.9f, 1.15f, 1.1f, -0.85f};
    const auto g = m::geometry(eyes);
    expect(g && g->center[0] == 0 && g->center[1] == 0.002f && g->horizontal == 1.15f && g->vertical == 1.2f,
        "one centered union frustum covers asymmetric runtime FOVs");
    if (g) {
        for (size_t i = 0; i != 2; ++i) {
            expect(std::abs((g->bounds[i][0] * 2 - 1) * g->horizontal - eyes[i].fov[0]) < 0.00001f &&
                std::abs((g->bounds[i][1] * 2 - 1) * g->horizontal - eyes[i].fov[1]) < 0.00001f &&
                std::abs((1 - g->bounds[i][2] * 2) * g->vertical - eyes[i].fov[2]) < 0.00001f &&
                std::abs((1 - g->bounds[i][3] * 2) * g->vertical - eyes[i].fov[3]) < 0.00001f,
                "submitted per-eye crops preserve the central image's exact angular rays");
        }
    }
    const auto rotation_matrix = [](const std::array<float, 4>& q) {
        double n = 0;
        for (auto v : q) { n += static_cast<double>(v) * v; }
        const double x = q[0]/std::sqrt(n), y = q[1]/std::sqrt(n), z = q[2]/std::sqrt(n), w = q[3]/std::sqrt(n);
        return std::array<std::array<double, 3>, 3>{{
            {1-2*y*y-2*z*z, 2*x*y-2*z*w, 2*x*z+2*y*w},
            {2*x*y+2*z*w, 1-2*x*x-2*z*z, 2*y*z-2*x*w},
            {2*x*z-2*y*w, 2*y*z+2*x*w, 1-2*x*x-2*y*y}}};
    };
    const auto check_geometry = [&](const std::array<m::Eye, 2>& sample) {
        const auto geometry = m::geometry(sample);
        expect(geometry.has_value(), "finite calibrated eye rays have a common Mono projection");
        if (!geometry) { return; }
        const auto source_rotation = rotation_matrix(geometry->orientation);
        for (size_t eye = 0; eye != 2; ++eye) {
            const auto target_rotation = rotation_matrix(sample[eye].orientation);
            const auto& f = sample[eye].fov;
            // Independent matrix-based oracle, including interior rays, against
            // the quaternion-based production frustum construction.
            for (int xi = 0; xi <= 4; ++xi) {
                for (int yi = 0; yi <= 4; ++yi) {
                    const std::array<double, 3> ray{std::lerp(f[0], f[1], xi / 4.f), std::lerp(f[3], f[2], yi / 4.f), -1};
                    std::array<double, 3> world{}, common{};
                    for (int r = 0; r != 3; ++r) {
                        for (int c = 0; c != 3; ++c) { world[r] += target_rotation[r][c] * ray[c]; }
                    }
                    for (int r = 0; r != 3; ++r) {
                        for (int c = 0; c != 3; ++c) { common[r] += source_rotation[c][r] * world[c]; }
                    }
                    const auto tx = common[0] / -common[2], ty = common[1] / -common[2];
                    const auto u = 0.5 + 0.5 * tx / geometry->horizontal;
                    const auto v = 0.5 - 0.5 * ty / geometry->vertical;
                    const auto& b = geometry->bounds[eye];
                    expect(common[2] < 0 && u >= b[0] - 1e-6 && u <= b[1] + 1e-6 &&
                        v >= b[2] - 1e-6 && v <= b[3] + 1e-6,
                        "common-basis bounds cover every calibrated eye ray without assuming parallel optics");
                }
            }
            for (const auto dims : {std::array{2472u,2416u}, std::array{3001u,1703u}, std::array{1u,1u}}) {
                const auto crop = m::projection_crop(*geometry, eye, dims[0], dims[1]);
                expect(crop && crop->width > 0 && crop->height > 0 && crop->x + crop->width <= dims[0] &&
                    crop->y + crop->height <= dims[1], "integer Mono crops are nonempty and contained in their eye region");
                if (!crop) { continue; }
                const auto& b = geometry->bounds[eye];
                expect(double(crop->x)/dims[0] <= b[0] && double(crop->x+crop->width)/dims[0] >= b[1] &&
                    double(crop->y)/dims[1] <= b[2] && double(crop->y+crop->height)/dims[1] >= b[3],
                    "outward-rounded crops never discard a requested edge ray");
                for (float t : {0.f,0.13f,0.5f,0.81f,1.f}) {
                    const auto tx = (2.0 * (crop->x + t*crop->width) / dims[0] - 1) * geometry->horizontal;
                    const auto ty = (1 - 2.0 * (crop->y + t*crop->height) / dims[1]) * geometry->vertical;
                    expect(std::abs(tx - std::lerp(crop->fov[0],crop->fov[1],t)) < 1e-5 &&
                        std::abs(ty - std::lerp(crop->fov[2],crop->fov[3],t)) < 1e-5,
                        "submitted FOV describes the actual integer crop's rays, including nonintegral edges");
                }
            }
        }
    };
    check_geometry(eyes);
    auto canted = eyes;
    canted[1].orientation = {0, std::sin(0.05f), 0, std::cos(0.05f)};
    check_geometry(canted);
    canted[1].orientation = {0, 0, std::sin(0.13f), std::cos(0.13f)};
    check_geometry(canted);
    canted[1].orientation = {std::sin(0.08f), 0, 0, std::cos(0.08f)};
    check_geometry(canted);

    // WMR Sandfall read-only capture: 0.400211 degrees between the optical axes.
    auto calibrated = eyes;
    calibrated[0].orientation = {-0.0009746211f,0.0012330962f,0.0007600746f,0.9999984503f};
    calibrated[1].orientation = {0.0009753961f,-0.0012333887f,-0.0007602721f,0.9999984503f};
    calibrated[0].position = {-0.0339998901f,-0.0000579178f,-0.0000477611f};
    calibrated[1].position = {0.0339998305f,0.0000573580f,0.0000488133f};
    calibrated[0].fov = {std::tan(-0.8581431508f),std::tan(0.7822603583f),std::tan(0.8117937446f),std::tan(-0.8128302097f)};
    calibrated[1].fov = {std::tan(-0.7820497751f),std::tan(0.8614106178f),std::tan(0.8153690696f),std::tan(-0.8087611198f)};
    check_geometry(calibrated);
    const auto calibrated_geometry = m::geometry(calibrated);
    auto stage = calibrated;
    stage[0].orientation = {-0.0952989757f,0.0157259218f,0.0320377983f,0.9948087335f};
    stage[1].orientation = {-0.0933033898f,0.0131897330f,0.0307295416f,0.9950759411f};
    check_geometry(stage);
    const auto stage_geometry = m::geometry(stage);
    expect(calibrated_geometry && stage_geometry &&
        std::abs(calibrated_geometry->horizontal - stage_geometry->horizontal) < 1e-5 &&
        std::abs(calibrated_geometry->vertical - stage_geometry->vertical) < 1e-5,
        "captured view-space and stage-space calibrations agree on the rendered union projection");
    for (auto& eye : calibrated) { for (auto& q : eye.orientation) { q = -q; } }
    check_geometry(calibrated);
    const auto opposite_sign = m::geometry(calibrated);
    expect(opposite_sign && calibrated_geometry && opposite_sign->bounds == calibrated_geometry->bounds,
        "quaternion sign changes do not alter the common-basis crop");

    auto invalid = eyes;
    invalid[1].orientation = {0, std::sin(0.7f), 0, std::cos(0.7f)};
    expect(!m::geometry(invalid), "frusta crossing the common perspective horizon remain fail-closed");
    invalid = eyes; invalid[1].orientation[3] = -1;
    expect(m::geometry(invalid).has_value(), "equivalent quaternion signs are accepted");
    invalid = eyes; invalid[0].fov[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!m::geometry(invalid), "nonfinite FOV fails closed");
    invalid = eyes; invalid[0].fov[0] = 0;
    expect(!m::geometry(invalid), "degenerate FOV fails closed");
    invalid = eyes; invalid[0].orientation = {};
    expect(!m::geometry(invalid), "zero orientation fails closed");
    invalid = eyes; invalid[0].position[0] = std::numeric_limits<float>::infinity();
    expect(!m::geometry(invalid), "nonfinite center position fails closed");
    invalid = eyes; invalid[1].fov[1] = 101;
    expect(!m::geometry(invalid), "unbounded frusta fail closed");
    if (g) {
        expect(!m::projection_crop(*g,2,2472,2416) && !m::projection_crop(*g,0,0,2416) &&
            !m::projection_crop(*g,0,2472,0) && !m::projection_crop(*g,0,65536,2416),
            "missing or unsupported submission regions fail closed");
        auto bad = *g; bad.bounds[0][1] = std::numeric_limits<float>::quiet_NaN();
        expect(!m::projection_crop(bad,0,2472,2416), "nonfinite crop cannot become a submitted image rectangle");
        bad = *g; bad.bounds[0][0] = -0.01f;
        expect(!m::projection_crop(bad,0,2472,2416), "out-of-bounds crop fails closed");
    }
    expect(m::clear_waiting_backbuffer(true,false,true,3,3),
        "blocked Mono submission still clears a ready current-generation backbuffer");
    expect(!m::clear_waiting_backbuffer(false,false,true,3,3) &&
        !m::clear_waiting_backbuffer(true,true,true,3,3) &&
        !m::clear_waiting_backbuffer(true,false,false,3,3) &&
        !m::clear_waiting_backbuffer(true,false,true,2,3) &&
        !m::clear_waiting_backbuffer(true,false,true,0,0),
        "Mono clearing never adds work during retirement, setup, a stale generation or an ordinary mode");
    for (auto width : {2048u, 4944u, 6008u}) {
        auto copies = m::copy_regions(width, 2416, width, 2416);
        expect(copies && (*copies)[0].left == (*copies)[1].left && (*copies)[0].right == (*copies)[1].right &&
            (*copies)[0].destination_x == 0 && (*copies)[1].destination_x == width / 2,
            "both output eyes consume the identical current source rectangle, never prior-eye history");
    }
    expect(!m::copy_regions(0,1080,0,1080) && !m::copy_regions(2001,1080,2001,1080) &&
        !m::copy_regions(4000,1080,2000,1080) && !m::copy_regions(4000,1080,4000,2160),
        "startup, odd, resize and stale destination extents are rejected");
    expect(m::frame_matches(2,2,42,42,42,true), "matching render, pose and projection token is accepted");
    expect(!m::needs_frame_gate(0,0,0) && m::needs_frame_gate(5,1,1),
        "ordinary modes avoid the gate; Mono validates every frame even after activation");
    expect(m::needs_frame_gate(0,2,1) && !m::needs_frame_gate(0,2,2) && m::needs_frame_gate(1,4,2),
        "publishing an exit generation closes its frame gate atomically; a stale readiness token cannot reopen it");
    expect(!m::frame_matches(2,1,42,42,42,true) && !m::frame_matches(2,2,42,41,42,true) &&
        !m::frame_matches(2,2,42,42,41,true) && !m::frame_matches(2,2,42,42,42,false),
        "old generations, stale eye frames and auxiliary families cannot publish Mono");
}
