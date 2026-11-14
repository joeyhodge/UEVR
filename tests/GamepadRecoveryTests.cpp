#include "mods/vr/GamepadRecovery.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using Recovery = uevr::input_recovery::GamepadRecovery;
using namespace std::chrono_literals;
namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
const auto origin = Recovery::TimePoint{100s};

void retries() {
    Recovery policy;
    int focus_queries{};
    const auto focused = [&]() { ++focus_queries; return true; };
    expect(!policy.request_retry(origin, false, true, focused) && !policy.request_retry(origin, true, false, focused) && focus_queries == 0,
        "healthy XInput or absent controller/pause intent never triggers recovery or SDK focus queries");
    auto now = origin;
    for (const auto delay : {2s, 4s, 8s, 16s, 30s, 30s, 30s}) {
        expect(policy.request_retry(now, true, true, focused), "eligible first attempt and eventual retries remain possible");
        const auto queries = focus_queries;
        for (int i = 0; i < 1000; ++i) {
            expect(!policy.request_retry(now + 1ms, true, true, focused), "poll bursts cannot spam window/device recovery");
        }
        expect(!policy.request_retry(now + delay - 1ms, true, true, focused) && focus_queries == queries,
            "backoff is checked before asking the runtime about focus");
        now += delay;
    }
    policy.observe_xinput_poll();
    expect(!policy.request_retry(now, false, true, focused), "a resumed gamepad poll clears recovery need");
    expect(policy.request_retry(now + 2s, true, true, focused), "reconnection resets the retry schedule");
    expect(policy.request_retry(now + 4s, true, true, focused), "the first post-reconnection delay is two seconds, not the cap");
    policy.reset();
    expect(policy.request_retry(now + 4s, true, true, focused), "runtime input reinitialization resets the schedule");

    policy.reset();
    focus_queries = 0;
    const auto unavailable = [&]() { ++focus_queries; return false; };
    for (int i = 0; i < 1000; ++i) {
        expect(!policy.request_retry(origin + std::chrono::milliseconds{i}, true, true, unavailable),
            "unfocused/unready runtime never activates the game window");
    }
    expect(focus_queries == 4, "unfocused retry probes are limited to four per second");
    expect(policy.request_retry(origin + 1s, true, true, focused), "focus returning allows recovery without spending attempts while unfocused");
    expect(!policy.request_retry(origin + 2999ms, true, true, focused) && policy.request_retry(origin + 3s, true, true, focused),
        "focus rejection did not escalate the retry delay");

    for (bool ready : {false, true}) {
        for (bool restarting : {false, true}) {
            for (bool has_focus : {false, true}) {
                expect(uevr::input_recovery::input_focus_allows_recovery(ready, restarting, has_focus) ==
                    (ready && !restarting && has_focus), "only ready, focused, non-restarting runtime permits recovery");
            }
        }
    }
}

void stalled_polling() {
    Recovery policy;
    int queries{}, updates{};
    const auto focused = [&]() { ++queries; return true; };
    expect(!policy.request_stalled_poll(origin + 1s, origin, focused) && queries == 0,
        "normal engine cadence and the one-second boundary are untouched");
    const auto start = origin + 1001ms;
    for (int ms = 0; ms < 1000; ++ms) {
        for (unsigned user_index = 0; user_index < 4; ++user_index) {
            if (policy.request_stalled_poll(start + std::chrono::milliseconds{ms}, origin, focused)) { ++updates; }
            policy.observe_xinput_poll();
        }
    }
    expect(updates == 125 && queries == 125, "all user indices share one bounded stalled-engine poll; reconnect reset cannot bypass it");
    expect(!policy.request_stalled_poll(start + 1s, start + 1s, focused), "engine resuming immediately disables fallback polling");

    policy.reset();
    queries = 0;
    const auto unavailable = [&]() { ++queries; return false; };
    for (int ms = 0; ms < 1000; ++ms) {
        expect(!policy.request_stalled_poll(start + std::chrono::milliseconds{ms}, origin, unavailable),
            "an unfocused runtime performs no fallback action sync");
    }
    expect(queries == 4 && policy.request_stalled_poll(start + 1s, origin, focused),
        "bounded focus probes allow stalled input to recover when focus returns");

    policy.reset();
    std::mutex mutex;
    std::atomic<int> concurrent_updates{};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 1000; ++i) {
                std::scoped_lock lock{mutex};
                if (policy.request_stalled_poll(start, origin, []() { return true; })) { ++concurrent_updates; }
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    expect(concurrent_updates == 1, "serialized concurrent XInput callbacks cannot multiply runtime polling");
}

void wiring(const std::string& root) {
    std::ifstream file{root + "/src/mods/VR.cpp", std::ios::binary};
    const std::string source{std::istreambuf_iterator<char>{file}, {}};
    expect(!source.empty(), "source wiring fixture is available");
    const auto focus_begin = source.find("bool VR::can_recover_gamepad_input() const");
    const auto update_begin = source.find("void VR::update_action_states(bool from_stalled_xinput)");
    expect(focus_begin != std::string::npos && update_begin != std::string::npos, "focus and recovery entrypoints are present");
    if (focus_begin == std::string::npos || update_begin == std::string::npos) { return; }
    const auto focus = source.substr(focus_begin, update_begin - focus_begin);
    expect(focus.find("XR_SESSION_STATE_FOCUSED") != std::string::npos && focus.find("IsInputAvailable()") != std::string::npos,
        "OpenXR and OpenVR use runtime input focus, not headset/controller identity");
    expect(focus.find("std::try_to_lock") != std::string::npos, "OpenXR focus checks do not add a blocking wait");
    const auto update = source.substr(update_begin, source.find("void VR::update_dpad_gestures()", update_begin) - update_begin);
    expect(update.find("request_stalled_poll") < update.find("UpdateActionState"), "fallback is gated before runtime action polling");
    expect(update.find("if (!action_lock.try_lock()) { return; }") < update.find("request_stalled_poll"),
        "stalled XInput fallback skips a busy driver update instead of queuing behind it");
    expect(update.find("action_lock.unlock()") < update.find("activate_window()"), "window activation does not hold the input mutex");
    expect(update.find("retry_gamepad && !m_spoofed_gamepad_connection && can_recover_gamepad_input()") != std::string::npos,
        "focus and recovered connection are rechecked after unlocking, before window activation");
    expect(source.find("update_action_states(true)") != std::string::npos && source.find("m_gamepad_recovery.observe_xinput_poll()") != std::string::npos,
        "XInput fallback uses the bounded entrypoint and selected polls reset retries");
    const auto xinput_begin = source.find("void VR::on_xinput_get_state(");
    const auto xinput = source.substr(xinput_begin, source.find("void VR::on_xinput_set_state(", xinput_begin) - xinput_begin);
    expect(xinput.find("std::scoped_lock _{m_gamepad_recovery_mtx}") != std::string::npos && xinput.find("m_actions_mtx") == std::string::npos,
        "ordinary XInput bookkeeping is not serialized behind VR driver action updates");
    expect(update.find("std::chrono::milliseconds(30)") != std::string::npos && update.find("runtime->wants_reinitialize = true;") != std::string::npos,
        "the separate OpenVR 30ms restart heuristic is intentionally unchanged");
    expect(source.find("uevr::steam_frame::migrate_stock_defaults(module_directory / it.first, it.second)") != std::string::npos,
        "stock migration is wired into OpenVR manifest initialization");
}
}
int main(int argc, char** argv) {
    try {
        retries(); stalled_polling();
        if (argc < 2) { throw std::runtime_error{"Pass the UEVR source root"}; }
        wiring(argv[1]);
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    if (failures == 0) { std::cout << "Gamepad recovery tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
