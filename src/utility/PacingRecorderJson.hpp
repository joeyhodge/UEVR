#pragma once

#include "PacingRecorder.hpp"
#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>

namespace utility::pacing {
template<size_t Capacity, class Gate>
nlohmann::json export_json(Recorder<Capacity, Gate>& source) {
    auto trace = std::make_unique<typename Recorder<Capacity, Gate>::Snapshot>();
    if (!source.snapshot(*trace)) {
        return {{"schema_version", 1}, {"diagnostic_only", true}, {"snapshot_busy", true}};
    }
    auto rows = nlohmann::json::array();
    auto stages = nlohmann::json::object();
    auto decisions = nlohmann::json::object();
    for (size_t i{}; i < trace->retained; ++i) {
        const auto& e = trace->events[i];
        const auto milliseconds = static_cast<double>(e.duration_ns) / 1.0e6;
        auto& stage = stages[name(e.stage)];
        if (stage.is_null()) { stage = {{"count", 0}, {"total_cpu_ms", 0.0}, {"max_cpu_ms", 0.0}}; }
        stage["count"] = stage["count"].template get<uint64_t>() + 1;
        stage["total_cpu_ms"] = stage["total_cpu_ms"].template get<double>() + milliseconds;
        stage["max_cpu_ms"] = (std::max)(stage["max_cpu_ms"].template get<double>(), milliseconds);
        if (e.stage == Stage::wait_decision) {
            auto& count = decisions[name(e.reason)];
            count = count.is_null() ? 1 : count.template get<uint64_t>() + 1;
        }
        rows.push_back({{"sequence", e.sequence}, {"epoch", e.epoch}, {"start_steady_ns", e.start_ns},
            {"duration_cpu_ms", milliseconds}, {"thread_id", e.thread}, {"stage", name(e.stage)},
            {"wait_owner", name(e.owner)}, {"reason", name(e.reason)}, {"frame_domain", name(e.domain)},
            {"frame", e.domain != Domain::unknown ? nlohmann::json(e.frame) : nlohmann::json(nullptr)},
            {"runtime_id", e.runtime_id ? nlohmann::json(e.runtime_id) : nlohmann::json(nullptr)},
            {"session_generation", e.session_generation}, {"observed_runtime_wait_sequence", e.wait_sequence},
            {"predicted_display_time", e.display_time ? nlohmann::json(e.display_time) : nlohmann::json(nullptr)},
            {"callsite_id", e.callsite}, {"api_result", e.result_known ? nlohmann::json(e.result) : nlohmann::json(nullptr)}});
    }
    return {{"schema_version", 1}, {"diagnostic_only", true}, {"snapshot_busy", false},
        {"enabled", trace->enabled}, {"epoch", trace->epoch}, {"capacity", Capacity}, {"retained", trace->retained},
        {"total", trace->total}, {"overwritten", trace->overwritten}, {"dropped_lifetime", trace->dropped_lifetime},
        {"gpu_timing_available", false}, {"changes_frame_pacing", false},
        {"note", "CPU intervals only, including blocking API time. Nested intervals overlap; do not sum stages or infer GPU cost. Events are completion-ordered, not start-time ordered. Missing events are not zero-cost frames. The observed wait sequence is the runtime clock, not a guarantee that it produced a pipelined submitted image; use display time and frame domain."},
        {"retained_stage_totals", std::move(stages)}, {"retained_wait_decisions", std::move(decisions)}, {"events", std::move(rows)}};
}
}
