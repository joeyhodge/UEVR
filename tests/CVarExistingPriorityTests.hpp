#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include "mods/vr/CVarExistingPriority.hpp"
#include "mods/vr/CVarDiagnostics.hpp"

namespace cvar_existing_priority_tests {
namespace p = uevr::cvar_existing_priority;

struct Fixture {
    static constexpr uintptr_t base = 0x10000, manager = base + 0x100,
        entries = base + 0x1000, text = base + 0x3000, object = base + 0x4000,
        table = base + 0x5000, code = base + 0x6000;
    std::array<uint8_t, 0x8000> bytes{};
    bool readable{true}, owned{true}, executable{true};
    size_t reads{};
    size_t mutate_at{};
    uintptr_t mutation_address{}, mutation_value{};
    template<class T> void put(uintptr_t address, const T& value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
    Fixture() {
        put(manager + 8, p::Table{entries, 1, 1});
        put(entries, p::Entry{text, 16, 16, object, {0, 0}});
        const std::u16string name = u"r.MaxAnisotropy";
        put(entries + 8, int32_t(name.size() + 1));
        put(entries + 12, int32_t(name.size() + 1));
        std::memcpy(bytes.data() + text - base, name.c_str(), (name.size() + 1) * 2);
        put(object, table);
        put(object + 0x18, uint32_t{0x0f000160});
        put(table + 2 * sizeof(uintptr_t), code + 0x100);
        put(table + 3 * sizeof(uintptr_t), code);
        put(code, p::flags_getter);
        put(code + 0x100, std::array<uint8_t, 4>{0x48, 0x83, 0xc1, 0x08});
    }
    p::Memory memory() {
        return {this,
            [](void* context, uintptr_t address, void* out, size_t count) {
                auto& self = *static_cast<Fixture*>(context);
                ++self.reads;
                if (self.reads == self.mutate_at) { self.put(self.mutation_address, self.mutation_value); }
                if (!self.readable || address < base || address - base > self.bytes.size() ||
                    count > self.bytes.size() - (address - base)) { return false; }
                std::memcpy(out, self.bytes.data() + address - base, count);
                return true;
            },
            [](void* context, uintptr_t address, size_t count, bool execute) {
                const auto& self = *static_cast<Fixture*>(context);
                return self.owned && (!execute || self.executable) &&
                    address >= table && address <= base + self.bytes.size() &&
                    count <= base + self.bytes.size() - address;
            }};
    }
};

struct Engine {
    p::Proof proof{Fixture::object, Fixture::table, Fixture::code, 0x0f000160};
    int resolves{}, snapshots{}, bindings{}, currents{}, writes{};
    bool bound{true}, readable{true}, callable{true}, accepted{true};
    int change_flags_at{}, change_binding_at{}, stale_at{}, rebind_at{}, unreadable_at{};
    int change_table_at{}, change_getter_at{};
    uint32_t priority_used{}, callback_flags{};
    double actual{8}, requested{};
    p::Backend backend() {
        return {
            [&]() { return ++currents != stale_at; },
            [&]() { return ++resolves == rebind_at ? Fixture::object + 8 : Fixture::object; },
            [&](uintptr_t) -> std::optional<p::Proof> {
                ++snapshots;
                if (!readable || snapshots == unreadable_at) { return std::nullopt; }
                auto value = proof;
                if (snapshots == change_flags_at) { value.flags ^= 0x4; }
                if (snapshots == change_table_at) { value.table += 8; }
                if (snapshots == change_getter_at) { value.getter += 8; }
                if (snapshots >= 3 && callback_flags) { value.flags = callback_flags; }
                return value;
            },
            [&](uintptr_t) { return bound && ++bindings != change_binding_at; },
            [&](uintptr_t, double value, uint32_t priority) {
                ++writes;
                requested = value;
                priority_used = priority;
                if (callable && accepted) { actual = value; }
                return callable;
            }};
    }
};

template<class Expect>
void run(const Expect& expect) {
    Fixture memory;
    const auto before = memory.bytes;
    const auto proof = p::read_flags(memory.memory(), Fixture::object);
    expect(proof && proof->flags == 0x0f000160 && proof->getter == Fixture::code,
        "Existing-priority action reads the source-verified slot-3 flags getter without invoking it");
    expect(memory.bytes == before, "Flag and registry observation never modifies input memory");
    expect(p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
        "Existing-priority action verifies the cached name/object binding");
    expect(p::registry_matches(memory.memory(), Fixture::manager, L"R.MAXANISOTROPY", Fixture::object),
        "Console binding comparison preserves case-insensitive names");
    expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MotionBlurQuality", Fixture::object) &&
        !p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object + 8),
        "A name or object mismatch is never accepted");
    for (size_t i = 0; i < p::flags_getter.size(); ++i) {
        memory.bytes[Fixture::code - Fixture::base + i] ^= 1;
        expect(!p::read_flags(memory.memory(), Fixture::object), "Changed flags getter code is rejected");
        memory.bytes[Fixture::code - Fixture::base + i] ^= 1;
    }
    memory.put(Fixture::table + 3 * sizeof(uintptr_t), Fixture::code + 0x100);
    expect(!p::read_flags(memory.memory(), Fixture::object),
        "Legacy slot-2 SetHelp/adjustor bytes are never invoked as GetFlags");
    memory = Fixture{};
    memory.executable = false;
    expect(!p::read_flags(memory.memory(), Fixture::object), "A non-executable accessor is rejected");
    memory = Fixture{};
    memory.owned = false;
    expect(!p::read_flags(memory.memory(), Fixture::object), "Non-module-owned vtables/accessors are rejected");
    memory = Fixture{};
    memory.readable = false;
    expect(!p::read_flags(memory.memory(), Fixture::object), "Unreadable flag data fails closed");
    memory = Fixture{};
    expect(!p::read_flags(memory.memory(), UINTPTR_MAX - 8) &&
        !p::read_flags(memory.memory(), Fixture::object + 1), "Overflowing or misaligned CVar pointers are rejected");
    for (const auto count : {0u, 65537u, UINT32_MAX}) {
        memory.put(Fixture::manager + 8, p::Table{Fixture::entries, count, count});
        expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
            "Invalid registry sizes are bounded before traversal");
    }
    memory = Fixture{};
    memory.put(Fixture::entries + 8, int32_t{100000});
    expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
        "Malformed name lengths are not dereferenced");
    memory = Fixture{};
    p::Entry entry{};
    std::memcpy(&entry, memory.bytes.data() + Fixture::entries - Fixture::base, sizeof(entry));
    memory.put(Fixture::entries, p::Entry{});
    memory.put(Fixture::entries + 128 * sizeof(entry), entry);
    memory.put(Fixture::manager + 8, p::Table{Fixture::entries, 129, 129});
    expect(p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object) &&
        memory.reads <= 6, "Registry binding validation crosses a bounded stack-chunk boundary");
    memory = Fixture{};
    memory.mutate_at = 4;
    memory.mutation_address = Fixture::manager + 8;
    memory.mutation_value = Fixture::entries + 32;
    expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
        "A registry allocation changed during the name read is rejected");
    memory = Fixture{};
    memory.mutate_at = 5;
    memory.mutation_address = Fixture::entries + 16;
    memory.mutation_value = Fixture::object + 8;
    expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
        "A registry entry changed during the final read is rejected");
    memory = Fixture{};
    memory.mutate_at = 5;
    memory.mutation_address = Fixture::object;
    memory.mutation_value = Fixture::table + 8;
    expect(!p::read_flags(memory.memory(), Fixture::object), "A changed flags-vtable snapshot is rejected");
    memory = Fixture{};
    memory.put(Fixture::text + 2 * std::u16string_view(u"r.MaxAnisotropy").size(), uint16_t{'x'});
    expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
        "Registry names must have a readable terminator at the expected length");
    for (const auto table : {p::Table{Fixture::entries, 2, 1}, p::Table{Fixture::entries, 1, 131073},
                            p::Table{Fixture::entries + 1, 1, 1}, p::Table{UINTPTR_MAX - 7, 1, 1}}) {
        memory = Fixture{};
        memory.put(Fixture::manager + 8, table);
        expect(!p::registry_matches(memory.memory(), Fixture::manager, L"r.MaxAnisotropy", Fixture::object),
            "Invalid capacity, alignment and entry-range arithmetic fail closed");
    }

    const p::Request request{true, true, false, 16, 1, 16};
    Engine engine;
    auto result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::dispatched && engine.writes == 1 &&
        engine.priority_used == 0x0f000000 && engine.actual == 16 &&
        result.after_flags == engine.proof.flags,
        "One explicit action writes once at the existing Console priority without changing flags");
    auto disabled = request;
    disabled.explicit_click = false;
    engine = Engine{};
    result = p::apply_once(disabled, engine.backend());
    expect(result.state == p::State::opt_out && !engine.resolves && !engine.writes,
        "No explicit click means no discovery or write");
    disabled = request;
    disabled.supported_abi = false;
    result = p::apply_once(disabled, engine.backend());
    expect(result.state == p::State::unsupported && !engine.resolves && !engine.writes,
        "Unsupported engine interfaces remain completely untouched");
    for (const uint32_t flag : {0x1u, 0x4u, 0x8u, 0x10u}) {
        engine = Engine{};
        engine.proof.flags |= flag;
        result = p::apply_once(request, engine.backend());
        expect(result.state == p::State::protected_variable && !engine.writes,
            "Cheat, read-only, unregistered and placeholder CVars are not overridden");
    }
    for (const uint32_t flags : {0u, 0x08000020u, 0x10000000u, 0xff000000u}) {
        engine = Engine{};
        engine.proof.flags = flags;
        result = p::apply_once(request, engine.backend());
        expect(result.state == p::State::no_priority_conflict && !engine.writes,
            "Low/equal and unsupported priorities do not trigger an override");
    }
    for (uint32_t byte = 9; byte <= 15; ++byte) {
        engine = Engine{};
        engine.proof.flags = (byte << 24) | 0x160;
        result = p::apply_once(request, engine.backend());
        expect(result.state == p::State::dispatched && engine.writes == 1 &&
            engine.priority_used == (byte << 24), "Existing owner priority is neither raised nor lowered");
    }
    engine = Engine{};
    engine.bound = false;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::binding_changed && !engine.writes, "Unregistered/rebound names are rejected");
    engine = Engine{};
    engine.change_binding_at = 2;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::binding_changed && !engine.writes, "A late registry change cancels the write");
    engine = Engine{};
    engine.rebind_at = 2;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::binding_changed && !engine.writes, "A changed cached object cancels the write");
    engine = Engine{};
    engine.change_flags_at = 2;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::owner_changed && !engine.writes, "Changed owner/protection flags cancel the write");
    for (const bool table : {true, false}) {
        engine = Engine{};
        if (table) { engine.change_table_at = 2; } else { engine.change_getter_at = 2; }
        result = p::apply_once(request, engine.backend());
        expect(result.state == p::State::owner_changed && !engine.writes,
            "A replaced accessor or vtable cancels the write before dispatch");
    }
    for (int check = 1; check <= 3; ++check) {
        engine = Engine{};
        engine.stale_at = check;
        result = p::apply_once(request, engine.backend());
        expect(result.state == p::State::stale_request && !engine.writes, "A newer edit cancels a queued stale action");
    }
    engine = Engine{};
    engine.readable = false;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::unsupported && !engine.writes, "Missing flags evidence never authorizes a write");
    engine = Engine{};
    engine.callable = false;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::setter_unavailable && engine.writes == 1 && engine.actual == 8,
        "A failed setter is not retried and cannot escalate priority");
    for (double value : {0.0, 17.0, 1.5, (std::numeric_limits<double>::infinity)(),
                          (std::numeric_limits<double>::quiet_NaN)()}) {
        auto invalid = request;
        invalid.value = value;
        engine = Engine{};
        result = p::apply_once(invalid, engine.backend());
        expect(result.state == p::State::invalid_value && !engine.writes, "Invalid values are rejected before conversion");
    }
    auto floating = p::Request{true, true, true, 0.25, 0, 1};
    engine = Engine{};
    result = p::apply_once(floating, engine.backend());
    expect(result.state == p::State::dispatched && engine.actual == 0.25, "Validated float edits preserve their numeric type");
    engine = Engine{};
    engine.accepted = false;
    result = p::apply_once(request, engine.backend());
    uevr::cvar_diagnostics::WriteObservation observation;
    const auto id = observation.begin(16, false);
    observation.dispatched(id, result.state == p::State::dispatched, 0);
    for (int i = 1; i <= 4; ++i) { observation.observe(id, engine.actual, i * 100); }
    expect(observation.state == uevr::cvar_diagnostics::WriteState::not_observed && engine.writes == 1,
        "Dispatched is not success: actual-value readback reports engine rejection without a retry");
    engine = Engine{};
    engine.callback_flags = 0x0e000160;
    result = p::apply_once(request, engine.backend());
    expect(result.after_flags == 0x0e000160 && engine.writes == 1,
        "Engine callback changes are reported, not undone by flag writes or enforcement");
    engine = Engine{};
    engine.unreadable_at = 3;
    result = p::apply_once(request, engine.backend());
    expect(result.state == p::State::dispatched && !result.after_flags && engine.writes == 1,
        "Unavailable post-dispatch flags are reported without a retry or assumed success");
}
} // namespace cvar_existing_priority_tests
