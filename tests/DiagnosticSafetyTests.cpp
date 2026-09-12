#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include "utility/Logging.hpp"
#include "utility/SupportDiagnostics.hpp"
// Some backend translation units still include Windows headers without NOMINMAX.
#define max(a, b) windows_max_macro_must_not_expand(a, b)
#include "mods/vr/CVarDiagnostics.hpp"
#undef max

int disabled_log_argument_evaluations();
int test_cached_cvar_reads();
int test_console_text();
int test_discovery_validation();
int test_cadence_replay();
int test_ue58_ui_initialization();

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

class CountingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::atomic<int> count{};
private:
    void sink_it_(const spdlog::details::log_msg&) override { ++count; }
    void flush_() override {}
};

void concurrent_sites(std::atomic<int>& arguments) {
    SPDLOG_INFO_ONCE("info {}", ++arguments);
    SPDLOG_WARN_ONCE("warn {}", ++arguments);
    SPDLOG_ERROR_ONCE("error {}", ++arguments);
}

void runtime_disabled_site(int& arguments) { SPDLOG_INFO_ONCE("{}", ++arguments); }

void test_logging() {
    const auto previous = spdlog::default_logger();
    const auto sink = std::make_shared<CountingSink>();
    const auto logger = std::make_shared<spdlog::logger>("diagnostic-test", sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
    std::atomic<int> arguments{};
    std::barrier start{16};
    std::vector<std::jthread> workers;
    for (int i = 0; i < 16; ++i) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int n = 0; n < 1000; ++n) { concurrent_sites(arguments); }
        });
    }
    workers.clear();
    expect(sink->count == 3, "concurrent once sites each emit exactly once");
    expect(arguments == 3, "once arguments are evaluated only by winning callers");
    int disabled_arguments{};
    logger->set_level(spdlog::level::off);
    runtime_disabled_site(disabled_arguments);
    logger->set_level(spdlog::level::trace);
    runtime_disabled_site(disabled_arguments);
    expect(sink->count == 3 && disabled_arguments == 1, "runtime filtering retains original once semantics");
    expect(disabled_log_argument_evaluations() == 0, "compiled-out log arguments are not evaluated");
    spdlog::set_default_logger(previous);
}

void test_readbacks() {
    using namespace uevr::cvar_diagnostics;
    WriteObservation rejected;
    auto id = rejected.begin(0, false);
    expect(rejected.state == WriteState::queued && !rejected.pending, "queued setter is not accepted/observed");
    expect(rejected.dispatched(id, true, 1000), "void setter completion starts readback only");
    expect(!rejected.observe(id, 0, 1099), "readbacks do not busy poll before their interval");
    for (int i = 1; i <= 4; ++i) { rejected.observe(id, 1, 1000 + i * 100); }
    expect(rejected.state == WriteState::not_observed && !rejected.pending, "ignored write is not reported as applied");
    expect(!rejected.observe(id, 0, 2000) && rejected.samples == 4, "readback stops at the bounded sample limit");

    WriteObservation delayed;
    id = delayed.begin(0, false);
    delayed.dispatched(id, true, 0);
    delayed.observe(id, 1, 100);
    delayed.observe(id, 1, 200);
    delayed.observe(id, 0, 300);
    delayed.observe(id, 0, 400);
    expect(delayed.state == WriteState::observed && delayed.actual == 0, "delayed application and real zero are observed");

    WriteObservation overwritten;
    id = overwritten.begin(0, false);
    overwritten.dispatched(id, true, 0);
    overwritten.observe(id, 0, 100);
    for (int i = 2; i <= 4; ++i) { overwritten.observe(id, 1, i * 100); }
    expect(overwritten.state == WriteState::changed_after_observed, "later game overwrite is distinct from never observed");

    WriteObservation failed;
    id = failed.begin(0, false);
    failed.dispatched(id, false, 0);
    expect(failed.state == WriteState::setter_unavailable && !failed.pending, "unavailable setter does not schedule getters");
    id = failed.begin(0, false);
    failed.dispatched(id, true, 0);
    for (int i = 1; i <= 4; ++i) { failed.observe(id, std::nullopt, i * 100); }
    expect(failed.state == WriteState::readback_unavailable && !failed.actual, "failed getter is not mistaken for zero");
    expect(to_json(failed)["actual"].is_null(), "unavailable value exports as null");

    WriteObservation latest;
    const auto old = latest.begin(1, false);
    const auto current = latest.begin(2, false);
    expect(!latest.dispatched(old, true, 0), "superseded setter cannot update latest diagnostics");
    latest.dispatched(current, true, 0);
    expect(!latest.observe(old, 1, 100), "superseded readback is discarded");
    latest.observe(current, 2, 100);
    expect(!latest.dispatched(current, true, 100), "duplicate completion cannot restart sampling");
    expect(latest.requested == 2 && latest.actual == 2, "latest request retains its own readback");

    for (const auto requested : {1, 2, 4, 8, 16}) {
        WriteObservation anisotropy;
        id = anisotropy.begin(requested, false);
        anisotropy.dispatched(id, true, 0);
        const auto previous = requested == 8 ? 4 : 8;
        anisotropy.observe(id, previous, 100);
        expect(anisotropy.state != WriteState::observed && anisotropy.actual == previous,
            "anisotropy readback must not equate different nonzero integer values");
        for (int i = 2; i <= 4; ++i) { anisotropy.observe(id, requested, i * 100); }
        expect(anisotropy.state == WriteState::observed && anisotropy.actual == requested && !anisotropy.pending,
            "anisotropy presets report the applied integer value and stop bounded readback polling");
    }

    WriteObservation floating;
    id = floating.begin(0.3, true);
    floating.dispatched(id, true, 0);
    floating.observe(id, static_cast<double>(0.3f), 100);
    expect(floating.state == WriteState::observed, "float conversion noise is tolerated");
    for (int i = 2; i <= 4; ++i) { floating.observe(id, std::numeric_limits<double>::quiet_NaN(), i * 100); }
    expect(floating.state == WriteState::readback_unavailable && !floating.actual, "nonfinite getter is unavailable");
}

void test_support_report() {
    using namespace utility::support;
    const auto build = build_identity("uevr-sha", "sdk-sha", "true", "false", "branch", "date", "time");
    expect(build["uesdk_commit"] == "sdk-sha" && build["uevr_tracked_changes"] == "true", "report identifies SDK and local changes");
    const auto capped = pacing(11111111, 75);
    expect(capped["positive_cap_below_refresh"] == true && capped["automatic_adjustment"] == false, "75/90 pacing is descriptive, never auto-corrected");
    expect(pacing(11111111, 0)["positive_cap_below_refresh"] == false, "uncapped is not treated as zero FPS");
    expect(pacing(0, 75)["headset_refresh_hz"].is_null(), "unknown refresh is not invented");
    expect(pacing(11111111, std::nullopt)["frame_cap_fps"].is_null(), "unknown cap is not guessed");
    expect(pacing(11111111, std::numeric_limits<double>::infinity())["frame_cap_fps"].is_null(), "invalid cap is unavailable");
}
}

int main() {
    test_logging();
    test_readbacks();
    test_support_report();
    failures += test_cached_cvar_reads();
    failures += test_console_text();
    failures += test_discovery_validation();
    failures += test_cadence_replay();
    failures += test_ue58_ui_initialization();
    if (failures != 0) { return 1; }
    std::cout << "Diagnostic safety tests passed\n";
    return 0;
}
