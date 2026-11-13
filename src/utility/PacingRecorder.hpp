#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace utility::pacing {
inline uint64_t next_runtime_id() noexcept {
    static std::atomic<uint64_t> sequence{};
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
enum class Stage : uint8_t {
    wait_frame, begin_frame, end_frame, wait_decision, present, post_present,
    d3d11_frame, d3d12_frame, ui_copy, scene_copy, spectator, marker, wait_poses, count
};
enum class Owner : uint8_t { normal, wait_only_worker, array_worker, normal_consumer };
enum class Domain : uint8_t { unknown, engine, xr_internal, uevr_render, next_engine };
enum class Reason : uint8_t {
    none, frame_lock_busy, assignment_busy, first_frame, normal_pacing,
    pose_ahead, reserved, reservation_busy, worker_busy, cancelled, stale_request,
    exception, reinitializing, valid_poses_required, submit_frame_required,
    frame_already_began, frame_already_synced, count
};

struct Event {
    uint64_t sequence{}, epoch{}, start_ns{}, duration_ns{}, runtime_id{}, session_generation{}, wait_sequence{};
    int64_t display_time{};
    uint32_t frame{}, thread{}, callsite{};
    int32_t result{};
    Stage stage{};
    Owner owner{};
    Domain domain{};
    Reason reason{};
    bool result_known{};
};
static_assert(std::is_trivially_copyable_v<Event>);

inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Single-attempt gate, never a spin lock. Diagnostic loss cannot delay rendering.
struct TryGate {
    bool try_lock() noexcept { return !m_flag.test_and_set(std::memory_order_acquire); }
    void unlock() noexcept { m_flag.clear(std::memory_order_release); }
private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

template<size_t Capacity = 4096, class Gate = TryGate> class Recorder {
    static_assert(Capacity > 0);
public:
    struct Summary {
        uint64_t epoch{}, total{}, overwritten{}, dropped_lifetime{};
        size_t retained{};
        bool enabled{};
    };
    struct Snapshot : Summary { std::array<Event, Capacity> events{}; };
    uint64_t control() const noexcept {
        const auto value = m_control.load(std::memory_order_acquire);
        return (value & 1) ? value : 0;
    }
    bool enabled() const noexcept { return control() != 0; }
    void set_enabled(bool enabled) noexcept {
        auto previous = m_control.load(std::memory_order_acquire);
        for (;;) {
            if (bool(previous & 1) == enabled) { return; }
            const auto next = enabled ? ((previous >> 1) + 1) * 2 + 1 : previous & ~uint64_t{1};
            if (m_control.compare_exchange_weak(previous, next, std::memory_order_acq_rel)) { return; }
        }
    }
    bool record(uint64_t token, Event event) noexcept {
        if (!active(token)) { return false; }
        Guard lock{m_gate};
        if (!lock) { m_dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        if (!active(token)) { return false; }
        if (m_epoch != token >> 1) {
            m_epoch = token >> 1;
            m_next = m_count = 0;
            m_total = m_overwritten = 0;
        }
        event.epoch = m_epoch;
        event.sequence = ++m_total;
        m_events[m_next] = event;
        m_next = (m_next + 1) % Capacity;
        if (m_count < Capacity) { ++m_count; } else { ++m_overwritten; }
        return true;
    }
    bool summary(Summary& out) noexcept {
        Guard lock{m_gate};
        if (!lock) { return false; }
        summary_locked(out);
        return true;
    }
    // Snapshot allocation and JSON formatting belong to the UI/export caller.
    bool snapshot(Snapshot& out) noexcept {
        Guard lock{m_gate};
        if (!lock) { return false; }
        summary_locked(out);
        const auto first = (m_next + Capacity - out.retained) % Capacity;
        for (size_t i{}; i < out.retained; ++i) { out.events[i] = m_events[(first + i) % Capacity]; }
        return true;
    }
private:
    struct Guard {
        Gate& gate;
        bool held;
        explicit Guard(Gate& value) noexcept : gate(value), held(gate.try_lock()) {}
        ~Guard() { if (held) { gate.unlock(); } }
        explicit operator bool() const noexcept { return held; }
        Guard(const Guard&) = delete;
    };
    bool active(uint64_t token) const noexcept {
        return (token & 1) && m_control.load(std::memory_order_acquire) == token;
    }
    void summary_locked(Summary& out) const noexcept {
        const auto value = m_control.load(std::memory_order_acquire);
        out = {};
        out.epoch = value >> 1;
        out.enabled = bool(value & 1);
        out.dropped_lifetime = m_dropped.load(std::memory_order_relaxed);
        if (out.epoch == m_epoch) {
            out.total = m_total;
            out.overwritten = m_overwritten;
            out.retained = m_count;
        }
    }
    Gate m_gate;
    std::atomic<uint64_t> m_control{}, m_dropped{};
    std::array<Event, Capacity> m_events{};
    uint64_t m_epoch{}, m_total{}, m_overwritten{};
    size_t m_next{}, m_count{};
};

Recorder<>& recorder() noexcept;
Owner wait_owner() noexcept;
class ScopedOwner {
public:
    explicit ScopedOwner(Owner owner) noexcept;
    ~ScopedOwner();
    ScopedOwner(const ScopedOwner&) = delete;
private:
    Owner m_previous;
};

class ScopedEvent {
public:
    explicit ScopedEvent(Stage stage, Domain domain = Domain::unknown, uint32_t frame = 0) noexcept;
    ~ScopedEvent() { finish(); }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
    Event& data() noexcept { return m_event; }
    void finish() noexcept;
    void finish_at(uint64_t end_ns, int32_t result) noexcept;
    void finish(int32_t result) noexcept { m_event.result = result; m_event.result_known = true; finish(); }
private:
    uint64_t m_token{};
    Event m_event{};
};
struct Context {
    uint64_t runtime_id{}, session_generation{}, wait_sequence{};
    int64_t display_time{};
    uint32_t next_frame{};
};
void decision(Reason reason, Context context = {}) noexcept;

const char* name(Stage value) noexcept;
const char* name(Owner value) noexcept;
const char* name(Domain value) noexcept;
const char* name(Reason value) noexcept;
}
