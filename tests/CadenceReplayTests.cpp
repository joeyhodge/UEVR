#include <cstdint>
#include <iostream>

namespace {
// Evidence/replay model of the existing Native Fix acceptance rules. Production
// frame counters, parity, grace windows and submit timing are not changed here.
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
    return (render_delta == 0 || engine_delta == 0 || (render_delta == 1 && engine_delta == 1)) &&
        value.packet_generation == value.current_generation && value.packet_resource != 0 &&
        value.packet_resource == value.current_resource && !value.rejected_generation;
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
    return failures;
}
