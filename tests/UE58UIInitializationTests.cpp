#include <atomic>
#include <array>
#include <barrier>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "mods/vr/UE58UIInitialization.hpp"

namespace {
using namespace uevr::ue58_ui;
struct Owner { int identity{}; };
using Coordinator = Initialization<Owner>;

int failures{};
void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED UE5.8 UI initialization: " << message << '\n';
    }
}

Coordinator::Ticket prepare(Coordinator& state, Source preferred, uint64_t now = 100) {
    auto request = state.begin(1920, 1080, preferred, now);
    expect(request != nullptr, "a valid request is accepted");
    expect(state.acquire_creation(now) == request, "game thread owns creation exactly once");
    expect(!state.acquire_creation(now), "a second game-thread pump cannot duplicate creation");
    expect(state.created(request, std::make_shared<Owner>(7), now), "created owner is accepted");
    return request;
}

void test_normal_startup() {
    for (const auto preferred : {Source::Slate, Source::PreRender}) {
        Coordinator state;
        expect(!state.pending() && state.snapshot().stage == Stage::Idle, "idle state has no pending work");
        expect(!state.begin(0, 1080, preferred, 100), "zero extent cannot start a request");
        expect(!state.begin(1920, 1080, Source::None, 100), "unvalidated callback cannot start a request");
        expect(!state.begin_if(1920, 1080, preferred, 100, [] { return false; }),
            "engine-owned target promotion between scheduling checks prevents creation");
        auto request = prepare(state, preferred);
        const auto backup = preferred == Source::Slate ? Source::PreRender : Source::Slate;
        expect(!state.acquire(backup, 101), "backup does not steal a healthy startup");
        auto lease = state.acquire(preferred, 101);
        expect(lease == request && state.snapshot().stage == Stage::Executing, "preferred callback validates");
        expect(!state.acquire(preferred, 102) && !state.acquire(backup, 102), "validation has one execution owner");
        int published{};
        expect(state.publish(lease, 103, [&] { ++published; return true; }), "validated target publishes");
        expect(!state.publish(lease, 104, [&] { ++published; return true; }), "completion cannot publish twice");
        expect(!state.pending() && published == 1, "published state is no longer pending");
        expect(!state.acquire(preferred, 10000) && !state.begin(1920, 1080, preferred, 10000),
            "steady-state callbacks neither validate nor allocate another target");
        const auto result = state.snapshot();
        expect(result.stage == Stage::Published && result.queued_source == preferred &&
            result.last_source == preferred && result.attempts == 1 && result.recoveries == 0,
            "successful startup exports a coherent generation and callback history");
    }
}

void test_callback_handoff() {
    for (const auto preferred : {Source::Slate, Source::PreRender}) {
        Coordinator state;
        const auto backup = preferred == Source::Slate ? Source::PreRender : Source::Slate;
        auto request = prepare(state, preferred);
        expect(!state.acquire(backup, 599), "handoff is bounded rather than immediate");
        auto lease = state.acquire(backup, 600);
        expect(lease == request, "late validated callback recovers a stranded request");
        expect(!state.acquire(preferred, 601), "late original callback cannot race the recovery lease");
        expect(!state.watchdog(100000), "watchdog never steals validation already executing");
        state.retry(lease, "resource not initialized", 601);
        expect(state.snapshot().stage == Stage::ResourceQueued, "an incomplete resource remains retryable");
        expect(!state.acquire(preferred, 602), "ownership follows the successful scheduling handoff");
        lease = state.acquire(backup, 602);
        expect(lease == request && lease->owner->identity == 7, "retry preserves the same owner and generation");
        expect(state.publish(lease, 603, [] { return true; }), "recovered initialization publishes");
        const auto snapshot = state.snapshot();
        expect(snapshot.queued_source == preferred && snapshot.active_source == backup &&
            snapshot.recoveries == 1 && snapshot.attempts == 2,
            "diagnostics distinguish original queue from the actual publisher");
    }
}

void test_voyage_startup_order_replay() {
    // The two 2026-09-10 logs establish ordering, not the exact original race.
    // Replay that order while deliberately withholding the first worker pump.
    Coordinator delayed;
    auto request = prepare(delayed, Source::Slate, 0);
    expect(delayed.watchdog(2000), "Voyage delayed callback is detected without render-queue execution");
    auto lease = delayed.acquire(Source::PreRender, 12536);
    expect(lease == request, "PreRender arriving 12.536 seconds later can service the original request");
    expect(delayed.publish(lease, 12537, [] { return true; }), "delayed startup can publish without reinjection");
    expect(delayed.snapshot().generation == 1 && delayed.snapshot().recoveries == 1,
        "delayed callback recovery does not churn generations or allocate another owner");

    Coordinator normal;
    prepare(normal, Source::PreRender, 1755);
    lease = normal.acquire(Source::PreRender, 2659);
    expect(normal.publish(lease, 2659, [] { return true; }) && normal.snapshot().recoveries == 0,
        "Voyage successful ordering retains its original callback and publication path");
}

void test_timeouts_and_failures() {
    Coordinator state;
    auto request = prepare(state, Source::Slate);
    auto owner = request->owner;
    expect(!state.watchdog(2099) && state.watchdog(2100), "watchdog notices starvation without a queue callback");
    expect(!state.watchdog(2101), "a cooldown is entered only once per timeout");
    expect(state.snapshot().stage == Stage::RetryWait && state.pending(), "request stays pending during cooldown");
    expect(!state.acquire(Source::PreRender, 4099), "cooldown prevents hot retry loops");
    auto lease = state.acquire(Source::PreRender, 4100);
    expect(lease == request && lease->owner == owner, "starvation recovery reuses the rooted owner");
    state.retry(lease, "resource still unavailable", 6100);
    expect(state.snapshot().timeouts == 2 && !state.acquire(Source::PreRender, 8099),
        "repeated failed validation is bounded without UObject allocation churn");
    lease = state.acquire(Source::PreRender, 8100);
    expect(lease == request, "same generation can recover after multiple rounds");
    expect(!state.publish(lease, 8101, [] { return false; }), "failed publication eligibility does not report ready");
    expect(state.pending() && state.snapshot().stage == Stage::RetryWait, "rejected publication remains fail-closed");
    lease = state.acquire(Source::PreRender, 10101);
    try {
        state.publish(lease, 10102, []() -> bool { throw std::runtime_error("fixture"); });
        expect(false, "publication exception must be propagated to the bounded caller");
    } catch (const std::runtime_error&) {}
    expect(state.snapshot().stage == Stage::RetryWait, "exception does not poison the executing state");
    lease = state.acquire(Source::PreRender, 12102);
    expect(state.publish(lease, 12103, [] { return true; }), "valid resource eventually publishes");

    Coordinator creation;
    auto failed = creation.begin(1920, 1080, Source::Slate, 100);
    expect(!creation.acquire(Source::Slate, 2000), "resource callbacks never execute game-thread creation");
    expect(creation.acquire_creation(200) == failed, "creation begins only on the game thread");
    expect(!creation.watchdog(99999), "slow creation is not replaced while an engine call could still be active");
    creation.creation_failed(failed, "world missing", 201);
    expect(!creation.pending() && !creation.begin(1920, 1080, Source::Slate, 2200), "creation failure has a cooldown");
    const auto replacement = creation.begin(1920, 1080, Source::Slate, 2201);
    expect(replacement && replacement->generation > failed->generation, "failed creation can retry with a fresh generation");
    creation.creation_failed(failed, "old completion", 2202);
    expect(creation.snapshot().generation == replacement->generation && creation.snapshot().stage == Stage::GameQueued,
        "stale creation failure cannot cancel a replacement");
}

void test_cancel_and_resize() {
    Coordinator state;
    int target{};
    auto old = state.begin(1920, 1080, Source::Slate, 100);
    expect(state.acquire_creation(101) == old, "old creation started");
    state.cancel("resize during creation", [&] { target = 0; });
    auto resized = state.begin(2560, 1440, Source::PreRender, 102);
    expect(resized && resized->generation > old->generation, "resize makes a newer immutable request");
    expect(!state.created(old, std::make_shared<Owner>(1), 103), "old UObject completion cannot replace the new owner");
    expect(state.acquire_creation(104) == resized, "new extent owns its creation");
    expect(state.created(resized, std::make_shared<Owner>(2), 105), "replacement owner accepted");
    expect(!state.cancel_if([] { return false; }, "same extent", [&] { target = -1; }),
        "unchanged extent does not cancel a live request");
    auto lease = state.acquire(Source::PreRender, 106);
    expect(lease == resized && lease->width == 2560 && lease->height == 1440, "lease contains one coherent extent");
    std::weak_ptr<Owner> held = lease->owner;
    state.cancel("engine-owned promotion", [&] { target = 99; });
    expect(!held.expired(), "cancellation retains a UI owner still used by validation");
    expect(!state.publish(lease, 107, [&] { target = 2; return true; }) && target == 99,
        "stale validation cannot replace an engine-owned target");
    state.retry(lease, "old callback failed", 108);
    state.owner_lost(lease, 108);
    expect(state.snapshot().stage == Stage::Cancelled, "old retry and failure cannot resurrect a cancelled generation");
    lease.reset();
    resized.reset();
    expect(held.expired(), "retired owner releases only after its final execution lease");

    auto queued = state.begin(1920, 1080, Source::Slate, 109);
    state.cancel("cancel before game thread", [] {});
    expect(!state.acquire_creation(110) && !state.created(queued, std::make_shared<Owner>(), 110),
        "cancelled queued work cannot create or publish later");
}

void test_dynamic_extents() {
    Coordinator state;
    uint64_t last_generation{};
    const std::array<std::pair<uint32_t, uint32_t>, 6> extents{{
        {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {5120, 1440},
    }};
    uint64_t now = 100;
    for (const auto [width, height] : extents) {
        state.cancel("validated Slate extent changed", [] {});
        const auto request = state.begin(width, height, Source::Slate, now++);
        expect(request && request->generation > last_generation, "each new UI extent has its own generation");
        last_generation = request->generation;
        state.acquire_creation(now++);
        state.created(request, std::make_shared<Owner>(), now++);
        const auto lease = state.acquire(Source::Slate, now++);
        std::pair<uint32_t, uint32_t> published{};
        expect(state.publish(lease, now++, [&] {
            published = {lease->width, lease->height};
            return true;
        }), "validated non-1080p UI dimensions can publish");
        expect(published == std::pair{width, height} && state.snapshot().width == width && state.snapshot().height == height,
            "request, publication and diagnostics preserve the exact Slate size without clamping to 1080p");
    }
}

void test_owner_loss_and_retirement() {
    Coordinator state;
    auto request = prepare(state, Source::Slate);
    auto lease = state.acquire(Source::Slate, 101);
    state.owner_lost(lease, 102);
    expect(!state.pending() && !state.begin(1920, 1080, Source::Slate, 2101), "invalid owner retires with a cooldown");
    expect(state.begin(1920, 1080, Source::Slate, 2102) != nullptr, "invalid weak owner cannot poison later requests");

    Coordinator retirement;
    std::atomic<int> destroyed{};
    auto owned_request = retirement.begin(1920, 1080, Source::Slate, 100);
    retirement.acquire_creation(100);
    auto owner = std::shared_ptr<Owner>{new Owner{}, [&](Owner* pointer) {
        // Re-entry would deadlock if the last owner were released inside the gate.
        (void)retirement.snapshot();
        ++destroyed;
        delete pointer;
    }};
    retirement.created(owned_request, std::move(owner), 101);
    owned_request.reset();
    retirement.cancel("retire outside gate", [] {});
    expect(destroyed == 1, "owner cleanup is outside the lifecycle gate and exactly once");
}

void test_concurrent_callbacks() {
    for (int iteration = 0; iteration < 64; ++iteration) {
        Coordinator state;
        auto request = prepare(state, Source::Slate);
        std::atomic<int> acquired{}, published{};
        std::barrier start{3};
        auto consumer = [&](Source source) {
            start.arrive_and_wait();
            auto lease = state.acquire(source, 1000);
            if (lease) {
                ++acquired;
                state.publish(lease, 1001, [&] { ++published; return true; });
            }
        };
        std::jthread slate{consumer, Source::Slate};
        std::jthread prerender{consumer, Source::PreRender};
        start.arrive_and_wait();
        slate.join();
        prerender.join();
        expect(acquired == 1 && published == 1, "simultaneous validated callbacks execute and publish once");
    }
    for (int iteration = 0; iteration < 64; ++iteration) {
        Coordinator state;
        auto request = prepare(state, Source::PreRender);
        auto lease = state.acquire(Source::PreRender, 101);
        std::barrier start{3};
        int target{}; // Every access is inside the publication gate or after join.
        std::jthread publisher{[&] {
            start.arrive_and_wait();
            state.publish(lease, 102, [&] { target = 1; return true; });
        }};
        std::jthread canceller{[&] {
            start.arrive_and_wait();
            state.cancel("external target", [&] { target = 2; });
        }};
        start.arrive_and_wait();
        publisher.join();
        canceller.join();
        expect(target == 2 && state.snapshot().stage == Stage::Cancelled,
            "cancellation wins over stale publication in either thread ordering");
    }
}
}

int test_ue58_ui_initialization() {
    test_normal_startup();
    test_callback_handoff();
    test_voyage_startup_order_replay();
    test_timeouts_and_failures();
    test_cancel_and_resize();
    test_dynamic_extents();
    test_owner_loss_and_retirement();
    test_concurrent_callbacks();
    return failures;
}
