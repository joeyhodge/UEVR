#pragma once

#include "utility/PacingRecorderJson.hpp"
#include <memory>
#include <thread>
#include <vector>

namespace pacing_recorder_tests {
struct BusyGate {
    static inline bool busy{};
    bool try_lock() noexcept { return !busy; }
    void unlock() noexcept {}
};

template<class Expect> void run(const Expect& expect) {
    using namespace utility::pacing;
    Recorder<4> ring;
    Recorder<4>::Snapshot sample{};
    expect(!ring.enabled() && ring.control() == 0 && ring.snapshot(sample) && !sample.retained,
        "pacing recorder is empty/default-off without needing a setting");
    expect(!ring.record(0, {}), "disabled recorder does not accept a payload");
    ring.set_enabled(true);
    const auto first = ring.control();
    const auto first_runtime = next_runtime_id(), second_runtime = next_runtime_id();
    expect(first_runtime != 0 && second_runtime != first_runtime, "replacement runtimes cannot alias a reset session generation");
    for (uint32_t i = 1; i <= 6; ++i) {
        Event e{};
        e.frame = i;
        e.domain = Domain::engine;
        e.start_ns = 1000 * i;
        e.duration_ns = 1000000;
        e.stage = Stage::wait_frame;
        e.owner = Owner::wait_only_worker;
        e.thread = 3;
        e.session_generation = 7;
        e.runtime_id = i < 5 ? first_runtime : second_runtime;
        e.wait_sequence = i;
        e.display_time = 3000 * i;
        e.result_known = true;
        expect(ring.record(first, e), "record into bounded ring");
    }
    expect(ring.snapshot(sample) && sample.retained == 4 && sample.total == 6 && sample.overwritten == 2 &&
        sample.events[0].frame == 3 && sample.events[3].frame == 6,
        "ring overwrite keeps completion order and exact counters");
    ring.set_enabled(false);
    expect(ring.snapshot(sample) && !sample.enabled && sample.retained == 4 && !ring.record(first, {}),
        "stop retains data and rejects in-flight producer completions");
    const auto output = export_json(ring);
    expect(output["retained"] == 4 && output["gpu_timing_available"] == false &&
        output["events"][0]["wait_owner"] == "wait_only_worker" && output["events"][0]["thread_id"] == 3 &&
        output["events"][0]["frame_domain"] == "engine_frame_argument" &&
        output["retained_stage_totals"]["xrWaitFrame"]["total_cpu_ms"] == 4.0 &&
        output["events"][0]["runtime_id"] != output["events"][3]["runtime_id"],
        "export distinguishes physical wait owner, frame domain and CPU-only durations");
    ring.set_enabled(true);
    expect(ring.control() != first && ring.snapshot(sample) && sample.retained == 0 && sample.total == 0,
        "re-arm starts a fresh logical epoch without clearing live memory");
    expect(!ring.record(first, {}), "old token cannot populate the next epoch");
    Event decision_event{};
    decision_event.stage = Stage::wait_decision;
    decision_event.reason = Reason::pose_ahead;
    ring.record(ring.control(), decision_event);
    const auto next = export_json(ring);
    expect(next["events"][0]["frame"].is_null() && next["events"][0]["api_result"].is_null() &&
        next["events"][0]["predicted_display_time"].is_null() &&
        next["retained_wait_decisions"]["next_pose_or_retained_slot_evidence"] == 1,
        "unavailable identities/results stay null; conservative pose evidence is not called a fresh pose");

    Recorder<2, BusyGate> contested;
    contested.set_enabled(true);
    Recorder<2, BusyGate>::Snapshot contested_sample{};
    BusyGate::busy = true;
    expect(!contested.record(contested.control(), {}) && !contested.snapshot(contested_sample) &&
        export_json(contested)["snapshot_busy"] == true,
        "busy producer/export gate drops immediately instead of waiting or inventing data");
    BusyGate::busy = false;
    expect(contested.snapshot(contested_sample) && contested_sample.dropped_lifetime == 1 && !contested_sample.retained,
        "contention is reported as loss rather than a zero-cost captured frame");

    Recorder<64> concurrent;
    concurrent.set_enabled(true);
    std::atomic<bool> done{};
    std::atomic<uint64_t> errors{}, accepted{};
    std::thread reader{[&] {
        Recorder<64>::Snapshot snapshot{};
        while (!done.load(std::memory_order_acquire)) {
            if (!concurrent.snapshot(snapshot)) { continue; }
            for (size_t i{}; i < snapshot.retained; ++i) {
                const auto& e = snapshot.events[i];
                if (e.epoch != snapshot.epoch || e.start_ns != uint64_t(e.frame) * e.thread ||
                    e.duration_ns != uint64_t(e.frame) + e.thread || (i && e.sequence <= snapshot.events[i-1].sequence)) {
                    errors.fetch_add(1);
                }
            }
        }
    }};
    std::vector<std::thread> writers;
    for (uint32_t thread = 1; thread <= 4; ++thread) {
        writers.emplace_back([&, thread] {
            for (uint32_t frame = 1; frame <= 15000; ++frame) {
                Event e{};
                e.frame = frame; e.thread = thread;
                e.start_ns = uint64_t(frame) * thread;
                e.duration_ns = uint64_t(frame) + thread;
                if (concurrent.record(concurrent.control(), e)) { accepted.fetch_add(1); }
            }
        });
    }
    for (int i{}; i < 40; ++i) { concurrent.set_enabled(false); concurrent.set_enabled(true); }
    for (auto& writer : writers) { writer.join(); }
    done.store(true, std::memory_order_release);
    reader.join();
    expect(!errors.load() && accepted.load() > 0, "concurrent producers, exports and re-arm never race plain event payloads");

    auto& production = recorder();
    production.set_enabled(false);
    {
        ScopedEvent ignored{Stage::marker};
        decision(Reason::normal_pacing);
    }
    production.set_enabled(true);
    {
        ScopedOwner owner{Owner::array_worker};
        {
            ScopedOwner inner{Owner::wait_only_worker};
            ScopedEvent event{Stage::wait_frame};
            event.finish(0);
        }
        expect(wait_owner() == Owner::array_worker, "nested diagnostic owner scope restores its caller");
        decision(Reason::reserved, {.runtime_id = second_runtime, .session_generation = 2, .wait_sequence = 3,
            .display_time = 4, .next_frame = 5});
    }
    expect(wait_owner() == Owner::normal, "owner annotation does not leak between callbacks");
    production.set_enabled(false);
    const auto real = export_json(production);
    expect(real["retained"] == 2 && real["events"][0]["thread_id"].get<uint32_t>() != 0 &&
        real["events"][0]["wait_owner"] == "wait_only_worker" && real["events"][1]["wait_owner"] == "array_preacquire_worker",
        "production spans complete once and retain actual thread/immutable worker operation");
}
}
