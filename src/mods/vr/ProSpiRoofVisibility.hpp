#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace sdk { class UObjectBase; }
namespace uevr::prospi::roof {
enum class Status : uint8_t { off, waiting, active, unsupported };
inline const char* status_name(Status status) {
    switch (status) {
    case Status::off: return "off";
    case Status::waiting: return "waiting for stadium geometry";
    case Status::active: return "active";
    default: return "unsupported layout/setter (unchanged)";
    }
}

class VisibilityGuard {
public:
    VisibilityGuard() = default;
    ~VisibilityGuard();
    // Called only by the game-thread viewport Draw callback, before rendering.
    void update(sdk::UObjectBase* viewport, bool enabled) noexcept;
    Status status() const noexcept { return m_status.load(std::memory_order_relaxed); }
    uint32_t count() const noexcept { return m_count.load(std::memory_order_relaxed); }
    uint64_t overrides() const noexcept { return m_overrides.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
    std::atomic<Status> m_status{Status::off};
    std::atomic<uint32_t> m_count{};
    std::atomic<uint64_t> m_overrides{};
};
}
