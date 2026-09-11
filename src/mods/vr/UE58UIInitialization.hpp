#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace uevr::ue58_ui {

enum class Source { None, Slate, PreRender };
enum class Stage { Idle, GameQueued, Creating, ResourceQueued, Executing, Validated, Published, RetryWait, Cancelled };

constexpr const char* to_string(Source source) noexcept {
    switch (source) {
    case Source::Slate: return "Slate";
    case Source::PreRender: return "PreRenderViewFamily";
    default: return "none";
    }
}

constexpr const char* to_string(Stage stage) noexcept {
    switch (stage) {
    case Stage::GameQueued: return "game queued";
    case Stage::Creating: return "creating UObject";
    case Stage::ResourceQueued: return "resource queued";
    case Stage::Executing: return "validating";
    case Stage::Validated: return "validated";
    case Stage::Published: return "published";
    case Stage::RetryWait: return "retry cooldown";
    case Stage::Cancelled: return "cancelled";
    default: return "idle";
    }
}

struct Snapshot {
    bool engaged{};
    uint64_t generation{};
    Stage stage{Stage::Idle};
    Source queued_source{Source::None};
    Source active_source{Source::None};
    Source last_source{Source::None};
    uint32_t width{}, height{};
    uint64_t attempts{}, recoveries{}, timeouts{};
    uint64_t queued_ms{}, progress_ms{}, retry_after_ms{};
    const char* reason{"not requested"};
};

inline uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Contains no engine calls or queue locks. Creation and validation run outside
// the gate; only accepting results and publishing/cancelling are serialized.
template<class Owner>
class Initialization {
public:
    static constexpr uint64_t recovery_delay_ms = 500;
    static constexpr uint64_t timeout_ms = 2000;
    static constexpr uint64_t retry_delay_ms = 2000;

    struct Request {
        const uint64_t generation;
        const uint32_t width, height;
        std::shared_ptr<Owner> owner{};
        Request(uint64_t serial, uint32_t w, uint32_t h) : generation{serial}, width{w}, height{h} {}
    };
    using Ticket = std::shared_ptr<Request>;

    Snapshot snapshot() const {
        std::scoped_lock lock{m_gate};
        return m_state;
    }

    bool pending() const {
        std::scoped_lock lock{m_gate};
        return m_current != nullptr && m_state.stage != Stage::Published;
    }

    Ticket begin(uint32_t width, uint32_t height, Source source, uint64_t now) {
        return begin_if(width, height, source, now, [] { return true; });
    }

    template<class Predicate>
    Ticket begin_if(uint32_t width, uint32_t height, Source source, uint64_t now, Predicate&& still_needed) {
        if (width == 0 || height == 0 || source == Source::None) return {};
        std::scoped_lock lock{m_gate};
        if (m_current != nullptr || now < m_next_creation_ms || !still_needed()) return {};
        auto ticket = std::make_shared<Request>(++m_serial, width, height);
        m_current = ticket;
        m_state = {true, ticket->generation, Stage::GameQueued, source, source, Source::None,
            width, height, 0, 0, 0, now, now, 0, "waiting for game thread"};
        return ticket;
    }

    Ticket acquire_creation(uint64_t now) {
        std::unique_lock lock{m_gate, std::try_to_lock};
        if (!lock || !m_current || m_state.stage != Stage::GameQueued) return {};
        m_state.stage = Stage::Creating;
        m_state.progress_ms = now;
        m_state.reason = "creating UI owner";
        return m_current;
    }

    bool created(const Ticket& ticket, std::shared_ptr<Owner> owner, uint64_t now) {
        std::scoped_lock lock{m_gate};
        if (!owner || !current(ticket) || m_state.stage != Stage::Creating) return false;
        ticket->owner = std::move(owner);
        m_state.stage = Stage::ResourceQueued;
        m_state.queued_ms = m_state.progress_ms = m_round_started_ms = now;
        m_state.reason = "waiting for render callback";
        return true;
    }

    void creation_failed(const Ticket& ticket, const char* reason, uint64_t now) {
        retire_failed(ticket, Stage::Creating, reason, now);
    }

    void owner_lost(const Ticket& ticket, uint64_t now) {
        retire_failed(ticket, Stage::Executing, "UI owner is no longer valid", now);
    }

private:
    void retire_failed(const Ticket& ticket, Stage expected, const char* reason, uint64_t now) {
        Ticket retired;
        {
            std::scoped_lock lock{m_gate};
            if (!current(ticket) || m_state.stage != expected) return;
            retired = std::move(m_current);
            m_state.stage = Stage::RetryWait;
            m_state.reason = reason;
            m_state.retry_after_ms = m_next_creation_ms = now + retry_delay_ms;
        }
    }

public:
    Ticket acquire(Source source, uint64_t now) {
        std::unique_lock lock{m_gate, std::try_to_lock};
        if (!lock || !m_current || !m_current->owner || source == Source::None) return {};
        if (m_state.stage != Stage::ResourceQueued && m_state.stage != Stage::RetryWait) return {};
        if (now < m_state.retry_after_ms) return {};
        if (source != m_state.active_source) {
            if (now < m_state.progress_ms || now - m_state.progress_ms < recovery_delay_ms) return {};
            m_state.active_source = source;
            ++m_state.recoveries;
        }
        if (m_state.stage == Stage::RetryWait) m_round_started_ms = now;
        m_state.stage = Stage::Executing;
        m_state.last_source = source;
        m_state.progress_ms = now;
        m_state.reason = "validating resource";
        ++m_state.attempts;
        return m_current;
    }

    void retry(const Ticket& ticket, const char* reason, uint64_t now) {
        std::scoped_lock lock{m_gate};
        if (!current(ticket) || m_state.stage != Stage::Executing) return;
        m_state.progress_ms = now;
        m_state.reason = reason;
        m_state.stage = Stage::ResourceQueued;
        if (now >= m_round_started_ms && now - m_round_started_ms >= timeout_ms) cooldown(now, reason);
    }

    // The watchdog only changes scheduling state. It never unroots an object,
    // starts engine work, or steals a validation already executing elsewhere.
    bool watchdog(uint64_t now) {
        std::unique_lock lock{m_gate, std::try_to_lock};
        if (!lock || !m_current || m_state.stage != Stage::ResourceQueued) return false;
        if (now < m_state.progress_ms || now - m_state.progress_ms < timeout_ms) return false;
        cooldown(now, "no render callback progress");
        return true;
    }

    template<class Publish>
    bool publish(const Ticket& ticket, uint64_t now, Publish&& publish_target) {
        std::scoped_lock lock{m_gate};
        if (!current(ticket) || m_state.stage != Stage::Executing) return false;
        m_state.stage = Stage::Validated;
        try {
            if (!publish_target()) {
                cooldown(now, "publication no longer eligible");
                return false;
            }
        } catch (...) {
            cooldown(now, "publication failed");
            throw;
        }
        m_state.stage = Stage::Published;
        m_state.progress_ms = now;
        m_state.reason = "ready";
        return true;
    }

    template<class Mutation>
    void cancel(const char* reason, Mutation&& mutate_target) {
        cancel_if([] { return true; }, reason, std::forward<Mutation>(mutate_target));
    }

    template<class Predicate, class Mutation>
    bool cancel_if(Predicate&& should_cancel, const char* reason, Mutation&& mutate_target) {
        Ticket retired;
        {
            std::scoped_lock lock{m_gate};
            if (!should_cancel()) return false;
            retired = std::move(m_current);
            m_next_creation_ms = 0;
            if (m_state.engaged) {
                m_state.stage = Stage::Cancelled;
                m_state.reason = reason;
            }
            mutate_target();
        }
        // Owner retirement is outside the gate; cleanup may need its own lock.
        return true;
    }

private:
    bool current(const Ticket& ticket) const noexcept {
        return ticket && m_current == ticket;
    }
    void cooldown(uint64_t now, const char* reason) {
        m_state.stage = Stage::RetryWait;
        m_state.reason = reason;
        m_state.retry_after_ms = now + retry_delay_ms;
        ++m_state.timeouts;
    }
    mutable std::mutex m_gate{};
    Ticket m_current{};
    Snapshot m_state{};
    uint64_t m_serial{}, m_next_creation_ms{}, m_round_started_ms{};
};

} // namespace uevr::ue58_ui
