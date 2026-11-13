#pragma once

#include "NativeOpenXRArrayPolicy.hpp"
#include <atomic>
#include <cstdint>
#include <optional>

namespace uevr::openxr_wait {
enum class Operation : uint8_t { array_pre_acquire, wait_only };
inline constexpr bool default_enabled = false;
inline bool load_setting(std::optional<bool> saved, bool defaults) noexcept {
    return !defaults && saved.value_or(default_enabled);
}

template<class WaitOnly, class WaitFrame, class Acquire, class Cancelled>
void run_operation(Operation operation, WaitOnly wait_only, WaitFrame wait_frame, Acquire acquire, Cancelled cancelled) {
    if (cancelled()) { return; }
    if (operation == Operation::wait_only) {
        wait_only();
    } else if (const auto frame = wait_frame(); frame && !cancelled()) {
        acquire(*frame);
    }
}

enum class Block : uint8_t { none, disabled, d3d12, openxr, native, stage, pre_acquire, array, excluded };
struct Scope {
    bool requested{}, d3d12{}, openxr{}, native{}, very_late{};
    bool pre_acquire_requested{}, array_requested{}, excluded{};

    Block blocked() const noexcept {
        if (!requested) { return Block::disabled; }
        if (!d3d12) { return Block::d3d12; }
        if (!openxr) { return Block::openxr; }
        if (!native) { return Block::native; }
        if (!very_late) { return Block::stage; }
        if (pre_acquire_requested) { return Block::pre_acquire; }
        if (array_requested) { return Block::array; }
        return excluded ? Block::excluded : Block::none;
    }
    bool enabled() const noexcept { return blocked() == Block::none; }
};

inline const char* text(Block value) noexcept {
    switch (value) {
    case Block::disabled: return "off: disabled for this game";
    case Block::d3d12: return "blocked: D3D12 required";
    case Block::openxr: return "blocked: OpenXR required";
    case Block::native: return "blocked: exact Native Stereo required";
    case Block::stage: return "blocked: Very Late synchronization required";
    case Block::pre_acquire: return "blocked: existing wait/pre-acquire selected";
    case Block::array: return "blocked: turn texture-array submit off for wait-only";
    case Block::excluded: return "blocked: compatibility/transition wait ownership";
    default: return "ready: waiting for a post-present frame boundary";
    }
}

enum class Progress : uint8_t { idle, waiting_frame, normal_pacing, pose_ahead, queued, waiting, active, cooperative, failed };
inline const char* text(Progress value) noexcept {
    switch (value) {
    case Progress::waiting_frame: return "waiting: first valid submitted XR frame";
    case Progress::normal_pacing: return "fallback: next wait not admissible at this frame boundary";
    case Progress::pose_ahead: return "fallback: next frame already has published poses";
    case Progress::queued: return "queued: next frame wait reserved";
    case Progress::waiting: return "waiting: xrWaitFrame is running on the worker";
    case Progress::active: return "active: last frame wait completed on the worker";
    case Progress::cooperative: return "fallback: normal consumer completed the frame wait";
    case Progress::failed: return "fell back: wait failed; toggle off/on after runtime recovery";
    default: return "ready: waiting for a post-present frame boundary";
    }
}

struct Reservation {
    uint64_t serial{};
    native_array::FrameKey before{};
    uint32_t next_frame{};
    bool operator==(const Reservation&) const = default;
};

// Mutators are serialized by the owning runtime's sync_mtx. The pending bit and
// UI counters are atomic so the feature-OFF pose path never needs that lock.
class Handoff {
public:
    bool pending() const noexcept { return m_pending.load(std::memory_order_acquire) != 0; }
    Progress progress() const noexcept { return m_progress.load(std::memory_order_acquire); }
    uint64_t worker_waits() const noexcept { return m_worker_waits.load(std::memory_order_relaxed); }
    uint64_t cooperative_waits() const noexcept { return m_cooperative_waits.load(std::memory_order_relaxed); }

    std::optional<Reservation> reserve(native_array::FrameKey before, uint32_t next_frame, bool posed) noexcept {
        if (pending() || progress() == Progress::failed) { return std::nullopt; }
        if (!before.valid() || !next_frame) { waiting_frame(); return std::nullopt; }
        if (posed) { m_progress.store(Progress::pose_ahead, std::memory_order_release); return std::nullopt; }
        if (++m_serial == 0) { ++m_serial; }
        m_reservation = {m_serial, before, next_frame};
        m_worker = false;
        m_progress.store(Progress::queued, std::memory_order_release);
        m_pending.store(m_serial, std::memory_order_release);
        return m_reservation;
    }
    bool owns(const Reservation& value) const noexcept {
        return pending() && m_pending.load(std::memory_order_relaxed) == value.serial && value == m_reservation;
    }
    bool begin_worker(const Reservation& value) noexcept {
        if (!owns(value)) { return false; }
        m_worker = true;
        m_progress.store(Progress::waiting, std::memory_order_release);
        return true;
    }
    bool applies_to(uint32_t frame) const noexcept {
        return pending() && (frame == 0 || static_cast<int32_t>(frame - m_reservation.next_frame) >= 0);
    }
    Reservation reservation() const noexcept { return m_reservation; }

    bool observe(native_array::FrameKey published) noexcept {
        if (!pending()) { return false; }
        const auto& before = m_reservation.before;
        if (published.session != before.session || published.generation != before.generation) { reset(); return false; }
        if (published.wait_sequence <= before.wait_sequence || published.display_time <= before.display_time) { return false; }
        if (m_worker) { ++m_worker_waits; } else { ++m_cooperative_waits; }
        m_progress.store(m_worker ? Progress::active : Progress::cooperative, std::memory_order_release);
        m_worker = false;
        m_pending.store(0, std::memory_order_release);
        return true;
    }
    void fail(const Reservation& value) noexcept {
        if (!owns(value)) { return; }
        m_worker = false;
        m_progress.store(Progress::failed, std::memory_order_release);
        m_pending.store(0, std::memory_order_release);
    }
    void waiting_frame() noexcept {
        if (!pending() && progress() != Progress::failed) { m_progress.store(Progress::waiting_frame, std::memory_order_release); }
    }
    void normal_pacing() noexcept {
        if (!pending() && progress() != Progress::failed) { m_progress.store(Progress::normal_pacing, std::memory_order_release); }
    }
    void rearm() noexcept {
        if (!pending() && progress() == Progress::failed) { m_progress.store(Progress::idle, std::memory_order_release); }
    }
    void reset() noexcept {
        m_worker = false;
        m_progress.store(Progress::idle, std::memory_order_release);
        m_pending.store(0, std::memory_order_release);
    }
private:
    Reservation m_reservation{};
    uint64_t m_serial{};
    bool m_worker{};
    std::atomic_uint64_t m_pending{}, m_worker_waits{}, m_cooperative_waits{};
    std::atomic<Progress> m_progress{Progress::idle};
};
}
