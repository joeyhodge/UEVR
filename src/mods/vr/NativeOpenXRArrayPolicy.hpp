#pragma once

#include <cstdint>

namespace uevr::native_array {
struct Scope {
    bool requested{}, native_fix{}, same_pass{}, d3d12{}, openxr{}, native{}, afr{}, excluded{};
    bool enabled() const noexcept {
        return requested && native_fix && !same_pass && d3d12 && openxr && native && !afr && !excluded;
    }
};
struct Key {
    uintptr_t session{};
    uint32_t width{}, height{};
    bool operator==(const Key&) const = default;
    bool valid() const noexcept { return session && width && height && width <= 32768 && height <= 32768; }
};
struct FrameKey {
    uintptr_t session{};
    uint64_t generation{}, wait_sequence{};
    int64_t display_time{};
    bool operator==(const FrameKey&) const = default;
    bool valid() const noexcept { return session != 0 && generation != 0 && display_time > 0; }
};
inline bool can_async_wait(FrameKey request, FrameKey current, bool running, bool synced, bool began) noexcept {
    return request.valid() && request == current && running && !synced && !began;
}
inline bool can_pre_acquire(FrameKey request, FrameKey current, bool running, bool synced, bool began, bool render) noexcept {
    return request.valid() && request == current && running && synced && !began && render;
}
inline bool current_copy(bool ready, bool afr, bool mono, int64_t copied_time, int64_t display_time) noexcept {
    return ready && !afr && !mono && display_time > 0 && copied_time == display_time;
}
inline bool copy_enqueued(bool recorded, bool ready, bool waiting, uint64_t before, uint64_t after) noexcept {
    return recorded && ready && waiting && after > before;
}

// A timed-out XR wait leaves the same image acquired, but not writable or releasable.
class ImageWait {
public:
    void acquired() noexcept { m_state = State::waiting; }
    bool waited(bool success) noexcept {
        if (!success || m_state != State::waiting) { return false; }
        m_state = State::writable;
        return true;
    }
    void released() noexcept { m_state = State::empty; }
    bool pending() const noexcept { return m_state == State::waiting; }
    bool writable() const noexcept { return m_state == State::writable; }
private:
    enum class State : uint8_t { empty, waiting, writable };
    State m_state{State::empty};
};

// A pending fence is not permission to reset an allocator or replace its sources.
template<class Commands> bool reclaim_array_commands(Commands& commands) {
    return commands.references_retired() && commands.try_wait() && commands.ready();
}

// Cleanup keeps the SAME acquisition on XR timeout, GPU backlog or release failure.
// Neither callback may block; callers retain the generation and source references.
template<class WaitImage, class Retired, class Release>
bool poll_array_release(ImageWait& image, WaitImage wait_image, Retired retired, Release release) {
    if (!image.writable() && (!image.pending() || !wait_image() || !image.writable())) { return false; }
    if (!retired() || !release()) { return false; }
    image.released();
    return true;
}

// Render/copy-mutex owned. Failure never requests a full swapchain rebuild.
// Retry only three times per request/session/extent, then require explicit re-arm.
class Retry {
public:
    bool attempt(bool requested, Key key, bool ready, uint64_t now_ns) noexcept {
        if (!requested) { m_requested = false; return false; }
        if (!m_requested || key != m_key) { m_key = key; m_attempts = 0; m_next = 0; }
        m_requested = true;
        if (ready) { succeeded(); return false; }
        if (!key.valid() || m_attempts >= 3 || now_ns < m_next) { return false; }
        ++m_attempts;
        m_next = now_ns + (m_attempts == 1 ? 250'000'000ull : 1'000'000'000ull);
        return true;
    }
    void succeeded() noexcept { m_attempts = 0; m_next = 0; }
    bool exhausted() const noexcept { return m_attempts >= 3; }
private:
    Key m_key{};
    uint64_t m_next{};
    uint32_t m_attempts{};
    bool m_requested{};
};
}
