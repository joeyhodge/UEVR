#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace utility::logging {
// Thread-confined, value-only log suppression. Never use it to authorize a
// resource or skip validation; callers keep their existing time-rate limit.
template<size_t Values, size_t Channels = 16>
class DiagnosticChanges {
public:
    bool should_log(bool changes_only, uintptr_t channel, const std::array<uint64_t, Values>& values) {
        if (!changes_only) { return true; }
        Entry* unused{};
        for (auto& entry : m_entries) {
            if (!entry.used) {
                if (!unused) { unused = &entry; }
            } else if (entry.channel == channel) {
                if (entry.values == values) { return false; }
                entry.values = values;
                return true;
            }
        }
        if (!unused) { return false; }
        *unused = {channel, values, true};
        return true;
    }
private:
    struct Entry { uintptr_t channel{}; std::array<uint64_t, Values> values{}; bool used{}; };
    std::array<Entry, Channels> m_entries{};
};
} // namespace utility::logging
