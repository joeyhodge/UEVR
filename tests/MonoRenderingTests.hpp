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
    auto invalid = eyes;
    invalid[1].orientation = {0, std::sin(0.05f), 0, std::cos(0.05f)};
    expect(!m::geometry(invalid), "canted displays fail closed rather than duplicate the wrong orientation");
    invalid = eyes; invalid[1].orientation[3] = -1;
    expect(m::geometry(invalid).has_value(), "equivalent quaternion signs are accepted");
    invalid = eyes; invalid[0].fov[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!m::geometry(invalid), "nonfinite FOV fails closed");
    invalid = eyes; invalid[0].fov[0] = 0;
    expect(!m::geometry(invalid), "degenerate FOV fails closed");
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
