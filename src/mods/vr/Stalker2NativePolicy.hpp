#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uevr::stalker2_native {
inline bool is_executable(std::wstring_view path) {
    const auto name = path.substr(path.find_last_of(L"/\\") == path.npos ? 0 : path.find_last_of(L"/\\") + 1);
    constexpr std::wstring_view expected{L"stalker2-win64-shipping.exe"};
    return name.size() == expected.size() && std::equal(name.begin(), name.end(), expected.begin(),
        [](wchar_t a, wchar_t b) { return std::towlower(a) == b; });
}
struct Scope {
    bool requested{}, stalker_ue55{}, dx12{}, native_fix{}, openxr{}, array_submit{}, two_d{};
    constexpr bool enabled() const {
        return requested && stalker_ue55 && dx12 && native_fix && openxr && !array_submit && !two_d;
    }
};

struct Key {
    uint64_t epoch{};
    uint32_t primary{}, secondary{};
    bool valid{};
    constexpr bool well_formed() const { return valid && epoch != 0 && uint32_t(secondary - primary) <= 1; }
    constexpr bool matches(uint32_t frame, uint64_t current_epoch) const {
        return well_formed() && epoch == current_epoch && (frame == primary || frame == secondary);
    }
};
inline Key make_key(uint64_t epoch, uint32_t primary, uint32_t secondary, uint32_t delay) {
    const auto delta = uint32_t(secondary - primary);
    return {epoch, primary + delay, secondary + delay, epoch != 0 && delta <= 1};
}

// The ring owns metadata, not historical pixels. Only the newest transaction
// may reference the reusable capture; earlier images require an owned pair.
template <class Packet, size_t Capacity = 16>
class PacketRing {
    static_assert(Capacity > 1);
public:
    using Ptr = std::shared_ptr<const Packet>;
    struct Handoff { uint64_t epoch{}; uint32_t frame{}, thread{}; };
    uint64_t epoch() const { std::scoped_lock lock{m_mutex}; return m_epoch; }
    void invalidate() {
        std::scoped_lock lock{m_mutex};
        ++m_epoch;
        m_latest.reset();
        m_ring.fill(nullptr);
        m_handoff = {};
    }
    bool publish(Ptr packet) {
        std::scoped_lock lock{m_mutex};
        if (!packet || !packet->stalker_key.well_formed() || packet->stalker_key.epoch != m_epoch) { return false; }
        if (m_latest && packet->serial <= m_latest->serial) { return false; }
        m_latest = packet;
        m_ring[packet->stalker_key.primary % Capacity] = packet;
        m_ring[packet->stalker_key.secondary % Capacity] = packet;
        return true;
    }
    Ptr select(uint32_t frame) const {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        if (!lock) { return nullptr; }
        const auto packet = m_ring[frame % Capacity];
        return packet && packet == m_latest && packet->stalker_key.matches(frame, m_epoch) ? packet : nullptr;
    }
    Ptr latest() const {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        return lock ? m_latest : nullptr;
    }
    void note_handoff(uint32_t frame, uint32_t thread) {
        std::scoped_lock lock{m_mutex}; m_handoff = {m_epoch, frame, thread};
    }
    Handoff handoff() const {
        std::unique_lock lock{m_mutex, std::try_to_lock};
        return lock ? m_handoff : Handoff{};
    }
    // A failure from an older slot quarantines its whole CURRENT generation.
    // A delayed failure from a superseded generation must not poison the new one.
    bool reject(const Ptr& packet, uint64_t current_generation) {
        std::scoped_lock lock{m_mutex};
        if (!packet || packet->stalker_key.epoch != m_epoch) { return false; }
        for (auto& slot : m_ring) {
            if (slot && slot->capture_generation == packet->capture_generation) { slot.reset(); }
        }
        if (packet->capture_generation != current_generation ||
            (m_latest && m_latest->capture_generation != current_generation)) { return false; }
        ++m_epoch;
        m_ring.fill(nullptr);
        m_latest.reset();
        m_handoff = {};
        return true;
    }
private:
    mutable std::mutex m_mutex;
    uint64_t m_epoch{1};
    Ptr m_latest;
    Handoff m_handoff{};
    std::array<Ptr, Capacity> m_ring{};
};

struct PairIdentity {
    uint64_t epoch{}, generation{}, serial{};
    uintptr_t device{}, queue{}, left{}, right{}, scene{}, target{};
    uint32_t source_frame{};
};
inline bool same_session(const PairIdentity& a, const PairIdentity& b) {
    return a.epoch != 0 && a.generation != 0 && a.epoch == b.epoch && a.generation == b.generation &&
        a.device != 0 && a.device == b.device && a.queue != 0 && a.queue == b.queue &&
        a.left != 0 && a.left == b.left && a.right != 0 && a.right == b.right &&
        a.scene != 0 && a.scene == b.scene && a.target != 0 && a.target == b.target;
}
struct PairValidity {
    PairIdentity identity{};
    int64_t submitted_ms{};
    uint32_t reuses{};
    bool submitted{};
    void invalidate() { *this = {}; }
    void commit(PairIdentity value, int64_t now, bool enqueued, bool released) {
        invalidate();
        if (enqueued && released && same_session(value, value) && value.serial != 0 && now >= 0) {
            identity = value; submitted_ms = now; submitted = true;
        }
    }
    bool reusable(const PairIdentity& current, uint32_t frame, int64_t now) const {
        return submitted && same_session(identity, current) && reuses < 3 &&
            uint32_t(frame - identity.source_frame) <= 3 && now >= submitted_ms && now - submitted_ms <= 100;
    }
};

inline bool fence_retired(uint64_t completed, uint64_t submitted) {
    return submitted != 0 && completed != (std::numeric_limits<uint64_t>::max)() && completed >= submitted;
}
struct SubmissionProof {
    uint64_t fence_before{}, fence_after{};
    bool recorded{}, healthy{}, waiting{}, has_fence{}, ordered_queue{}, same_device{};
    constexpr bool confirmed() const {
        return recorded && healthy && waiting && has_fence && ordered_queue && same_device &&
            fence_after > fence_before && fence_after != (std::numeric_limits<uint64_t>::max)();
    }
};
inline bool valid_pair_bounds(uint64_t left_width, uint32_t left_height, uint64_t right_width,
    uint32_t right_height, uint64_t dst_width, uint32_t dst_height, uint32_t eye_width, uint32_t eye_height) {
    return eye_width != 0 && eye_height != 0 && left_width >= eye_width && left_height >= eye_height &&
        right_width == eye_width && right_height == eye_height &&
        dst_width == uint64_t(eye_width) * 2 && dst_height == eye_height;
}

inline bool close_float(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) &&
        std::abs(a - b) <= std::numeric_limits<float>::epsilon() * 4 * (std::max)(1.0f, std::abs(b));
}
inline std::wstring float_text(float value) {
    if (!std::isfinite(value)) { return {}; }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    return result.ec == std::errc{} ? std::wstring(buffer.data(), result.ptr) : std::wstring{};
}
inline bool flags_accessor(std::span<const uint8_t> code) {
    // Source-backed UE5.5 GetFlags: validate the accessor before calling it.
    return code.size() >= 4 && code[0] == 0x8b && code[1] == 0x41 && code[2] == 0x18 && code[3] == 0xc3;
}
struct SharpenPriority {
    uint32_t mismatches{}, priority{};
    uintptr_t object{};
    float requested{};
    std::optional<uint32_t> observe(bool enabled, uintptr_t variable, uint32_t flags, float wanted, float actual) {
        const auto current = flags & 0xff000000;
        if (!enabled || !variable || (current != 0x0c000000 && current != 0x0d000000) ||
            (flags & (0x1 | 0x4 | 0x8 | 0x10)) != 0 || !std::isfinite(wanted) || !std::isfinite(actual)) {
            *this = {}; return std::nullopt;
        }
        if (object != variable || priority != current || !close_float(requested, wanted)) {
            *this = {0, current, variable, wanted};
        }
        if (close_float(wanted, actual)) { mismatches = 0; return std::nullopt; }
        if (++mismatches < 3) { return std::nullopt; }
        mismatches = 0;
        // Match the observed game priority, never elevate it to Console. There
        // is no sticky priority to undo and the game's equal-priority writes work.
        return current;
    }
};
} // namespace uevr::stalker2_native
