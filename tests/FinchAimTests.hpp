#pragma once

#include <sdk/FinchAim.hpp>

namespace {
struct FinchAimFixture : FinchFixture {
    static constexpr uintptr_t camera = 0x300010000, controller = 0x300012000, player = 0x300014000;
    static constexpr uintptr_t controller_table = 0x300016000, camera_table = 0x300018000;
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
    }

    static bool live(uintptr_t p, sdk::finch::aim::Role role) {
        using R = sdk::finch::aim::Role;
        return (role == R::Camera && p == camera) || (role == R::Controller && p == controller) ||
            (role == R::LocalPlayer && p == player);
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
    expect(known && known->process_rotation == good.base + a::code::process_view_rotation_rva,
        "Finch selects its original native rotation path without an HMD object or newer XR table");
    if (!known) { return; }
    expect(a::native_version(good.memory(), good.base)->minor == 10,
        "Finch reads initialized native 4.10.1 metadata without a file version or branch regex");
    auto context = a::context(good.memory(), *known, good.layout, F::camera, F::live);
    expect(context && context->controller == F::controller && context->player == F::player,
        "Finch accepts a live reciprocal controller/camera/local-player binding");
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
