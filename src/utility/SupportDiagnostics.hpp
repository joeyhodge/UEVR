#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace utility::support {

inline nlohmann::json build_identity(
    std::string_view uevr_commit, std::string_view uesdk_commit,
    std::string_view uevr_dirty, std::string_view uesdk_dirty,
    std::string_view branch, std::string_view date, std::string_view time) {
    return {
        {"uevr_commit", uevr_commit}, {"uesdk_commit", uesdk_commit},
        {"uevr_tracked_changes", uevr_dirty}, {"uesdk_tracked_changes", uesdk_dirty},
        {"branch", branch}, {"build_date", date}, {"build_time_local", time},
    };
}

inline nlohmann::json pacing(int64_t display_period_ns, std::optional<double> frame_cap) {
    const bool have_period = display_period_ns > 0;
    const bool have_cap = frame_cap && std::isfinite(*frame_cap);
    nlohmann::json result{
        {"display_period_ns", have_period ? nlohmann::json(display_period_ns) : nlohmann::json(nullptr)},
        {"headset_refresh_hz", have_period ? nlohmann::json(1.0e9 / display_period_ns) : nlohmann::json(nullptr)},
        {"frame_cap_fps", have_cap ? nlohmann::json(*frame_cap) : nlohmann::json(nullptr)},
        {"automatic_adjustment", false},
        {"note", "A cap below refresh may be intentional for the selected mode; this is not a judder diagnosis."},
    };
    result["positive_cap_below_refresh"] = have_period && have_cap
        ? nlohmann::json(*frame_cap > 0.0 && *frame_cap < 1.0e9 / display_period_ns)
        : nlohmann::json(nullptr);
    return result;
}

} // namespace utility::support
