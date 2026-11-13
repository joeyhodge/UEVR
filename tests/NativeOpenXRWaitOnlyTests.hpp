#pragma once

#include "mods/vr/NativeOpenXRWaitOnly.hpp"
#include "mods/vr/NativeOpenXRAsyncWorker.hpp"
#include "NativeOpenXRAsyncTests.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace native_wait_only_tests {
namespace w = uevr::openxr_wait;
namespace a = uevr::native_array;
using native_async_tests::require;
using native_async_tests::until;
using namespace std::chrono_literals;

inline void policy() {
    require(!w::default_enabled, "wait-only must be off in a new game profile");
    require(!w::load_setting({},false) && !w::load_setting(false,false) && w::load_setting(true,false) &&
        !w::load_setting(true,true) && !w::load_setting({},true), "profile missing/reset/explicit opt-in persistence changed");
    for (unsigned bits = 0; bits < 256; ++bits) {
        const w::Scope scope{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),bool(bits&16),
            bool(bits&32),bool(bits&64),bool(bits&128)};
        require(scope.enabled() == (bits == 31), "wait-only bypassed a backend/mode/stage/ownership guard");
        require(scope.enabled() == (scope.blocked() == w::Block::none), "UI and execution policy disagree");
    }
    require(w::Scope{true,true,true,true,true,true,true,false}.blocked() == w::Block::pre_acquire,
        "existing wait/pre-acquire must retain precedence even when arrays are unavailable");
    for (bool same_pass : {false,true}) {
        // Wait-only must not require changing a game's working Same Pass choice.
        require(w::Scope{true,true,true,true,true,false,false,false}.enabled(), "Same Pass altered wait-only eligibility");
        require(a::Scope{true,true,same_pass,true,true,true,false,false}.enabled() == !same_pass,
            "wait-only loosened the separate array Same Pass guard");
    }
}

inline void dispatch() {
    unsigned only{}, frame{}, acquire{};
    bool cancelled{};
    const auto run = [&](w::Operation operation) {
        w::run_operation(operation, [&] { ++only; }, [&] { ++frame; return std::optional<int>{42}; },
            [&](int value) { require(value == 42, "wrong acquired frame"); ++acquire; }, [&] { return cancelled; });
    };
    run(w::Operation::wait_only);
    require(only == 1 && frame == 0 && acquire == 0, "wait-only touched array wait/acquisition");
    run(w::Operation::array_pre_acquire);
    require(only == 1 && frame == 1 && acquire == 1, "original array operation changed");
    cancelled = true;
    run(w::Operation::wait_only); run(w::Operation::array_pre_acquire);
    require(only == 1 && frame == 1 && acquire == 1, "cancelled queued work entered the runtime");
    cancelled = false;
    w::run_operation(w::Operation::array_pre_acquire, [&] { ++only; },
        [&] { cancelled = true; ++frame; return std::optional<int>{43}; }, [&](int) { ++acquire; }, [&] { return cancelled; });
    require(frame == 2 && acquire == 1, "cancellation after wait started another acquisition");
    cancelled = false;
    w::run_operation(w::Operation::array_pre_acquire, [&] { ++only; },
        [&] { ++frame; return std::optional<int>{}; }, [&](int) { ++acquire; }, [&] { return cancelled; });
    require(frame == 3 && acquire == 1, "failed original wait pre-acquired an image");
}

inline void handoff() {
    w::Handoff gate;
    const a::FrameKey before{1,2,3,40}, after{1,2,4,50};
    require(!w::frame_at_or_after(9,10) && w::frame_at_or_after(10,10) && w::frame_at_or_after(12,10) &&
        w::frame_at_or_after(2,0xfffffff0u) && !w::frame_at_or_after(0xfffffff0u,2),
        "future/skipped/wrapped engine poses escaped wait admission");
    require(!gate.pending() && gate.progress() == w::Progress::idle, "new handoff is not inert");
    require(!gate.reserve({},10,false) && !gate.reserve(before,0,false), "invalid clock/frame reserved a wait");
    require(!gate.reserve(before,10,true) && gate.progress() == w::Progress::pose_ahead,
        "an already-rendered/published pose could have its clock overwritten");
    const auto first = gate.reserve(before,10,false);
    require(first && gate.pending() && !gate.reserve(before,11,false), "two waits own one frame");
    require(!gate.applies_to(9) && gate.applies_to(10) && gate.applies_to(11) && gate.applies_to(0),
        "wait-only retimed an older queued pose or missed its next pose");
    gate.rearm(); gate.waiting_frame();
    require(gate.owns(*first), "a UI retry discarded a pending wait");
    require(!gate.observe(before) && !gate.observe({1,2,3,50}) && !gate.observe({1,2,4,40}),
        "a repeated sequence or clock was mistaken for a completed wait");
    require(gate.begin_worker(*first) && gate.observe(after), "worker could not publish its real wait result");
    gate.fail(*first);
    require(!gate.pending() && gate.progress() == w::Progress::active && gate.worker_waits() == 1 && gate.cooperative_waits() == 0,
        "cancellation/failure discarded a completed worker wait");
    const auto next = gate.reserve(after,11,false);
    require(next && !gate.begin_worker(*first), "late worker reused an old reservation");
    require(gate.observe({1,2,5,60}) && !gate.begin_worker(*next) && gate.cooperative_waits() == 1,
        "normal consumer and delayed worker both waited for the same frame");
    const auto failed = gate.reserve({1,2,5,60},12,false);
    require(failed.has_value(), "cannot reserve failure test"); gate.fail(*failed);
    require(gate.progress() == w::Progress::failed && !gate.reserve(after,12,false), "failed wait did not stop new work");
    gate.rearm();
    const auto replacement = gate.reserve(after,12,false);
    require(replacement && replacement->serial != failed->serial, "explicit retry reused a reservation");
    gate.fail(*failed);
    require(gate.owns(*replacement), "old failure poisoned a replacement request");
    gate.reset();
    const auto recreated = gate.reserve({2,3,1,70},13,false);
    require(recreated && recreated->serial != replacement->serial && !gate.begin_worker(*replacement),
        "resolution/session reset authorized stale queued work");
    require(!gate.observe({2,4,2,80}) && !gate.pending(), "session-control change completed an old wait");
    const auto wrapped = gate.reserve(before,0xfffffff0u,false);
    require(wrapped && gate.applies_to(2) && !gate.applies_to(0xffffffefu), "engine frame wrap changed wait ownership");
    gate.reset();
}

inline void pose_admission() {
    for (bool coherent_extra : {false,true}) {
        w::Handoff gate;
        std::recursive_mutex clock_mutex;
        std::mutex extra_mutex;
        std::unique_lock clock{clock_mutex,std::defer_lock};
        std::recursive_mutex assignment_mutex;
        std::unique_lock assignment{assignment_mutex,std::defer_lock};
        std::unique_lock extra{extra_mutex,std::defer_lock};
        unsigned services{};
        require(w::lock_pose_clock(gate,clock,assignment,extra,coherent_extra,[&] { ++services; return true; }) &&
            !clock.owns_lock() && assignment.owns_lock() && extra.owns_lock() == coherent_extra && services == 0,
            "feature-OFF pose path acquired clock or changed extra/assignment ownership");
    }
    for (bool coherent_extra : {false,true}) {
        w::Handoff gate;
        std::recursive_mutex clock_mutex;
        std::mutex extra_mutex;
        std::promise<void> checked_off, continue_pose;
        const auto checked_future = checked_off.get_future();
        struct PausedAssignment {
            std::recursive_mutex mutex;
            std::promise<void>* checked;
            std::shared_future<void> resume;
            void lock() {
                if (checked) {
                    checked->set_value(); checked = nullptr;
                    require(resume.wait_for(2s) == std::future_status::ready, "pose admission barrier timed out");
                }
                mutex.lock();
            }
            void unlock() { mutex.unlock(); }
        } assignment_mutex{{},&checked_off,continue_pose.get_future().share()};
        const a::FrameKey before{1,2,3,40}, after{1,2,4,50};
        auto pose = std::async(std::launch::async,[&] {
            std::unique_lock clock{clock_mutex,std::defer_lock};
            std::unique_lock assignment{assignment_mutex,std::defer_lock};
            std::unique_lock extra{extra_mutex,std::defer_lock};
            unsigned services{};
            require(w::lock_pose_clock(gate,clock,assignment,extra,coherent_extra,[&] {
                require(clock.owns_lock() && !assignment.owns_lock() && !extra.owns_lock(),
                    "pose retry inverted clock/extra/assignment lock order");
                ++services; return gate.observe(after);
            }), "pose retry did not service the newly reserved wait");
            require(clock.owns_lock() && assignment.owns_lock() && extra.owns_lock() == coherent_extra && services == 1,
                "pose slipped past reservation after its first pending check");
        });
        require(checked_future.wait_for(1s) == std::future_status::ready, "pose did not reach the admission race");
        {
            std::scoped_lock clock{clock_mutex};
            std::scoped_lock assignment{assignment_mutex.mutex};
            require(gate.reserve(before,10,false).has_value(), "reservation did not fit between pose's two checks");
        }
        continue_pose.set_value();
        require(pose.wait_for(1s) == std::future_status::ready, "pose lock order deadlocked the handoff"); pose.get();
        require(!gate.pending() && gate.cooperative_waits() == 1, "pose did not consume its newly reserved clock");
    }
}

inline void uncertain_wait() {
    w::Handoff gate;
    const a::FrameKey before{1,2,3,40};
    require(gate.reserve(before,10,false).has_value(), "exception reservation failed");
    {
        w::WaitFailureGuard guard{gate};
    }
    require(!gate.faulted() && gate.pending(), "normal scope exit quarantined a valid pending wait");
    struct WitnessMutex {
        w::Handoff& gate;
        bool fault_before_unlock{};
        void lock() {}
        void unlock() { fault_before_unlock = gate.faulted(); }
    } mutex{gate};
    try {
        std::scoped_lock lock{mutex};
        w::WaitFailureGuard guard{gate};
        throw std::runtime_error("fake uncertain XR wait");
    } catch (const std::runtime_error&) {}
    require(gate.faulted() && !gate.pending() && mutex.fault_before_unlock,
        "an exception allowed another consumer into an unproven XR frame loop");
    gate.rearm(); gate.reset(); gate.waiting_frame(); gate.normal_pacing();
    require(gate.faulted() && gate.progress() == w::Progress::failed && !gate.reserve(before,11,false),
        "off/on or a focus/resolution event reopened uncertain XR call order");
    w::Handoff replacement;
    require(!replacement.faulted() && replacement.reserve({2,1,1,50},11,false).has_value(),
        "fault from an old runtime poisoned its replacement");
}

// A bounded XR stand-in exercises the production worker, dispatch, frame key and
// handoff together. It does not replace a real runtime/HMD transition test.
struct Runtime {
    std::recursive_mutex sync;
    w::Handoff gate;
    a::FrameKey clock{1,2,3,40};
    unsigned waits{};
    bool synced{}, began{}, running{true};
    std::optional<w::Reservation> reserve(uint32_t next = 10) {
        std::scoped_lock lock{sync};
        if (!a::can_async_wait(clock,clock,running,synced,began)) { return {}; }
        return gate.reserve(clock,next,false);
    }
    void publish() {
        require(running && !synced && !began, "overlapping wait/begin/end ownership");
        ++waits; ++clock.wait_sequence; clock.display_time += 10; synced = true;
        require(gate.observe(clock), "published wait did not satisfy its pose-clock handoff");
    }
    void worker(w::Reservation ticket) {
        std::scoped_lock lock{sync};
        if (!gate.owns(ticket)) { return; }
        if (!a::can_async_wait(ticket.before,clock,running,synced,began)) { gate.fail(ticket); return; }
        require(gate.begin_worker(ticket), "worker lost a live ticket"); publish();
    }
    int64_t pose(uint32_t frame = 10) {
        std::scoped_lock lock{sync};
        if (gate.applies_to(frame)) { publish(); }
        return clock.display_time;
    }
};

struct Request { std::shared_ptr<Runtime> owner; w::Reservation ticket; w::Operation operation{w::Operation::wait_only}; };

inline void races() {
    a::AsyncWorker<Request> worker;
    std::atomic_uint acquired{};
    const auto callback = [&](const Request& request, const auto& cancellation) {
        w::run_operation(request.operation, [&] { request.owner->worker(request.ticket); },
            [&] { return std::optional<int>{7}; }, [&](int) { ++acquired; }, [&] { return cancellation.cancelled(); });
    };
    for (unsigned i = 0; i < 100; ++i) {
        auto runtime = std::make_shared<Runtime>();
        const auto ticket = runtime->reserve(); require(ticket.has_value(), "race reservation failed");
        require(worker.submit(Request{runtime,*ticket}, callback), "race request rejected");
        if (i % 3 == 0) { worker.cancel_pending(); }
        require(runtime->pose() == 50, "pose consumer used the old frame's clock");
        until([&] { return !worker.busy(); });
        require(runtime->waits == 1 && runtime->synced && !runtime->gate.pending() && !worker.failed(),
            "cancelled/delayed worker and normal consumer issued duplicate waits");
        // A completed wait stays consumable even if the option was turned off.
        runtime->began = true; runtime->synced = false;
        require(!runtime->reserve(), "a second wait was queued before the prior begin/end completed");
        runtime->began = false;
    }
    require(acquired == 0, "wait-only acquired an image");
    // The same persistent callback must use EACH request's operation, not the
    // operation selected when its thread was first created.
    require(worker.submit(Request{nullptr,{},w::Operation::array_pre_acquire}, callback), "array operation rejected after wait-only");
    until([&] { return !worker.busy(); });
    require(acquired == 1 && !worker.failed(), "worker retained the first request's operation");
    auto old = std::make_shared<Runtime>();
    auto current = std::make_shared<Runtime>(); current->clock.session = 2;
    const auto ticket = old->reserve(); require(ticket.has_value(), "captured owner reservation failed");
    require(worker.submit(Request{old,*ticket}, callback), "captured owner request rejected");
    until([&] { return !worker.busy(); });
    require(old->waits == 1 && current->waits == 0 && acquired == 1, "request was redirected to a replacement runtime/operation");
    worker.stop();

    // In-progress wait is intentionally not cancellable. Stop must join before
    // releasing runtime ownership; its real completion must remain published.
    auto inflight = std::make_shared<Runtime>();
    auto reserved = inflight->reserve(); require(reserved.has_value(), "in-flight reservation failed");
    std::promise<void> entered, release;
    const auto entered_future = entered.get_future();
    const auto release_future = release.get_future().share();
    require(worker.submit(Request{inflight,*reserved}, [&](const Request& request, const auto&) {
        std::scoped_lock lock{request.owner->sync};
        require(request.owner->gate.begin_worker(request.ticket), "blocked wait ticket invalid");
        entered.set_value();
        require(release_future.wait_for(2s) == std::future_status::ready, "blocked fake wait timed out");
        request.owner->publish();
    }), "blocked wait request rejected");
    require(entered_future.wait_for(1s) == std::future_status::ready, "fake wait did not start");
    worker.cancel_pending();
    auto stopped = std::async(std::launch::async, [&] { worker.stop(); });
    require(stopped.wait_for(20ms) == std::future_status::timeout && worker.busy(), "cancel falsely completed an in-progress wait");
    release.set_value();
    require(stopped.wait_for(1s) == std::future_status::ready, "in-flight wait did not drain"); stopped.get();
    require(inflight->waits == 1 && inflight->synced && inflight->gate.progress() == w::Progress::active,
        "draining discarded an actual completed XR wait");

    auto reset = std::make_shared<Runtime>();
    const auto stale = reset->reserve(); require(stale.has_value(), "reset reservation failed");
    reset->gate.reset(); ++reset->clock.generation;
    require(worker.submit(Request{reset,*stale}, callback), "stale test request rejected");
    until([&] { return !worker.busy(); });
    require(reset->waits == 0 && !worker.failed(), "reset session/resolution accepted queued old work");
    worker.stop();
}

inline std::string read(const std::filesystem::path& path) {
    std::ifstream stream{path}; require(stream.good(), "missing source-wiring input");
    std::ostringstream out; out << stream.rdbuf(); return out.str();
}
inline void wiring(const std::filesystem::path& root) {
    const auto header = read(root/"src/mods/VR.hpp"), cpp = read(root/"src/mods/VR.cpp");
    const auto xr = read(root/"src/mods/vr/runtimes/OpenXR.cpp");
    require(header.find("generate_name(\"NativeOpenXRAsyncWaitOnly\"), uevr::openxr_wait::default_enabled") != std::string::npos &&
        header.find("*m_native_openxr_wait_only,") != std::string::npos, "wait-only is not a separate default-off saved per-profile option");
    require(cpp.find("m_native_openxr_wait_only->draw(\"Experimental Async Wait Only (Same Pass compatible)\")") != std::string::npos,
        "wait-only has no in-game checkbox");
    require(cpp.find("is_dune_awakening_executable_cached() || is_stalker2_legacy_ue51_runtime_cached()};") != std::string::npos &&
        cpp.find("uevr::nascar::is_target() ||") != std::string::npos,
        "wait-only bypasses existing title/mode transition guards");
    require(cpp.find("run_operation(request.operation,") != std::string::npos &&
        cpp.find("request.runtime->synchronize_native_wait_only(request.wait_only)") != std::string::npos,
        "production callback is not using tested per-request dispatch/captured owner");
    const auto pose = xr.find("VRRuntime::Error OpenXR::update_poses(");
    require(xr.find("uevr::openxr_wait::lock_pose_clock(native_wait_only_handoff, wait_only_clock, assignment,",pose) <
        xr.find("if (!this->session_ready)",pose), "pose consumer bypassed the tested two-phase clock/assignment gate");
    const auto reset = xr.find("void OpenXR::prepare_resolution_scale_reconfigure(");
    require(xr.find("native_wait_only_handoff.reset();",reset) < xr.find("const auto safe_reason",reset),
        "resolution reconfigure leaves stale wait tickets authorized");
    require(xr.find("uevr::openxr_wait::frame_at_or_after(last_pose_update_frame_count, next)") != std::string::npos,
        "a skipped next ring slot could admit a wait below an already-published future pose");
    require(cpp.find("cfg.get<bool>(m_native_openxr_wait_only->get_config_name()), set_defaults)") != std::string::npos,
        "missing profile key can inherit a previously enabled wait-only setting");
    const auto sync = xr.find("VRRuntime::Error OpenXR::synchronize_frame(");
    require(xr.find("wait_only_failure.emplace(native_wait_only_handoff)",sync) < xr.find("xrWaitFrame(this->session",sync) &&
        cpp.find("runtime->is_openxr() && m_openxr->native_wait_only_handoff.faulted()") != std::string::npos,
        "uncertain wait does not quarantine before unlocking/request control-thread recovery");
}

inline void run() { policy(); dispatch(); handoff(); pose_admission(); uncertain_wait(); races(); }
}
