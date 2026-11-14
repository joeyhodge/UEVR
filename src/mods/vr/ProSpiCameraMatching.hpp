#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace uevr::prospi::matching {

template <typename Calibration>
class PublishedCalibrations {
public:
    using Entries = std::vector<Calibration>;
    using Snapshot = std::shared_ptr<const Entries>;

    // Writers serialize edits and publication with the calibration map's mutex.
    bool publish(const auto& calibrations) noexcept {
        try {
            auto entries = std::make_shared<Entries>();
            entries->reserve(calibrations.size());
            for (const auto& [id, calibration] : calibrations) {
                entries->push_back(calibration);
            }
            m_entries.store(std::move(entries), std::memory_order_release);
            return true;
        } catch (...) {
            // Never expose deleted/edited entries on subsequent reads after a failed rebuild.
            m_entries.store({}, std::memory_order_release);
            return false;
        }
    }

    Snapshot read() const noexcept { return m_entries.load(std::memory_order_acquire); }

private:
    std::atomic<Snapshot> m_entries{};
};

template <typename Calibration>
struct Candidate {
    const Calibration* calibration{};
    float score{}, confidence{};
};

template <typename T>
class Workspace {
public:
    class Lease {
    public:
        Lease(Workspace& owner, std::size_t capacity)
            : m_values(owner.m_in_use ? &m_nested : &owner.m_values) {
            m_values->reserve(capacity);
            if (m_values == &owner.m_values) {
                owner.m_in_use = true;
                m_owner = &owner;
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            m_values->clear();
            if (m_owner) { m_owner->m_in_use = false; }
        }
        std::vector<T>& values() noexcept { return *m_values; }

    private:
        std::vector<T> m_nested{};
        std::vector<T>* m_values{};
        Workspace* m_owner{};
    };

    // A nested update must not invalidate pointers/candidates in the outer update.
    Lease acquire(std::size_t capacity) { return Lease{*this, capacity}; }

private:
    std::vector<T> m_values{};
    bool m_in_use{};
};

} // namespace uevr::prospi::matching
