#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include "mods/vr/NativeFrameDiagnosticsJson.hpp"

namespace {
namespace nf = uevr::native_frame;
// The frame window is the production helper. The remaining resource checks here
// are a replay model, not a GPU/backend implementation.
struct Observation {
    int32_t packet_render, submit_render;
    uint32_t packet_engine, current_engine;
    uint64_t packet_generation, current_generation;
    uintptr_t packet_resource, current_resource;
    bool rejected_generation{};
};
constexpr bool accepted(const Observation& value) {
    const auto render_delta = int64_t(value.submit_render) - int64_t(value.packet_render);
    const auto engine_delta = uint32_t(value.current_engine - value.packet_engine);
    return nf::frame_window(render_delta, engine_delta, value.current_engine == value.packet_engine).accepted() &&
        value.packet_generation == value.current_generation && value.packet_resource != 0 &&
        value.packet_resource == value.current_resource && !value.rejected_generation;
}

int test_frame_recorder() {
    int failures{};
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    for (const auto render : {INT64_MIN, int64_t(-1), int64_t(0), int64_t(1), int64_t(2), INT64_MAX}) {
        for (const auto engine : {0u, 1u, 2u, UINT32_MAX}) {
            for (const bool same : {false, true}) {
                const bool old = render == 0 || same || (render == 1 && engine == 1);
                expect(nf::frame_window(render, engine, same).accepted() == old, "production frame-window truth table unchanged");
            }
        }
    }
    for (const auto engine : {0u, 1u, UINT32_MAX}) {
        for (const auto render : {INT32_MIN, -1, 0, 1, INT32_MAX}) {
            const auto packed = nf::pack_clocks(engine, render);
            expect(nf::engine_clock(packed) == engine && nf::present_clock(packed) == render, "packed clocks retain signed/unsigned bits");
        }
    }

    nf::Recorder<16> recorder;
    expect(!recorder.enabled() && !recorder.ticket(0, 5), "diagnostics default off");
    recorder.observe_engine(0, 1, 1);
    recorder.observe_present(0, 1, 2);
    expect(!recorder.record(0, {}) && !recorder.clock(0).ready, "disabled helpers do not publish");
    expect(nf::export_json(recorder)["enabled"] == false && !recorder.enabled(), "export never enables recording");
    recorder.set_enabled(true);
    const auto token = recorder.control();
    const auto epoch = token >> 1;
    expect(!recorder.clock(token).ready, "enabling does not invent initialized counters");
    recorder.observe_engine(token, 0, 1);
    expect(!recorder.clock(token).ready, "one observed clock is insufficient");
    recorder.observe_present(token, 0, 2);
    expect(recorder.clock(token).ready, "zero is valid after both owners observe it");
    const auto ticket = recorder.ticket(token, 91);
    expect(ticket && ticket.packet_serial == 91, "ticket binds selection serial");
    nf::Event event{};
    event.stage = nf::Stage::selection;
    event.attempt = ticket.attempt;
    event.packet_serial = ticket.packet_serial;
    event.producer_clock = recorder.clock(token);
    recorder.observe_present(token, 12, 2);
    event.consumer_clock = recorder.clock(token);
    event.window_observed = event.legacy_same_engine = event.packet_accepted = true;
    event.render_delta = 12;
    event.epoch = epoch;
    event.reason = nf::Reason::same_engine_frame;
    expect(recorder.record(token, event), "record same-engine repeated-Present grace");
    nf::Recorder<16>::Snapshot snapshot;
    expect(recorder.snapshot(snapshot) && snapshot.count == 1 && snapshot.events[0].shadow == nf::Shadow::matches,
        "coherent observed window agrees without requiring equal clocks");

    event.producer_clock = {epoch, nf::pack_clocks(100, 20), true};
    event.consumer_clock = {epoch, nf::pack_clocks(101, 21), true};
    event.engine_delta = 2;
    event.render_delta = 2;
    event.legacy_same_engine = event.packet_accepted = false;
    event.reason = nf::Reason::stale_frames;
    nf::compare_window(event);
    expect(event.shadow == nf::Shadow::differs && event.shadow_window && !event.legacy_window && !event.packet_accepted,
        "shadow disagreement cannot select a packet or override legacy outcome");
    event.legacy_same_engine = true;
    nf::compare_window(event);
    expect(event.shadow == nf::Shadow::legacy_changed, "disagreeing legacy reads are indeterminate, not a clean comparison");
    event.legacy_same_engine = false;
    event.consumer_clock.ready = false;
    event.shadow = nf::Shadow::unavailable;
    nf::compare_window(event);
    expect(event.shadow == nf::Shadow::unavailable, "missed clock observation cannot become zero identity");
    recorder.observe_engine(token, 0, 3);
    expect(!recorder.clock(token).ready && recorder.clock(token).owner_changed, "writer migration blocks shadow readiness");
    recorder.set_enabled(false);
    expect(!recorder.record(token, event), "late writer rejected while disabled");
    expect(nf::export_json(recorder)["retained"] == 1, "disabling preserves trace for export");
    recorder.set_enabled(true);
    const auto next_token = recorder.control();
    expect(next_token != token && !recorder.record(ticket.control, event), "late copy ticket cannot leak into next session");
    expect(nf::export_json(recorder)["retained"] == 0, "new epoch never relabels old payloads");
    recorder.observe_engine(next_token, UINT32_MAX, 1);
    recorder.observe_present(next_token, INT32_MAX, 2);
    expect(recorder.clock(next_token).ready && !recorder.clock(next_token).owner_changed, "new epoch resets owner evidence, terminal values remain valid");
    event = {};
    event.packet_serial = 91;
    for (uint64_t i = 1; i <= 35; ++i) {
        event.transaction = i;
        expect(recorder.record(next_token, event), "bounded recorder overwrites only diagnostic events");
    }
    expect(recorder.snapshot(snapshot) && snapshot.count == 16 && snapshot.total == 35 && snapshot.overwritten == 19,
        "full buffer has bounded retention and explicit overwritten count");
    for (size_t i = 0; i < snapshot.count; ++i) {
        expect(snapshot.events[i].transaction == 20 + i && snapshot.events[i].sequence == 20 + i &&
            snapshot.events[i].epoch == next_token >> 1, "ring export ordered and epoch coherent");
    }

    // Both backend/runtime tags and late-copy attribution are transported by the
    // production recorder. Source-level checks below cover the actual adapters.
    for (const auto backend : {nf::Backend::d3d11, nf::Backend::d3d12}) {
        for (const auto runtime : {nf::Runtime::openxr, nf::Runtime::openvr}) {
            nf::Recorder<16> trace;
            trace.set_enabled(true);
            const auto t = trace.control();
            auto retained_ticket = trace.ticket(t, 91);
            retained_ticket.consumer_clock = {t >> 1, nf::pack_clocks(100, 99), true};
            retained_ticket.submit_render = 99;
            nf::Event selected{};
            selected.backend = backend;
            selected.runtime = runtime;
            selected.packet_serial = 91;
            selected.attempt = retained_ticket.attempt;
            selected.stage = nf::Stage::selection;
            selected.packet_accepted = selected.window_observed = selected.legacy_same_engine = true;
            selected.consumer_clock = selected.producer_clock = retained_ticket.consumer_clock;
            selected.reason = nf::Reason::exact_frame;
            trace.record(t, selected);
            auto newer = selected;
            newer.packet_serial = 92;
            newer.attempt = 0;
            newer.stage = nf::Stage::producer;
            trace.record(t, newer);
            // Invalidation/new publication must not relabel a retained copy.
            nf::Event copy{};
            copy.packet_serial = retained_ticket.packet_serial;
            copy.attempt = retained_ticket.attempt;
            copy.backend = backend;
            copy.runtime = runtime;
            copy.consumer_clock = retained_ticket.consumer_clock;
            copy.submit_render = retained_ticket.submit_render;
            copy.stage = nf::Stage::copy_recorded;
            copy.copy_source = 0x1234;
            copy.copy_destination = 0x5678;
            trace.record(t, copy);
            auto submit = copy;
            submit.stage = nf::Stage::submit_attempt;
            trace.record(t, submit);
            submit.stage = nf::Stage::submit_result;
            submit.api_result = -1;
            trace.record(t, submit);
            const auto json = nf::export_json(trace);
            expect(json["newest_copy_recorded_serial"] == 91 && json["consumer_identity_observed"] == true,
                "new producer does not misattribute old copy");
            expect(json["selections_without_copy_in_buffer"] == 0 && json["submission_attempts_without_result_in_buffer"] == 0,
                "submission/copy correlation uses retained selection attempt");
            expect(json["events"][2]["copy_right_source"] == "0x1234" && json["events"][2]["submission_result"].is_null(),
                "copy evidence is not mislabeled successful submission");
            expect(json["events"][2]["packet_accepted"].is_null(), "copy event does not invent a new selection verdict");
            expect(json["events"][4]["submission_result"] == -1 && json["events"][4]["runtime"] == nf::name(runtime) &&
                json["events"][4]["backend"] == nf::name(backend), "actual result code and backend/runtime preserved");
            expect(json["events"][2]["consumer_shadow_clock"]["engine"] == 100 && json["events"][2]["consumer_render"] == 99,
                "late copy retains frozen consumer clocks");
        }
    }

    // Ordered owners synchronize only in this fixture, never in the runtime.
    nf::Recorder<16> ordered;
    ordered.set_enabled(true);
    const auto ordered_token = ordered.control();
    std::barrier step{2};
    std::thread draw([&] {
        for (uint32_t i = 0; i < 1000; ++i) {
            ordered.observe_engine(ordered_token, i, 1);
            step.arrive_and_wait();
            step.arrive_and_wait();
        }
    });
    for (int32_t i = 0; i < 1000; ++i) {
        step.arrive_and_wait();
        ordered.observe_present(ordered_token, i + 1, 2);
        const auto clock = ordered.clock(ordered_token);
        expect(clock.ready && nf::engine_clock(clock.clocks) == uint32_t(i) && nf::present_clock(clock.clocks) == i + 1,
            "ordered different owners preserve both clock fields");
        step.arrive_and_wait();
    }
    draw.join();

    nf::Recorder<64> concurrent;
    concurrent.set_enabled(true);
    std::barrier start{4};
    std::atomic_bool finished{};
    std::atomic<int> torn{};
    std::thread producer([&] {
        start.arrive_and_wait();
        for (uint64_t i = 1; i <= 20000; ++i) {
            const auto t = concurrent.control();
            concurrent.observe_engine(t, uint32_t(i), 1);
            nf::Event e{};
            e.packet_serial = i;
            e.generation = i ^ 0xABCDEF;
            e.transaction = i * 7;
            e.resource = uintptr_t(i + 23);
            e.producer_clock = concurrent.clock(t);
            concurrent.record(t, e);
        }
        finished = true;
    });
    std::thread present([&] {
        start.arrive_and_wait();
        for (int32_t i = 0; !finished.load(); ++i) {
            const auto t = concurrent.control();
            concurrent.observe_present(t, i, 2);
            (void)concurrent.clock(t);
        }
    });
    std::thread toggle([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 1000; ++i) {
            concurrent.set_enabled(false);
            concurrent.set_enabled(true);
        }
    });
    start.arrive_and_wait();
    nf::Recorder<64>::Snapshot concurrent_snapshot;
    do {
        if (!concurrent.snapshot(concurrent_snapshot)) { continue; }
        for (size_t i = 0; i < concurrent_snapshot.count; ++i) {
            const auto& e = concurrent_snapshot.events[i];
            if (e.epoch != concurrent_snapshot.epoch || e.generation != (e.packet_serial ^ 0xABCDEF) ||
                e.transaction != e.packet_serial * 7 || e.resource != e.packet_serial + 23 ||
                (i && e.sequence != concurrent_snapshot.events[i - 1].sequence + 1)) { ++torn; }
        }
    } while (!finished.load());
    producer.join(); present.join(); toggle.join();
    expect(torn == 0, "concurrent overwrite/export/toggle never exposes torn or relabeled payloads");
    const auto t = concurrent.control();
    concurrent.observe_engine(t, 123, 1);
    concurrent.observe_present(t, 456, 2);
    expect(concurrent.clock(t).ready, "missed diagnostic clock updates recover only after both owners publish again");

    // Measurements include timer overhead, not an FPS claim or a timing gate.
    nf::Recorder<64> measured;
    for (const bool enabled : {false, true}) {
        measured.set_enabled(enabled);
        std::vector<double> ns;
        for (int i = 0; i < 2048; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            for (int j = 0; j < 128; ++j) {
                if (const auto control = measured.control()) { measured.record(control, event); }
            }
            ns.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / 128.0);
        }
        std::sort(ns.begin(), ns.end());
        std::cout << "Native frame recorder " << (enabled ? "on" : "off") << " ns/op p50=" << ns[1024]
            << " p95=" << ns[1945] << " p99=" << ns[2027] << " (batch-normalized, includes timer overhead)\n";
    }
    return failures;
}

int test_native_frame_adapters() {
    int failures{};
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    const auto root = std::filesystem::path{__FILE__}.parent_path().parent_path();
    const auto source = [&](const char* path) {
        std::ifstream file{root / path};
        expect(file.good(), "source contract fixture can read current implementation");
        return std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    };
    for (const auto backend : {"D3D11", "D3D12"}) {
        const auto file = "src/mods/vr/" + std::string(backend) + "Component.cpp";
        const auto code = source(file.c_str());
        const auto getter = code.find("get_native_stereo_frame_packet_for_submit(");
        expect(getter != std::string::npos && code.substr(getter, 180).find("&native_frame_ticket") != std::string::npos,
            "backend selection freezes a diagnostic ticket");
        size_t pos = 0, copies = 0, submits = 0;
        while ((pos = code.find("->note_native_stereo_frame_packet_consumed(", pos)) != std::string::npos) {
            const auto before = code.substr(pos > 750 ? pos - 750 : 0, pos > 750 ? 750 : pos);
            expect(before.find("record_native_frame_stage(*native_stereo_packet, native_frame_ticket") != std::string::npos &&
                before.find("Stage::copy_recorded") != std::string::npos, "every existing consume site records retained packet, not newest global packet");
            ++pos; ++copies;
        }
        for (const auto needle : {"vr::VRCompositor()->Submit(", "vr->m_openxr->end_frame("}) {
            pos = 0;
            while ((pos = code.find(needle, pos)) != std::string::npos) {
                const auto before = code.substr(pos > 250 ? pos - 250 : 0, pos > 250 ? 250 : pos);
                const auto after = code.substr(pos, 500);
                expect(before.find("Stage::submit_attempt") != std::string::npos && after.find("Stage::submit_result") != std::string::npos,
                    "existing submission calls retain attempt and actual result observations");
                ++pos; ++submits;
            }
        }
        expect(copies == (std::string(backend) == "D3D11" ? 2 : 3) && submits == (std::string(backend) == "D3D11" ? 4 : 5),
            "expected backend copy/submit coverage (update fixture for new paths)");
        if (std::string(backend) == "D3D11") {
            const auto copy = code.find("bool D3D11Component::OpenXR::copy(");
            expect(copy != std::string::npos && code.substr(copy).find("pre_commands(ctx.textures[texture_index].texture);") != std::string::npos,
                "D3D11 callback executes synchronously while submit owns the packet and ticket");
        } else {
            expect(code.find("native_stereo_packet,\n        native_frame_ticket,") != std::string::npos &&
                code.find("native_stereo_packet, native_stereo_hook, native_frame_ticket]") != std::string::npos,
                "D3D12 deferred double-wide and array copies retain selection ticket by value");
        }
    }
    const auto hook = source("src/mods/vr/FFakeStereoRenderingHook.cpp");
    const auto begin = hook.find("FFakeStereoRenderingHook::get_native_stereo_frame_packet_for_submit(");
    const auto end = hook.find("void FFakeStereoRenderingHook::note_native_stereo_frame_packet_consumed(", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        expect(false, "production selector boundaries found");
    } else {
        const auto selector = hook.substr(begin, end - begin);
        expect(selector.find("frame_diag::frame_window(render_frame_delta, engine_frame_delta, same_engine_frame_present_grace).accepted()") != std::string::npos,
            "production selection uses the tested unchanged frame-window helper");
        expect(selector.find("shadow_window") == std::string::npos && selector.find("Shadow::") == std::string::npos,
            "shadow verdict cannot control production selection");
    }
    return failures;
}
}

int test_cadence_replay() {
    int failures{};
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    for (const bool dx12 : {false, true}) {
        for (const bool one_frame_lag : {false, true}) {
            for (const bool ghost_requested : {false, true}) {
                (void)dx12; (void)ghost_requested;
                Observation value{100, one_frame_lag ? 101 : 100, 50, one_frame_lag ? 51u : 50u, 2, 2, 0x1000, 0x1000};
                expect(accepted(value), "Native Fix exact/one-frame handoff remains eligible");
                value.current_engine = value.packet_engine;
                value.submit_render += 10;
                expect(accepted(value), "paused/repeated Presents retain same-engine-frame grace");
                value.current_generation++;
                expect(!accepted(value), "resize rejects previous generation even during pause");
                value.current_generation = value.packet_generation;
                value.current_resource = 0x2000;
                expect(!accepted(value), "resource replacement invalidates old packet");
                value.current_resource = value.packet_resource;
                value.current_engine += 2;
                expect(!accepted(value), "skipped Draws do not permit wider cross-frame reuse");
            }
        }
    }
    expect(accepted({INT32_MAX, INT32_MAX, UINT32_MAX, UINT32_MAX, 1, 1, 1, 1}), "exact terminal counter values");
    expect(accepted({100, 101, UINT32_MAX, 0, 1, 1, 1, 1}), "engine-frame unsigned wrap preserves one-frame handoff");
    expect(!accepted({INT32_MAX, INT32_MIN, 8, 9, 1, 1, 1, 1}), "render counter wrap is not silently broadened");
    expect(!accepted({100, 100, 50, 50, 1, 1, 1, 1, true}), "explicit rejection cannot be revived by exact frames");
    for (const auto& historical : {Observation{5582, 5583, 5584, 5584, 1, 1, 1, 1},
        {6162, 6163, 6164, 6164, 1, 1, 1, 1}, {3027, 3028, 3028, 3028, 1, 1, 1, 1},
        {554, 555, 555, 555, 1, 1, 1, 1}}) {
        expect(accepted(historical), "Stellar/Medium/DaysGone/Hellblade logged handoffs stay eligible");
    }
    expect(!accepted({3914, 3915, 3915, UINT32_MAX, 1, 1, 1, 1}), "Breathedge logged stale input is not widened into a handoff");
    expect(!accepted({1186, 2, 1186, 2, 1, 1, 1, 1}), "historical rollback is not a true one-frame wrap");
    failures += test_frame_recorder();
    failures += test_native_frame_adapters();
    return failures;
}
