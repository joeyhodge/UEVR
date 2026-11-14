#pragma once
#include "utility/DiagnosticSnapshot.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace diagnostic_snapshot_tests {
struct Packet {
    uint64_t sequence{}, inverse{};
    std::array<uint64_t, 8> values{};
};
inline Packet packet(uint64_t n) {
    Packet result{n, ~n};
    for (size_t i{}; i < result.values.size(); ++i) { result.values[i] = n + i; }
    return result;
}
inline bool valid(const Packet& value) {
    if (value.inverse != ~value.sequence) { return false; }
    for (size_t i{}; i < value.values.size(); ++i) {
        if (value.values[i] != value.sequence + i) { return false; }
    }
    return true;
}

struct ControlledMutex {
    static inline bool busy{};
    static inline size_t attempts{}, unlocks{}, blocking_calls{};
    bool try_lock() { ++attempts; return !busy; }
    void unlock() { ++unlocks; }
    void lock() { ++blocking_calls; throw std::logic_error{"diagnostics must not wait"}; }
};

template<class Expect> void run(const Expect& expect) {
    using utility::diagnostics::Snapshot;
    Snapshot<Packet, ControlledMutex> controlled;
    ControlledMutex::busy = false;
    ControlledMutex::attempts = ControlledMutex::unlocks = ControlledMutex::blocking_calls = 0;
    const auto cold = controlled.try_read();
    expect(!cold.value && !cold.busy, "unpublished diagnostics are unavailable, not a fabricated empty runtime");
    expect(controlled.try_publish(packet(7)), "a free diagnostic slot accepts a complete value");
    const auto warm = controlled.try_read();
    expect(warm.value && valid(*warm.value) && warm.value->sequence == 7 && !warm.busy,
        "published diagnostics retain coherent copied values");
    const auto unlocks = ControlledMutex::unlocks;
    ControlledMutex::busy = true;
    const auto busy = controlled.try_read();
    expect(!busy.value && busy.busy && !controlled.try_publish(packet(8)) && !controlled.try_clear(),
        "busy diagnostic readers, publishers and retirement all skip rather than wait");
    expect(ControlledMutex::blocking_calls == 0 && ControlledMutex::unlocks == unlocks,
        "no diagnostic path calls blocking lock or unlocks an unowned mutex");
    ControlledMutex::busy = false;
    const auto retained = controlled.try_read();
    expect(retained.value && retained.value->sequence == 7,
        "a skipped publication cannot partly overwrite or refresh the old packet");
    expect(controlled.try_clear() && !controlled.try_read().value,
        "collection reset retires the prior packet without retaining raw resources");
    expect(ControlledMutex::attempts == ControlledMutex::unlocks + 3,
        "every successfully acquired diagnostic lock is released exactly once");

    Snapshot<Packet> shared;
    std::barrier start{3};
    std::atomic<bool> intact{true};
    const auto reader = [&] {
        start.arrive_and_wait();
        for (size_t i{}; i < 30000; ++i) {
            const auto read = shared.try_read();
            if (read.value && (!valid(*read.value) || read.busy)) { intact.store(false); }
        }
    };
    std::jthread a{reader}, b{reader};
    start.arrive_and_wait();
    for (uint64_t i = 1; i < 30000; ++i) {
        shared.try_publish(packet(i));
        if ((i & 255) == 0) { shared.try_clear(); }
    }
    a.join(); b.join();
    expect(intact.load(), "concurrent optional consumers never see a torn render-thread packet");
    expect(shared.try_publish(packet(30000)), "a skipped attempt does not poison later publication");
    const auto final = shared.try_read();
    expect(final.value && valid(*final.value) && final.value->sequence == 30000,
        "a complete current observation remains readable after contention");
}
}
