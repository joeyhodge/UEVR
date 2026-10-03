#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

namespace uevr::cvar_diagnostics {

enum class WriteState {
    idle, queued, awaiting_readback, observed, not_observed,
    changed_after_observed, setter_unavailable, readback_unavailable
};

inline const char* name(WriteState state) {
    switch (state) {
    case WriteState::queued: return "queued";
    case WriteState::awaiting_readback: return "awaiting readback";
    case WriteState::observed: return "requested value observed";
    case WriteState::not_observed: return "requested value not observed";
    case WriteState::changed_after_observed: return "value changed after being observed";
    case WriteState::setter_unavailable: return "setter unavailable";
    case WriteState::readback_unavailable: return "readback unavailable";
    default: return "not edited this session";
    }
}

struct WriteObservation {
    static constexpr uint32_t sample_limit = 4;
    static constexpr int64_t sample_interval_ms = 100;

    uint64_t request_id{};
    WriteState state{WriteState::idle};
    double requested{};
    std::optional<double> actual{};
    bool floating_point{};
    bool saw_requested_value{};
    bool pending{};
    uint32_t samples{};
    int64_t last_sample_ms{};

    uint64_t begin(double value, bool floating) {
        const auto next = request_id + 1;
        *this = {};
        request_id = next;
        requested = value;
        floating_point = floating;
        state = WriteState::queued;
        return request_id;
    }

    bool dispatched(uint64_t id, bool callable, int64_t now_ms) {
        if (id != request_id || state != WriteState::queued) {
            return false;
        }
        state = callable ? WriteState::awaiting_readback : WriteState::setter_unavailable;
        pending = callable;
        last_sample_ms = now_ms;
        return pending;
    }

    bool due(int64_t now_ms) const {
        return pending && now_ms - last_sample_ms >= sample_interval_ms;
    }

    bool observe(uint64_t id, std::optional<double> value, int64_t now_ms) {
        if (id != request_id || !due(now_ms)) {
            return false;
        }
        last_sample_ms = now_ms;
        actual = value && std::isfinite(*value) ? value : std::nullopt;
        ++samples;
        pending = samples < sample_limit;

        // Readback proves an observation, not why Unreal accepted/rejected a write.
        if (!actual || !std::isfinite(requested)) {
            state = pending ? WriteState::awaiting_readback : WriteState::readback_unavailable;
        } else {
            const auto tolerance = floating_point
                ? 1.0e-5 * (std::max)({1.0, std::abs(requested), std::abs(*actual)}) : 0.0;
            if (std::abs(*actual - requested) <= tolerance) {
                saw_requested_value = true;
                state = WriteState::observed;
            } else if (saw_requested_value) {
                state = WriteState::changed_after_observed;
            } else {
                state = pending ? WriteState::awaiting_readback : WriteState::not_observed;
            }
        }
        return true;
    }
};

inline nlohmann::json to_json(const WriteObservation& value) {
    return {
        {"request_id", value.request_id}, {"state", name(value.state)},
        {"requested", std::isfinite(value.requested) ? nlohmann::json(value.requested) : nlohmann::json(nullptr)},
        {"actual", value.actual ? nlohmann::json(*value.actual) : nlohmann::json(nullptr)},
        {"readback_pending", value.pending}, {"samples", value.samples},
        {"last_sample_steady_ms", value.last_sample_ms},
        {"setter_priority_changed", false},
    };
}

} // namespace uevr::cvar_diagnostics
