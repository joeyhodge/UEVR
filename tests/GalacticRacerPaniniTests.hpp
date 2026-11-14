#pragma once

#include <fstream>
#include "mods/vr/GalacticRacerPaniniPolicy.hpp"
#include "fixtures/SwgrPaniniCode.hpp"

void test_swgr_panini() {
    namespace p = uevr::swgr_panini;
    const std::array<uint8_t,7> flags{0x8b,0x41,0x18,0xc3,0xcc,0xcc,0xcc};
    const std::array<uint8_t,7> wide_flags{0x8b,0x81,0x80,0,0,0,0xc3};
    expect(p::flags_offset(flags) == 0x18 && p::flags_offset(wide_flags) == 0x80,
        "UE5.7 flags use a proven pure scalar accessor, not the old SetHelp slot");
    for (const auto bytes : {std::array<uint8_t,7>{0x89,0x41,0x18,0xc3,0,0,0},
            std::array<uint8_t,7>{0x8b,0x41,0xf8,0xc3,0,0,0},
            std::array<uint8_t,7>{0x8b,0x41,0x19,0xc3,0,0,0},
            std::array<uint8_t,7>{0x8b,0x81,0,2,0,0,0xc3},
            std::array<uint8_t,7>{0xe9,0,0,0,0,0,0}}) {
        expect(!p::flags_offset(bytes), "mutable/unaligned/negative/oversized/detoured flags accessors reject");
    }
    struct Console {
        std::optional<p::ConsoleValue> value{p::ConsoleValue{0x10000, 0x08000000, 1}};
        int writes{};
        bool reject{}, lose_readback{};
        auto read() { return value; }
        void write(const p::ConsoleValue& before, int desired) {
            ++writes;
            expect(value && before.object == value->object && before.flags == value->flags,
                "Panini write preserves object identity and console priority");
            if (!reject) { value->value = desired; }
            if (lose_readback) { value.reset(); }
        }
    } console;
    p::ConsoleLease lease;
    const auto read = [&] { return console.read(); };
    const auto write = [&](const auto& before, int desired) { console.write(before, desired); };
    expect(lease.apply(read, write) == p::Status::active && console.value->value == 0 && console.writes == 1,
        "Panini disables the game CVar once after saving the original");
    expect(lease.apply(read, write) == p::Status::active && console.writes == 1,
        "healthy Panini polling does not issue additional writes");
    expect(lease.restore(read, write) == p::Status::off && console.value->value == 1 && !lease.pending(),
        "Panini disable restores the original enabled value");
    console.value->value = 0;
    const auto writes = console.writes;
    expect(lease.apply(read, write) == p::Status::already_off && !lease.pending() &&
        lease.restore(read, write) == p::Status::off && console.writes == writes,
        "an already disabled game CVar is not owned or restored to an invented value");
    for (auto invalid : {p::ConsoleValue{0,0,1}, p::ConsoleValue{0x10000,4,1},
            p::ConsoleValue{0x10000,8,1}, p::ConsoleValue{0x10000,0,2}}) {
        console.value = invalid;
        expect(lease.apply(read, write) == p::Status::unavailable && console.writes == writes,
            "null/read-only/unregistered/non-boolean console entries remain unchanged");
    }
    console.value = p::ConsoleValue{0x10000,0,1}; console.reject = true;
    expect(lease.apply(read, write) == p::Status::unavailable && !lease.pending() && console.value->value == 1,
        "a rejected Set is not reported as active");
    console.reject = false;
    expect(lease.apply(read, write) == p::Status::active, "Panini can be retried after a rejected write");
    const auto saved = console.value;
    console.value.reset();
    expect(lease.restore(read, write) == p::Status::restore_pending && lease.pending(),
        "temporarily unavailable restoration retains the original lease");
    console.value = saved;
    expect(lease.restore(read, write) == p::Status::off && console.value->value == 1,
        "restoration completes after console availability returns");
    for (int change = 0; change < 3; ++change) {
        console.value = p::ConsoleValue{0x10000,0x08000000,1};
        expect(lease.apply(read, write) == p::Status::active, "external-change fixture owns a disable");
        if (change == 0) { console.value->object += 8; }
        if (change == 1) { console.value->flags = 0x09000000; }
        if (change == 2) { console.value->value = 1; }
        const auto count = console.writes;
        expect(lease.restore(read, write) == p::Status::external_change && !lease.pending() && console.writes == count,
            "external identity, priority or value changes cannot be overwritten by restoration");
    }
    console.value = p::ConsoleValue{0x10000,0,1}; console.lose_readback = true;
    expect(lease.apply(read, write) == p::Status::restore_pending && lease.pending(),
        "lost readback after a write must not discard restoration ownership");
    console.lose_readback = false; console.value = p::ConsoleValue{0x10000,0,0};
    expect(lease.restore(read, write) == p::Status::off, "lost-readback lease remains restorable");

    for (uint32_t starting_flags : {0u, 0x0e000000u}) {
        Console reset_console;
        reset_console.value = p::ConsoleValue{0x10000, starting_flags, 1};
        p::ConsoleLease reset_lease;
        const auto read_reset = [&] { return reset_console.read(); };
        const auto write_reset = [&](const auto& before, int desired) { reset_console.write(before, desired); };
        expect(reset_lease.apply(read_reset, write_reset, true) == p::Status::active,
            "game-reset fixture initially owns a disabled CVar");
        reset_console.value->flags = 0x0e000000;
        reset_console.value->value = 1;
        expect(reset_lease.apply(read_reset, write_reset, true) == p::Status::active &&
            reset_lease.reset_attempted() && reset_console.writes == 2 && reset_console.value->value == 0,
            "the live-traced same-object game-code reset re-arms without raising its current priority");
        expect(reset_lease.restore(read_reset, write_reset) == p::Status::off &&
            reset_console.value->value == 1 && reset_console.value->flags == 0x0e000000,
            "disable restores the latest game value, not the pre-level priority baseline");
    }
    for (const auto external : {p::ConsoleValue{0x10008,0x0e000000,1},
            p::ConsoleValue{0x10000,0x0f000000,1}, p::ConsoleValue{0x10000,0x09000000,1},
            p::ConsoleValue{0x10000,0x0e000020,1}, p::ConsoleValue{0x10000,0x0e000004,1},
            p::ConsoleValue{0x10000,0x0e000008,1}, p::ConsoleValue{0x10000,0x0e000000,2}}) {
        Console guarded;
        guarded.value = p::ConsoleValue{0x10000,0x0e000000,1};
        p::ConsoleLease guard;
        const auto r = [&] { return guarded.read(); };
        const auto w = [&](const auto& before, int desired) { guarded.write(before, desired); };
        expect(guard.apply(r,w,true) == p::Status::active, "protected-change fixture owns its initial disable");
        guarded.value = external;
        const auto result = guard.apply(r,w,true);
        expect((result == p::Status::external_change || result == p::Status::unavailable) &&
            !guard.reset_attempted() && guarded.writes == 1 && guarded.value->value == external.value,
            "game-reset recovery cannot override replacement, console priority, other flags or invalid values");
    }
    for (uint32_t starting_priority : {0x02000000u,0x08000000u,0x0f000000u}) {
        Console guarded;
        guarded.value = p::ConsoleValue{0x10000,starting_priority,1};
        p::ConsoleLease guard;
        const auto r = [&] { return guarded.read(); };
        const auto w = [&](const auto& before, int desired) { guarded.write(before, desired); };
        expect(guard.apply(r,w,true) == p::Status::active, "other-priority fixture owns the initial disable");
        guarded.value = p::ConsoleValue{0x10000,0x0e000000,1};
        expect(guard.apply(r,w,true) == p::Status::external_change && guarded.writes == 1,
            "code-reset recovery cannot inherit another owner's scalability, plugin or console priority");
    }
    {
        Console repeated;
        repeated.value = p::ConsoleValue{0x10000,0x0e000000,1};
        p::ConsoleLease owned;
        p::GameResetBudget budget;
        const auto r = [&] { return repeated.read(); };
        const auto w = [&](const auto& before, int desired) { repeated.write(before, desired); };
        expect(owned.apply(r,w,budget.allowed()) == p::Status::active, "bounded-reset fixture owns the CVar");
        for (uint32_t attempt = 0; attempt < p::GameResetBudget::limit + 1; ++attempt) {
            repeated.value->value = 1;
            const auto result = owned.apply(r,w,budget.allowed());
            budget.observe(owned.reset_attempted(),result);
            expect(attempt < p::GameResetBudget::limit ? result == p::Status::active : result == p::Status::external_change,
                "only the first three repeated code resets may be automatically re-armed");
        }
        expect(!budget.allowed() && repeated.writes == 4 && repeated.value->value == 1 && !owned.pending(),
            "a repeatedly reset game CVar stops instead of issuing unbounded writes");
        for (uint32_t poll = 1; poll < p::GameResetBudget::stable_polls; ++poll) {
            budget.observe(false,p::Status::active);
            expect(!budget.allowed(), "a short successful interval does not replenish game-reset retries");
        }
        budget.observe(false,p::Status::unavailable);
        for (uint32_t poll = 1; poll < p::GameResetBudget::stable_polls; ++poll) {
            budget.observe(false,p::Status::active);
            expect(!budget.allowed(), "a failed health poll restarts the full stable interval");
        }
        budget.observe(false,p::Status::active);
        expect(budget.allowed() && budget.attempts() == 0,
            "five consecutive healthy polls replenish recovery for later level transitions");
    }
    {
        Console pending;
        pending.value = p::ConsoleValue{0x10000,0x0e000000,1};
        p::ConsoleLease owned;
        const auto r = [&] { return pending.read(); };
        const auto w = [&](const auto& before, int desired) { pending.write(before, desired); };
        expect(owned.apply(r,w,true) == p::Status::active, "re-arm readback fixture initially disables the CVar");
        pending.value->value = 1;
        pending.lose_readback = true;
        expect(owned.apply(r,w,true) == p::Status::restore_pending && owned.reset_attempted() && owned.pending(),
            "a re-arm with lost readback retains the latest restoration baseline");
        pending.lose_readback = false;
        pending.value = p::ConsoleValue{0x10000,0x0e000000,0};
        expect(owned.restore(r,w) == p::Status::off && pending.value->value == 1,
            "a later disable can restore a re-arm whose initial readback was unavailable");
        expect(owned.apply(r,w,true) == p::Status::active, "strict-mode fixture owns the CVar again");
        pending.value->value = 1;
        const auto writes_before = pending.writes;
        expect(owned.apply(r,w) == p::Status::external_change && !owned.reset_attempted() && pending.writes == writes_before,
            "game-reset recovery remains explicit; default lease semantics never re-acquire an external write");
        expect(owned.apply(r,w,true) == p::Status::active, "disable-path fixture initially owns the CVar");
        pending.value->value = 1;
        const auto writes_before_disable = pending.writes;
        expect(owned.restore(r,w) == p::Status::external_change && pending.writes == writes_before_disable,
            "turning the option off must never re-acquire even an otherwise eligible game-code reset");
        expect(owned.apply(r,w,true) == p::Status::active, "rejected re-arm fixture initially owns the CVar");
        pending.value->value = 1;
        pending.reject = true;
        expect(owned.apply(r,w,true) == p::Status::unavailable && owned.reset_attempted() && !owned.pending(),
            "a rejected game-reset write loses ownership and signals the runtime to stop automatic recovery");
    }
    {
        Console stable;
        stable.value = p::ConsoleValue{0x10000,0x0e000000,1};
        p::ConsoleLease owned;
        p::GameResetBudget budget;
        const auto r = [&] { return stable.read(); };
        const auto w = [&](const auto& before, int desired) { stable.write(before, desired); };
        expect(owned.apply(r,w,true) == p::Status::active, "stable-level fixture initially disables Panini");
        for (uint32_t attempt = 0; attempt < p::GameResetBudget::limit; ++attempt) {
            stable.value->value = 1;
            const auto result = owned.apply(r,w,budget.allowed());
            budget.observe(owned.reset_attempted(),result);
        }
        expect(!budget.allowed(), "three recent game-code resets consume the current allowance");
        const auto write_count = stable.writes;
        for (uint32_t poll = 0; poll < p::GameResetBudget::stable_polls; ++poll) {
            const auto result = owned.apply(r,w,budget.allowed());
            expect(result == p::Status::active && !owned.reset_attempted(),
                "ownership remains active without writes while the game stops resetting Panini");
            budget.observe(owned.reset_attempted(),result);
        }
        expect(budget.allowed() && stable.writes == write_count,
            "ten seconds of real owned health polls restores allowance without further CVar writes");
        stable.value->value = 1;
        const auto result = owned.apply(r,w,budget.allowed());
        budget.observe(owned.reset_attempted(),result);
        expect(result == p::Status::active && budget.attempts() == 1,
            "a later level can recover after the preceding level established stable ownership");
    }

    uint8_t flag{};
    { p::TickSuppression guard{flag, false}; expect(flag == 0, "inactive fallback leaves component unchanged"); }
    {
        p::TickSuppression guard{flag, true};
        expect(flag == 1, "component Tick sees the game's temporary-camera suppression");
        { p::TickSuppression nested{flag, true}; expect(flag == 1, "nested suppression does not steal the flag"); }
        expect(flag == 1, "inner return preserves the outer suppression");
    }
    expect(flag == 0, "component flag is immediately restored after the original Tick");
    try { p::TickSuppression guard{flag, true}; throw 1; } catch (...) {}
    expect(flag == 0, "component flag is restored on C++ unwinding");
    flag = 1;
    { p::TickSuppression guard{flag, true}; }
    expect(flag == 1, "pre-existing temporary-camera suppression remains owned by the game");
    flag = 2;
    { p::TickSuppression guard{flag, true}; }
    expect(flag == 2, "invalid private bool representation is not modified");
    flag = 0;
    { p::TickSuppression guard{flag, true}; flag = 0; }
    expect(flag == 0, "an original-call flag change is not undone");
    expect(p::exclusive_component(false,true) && !p::exclusive_component(true,true) &&
        !p::exclusive_component(false,false) && !p::exclusive_component(true,false),
        "component fallback never activates implicitly or alongside the CVar path");
    expect(p::suppression_allowed(true,7,7,0x10000) && !p::suppression_allowed(false,7,7,0x10000) &&
        !p::suppression_allowed(true,0,0,0x10000) && !p::suppression_allowed(true,7,8,0x10000) &&
        !p::suppression_allowed(true,7,7,0) && !p::suppression_allowed(true,7,7,0x10001) &&
        !p::suppression_allowed(true,7,7,0xffff000000010000ull),
        "only an armed game-thread Tick with a canonical aligned component may change its flag");
    std::array<uint8_t,0> empty{};
    expect(p::fingerprint(empty) == 14695981039346656037ull, "fingerprint starts from its specified offset basis");
    const auto unavailable = [](uint32_t, std::span<uint8_t>) { return false; };
    const auto wrong_size = [](uint32_t) -> size_t { return 1; };
    expect(!p::component_contract(unavailable, wrong_size), "missing or wrong-sized native functions fail closed");

    {
        struct Function { uint32_t rva; size_t unwind; std::vector<uint8_t> code; bool readable{true}; };
        std::vector<Function> functions;
        const auto install = [&](const auto& fixture, bool clear = true) {
            if (clear) { functions.clear(); }
            for (const auto& fn : fixture) {
                functions.push_back({fn.rva, fn.code.size(), {fn.code.begin(), fn.code.end()}});
            }
        };
        const auto read = [&](uint32_t rva, std::span<uint8_t> bytes) {
            for (const auto& fn : functions) {
                if (fn.rva == rva && fn.readable && fn.code.size() == bytes.size()) {
                    std::copy(fn.code.begin(), fn.code.end(), bytes.begin()); return true;
                }
            }
            return false;
        };
        const auto size = [&](uint32_t rva) -> size_t {
            for (const auto& fn : functions) { if (fn.rva == rva) { return fn.unwind; } }
            return 0;
        };
        for (const auto* fixture : {&swgr_panini_fixture::old, &swgr_panini_fixture::new_}) {
            install(*fixture);
            expect(p::component_tick_rva(read, size) == fixture->front().rva && p::component_contract(read, size),
                "each complete observed build selects its own validated Tick, never the other build's RVA");
            for (auto& fn : functions) {
                for (auto& byte : fn.code) {
                    byte ^= 1;
                    expect(!p::component_tick_rva(read, size), "every original or updated code byte rejects if changed");
                    byte ^= 1;
                }
                ++fn.unwind;
                expect(!p::component_tick_rva(read, size), "each companion requires its exact unwind boundary");
                --fn.unwind;
                fn.readable = false;
                expect(!p::component_tick_rva(read, size), "each companion must be readable");
                fn.readable = true;
                ++fn.rva;
                expect(!p::component_tick_rva(read, size), "a matching body at an unverified entry is rejected");
                --fn.rva;
                const auto last = fn.code.back(); fn.code.pop_back();
                expect(!p::component_tick_rva(read, size), "truncated companion bodies are rejected");
                fn.code.push_back(last);
            }
            const auto& other = fixture == &swgr_panini_fixture::old ? swgr_panini_fixture::new_ : swgr_panini_fixture::old;
            for (size_t i = 0; i < functions.size(); ++i) {
                const auto original = functions[i].code;
                functions[i].code.assign(other[i].code.begin(), other[i].code.end());
                expect(!p::component_tick_rva(read, size), "another build's bytes cannot satisfy an old RVA contract");
                functions[i].code = original;
            }
        }
        for (unsigned subset = 1; subset < 15; ++subset) {
            functions.clear();
            for (size_t i = 0; i < 4; ++i) {
                const auto& fn = (subset & (1u << i)) ? swgr_panini_fixture::new_[i] : swgr_panini_fixture::old[i];
                functions.push_back({fn.rva, fn.code.size(), {fn.code.begin(), fn.code.end()}});
            }
            expect(!p::component_tick_rva(read, size), "intact helpers from mixed builds never authorize a Tick");
        }
        install(swgr_panini_fixture::old); install(swgr_panini_fixture::new_, false);
        expect(!p::component_tick_rva(read, size), "two complete matching variants are ambiguous, not an arbitrary hook choice");
    }
}

// Optional local evidence check. Never maps executable code or calls the game.
void test_swgr_panini_image(const char* path) {
    namespace p = uevr::swgr_panini;
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    const auto file_size = file ? static_cast<uint64_t>(file.tellg()) : 0;
    std::array<uint8_t,65536> header{};
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
        expect(false, "read SWGR Panini PE header"); return;
    }
    const auto u16 = [&](size_t o) { uint16_t v{}; if(o+2<=header.size())std::memcpy(&v,header.data()+o,2); return v; };
    const auto u32 = [&](size_t o) { uint32_t v{}; if(o+4<=header.size())std::memcpy(&v,header.data()+o,4); return v; };
    const auto nt = u32(0x3c), optional = nt + 24;
    const auto count = u16(nt+6), opt_size = u16(nt+20);
    if (nt > 4096 || u32(nt) != 0x4550 || u16(optional) != 0x20b || !count || count > 96 ||
        uint64_t(optional)+opt_size+uint64_t(count)*40 > header.size()) {
        expect(false, "validate SWGR Panini PE sections"); return;
    }
    const auto raw = [&](uint32_t rva, size_t length) -> std::optional<uint64_t> {
        for (uint32_t i=0;i<count;++i) {
            const size_t section = optional + opt_size + i*40;
            const auto va=u32(section+12), size=u32(section+16), start=u32(section+20);
            if (rva>=va && rva-va<=size && length<=size-(rva-va)) {
                const uint64_t offset=uint64_t(start)+rva-va;
                if (offset<=file_size && length<=file_size-offset) { return offset; }
            }
        }
        return {};
    };
    const auto read_file = [&](uint32_t rva, void* out, size_t size) {
        const auto offset = raw(rva,size);
        if (!offset) { return false; }
        file.clear(); file.seekg(*offset);
        return static_cast<bool>(file.read(static_cast<char*>(out),size));
    };
    const auto directory=u32(optional+112+24), directory_size=u32(optional+112+28);
    if (!directory_size || directory_size>8*1024*1024 || directory_size%12) {
        expect(false, "validate SWGR Panini unwind directory"); return;
    }
    std::vector<std::array<uint32_t,3>> entries(directory_size/12);
    if (!read_file(directory,entries.data(),directory_size)) { expect(false,"read SWGR unwind table"); return; }
    struct Function { uint32_t rva{}; size_t unwind{}; std::vector<uint8_t> code; };
    std::vector<Function> functions;
    for (const auto& variant : p::component_variants) for (const auto& witness : variant) {
        const auto found=std::lower_bound(entries.begin(),entries.end(),witness.rva,
            [](const auto& entry,uint32_t rva){return entry[0]<rva;});
        Function fn{witness.rva,found!=entries.end()&&(*found)[0]==witness.rva&&(*found)[1]>witness.rva
            ? (*found)[1]-witness.rva:0,std::vector<uint8_t>(witness.size)};
        if (!read_file(witness.rva,fn.code.data(),fn.code.size())) { expect(false,"read original Panini code"); return; }
        functions.push_back(std::move(fn));
    }
    const auto read = [&](uint32_t rva,std::span<uint8_t> bytes) {
        for (const auto& fn:functions) if(fn.rva==rva&&fn.code.size()==bytes.size()) {
            std::copy(fn.code.begin(),fn.code.end(),bytes.begin());return true;
        }
        return false;
    };
    const auto size = [&](uint32_t rva)->size_t { for(const auto& fn:functions)if(fn.rva==rva)return fn.unwind;return 0; };
    const auto tick = p::component_tick_rva(read,size);
    expect(tick.has_value(), "actual SWGR Tick, D/S writers and tagged setter prove the selected build");
    if (!tick) { return; }
    for (const auto& variant : p::component_variants) {
        if (variant.front().rva != *tick) { continue; }
        for (auto& fn : functions) {
            if (std::none_of(variant.begin(), variant.end(), [&](const auto& witness) { return witness.rva == fn.rva; })) { continue; }
            for (auto& byte : fn.code) {
                byte ^= 1;
                expect(!p::component_contract(read,size), "every changed native byte rejects private component suppression");
                byte ^= 1;
            }
        }
    }
}
