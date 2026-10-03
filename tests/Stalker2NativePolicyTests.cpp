#include "mods/vr/Stalker2NativePolicy.hpp"
#include "mods/vr/NativeFrameDiagnosticsJson.hpp"
#include "mods/GameSpecific.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

namespace policy = uevr::stalker2_native;
namespace diag = uevr::native_frame;
static int failures{};
static void expect(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Packet {
    policy::Key stalker_key{};
    uint64_t serial{}, capture_generation{};
};
using Ring = policy::PacketRing<Packet, 4>;
static Ring::Ptr packet(uint64_t epoch, uint64_t serial, uint64_t generation, uint32_t primary, uint32_t secondary) {
    return std::make_shared<Packet>(Packet{policy::make_key(epoch, primary, secondary, 0), serial, generation});
}

static void test_scope() {
    const policy::Scope enabled{true, true, true, true, true, false, false};
    expect(enabled.enabled(), "validated opted-in scope enabled");
    for (int field = 0; field != 7; ++field) {
        auto disabled = enabled;
        if (field == 0) { disabled.requested = false; }
        if (field == 1) { disabled.stalker_ue55 = false; }
        if (field == 2) { disabled.dx12 = false; }
        if (field == 3) { disabled.native_fix = false; }
        if (field == 4) { disabled.openxr = false; }
        if (field == 5) { disabled.array_submit = true; }
        if (field == 6) { disabled.two_d = true; }
        expect(!disabled.enabled(), "each unsupported mode/backend/runtime leaves existing path");
    }
    expect(!policy::Scope{}.enabled(), "default off");
    constexpr auto exe = L"D:\\Games\\Stalker2\\Stalker2-Win64-Shipping.exe";
    expect(policy::is_executable(exe) && policy::is_executable(L"STALKER2-WIN64-SHIPPING.EXE"), "exact filename case insensitive");
    expect(!policy::is_executable(L"D:\\Stalker2-Win64-Shipping\\Other.exe") &&
        !policy::is_executable(L"Stalker2-Win64-Shipping-copy.exe"), "directory substring and renamed binaries not accepted");
    for (const auto version : {L"5.4.4", L"5.6.1", L"5.7.4", L"5.8.3"}) {
        expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(exe, version, 0x00050005, true, true),
            "detected non-5.5 engine cannot activate experiment");
    }
    expect(uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(exe, L"5.5.4", 0, true, true), "UE5.5 game gate");
    expect(!uevr::games::should_use_stalker2_ue55_native_fix_capture_layout(L"Bodycam-Win64-Shipping.exe", L"5.5.4", 0, true, true),
        "other UE5.5 game unchanged");
}
static void test_keys_and_rejection() {
    const auto equal = policy::make_key(1, 5, 5, 2);
    const auto next = policy::make_key(1, 5, 6, 2);
    expect(equal.matches(7, 1) && !equal.matches(8, 1), "same-family transaction plus compensation");
    expect(next.matches(7, 1) && next.matches(8, 1) && !next.matches(9, 1), "primary and secondary+1 exact keys only");
    expect(!policy::make_key(1, 5, 7, 0).valid && !policy::make_key(0, 5, 6, 0).valid, "unknown delta/epoch fail closed");
    const auto wrap = policy::make_key(1, UINT32_MAX, 0, 1);
    expect(wrap.matches(0, 1) && wrap.matches(1, 1) && !wrap.matches(UINT32_MAX, 1), "32-bit wrap with compensation");
    Ring ring;
    const auto epoch = ring.epoch();
    auto older = packet(epoch, 21, 7, 10, 11), latest = packet(epoch, 22, 7, 14, 15);
    expect(ring.publish(older) && ring.select(10) == older && ring.select(11) == older, "both exact ring keys return packet");
    expect(ring.publish(latest) && !ring.select(10) && ring.select(14) == latest, "colliding older metadata is not historical image ownership");
    expect(!ring.publish(older), "out-of-order serial cannot replace latest");
    ring.note_handoff(15, 9);
    expect(ring.handoff().frame == 15 && ring.handoff().thread == 9, "RHI thread/frame captured together");
    expect(ring.reject(older, 7) && ring.epoch() != epoch && !ring.latest() && !ring.handoff().epoch,
        "rejection of older selected pointer quarantines current generation");
    expect(!ring.publish(latest), "old epoch cannot republish");
    const auto current_epoch = ring.epoch();
    older = packet(current_epoch, 30, 7, 20, 21); latest = packet(current_epoch, 31, 8, 22, 23);
    expect(ring.publish(older) && ring.publish(latest), "replacement generation published");
    expect(!ring.reject(older, 8) && ring.epoch() == current_epoch && ring.select(23) == latest,
        "delayed rejection does not poison replacement generation");
    ring.invalidate();
    expect(!ring.latest() && !ring.select(23), "travel/mode/reset invalidates ring");
}
static void test_priority() {
    policy::SharpenPriority priority;
    const auto observe = [&](uint32_t flags, float wanted = 0.5f, float actual = 0.0f) {
        return priority.observe(true, 0x1000, flags, wanted, actual);
    };
    expect(!observe(0x0c000060) && !observe(0x0c000060) && observe(0x0c000060) == 0x0c000000,
        "three observed mismatches match game Commandline priority without Console escalation");
    expect(!observe(0x0d000060) && !observe(0x0d000060) && observe(0x0d000060) == 0x0d000000,
        "changed game priority resets evidence");
    for (const auto flags : {0x08000060u, 0x0e000060u, 0x0c000061u, 0x0c000064u, 0x0c000068u, 0x0c000070u}) {
        expect(!observe(flags) && !observe(flags) && !observe(flags), "lower/Console/cheat/read-only/unregistered/placeholder refused");
    }
    priority = {};
    expect(!observe(0x0c000060) && !observe(0x0c000060), "pending mismatch evidence");
    expect(!priority.observe(false, 0x1000, 0x0c000060, 0.5f, 0.0f) && !priority.mismatches,
        "disable/unfreeze clears local state without engine priority writes");
    expect(!observe(0x0c000060), "new request does not inherit count");
    const auto requested = 0.1234567f;
    const auto text = policy::float_text(requested);
    const std::string narrow{text.begin(), text.end()};
    float decoded{};
    const auto read = std::from_chars(narrow.data(), narrow.data() + narrow.size(), decoded);
    expect(read.ec == std::errc{} && decoded == requested && policy::close_float(requested, decoded), "round-trip-safe float serialization");
    for (int i = 0; i != 10; ++i) { expect(!observe(0x0c000060, requested, decoded), "accepted float cannot falsely escalate"); }
    expect(policy::float_text(std::numeric_limits<float>::infinity()).empty(), "nonfinite target refused");
    expect(!observe(0x0c000060, 0.5f, std::numeric_limits<float>::quiet_NaN()), "nonfinite readback refused");
    const std::array<uint8_t, 4> good{0x8b, 0x41, 0x18, 0xc3}, bad{0x8b, 0x41, 0x10, 0xc3};
    expect(policy::flags_accessor(good) && !policy::flags_accessor(bad) &&
        !policy::flags_accessor(std::span{good}.first(3)), "source-backed GetFlags accessor instruction validated");
}
static void test_pair_validity() {
    const policy::PairIdentity identity{2, 7, 99, 1, 2, 3, 4, 5, 6, UINT32_MAX - 1};
    policy::PairValidity pair;
    pair.commit(identity, 100, false, true);
    expect(!pair.submitted, "Close/null-queue/Signal failure cannot publish cache");
    pair.commit(identity, 100, true, false);
    expect(!pair.submitted, "release failure cannot publish cache");
    pair.commit(identity, 100, true, true);
    expect(pair.reusable(identity, 0, 199), "bounded pair across frame-number wrap");
    expect(!pair.reusable(identity, 2, 199) && !pair.reusable(identity, 0, 201) && !pair.reusable(identity, 0, 99),
        "frame/time/backwards-clock bounds enforced");
    for (int field = 0; field != 8; ++field) {
        auto changed = identity;
        if (field == 0) { ++changed.epoch; } if (field == 1) { ++changed.generation; }
        if (field == 2) { ++changed.device; } if (field == 3) { ++changed.queue; }
        if (field == 4) { ++changed.left; } if (field == 5) { ++changed.right; }
        if (field == 6) { ++changed.scene; } if (field == 7) { ++changed.target; }
        expect(!pair.reusable(changed, 0, 150), "epoch/generation/device/queue/source/scene/target invalidates reuse");
    }
    for (int i = 0; i != 3; ++i) { expect(pair.reusable(identity, 0, 150), "three reuses supported"); ++pair.reuses; }
    expect(!pair.reusable(identity, 0, 150), "fourth reuse refused");
    pair.invalidate(); expect(!pair.submitted, "invalidated pair not reusable");
    expect(policy::fence_retired(5, 5) && !policy::fence_retired(4, 5) &&
        !policy::fence_retired(UINT64_MAX, 5) && !policy::fence_retired(0, 0), "fence sentinel/unsubmitted values never prove retirement");
    policy::SubmissionProof proof{10, 11, true, true, true, true, true, true};
    expect(proof.confirmed(), "successful recorded enqueue on binding queue confirmed");
    for (int field = 0; field != 8; ++field) {
        auto rejected = proof;
        if (field == 0) { rejected.recorded = false; } if (field == 1) { rejected.healthy = false; }
        if (field == 2) { rejected.waiting = false; } if (field == 3) { rejected.has_fence = false; }
        if (field == 4) { rejected.ordered_queue = false; } if (field == 5) { rejected.same_device = false; }
        if (field == 6) { rejected.fence_after = 10; } if (field == 7) { rejected.fence_after = UINT64_MAX; }
        expect(!rejected.confirmed(), "incomplete submission cannot confirm cache");
    }
    expect(policy::valid_pair_bounds(64, 32, 32, 32, 64, 32, 32, 32), "exact double-wide bounds");
    expect(!policy::valid_pair_bounds(31, 32, 32, 32, 64, 32, 32, 32) &&
        !policy::valid_pair_bounds(64, 32, 16, 16, 64, 32, 32, 32) &&
        !policy::valid_pair_bounds(64, 32, 32, 32, 65, 32, 32, 32), "undersized/quarter-sized/non-double-wide rejected");
}
static void test_diagnostics_and_concurrency() {
    diag::Recorder<16> recorder;
    expect(!recorder.control() && !recorder.ticket(0, 1), "diagnostics default off");
    recorder.set_enabled(true);
    const auto token = recorder.control();
    auto ticket = recorder.ticket(token, 42); ticket.submit_render = 7; ticket.pair_reused = true;
    diag::Event event{};
    event.attempt = ticket.attempt; event.packet_serial = 42; event.submit_render = ticket.submit_render;
    event.transaction_epoch = 9; event.transaction_primary = 5; event.transaction_secondary = 6;
    event.source_pose_frame = 4; event.pair_reused = ticket.pair_reused;
    event.stage = diag::Stage::selection; event.reason = diag::Reason::cached_pair; event.packet_accepted = true;
    recorder.record(token, event);
    event.stage = diag::Stage::copy_recorded; recorder.record(token, event);
    event.stage = diag::Stage::submit_attempt; recorder.record(token, event);
    event.stage = diag::Stage::submit_result; recorder.record(token, event);
    const auto json = diag::export_json(recorder);
    expect(json.at("consumer_identity_observed") == true && json.at("events").size() == 4,
        "keyed selection remains visible through copy/submit diagnostics");
    for (const auto& saved : json.at("events")) {
        expect(saved.at("selection_attempt") == ticket.attempt && saved.at("source_pose_frame") == 4 &&
            saved.at("stalker_transaction_epoch") == 9 && saved.at("stalker_pair_reused") == true,
            "cached source pose/epoch/reuse and ticket survive export");
    }
    Ring ring; std::atomic_bool bad{false};
    std::jthread publisher{[&] {
        for (uint32_t i = 1; i != 20001; ++i) {
            const auto epoch = ring.epoch();
            ring.publish(packet(epoch, i, epoch, i, i + 1));
            ring.note_handoff(i + 1, 1);
        }
    }};
    std::jthread reader{[&] {
        for (uint32_t i = 1; i != 20001; ++i) {
            if (const auto selected = ring.select(i); selected && !selected->stalker_key.matches(i, selected->stalker_key.epoch)) { bad = true; }
            if (i % 113 == 0) { ring.invalidate(); }
        }
    }};
    publisher.join(); reader.join();
    expect(!bad, "concurrent publication/invalidation/selection preserves immutable transaction keys");
}
int main() {
    test_scope(); test_keys_and_rejection(); test_priority(); test_pair_validity(); test_diagnostics_and_concurrency();
    std::printf("Stalker 2 opt-in scope/key/rejection/priority/cache/diagnostic checks: %d failures\n", failures);
    return failures != 0;
}
