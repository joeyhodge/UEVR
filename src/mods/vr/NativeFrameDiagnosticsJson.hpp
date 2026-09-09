#pragma once

#include <charconv>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <nlohmann/json.hpp>
#include "NativeFrameDiagnostics.hpp"

namespace uevr::native_frame {
inline nlohmann::json address_json(uintptr_t address) {
    if (address == 0) { return nullptr; }
    std::array<char, sizeof(address) * 2> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), address, 16);
    return "0x" + std::string(text.data(), result.ptr);
}

inline nlohmann::json clock_json(const ClockStamp& clock) {
    return {
        {"epoch", clock.epoch}, {"ready", clock.ready}, {"owner_changed", clock.owner_changed},
        {"engine", clock.ready ? nlohmann::json(engine_clock(clock.clocks)) : nlohmann::json(nullptr)},
        {"completed_present", clock.ready ? nlohmann::json(present_clock(clock.clocks)) : nlohmann::json(nullptr)},
    };
}

// UI/export only. No formatter or allocation is called by record/observe.
template <size_t Capacity>
nlohmann::json export_json(Recorder<Capacity>& recorder) {
    auto snapshot = std::make_unique<typename Recorder<Capacity>::Snapshot>();
    if (!recorder.snapshot(*snapshot)) {
        return {{"schema_version", 1}, {"diagnostic_only", true}, {"snapshot_busy", true}};
    }
    const auto& trace = *snapshot;
    auto events = nlohmann::json::array();
    std::map<std::string, uint64_t> stages, reasons, shadows;
    std::set<uint64_t> selected, copied;
    using SubmitKey = std::tuple<uint64_t, Runtime, uint8_t, uint8_t>;
    std::map<SubmitKey, std::pair<uint64_t, uint64_t>> submits;
    uint64_t newest_selected{}, newest_copied{};
    bool consumer_observed{};
    for (size_t i = 0; i < trace.count; ++i) {
        const auto& e = trace.events[i];
        ++stages[name(e.stage)];
        if (e.stage == Stage::selection) {
            ++reasons[name(e.reason)];
            consumer_observed |= e.window_observed;
            if (e.window_observed) { ++shadows[name(e.shadow)]; }
            if (e.packet_accepted) { selected.insert(e.attempt); newest_selected = e.packet_serial; }
        } else if (e.stage == Stage::copy_recorded) {
            copied.insert(e.attempt);
            newest_copied = e.packet_serial;
        } else if (e.stage == Stage::submit_attempt || e.stage == Stage::submit_result) {
            auto& counts = submits[{e.attempt, e.runtime, e.submit_eye, e.submit_call}];
            ++(e.stage == Stage::submit_attempt ? counts.first : counts.second);
        }
        nlohmann::json window = nullptr;
        if (e.window_observed) {
            window = {{"render_delta", e.render_delta}, {"engine_delta", e.engine_delta},
                {"same_engine_grace", e.legacy_same_engine}, {"legacy_accepted", e.legacy_window},
                {"comparison", name(e.shadow)},
                {"shadow_accepted", e.shadow == Shadow::matches || e.shadow == Shadow::differs
                    ? nlohmann::json(e.shadow_window) : nlohmann::json(nullptr)}};
        }
        events.push_back({
            {"sequence", e.sequence}, {"steady_timestamp_ns", e.timestamp_ns}, {"epoch", e.epoch},
            {"stage", name(e.stage)}, {"backend", name(e.backend)}, {"runtime", name(e.runtime)},
            {"thread", e.thread}, {"selection_attempt", e.attempt}, {"serial", e.packet_serial},
            {"generation", e.generation}, {"auxiliary_transaction", e.transaction},
            {"family", address_json(e.family)}, {"packet_rhi", address_json(e.rhi)},
            {"packet_resource", address_json(e.resource)},
            {"producer_engine", e.packet_serial ? nlohmann::json(e.producer_engine) : nlohmann::json(nullptr)},
            {"producer_render", e.packet_serial ? nlohmann::json(e.producer_render) : nlohmann::json(nullptr)},
            {"consumer_render", e.attempt ? nlohmann::json(e.submit_render) : nlohmann::json(nullptr)},
            {"producer_shadow_clock", clock_json(e.producer_clock)},
            {"consumer_shadow_clock", clock_json(e.consumer_clock)}, {"frame_window", std::move(window)},
            {"selection_reason", name(e.reason)},
            {"packet_accepted", e.stage == Stage::selection ? nlohmann::json(e.packet_accepted) : nlohmann::json(nullptr)},
            {"resource_checks_reached", e.resource_checks_ran}, {"observed_generation", e.current_generation},
            {"observed_rhi", address_json(e.current_rhi)}, {"observed_resource", address_json(e.current_resource)},
            {"copy_right_source", address_json(e.copy_source)}, {"copy_destination", address_json(e.copy_destination)},
            {"invalidation_state", e.stage == Stage::invalidation ? nlohmann::json(e.invalidation_state) : nlohmann::json(nullptr)},
            {"submit_eye", e.submit_eye == 2 ? "pair" : e.submit_eye == 0 ? "left" : "right"},
            {"submit_call", e.submit_call},
            {"submission_result", e.stage == Stage::submit_result ? nlohmann::json(e.api_result) : nlohmann::json(nullptr)},
        });
    }
    uint64_t selection_without_copy{}, copy_without_selection{}, attempt_without_result{};
    for (const auto attempt : selected) { selection_without_copy += !copied.contains(attempt); }
    for (const auto attempt : copied) { copy_without_selection += !selected.contains(attempt); }
    for (const auto& [key, counts] : submits) {
        if (counts.first > counts.second) { attempt_without_result += counts.first - counts.second; }
    }
    return {
        {"schema_version", 1}, {"diagnostic_only", true}, {"changes_submit_policy", false},
        {"shadow_only", true}, {"enabled", trace.enabled}, {"session_only", true}, {"snapshot_busy", false},
        {"epoch", trace.epoch}, {"capacity", Capacity}, {"retained", trace.count}, {"recorded_in_epoch", trace.total},
        {"overwritten_in_epoch", trace.overwritten}, {"contention_drops_lifetime", trace.dropped},
        {"missed_engine_observations_lifetime", trace.missed_engine},
        {"missed_present_observations_lifetime", trace.missed_present},
        {"engine_writer_thread", trace.engine_owner}, {"present_writer_thread", trace.present_owner},
        {"current_shadow_clock", clock_json(trace.clock)}, {"consumer_identity_observed", consumer_observed},
        {"summary_scope", "retained events only; missing stages can be skipped, overwritten, or dropped, not necessarily failures"},
        {"frame_window_scope", "legacy local decision inputs; separate plain reads are not asserted coherent; shadow never changes acceptance"},
        {"submission_scope", "OpenXR::end_frame or IVRCompositor::Submit result, not GPU completion or display proof; correlate selection_attempt with copy records"},
        {"stages", stages}, {"selection_reasons", reasons}, {"shadow_comparisons", shadows},
        {"newest_selected_serial", newest_selected}, {"newest_copy_recorded_serial", newest_copied},
        {"selections_without_copy_in_buffer", selection_without_copy},
        {"copies_without_selection_in_buffer", copy_without_selection},
        {"submission_attempts_without_result_in_buffer", attempt_without_result}, {"events", std::move(events)},
    };
}
}
