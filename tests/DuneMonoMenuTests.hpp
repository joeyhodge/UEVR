#include "mods/vr/DuneMonoMenu.hpp"
#include "mods/vr/NativeOpenXRArrayPolicy.hpp"

void test_dune_mono_menu() {
    namespace d = uevr::dune_menu;
    std::vector<uint8_t> code(0x240, 0x90);
    const auto put = [&](size_t offset, const auto& bytes) {
        std::copy(bytes.begin(), bytes.end(), code.begin() + offset);
    };
    put(0x60, d::draw_this);
    put(0x100, d::draw_test);
    const int32_t skip = 0x20;
    std::memcpy(code.data() + 0x106, &skip, sizeof(skip));
    put(0x1b0, d::draw_render);
    put(0x1c0, d::draw_cleanup);
    expect(d::valid_draw(code), "Dune menu reader requires its independent Draw suppression/renderer/cleanup contract");
    const auto good = code;
    code[0x103] = 2;
    expect(!d::valid_draw(code), "Breathedge's world-disable bit cannot authorize Dune's menu reader");
    code = good;
    code[0x102] = 0x6c;
    expect(!d::valid_draw(code), "canonical UObject and secondary Draw this offsets are not interchangeable");
    code = good;
    std::fill(code.begin() + 0x106, code.begin() + 0x10a, 0xff);
    expect(!d::valid_draw(code), "a backward suppression branch is not the verified menu path");
    code = good;
    const int32_t past_render = 0x180;
    std::memcpy(code.data() + 0x106, &past_render, sizeof(past_render));
    expect(!d::valid_draw(code), "changed suppression branch flow rejects the reader");
    code = good;
    put(0x200, d::draw_render);
    expect(!d::valid_draw(code), "ambiguous renderer anchors cannot authorize private layout access");
    code = good;
    code.resize(0x4001);
    expect(!d::valid_draw(code), "Draw contract decoding remains bounded");
    expect(d::canonical_client(0x20028) == 0x20000 && d::canonical_client(0x28) == 0 &&
        d::canonical_client(0x20029) == 0, "only the proved secondary-base adjustment yields a candidate UObject");

    d::Observation state{4, 9, 0x50000, 0x60000, 15, 1000000000, true};
    expect(d::current(state, 4, 9, 0x50000, 1000000000), "fresh UI-only owner is usable without any scene frame number");
    expect(d::current(state, 4, 9, 0x50000, 2000000000), "observation expiry is inclusive and not an activation delay");
    expect(!d::current(state, 4, 9, 0x50000, 2000000001), "a stopped Draw producer cannot leave UI-only state authorized forever");
    expect(!d::current(state, 4, 9, 0x50000, 999999999), "invalid observation clock order fails closed");
    expect(!d::current(state, 5, 9, 0x50000, 1000000000) && !d::current(state, 4, 10, 0x50000, 1000000000),
        "mode exit, re-entry and cancelled requests reject old menu observations");
    expect(!d::current(state, 4, 9, 0x50008, 1000000000), "a changed viewport cannot inherit menu ownership");
    auto after = state;
    ++after.observed_ns;
    expect(d::same_owner(state, after), "successive Draw samples of the same menu do not invalidate each other's frame");
    ++after.client_serial;
    expect(!d::same_owner(state, after), "recycled viewport UObjects cannot complete old menu frames");
    after = state;
    after.world_disabled = false;
    expect(d::same_owner(state, after) && !after.world_disabled, "closing the menu is observed without restoring or writing engine flags");
    const d::Frame frame{state, 3, 50000000000, 7, 77, true};
    expect(d::same_frame(frame, 3, 77), "UI submission uses its own successfully begun wait sequence");
    expect(!d::same_frame(frame, 4, 77) && !d::same_frame(frame, 3, 78),
        "session recreation and a different outstanding wait reject a retained UI frame");
    expect(!d::same_frame({}, 0, 0), "uninitialized frame tickets are never valid");
    auto reused_time = frame;
    ++reused_time.serial;
    expect(d::same_lease(frame, frame) && !d::same_lease(frame, reused_time),
        "an old UI scope cannot close a different wait lease even if its predicted time repeats");
    auto extrapolated = frame;
    extrapolated.display_time += 11111111;
    expect(!d::same_lease(frame, extrapolated) && d::same_frame(frame, 3, 77),
        "GT pose extrapolation cannot replace the UI owner's immutable wait display time");
    extrapolated = frame;
    extrapolated.should_render = false;
    expect(!d::same_lease(frame, extrapolated), "a stale or changed shouldRender decision cannot replace the owned UI lease");

    d::SceneCutoff cutoff;
    expect(cutoff.allows(123), "ordinary Mono retains its previous behavior before any UI-only frame");
    for (int64_t time = 1000; time != 1060; ++time) {
        cutoff.observe(time);
        expect(!cutoff.allows(time) && !cutoff.allows(time - 1) && cutoff.allows(time + 1),
            "even with a stalled engine counter a UI-only wait never makes an old world scene current");
    }
    cutoff.observe(1);
    expect(!cutoff.allows(1059), "out-of-order callbacks cannot lower the world-scene freshness requirement");
    expect(!cutoff.allows(1058) && cutoff.allows(1060), "menu closure still requires a newly posed real scene");
    cutoff.reset();
    expect(cutoff.allows(1), "mode/session reset clears the old menu clock domain");

    uevr::native_array::ImageWait image;
    image.acquired();
    int releases{};
    bool gpu_retired{}, release_ok{};
    const auto release = [&] { ++releases; return release_ok; };
    expect(!uevr::native_array::poll_array_release(image, [] { return false; }, [] { return true; }, release) && releases == 0,
        "an unwaited UI acquisition remains owned, not illegally released on wait failure");
    expect(image.pending() && image.waited(true), "cleanup can finish the same UI image wait without reacquiring");
    expect(!uevr::native_array::poll_array_release(image, [] { return false; }, [&] { return gpu_retired; }, release) && releases == 0,
        "UI error cleanup polls GPU retirement without any blocking or source destruction");
    gpu_retired = true;
    expect(!uevr::native_array::poll_array_release(image, [] { return false; }, [&] { return gpu_retired; }, release) && image.writable(),
        "release failure preserves the same waited image lease");
    release_ok = true;
    expect(uevr::native_array::poll_array_release(image, [] { return false; }, [&] { return gpu_retired; }, release) &&
        !image.pending() && !image.writable(), "only a retired and successfully released UI image loses ownership");

    for (const auto width : {1280u, 1920u, 2560u, 3840u}) {
        expect(d::ui_extent(width, 1080, 1, 1, 1, width, 1080), "UI-only copies do not assume a 1920-pixel window");
        expect(!d::ui_extent(width, 1080, 1, 1, 1, width + 1, 1080), "resizing never copies an old full resource into a different extent");
    }
    expect(!d::ui_extent(0, 1080, 1, 1, 1, 0, 1080) && !d::ui_extent(1920, 0, 1, 1, 1, 1920, 0),
        "empty UI resources fail closed");
    expect(!d::ui_extent(1920, 1080, 2, 1, 1, 1920, 1080) && !d::ui_extent(1920, 1080, 1, 2, 1, 1920, 1080) &&
        !d::ui_extent(1920, 1080, 1, 1, 4, 1920, 1080), "UI-only copies reject arrays, mips and multisampled sources");
}
