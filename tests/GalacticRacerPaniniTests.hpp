#pragma once

#include <fstream>
#include "mods/vr/GalacticRacerPaniniPolicy.hpp"

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
    for (const auto& witness : p::component_witnesses) {
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
    expect(p::component_contract(read,size), "actual updated SWGR Tick, D/S writers and tagged setter prove the fallback");
    for (auto& fn:functions) for (auto& byte:fn.code) {
        byte^=1;
        expect(!p::component_contract(read,size), "every changed native byte rejects private component suppression");
        byte^=1;
    }
}
