#pragma once

#include <sdk/FinchAim.hpp>
#include "mods/vr/FinchAimPose.hpp"

namespace {
struct FinchAimFixture : FinchFixture {
    static constexpr uintptr_t camera = 0x300010000, controller = 0x300012000, player = 0x300014000;
    static constexpr uintptr_t controller_table = 0x300016000, camera_table = 0x300018000;
    static constexpr uintptr_t pawn = 0x300020000, movement = 0x300022000, component = 0x300024000;
    sdk::finch::aim::Layout layout{0x3d0, 0x30};

    explicit FinchAimFixture(const nlohmann::json& fixture, uintptr_t load_base = 0x140000000)
        : FinchFixture{fixture, load_base} {
        namespace a = sdk::finch::aim;
        add(base + a::version_record_rva, sizeof(a::Version));
        put(base + a::version_record_rva, a::Version{4, 10, 1, 0, 0x8001424f});
        add(base + a::camera_vtable_rva, (a::process_rotation_slot + 1) * sizeof(uintptr_t));
        add(base + a::controller_vtable_rva, (a::controller_rotation_slot + 1) * sizeof(uintptr_t));
        add(base + a::finch_controller_vtable_rva, (a::controller_rotation_slot + 1) * sizeof(uintptr_t));
        put(base + a::camera_vtable_rva + a::process_rotation_slot * sizeof(uintptr_t), base + a::code::process_view_rotation_rva);
        put(base + a::controller_vtable_rva + a::controller_rotation_slot * sizeof(uintptr_t), base + a::code::controller_rotation_rva);
        put(base + a::finch_controller_vtable_rva + a::controller_rotation_slot * sizeof(uintptr_t), base + a::code::controller_rotation_rva);
        add(camera, 0x1008); add(controller, 0x1008); add(player, 0x1008);
        add(controller_table, (a::controller_rotation_slot + 1) * sizeof(uintptr_t));
        add(camera_table, (a::process_rotation_slot + 1) * sizeof(uintptr_t));
        put(camera, camera_table); put(controller, controller_table);
        put(camera + a::owner_offset, controller);
        put(controller + a::camera_offset, camera);
        put(controller + layout.player, player);
        put(player + layout.local_controller, controller);
        put(controller_table + a::controller_rotation_slot * sizeof(uintptr_t), base + a::code::controller_rotation_rva);
        put(camera_table + a::process_rotation_slot * sizeof(uintptr_t), base + a::code::process_view_rotation_rva);
        add(pawn, 0x800); add(movement, 0x700); add(component, 0x600);
        put(controller + 0x350, pawn); put(camera + 0xb78, pawn); put(camera + 0xf60, uintptr_t{0});
        put(pawn + 0x3b8, movement); put(pawn + 0x598, component);
        put(movement + 0x618, pawn); put(movement + 0x4c8, component); put(component + 0xa2, uint8_t{4});
    }

    static bool live(uintptr_t p, sdk::finch::aim::Role role) {
        using R = sdk::finch::aim::Role;
        return (role == R::Camera && p == camera) || (role == R::Controller && p == controller) ||
            (role == R::LocalPlayer && p == player) || (role == R::Character && p == pawn) ||
            (role == R::Movement && p == movement) || (role == R::CameraComponent && p == component);
    }
};

void test_finch_aim_contracts() {
    namespace a = sdk::finch::aim;
    using F = FinchAimFixture;
    std::ifstream file{std::filesystem::path{__FILE__}.parent_path() / "fixtures/finch-20170621-aim-contracts.json"};
    expect(file.good(), "Finch native aim fixture exists");
    if (!file.good()) { return; }
    const auto captured = nlohmann::json::parse(file);
    F good{captured};
    const auto known = a::contract(good.memory(), good.base);
    expect(known && known->process_rotation == good.base + a::code::process_view_rotation_rva &&
        known->sync_rotation == good.base + a::code::sync_rotation_rva &&
        a::is_camera_handoff(*known, good.base + 0x12768b),
        "Finch validates the authoritative camera handoff rather than mutating early ProcessViewRotation");
    if (!known) { return; }
    expect(a::native_version(good.memory(), good.base)->minor == 10,
        "Finch reads initialized native 4.10.1 metadata without a file version or branch regex");
    auto context = a::context(good.memory(), *known, good.layout, F::camera, F::live);
    expect(context && context->controller == F::controller && context->player == F::player,
        "Finch accepts a live reciprocal controller/camera/local-player binding");
    expect(a::controller_context(good.memory(), *known, good.layout, F::controller, F::live).has_value() &&
        !a::controller_context(good.memory(), *known, good.layout, F::camera, F::live),
        "Finch native handoff requires its actual local controller, not a camera or foreign object");
    expect(!a::is_camera_handoff(*known, known->sync_return - 5) && !a::is_camera_handoff(*known, 0),
        "Finch rejects helper calls without the verified camera-component consumer");
    auto wrong_handoff = *known;
    wrong_handoff.sync_rotation += 8;
    expect(!a::is_camera_handoff(wrong_handoff, known->sync_return), "Finch rejects a different native helper contract");
    expect(a::camera_consumer(good.memory(), *known, good.layout, F::controller, F::live).has_value(),
        "Finch only compensates for a live active camera reached by the verified movement-camera consumer");
    for (const auto [address, value] : std::array<std::pair<uintptr_t, uintptr_t>, 7>{{
        {F::controller + 0x350, 0}, {F::camera + 0xb78, F::player}, {F::camera + 0xf60, F::pawn},
        {F::pawn + 0x3b8, 0}, {F::pawn + 0x598, 0}, {F::movement + 0x618, F::player},
        {F::movement + 0x4c8, 0}
    }}) {
        auto missing = good; missing.put(address, value);
        expect(!a::camera_consumer(missing.memory(), *known, missing.layout, F::controller, F::live),
            "Finch missing, replaced, detached or blended camera consumers cannot authorize recenter compensation");
    }
    auto inactive = good; inactive.put(F::component + 0xa2, uint8_t{0});
    expect(!a::camera_consumer(inactive.memory(), *known, inactive.layout, F::controller, F::live),
        "Finch inactive camera components preserve original game aim");
    F relocated{captured, 0x7ff600000000};
    expect(a::contract(relocated.memory(), relocated.base).has_value(), "Finch aim proof is ASLR-safe");

    for (const auto& block : good.blocks) {
        if (!block.code) { continue; }
        for (size_t i = 0; i < block.bytes.size(); ++i) {
            auto changed = good;
            changed.put(block.address + i, uint8_t(block.bytes[i] ^ 1));
            expect(!a::contract(changed.memory(), changed.base), "Finch aim rejects every changed original-code witness byte");
        }
    }
    auto changed = good;
    changed.put(good.base + a::version_record_rva, a::Version{4, 11, 1, 0, 0x8001424f});
    expect(!a::contract(changed.memory(), changed.base), "Finch cannot select its legacy aim ABI from a different native engine version");
    changed = good; changed.put(good.base + 0x88, uint32_t{0});
    expect(!a::contract(changed.memory(), changed.base), "Finch aim rejects a different executable image");
    changed = good; changed.executable = false;
    expect(!a::contract(changed.memory(), changed.base), "Finch aim requires executable module-owned original code");
    changed = good; changed.readable = false;
    expect(!a::contract(changed.memory(), changed.base), "Finch aim rejects unreadable memory without executing candidate code");
    for (const auto table : {a::controller_vtable_rva, a::finch_controller_vtable_rva}) {
        changed = good;
        changed.put(good.base + table + a::controller_rotation_slot * sizeof(uintptr_t), good.base + a::code::process_view_rotation_rva);
        expect(!a::contract(changed.memory(), changed.base), "Finch cannot substitute an unrelated controller vtable method");
    }
    changed = good;
    changed.put(good.base + a::camera_vtable_rva + a::process_rotation_slot * sizeof(uintptr_t), good.base + a::code::controller_rotation_rva);
    expect(!a::contract(changed.memory(), changed.base), "Finch cannot substitute a different camera callback ABI");

    const auto accepts = [&](F& fixture, a::Layout layout = {0x3d0, 0x30}) {
        return a::context(fixture.memory(), *known, layout, F::camera, F::live).has_value();
    };
    for (const auto [address, value] : std::array<std::pair<uintptr_t, uintptr_t>, 6>{{
        {F::camera + a::owner_offset, 0}, {F::controller + a::camera_offset, F::player},
        {F::controller + good.layout.player, F::controller}, {F::player + good.layout.local_controller, F::player},
        {F::controller_table + a::controller_rotation_slot * sizeof(uintptr_t), known->process_rotation},
        {F::camera_table + a::process_rotation_slot * sizeof(uintptr_t), good.base + a::code::controller_rotation_rva}
    }}) {
        changed = good; changed.put(address, value);
        expect(!accepts(changed), "Finch rejects retired, mismatched, foreign or overridden current camera bindings");
    }
    expect(!a::context(good.memory(), *known, good.layout, F::camera,
        [](uintptr_t, a::Role) { return false; }), "Finch never adjusts dead or wrong-class UObjects");
    expect(!a::context(good.memory(), *known, good.layout, 0, F::live) &&
        !a::context(good.memory(), *known, good.layout, 0x7ffffffffff8, F::live), "Finch rejects null/overflowing camera pointers");
    for (const auto layout : {a::Layout{}, a::Layout{0x27, 0x30}, a::Layout{0x3d1, 0x30},
                             a::Layout{0x3d8, 0x30}, a::Layout{0x3d0, 0x1008}}) {
        expect(!accepts(good, layout), "Finch rejects unknown or overlapping reflected ownership fields");
    }
    auto corrupt = *known; corrupt.process_rotation += 8;
    expect(!a::context(good.memory(), corrupt, good.layout, F::camera, F::live), "Finch binding cannot authorize a different hook target");

    for (unsigned mask = 0; mask != 64; ++mask) {
        const bool aim = mask & 1, hmd = mask & 2, controller = mask & 4, tracked = mask & 8,
            guard = mask & 16, local = mask & 32;
        expect(a::should_adjust(aim, hmd, controller, tracked, guard, local) ==
            (aim && hmd && local && !guard && (!controller || tracked)),
            "Finch Game aim, inactive HMD, unavailable controller, conflict guard and foreign context preserve native output");
    }
    a::SetupBudget budget;
    expect(budget.take(0) && !budget.take(999) && budget.take(1000) && budget.take(2000) &&
        !budget.take(3000) && !budget.take(UINT64_MAX), "Finch cold setup is delayed and capped at three attempts");
    budget.reset();
    expect(budget.take(3000) && budget.attempts == 1, "Returning to Game aim permits a deliberate setup retry");
    budget.reset();
    expect(budget.take(UINT64_MAX - 1) && !budget.take(UINT64_MAX - 2) &&
        budget.next_ms == UINT64_MAX, "Finch setup delay cannot wrap its monotonic clock");
}

void test_finch_aim_pose() {
    namespace p = uevr::finch_aim;
    const auto identity = glm::identity<glm::quat>();
    const auto orientation = [](float pitch, float yaw, float roll) {
        return glm::quat{glm::yawPitchRoll(glm::radians(-yaw), glm::radians(pitch), glm::radians(-roll))};
    };
    const auto close = [](float a, float b) { return std::abs(a - b) < 0.002f; };
    const auto right = p::plan({0, 0, 0}, orientation(0, 30, 0), identity);
    const auto left = p::plan({0, 0, 0}, orientation(0, -30, 0), identity);
    const auto up = p::plan({0, 0, 0}, orientation(25, 0, 0), identity);
    const auto down = p::plan({0, 0, 0}, orientation(-25, 0, 0), identity);
    expect(right && close(right->world_degrees.y, 30) && left && close(left->world_degrees.y, -30),
        "Finch right/left HMD yaw maps to the same Unreal aiming direction, without an inversion setting");
    expect(up && close(up->world_degrees.x, 25) && down && close(down->world_degrees.x, -25),
        "Finch vertical HMD aiming is retained rather than flattened out of the authoritative camera");
    for (const auto yaw : {-179.0f, -45.16595f, 0.0f, 90.0f, 179.0f}) {
        for (const auto pitch : {-75.0f, -25.0f, 0.0f, 25.0f, 75.0f}) {
            for (const auto head_yaw : {-80.0f, -20.0f, 0.0f, 20.0f, 80.0f}) {
                for (const auto roll : {-20.0f, 0.0f, 20.0f}) {
                    const auto hmd = orientation(pitch, head_yaw, roll);
                    const auto plan = p::plan({-63.28015f, yaw, 0}, hmd, identity);
                    expect(plan.has_value(), "Finch supports pitched authored cameras, combined head rotation and wraparound");
                    if (!plan) { continue; }
                    auto native_output = plan->world_degrees;
                    for (glm::length_t i = 0; i < 3; ++i) {
                        native_output[i] = std::fmod(native_output[i], 360.0f);
                        if (native_output[i] < 0) { native_output[i] += 360.0f; }
                    }
                    expect(p::accepted(*plan, native_output), "Finch accepts the native helper's normalized output without changing its orientation");
                    const auto camera = p::engine_basis(native_output);
                    const auto heading = camera ? p::yaw_only(*camera) : std::nullopt;
                    const auto rendered = heading ? glm::normalize(*heading * plan->next_offset * hmd) : identity;
                    expect(std::abs(glm::dot(rendered, plan->world)) > 0.99999f,
                        "Finch recenter plus decoupled-pitch rendering consumes head motion exactly once after the camera handoff");
                    const auto authored = p::engine_basis({-63.28015f, yaw, 0});
                    expect(authored && !p::accepted(*plan, {-63.28015f, yaw, 0}),
                        "Finch refuses recenter compensation when a later writer restores the old camera (captured failure)");
                }
            }
        }
    }
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expect(!p::plan({nan, 0, 0}, identity, identity) && !p::plan({0, 0, 0}, {0, 0, 0, 0}, identity) &&
        !p::plan({0, 0, 0}, identity, {nan, 0, 0, 0}), "Finch invalid poses cannot create a compensation plan");
    expect(!p::yaw_only(orientation(90, 0, 0)), "Finch vertical singularities fail closed instead of creating an invalid yaw offset");
    auto camera = glm::vec3{-63.28015f, -45.16595f, 0};
    auto control = up->world_degrees;
    control = camera;
    expect(!p::accepted(*up, control), "Finch early control-rotation mutation is overwritten by the game's camera writer");
    control = up->world_degrees;
    camera = control;
    expect(p::accepted(*up, camera), "Finch's verified handoff keeps controller and camera on the selected orientation");
}

void test_finch_aim_image(const char* path) {
    std::ifstream stream{path, std::ios::binary};
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (bytes.size() != sdk::finch::image_size) { expect(false, "Finch aim needs the complete RVA-mapped capture"); return; }
    uint32_t nt{}; uintptr_t base{};
    std::memcpy(&nt, bytes.data() + 0x3c, sizeof(nt));
    if (nt > 0x300) { expect(false, "Finch aim image header is in bounds"); return; }
    std::memcpy(&base, bytes.data() + nt + 24 + 24, sizeof(base));
    struct Image { uintptr_t base; const std::vector<uint8_t>& bytes; } image{base, bytes};
    sdk::discovery::Memory memory{&image, [](void* c, uintptr_t p, void* out, size_t n) {
        const auto& i = *static_cast<Image*>(c);
        if (p < i.base || p - i.base > i.bytes.size() || n > i.bytes.size() - (p - i.base)) { return false; }
        std::memcpy(out, i.bytes.data() + p - i.base, n); return true;
    }, [](void* c, uintptr_t p, size_t n) {
        const auto& i = *static_cast<Image*>(c);
        return p >= i.base + 0x1000 && p - i.base < 0x1f1be6b && n <= 0x1f1be6b - (p - i.base);
    }};
    expect(sdk::finch::aim::native_version(memory, base).has_value(), "Finch original image proves the native 4.10.1 metadata");
    expect(sdk::finch::aim::contract(memory, base).has_value(), "Finch original image proves both rotation ABIs and their vtable ownership");
}
}
