#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace uevr::native_frame {
enum class Backend : uint8_t { unknown, d3d11, d3d12 };
enum class Runtime : uint8_t { unknown, openxr, openvr };
enum class Stage : uint8_t { producer, selection, invalidation, copy_recorded, submit_attempt, submit_result };
enum class Reason : uint8_t {
    none, feature_off, no_packet, stale_frames, rejected_generation, no_capture,
    changed_generation, changed_resource, exact_frame, same_engine_frame, one_frame_handoff
};
enum class Shadow : uint8_t { unavailable, epoch_changed, owner_changed, legacy_changed, matches, differs };

struct Window {
    bool exact{}, same_engine{}, next_frame{};
    constexpr bool accepted() const { return exact || same_engine || next_frame; }
};

// Keep the existing signed render delta and unsigned engine wrap semantics.
constexpr Window frame_window(int64_t render_delta, uint32_t engine_delta, bool same_engine) {
    return {render_delta == 0, same_engine, render_delta == 1 && engine_delta == 1};
}

constexpr uint64_t pack_clocks(uint32_t engine, int32_t present) {
    return uint64_t(engine) << 32 | std::bit_cast<uint32_t>(present);
}
constexpr uint32_t engine_clock(uint64_t clocks) { return uint32_t(clocks >> 32); }
constexpr int32_t present_clock(uint64_t clocks) { return std::bit_cast<int32_t>(uint32_t(clocks)); }

struct ClockStamp {
    uint64_t epoch{}, clocks{};
    bool ready{}, owner_changed{};
};
struct Ticket {
    uint64_t control{}, attempt{}, packet_serial{};
    ClockStamp consumer_clock{};
    int32_t submit_render{};
    explicit operator bool() const { return (control & 1) != 0; }
};
struct Event {
    uint64_t sequence{}, timestamp_ns{}, epoch{}, attempt{}, packet_serial{}, generation{}, transaction{}, current_generation{};
    uintptr_t rhi{}, resource{}, family{}, current_rhi{}, current_resource{}, copy_source{}, copy_destination{};
    uint32_t producer_engine{};
    int32_t producer_render{}, submit_render{};
    int64_t render_delta{};
    uint32_t engine_delta{}, thread{};
    int32_t api_result{}, invalidation_state{};
    uint8_t submit_eye{2}, submit_call{};
    Backend backend{};
    Runtime runtime{};
    Stage stage{};
    Reason reason{};
    Shadow shadow{};
    bool legacy_same_engine{}, window_observed{}, legacy_window{}, shadow_window{}, resource_checks_ran{}, packet_accepted{};
    ClockStamp producer_clock{}, consumer_clock{};
};
static_assert(std::is_trivially_copyable_v<Event>);

inline void compare_window(Event& event) {
    event.shadow = Shadow::unavailable;
    event.shadow_window = false;
    if (!event.window_observed) { return; }
    event.legacy_window = frame_window(event.render_delta, event.engine_delta, event.legacy_same_engine).accepted();
    const auto& a = event.producer_clock;
    const auto& b = event.consumer_clock;
    if ((event.engine_delta == 0) != event.legacy_same_engine) {
        event.shadow = Shadow::legacy_changed;
    } else if (a.epoch == 0 || b.epoch == 0 || a.epoch != b.epoch || a.epoch != event.epoch) {
        event.shadow = Shadow::epoch_changed;
    } else if (a.owner_changed || b.owner_changed) {
        event.shadow = Shadow::owner_changed;
    } else if (a.ready && b.ready) {
        const auto render_delta = int64_t(present_clock(b.clocks)) - int64_t(present_clock(a.clocks));
        const auto engine_delta = uint32_t(engine_clock(b.clocks) - engine_clock(a.clocks));
        event.shadow_window = frame_window(render_delta, engine_delta,
            engine_clock(a.clocks) == engine_clock(b.clocks)).accepted();
        event.shadow = event.legacy_window == event.shadow_window ? Shadow::matches : Shadow::differs;
    }
}

// Observation only. No recorder result is an input to rendering or packet
// acceptance. The try-only gate bounds work and protects every plain payload
// access, including ring overwrite/export; it is not a seqlock over raced data.
template <size_t Capacity = 2048>
class Recorder {
    static_assert(Capacity > 0);
public:
    struct Snapshot {
        uint64_t epoch{}, total{}, overwritten{}, dropped{}, missed_engine{}, missed_present{};
        uint32_t engine_owner{}, present_owner{};
        size_t count{};
        bool enabled{};
        ClockStamp clock{};
        std::array<Event, Capacity> events{};
    };

    uint64_t control() const noexcept {
        const auto value = m_control.load(std::memory_order_acquire);
        return (value & 1) != 0 ? value : 0;
    }
    bool enabled() const noexcept { return control() != 0; }

    void set_enabled(bool enabled) noexcept {
        auto previous = m_control.load(std::memory_order_acquire);
        for (;;) {
            if (bool(previous & 1) == enabled) { return; }
            // Disabling retains the last trace. Re-enabling starts a fresh
            // epoch without clearing memory an in-flight writer may still own.
            const auto next = enabled ? ((previous >> 1) + 1) * 2 + 1 : previous & ~uint64_t{1};
            if (m_control.compare_exchange_weak(previous, next, std::memory_order_acq_rel)) { return; }
        }
    }

    Ticket ticket(uint64_t token, uint64_t serial) noexcept {
        if (!active(token)) { return {}; }
        return {token, m_attempt.fetch_add(1, std::memory_order_relaxed) + 1, serial};
    }

    void observe_engine(uint64_t token, uint32_t frame, uint32_t thread) noexcept {
        observe_clock(token, frame, thread, true);
    }
    void observe_present(uint64_t token, int32_t frame, uint32_t thread) noexcept {
        observe_clock(token, std::bit_cast<uint32_t>(frame), thread, false);
    }

    ClockStamp clock(uint64_t token) noexcept {
        if (!active(token)) { return {}; }
        Guard lock{m_gate};
        if (!lock || !active(token)) { return {}; }
        if (m_epoch != token >> 1) { return {token >> 1}; }
        return clock_locked();
    }

    bool record(uint64_t token, Event event) noexcept {
        if (!active(token)) { return false; }
        Guard lock{m_gate};
        if (!lock) { m_dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        if (!active(token)) { return false; }
        enter_epoch(token);
        event.epoch = token >> 1;
        event.sequence = ++m_total;
        event.timestamp_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        compare_window(event);
        m_events[m_next] = event;
        m_next = (m_next + 1) % Capacity;
        if (m_count < Capacity) { ++m_count; } else { ++m_overwritten; }
        return true;
    }

    // Allocate Snapshot on the UI/export side, never in a render callback.
    bool snapshot(Snapshot& out) noexcept {
        Guard lock{m_gate};
        if (!lock) { return false; }
        const auto current = m_control.load(std::memory_order_acquire);
        out.enabled = bool(current & 1);
        out.epoch = current >> 1;
        out.count = 0;
        out.total = out.overwritten = 0;
        out.engine_owner = out.present_owner = 0;
        out.clock = {};
        out.dropped = m_dropped.load(std::memory_order_relaxed);
        out.missed_engine = m_missed_engine.load(std::memory_order_acquire);
        out.missed_present = m_missed_present.load(std::memory_order_acquire);
        if (out.epoch != m_epoch) { return true; }
        out.total = m_total;
        out.overwritten = m_overwritten;
        out.count = m_count;
        out.engine_owner = m_engine_owner;
        out.present_owner = m_present_owner;
        out.clock = clock_locked();
        const auto first = (m_next + Capacity - m_count) % Capacity;
        for (size_t i = 0; i < m_count; ++i) { out.events[i] = m_events[(first + i) % Capacity]; }
        return true;
    }

private:
    struct Guard {
        std::atomic_flag& gate;
        bool owned;
        explicit Guard(std::atomic_flag& value) : gate(value), owned(!gate.test_and_set(std::memory_order_acquire)) {}
        ~Guard() { if (owned) { gate.clear(std::memory_order_release); } }
        explicit operator bool() const { return owned; }
        Guard(const Guard&) = delete;
    };

    bool active(uint64_t token) const noexcept {
        return (token & 1) != 0 && m_control.load(std::memory_order_acquire) == token;
    }
    void enter_epoch(uint64_t token) noexcept {
        if (m_epoch == token >> 1) { return; }
        m_epoch = token >> 1;
        m_next = m_count = 0;
        m_total = m_overwritten = 0;
        m_clocks = 0;
        m_engine_seen = m_present_seen = m_owner_changed = false;
        m_engine_owner = m_present_owner = 0;
    }
    ClockStamp clock_locked() const noexcept {
        const auto ready = m_engine_seen && m_present_seen && !m_owner_changed &&
            m_engine_miss_seen == m_missed_engine.load(std::memory_order_acquire) &&
            m_present_miss_seen == m_missed_present.load(std::memory_order_acquire);
        return {m_epoch, m_clocks, ready, m_owner_changed};
    }
    void observe_clock(uint64_t token, uint32_t value, uint32_t thread, bool engine) noexcept {
        if (!active(token)) { return; }
        auto& missed = engine ? m_missed_engine : m_missed_present;
        const auto missed_at_entry = missed.load(std::memory_order_acquire);
        Guard lock{m_gate};
        if (!lock) { missed.fetch_add(1, std::memory_order_release); return; }
        if (!active(token)) { return; }
        enter_epoch(token);
        auto& owner = engine ? m_engine_owner : m_present_owner;
        auto& seen = engine ? m_engine_seen : m_present_seen;
        auto& miss_seen = engine ? m_engine_miss_seen : m_present_miss_seen;
        if (thread == 0 || (seen && owner != thread)) { m_owner_changed = true; }
        owner = thread;
        seen = true;
        // A concurrent missed publication must not be hidden by this update.
        miss_seen = missed_at_entry;
        m_clocks = engine ? pack_clocks(value, present_clock(m_clocks))
            : pack_clocks(engine_clock(m_clocks), std::bit_cast<int32_t>(value));
    }

    std::atomic<uint64_t> m_control{}, m_attempt{}, m_dropped{}, m_missed_engine{}, m_missed_present{};
    std::atomic_flag m_gate = ATOMIC_FLAG_INIT;
    uint64_t m_epoch{}, m_clocks{}, m_total{}, m_overwritten{}, m_engine_miss_seen{}, m_present_miss_seen{};
    uint32_t m_engine_owner{}, m_present_owner{};
    bool m_engine_seen{}, m_present_seen{}, m_owner_changed{};
    size_t m_next{}, m_count{};
    std::array<Event, Capacity> m_events{};
};

inline const char* name(Backend value) {
    switch (value) { case Backend::d3d11: return "D3D11"; case Backend::d3d12: return "D3D12"; default: return "unknown"; }
}
inline const char* name(Runtime value) {
    switch (value) { case Runtime::openxr: return "OpenXR"; case Runtime::openvr: return "OpenVR"; default: return "unknown"; }
}
inline const char* name(Stage value) {
    switch (value) {
    case Stage::producer: return "producer_published";
    case Stage::selection: return "packet_selection";
    case Stage::invalidation: return "packet_invalidated";
    case Stage::copy_recorded: return "copy_recorded_not_gpu_complete";
    case Stage::submit_attempt: return "submission_attempt";
    default: return "submission_result";
    }
}
inline const char* name(Reason value) {
    switch (value) {
    case Reason::feature_off: return "native_fix_off";
    case Reason::no_packet: return "no_packet";
    case Reason::stale_frames: return "stale_frame_window";
    case Reason::rejected_generation: return "rejected_generation";
    case Reason::no_capture: return "no_current_capture";
    case Reason::changed_generation: return "changed_generation";
    case Reason::changed_resource: return "changed_resource";
    case Reason::exact_frame: return "accepted_exact_render_frame";
    case Reason::same_engine_frame: return "accepted_same_engine_grace";
    case Reason::one_frame_handoff: return "accepted_one_frame_handoff";
    default: return "not_observed";
    }
}
inline const char* name(Shadow value) {
    switch (value) {
    case Shadow::epoch_changed: return "unavailable_epoch_changed";
    case Shadow::owner_changed: return "unavailable_clock_owner_changed";
    case Shadow::legacy_changed: return "unavailable_legacy_reads_disagree";
    case Shadow::matches: return "same_frame_window_decision";
    case Shadow::differs: return "different_frame_window_decision";
    default: return "unavailable_clock_observation";
    }
}
}
