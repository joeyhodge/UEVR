#pragma once
#include "mods/vr/ProSpiPlayerVisibility.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace prospi_player_tests {
using namespace uevr::prospi::players;
inline Matrix identity_matrix() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline Matrix projection() { return {1,0,0,0, 0,1,0,0, 0,0,0,1, 0,0,1,0}; }
inline std::vector<uint8_t> hex(std::string_view value) {
    std::vector<uint8_t> bytes;
    for (size_t i{}; i < value.size(); i += 2) { bytes.push_back(static_cast<uint8_t>(std::stoul(std::string{value.substr(i,2)}, nullptr, 16))); }
    return bytes;
}
template<class Expect> void run(const Expect& expect) {
    const auto fixtures = std::filesystem::path{__FILE__}.parent_path()/"fixtures";
    nlohmann::json contracts, roster;
    try {
        std::ifstream cf{fixtures/"prospi-player-contracts-20261105.json"}; cf >> contracts;
        std::ifstream rf{fixtures/"prospi-player-roster-20261105.json"}; rf >> roster;
    } catch (const std::exception&) { expect(false, "ProSpi native code and read-only roster fixtures load"); return; }
    for (const auto& contract : contracts) {
        constexpr uintptr_t base = 0x7ff700000000;
        struct Chunk { uintptr_t address{}; std::vector<uint8_t> bytes; };
        std::vector<Chunk> chunks;
        for (const auto& w : contract.at("witnesses")) { chunks.push_back({base+w.at("rva").get<uint32_t>(), hex(w.at("bytes").get<std::string>())}); }
        size_t reads{};
        const auto read = [&](uintptr_t at, void* out, size_t size) {
            ++reads;
            for (const auto& c : chunks) {
                if (at >= c.address && at-c.address <= c.bytes.size() && size <= c.bytes.size()-(at-c.address)) {
                    std::memcpy(out, c.bytes.data()+(at-c.address), size); return true;
                }
            }
            return false;
        };
        const auto size = contract.at("image_size").get<uint32_t>(), timestamp = contract.at("timestamp").get<uint32_t>();
        auto layout = verified_layout(read, base, size, timestamp);
        expect(layout && layout->handoff == contract.at("handoff_rva") && layout->enabled == contract.at("enabled") &&
            layout->bounds == contract.at("bounds") && layout->transform == contract.at("transform") &&
            layout->r15 == (contract.at("register") == "r15b"), "each edition independently validates its real enabled/bounds/transform/register contract");
        expect(reads == chunks.size(), "native layout validation is bounded to the code witnesses, not an object scan");
        if (!layout) { continue; }
        expect(!verified_layout(read, base, size, timestamp+1), "unknown game update cannot borrow a native layout");
        expect(!verified_layout(read, base, size-1, timestamp), "wrong image size fails closed");
        expect(!verified_layout(read, UINTPTR_MAX-100, size, timestamp), "native code range overflow is rejected");
        for (auto& c : chunks) {
            for (auto& b : c.bytes) {
                b ^= 1;
                expect(!verified_layout(read, base, size, timestamp), "every mutated native contract witness fails closed");
                b ^= 1;
            }
            auto saved = c.bytes.back(); c.bytes.pop_back();
            expect(!verified_layout(read, base, size, timestamp), "truncated code witness fails closed");
            c.bytes.push_back(saved);
        }
        struct Context { uint64_t r12{0x1234567800}, r15{0xfedcba9800}, other{12345}; } ctx;
        restore_visibility(ctx, layout->r15);
        expect(ctx.r12 == (layout->r15 ? 0x1234567800ull : 0x1234567801ull) &&
            ctx.r15 == (layout->r15 ? 0xfedcba9801ull : 0xfedcba9800ull) && ctx.other == 12345,
            "only the proven low-byte visibility register changes; other bits/registers stay intact");

        std::vector<uint8_t> state_memory(0xa00);
        const uintptr_t state_address = 0x200000, transform = 0x300000, vtable = base+0x8000;
        uint64_t model = 0; // Zero is a real live roster handle, not a null object pointer.
        std::memcpy(state_memory.data(), &vtable, 8);
        std::memcpy(state_memory.data()+8, &model, 8);
        std::memcpy(state_memory.data()+layout->transform, &transform, 8);
        state_memory[layout->enabled] = 1;
        const auto state_read = [&](uintptr_t at, void* out, size_t bytes) {
            if (at < state_address || at-state_address > state_memory.size() || bytes > state_memory.size()-(at-state_address)) { return false; }
            std::memcpy(out, state_memory.data()+(at-state_address), bytes); return true;
        };
        const auto live = read_state(state_read, *layout, state_address);
        expect(live && live->identity.model == 0 && live->enabled == 1, "valid model handle zero is preserved");
        state_memory[layout->enabled] = 0;
        expect(read_state(state_read, *layout, state_address) != live, "PLAYEROFF between sample and final recheck is detected");
        state_memory[layout->enabled] = 2;
        expect(!read_state(state_read, *layout, state_address), "non-boolean enabled flag rejected");
        state_memory[layout->enabled] = 1;
        model = UINT64_MAX; std::memcpy(state_memory.data()+8, &model, 8);
        expect(!read_state(state_read, *layout, state_address), "live dummy model -1 is not a player to restore");
        model = 1; std::memcpy(state_memory.data()+8, &model, 8);
        const uintptr_t external = 0x12340000; std::memcpy(state_memory.data(), &external, 8);
        expect(!read_state(state_read, *layout, state_address), "foreign/reused native vtable rejected");
        expect(!read_state(state_read, *layout, state_address+1), "unaligned native player rejected");
    }

    Cache cache;
    Identity player{0x20000,0x140008000,0x30000,0};
    for (const bool heavy : {false,true}) {
        cache.clear();
        expect(cache.consider(player,1,0,heavy,true,100,1) == Decision::restore, "enabled player never seen by broadcast camera is preserved in HMD");
        expect(cache.consider(player,0,0,heavy,true,101,1) == Decision::intentional_off, "both modes respect intentional player off");
        expect(cache.consider(player,1,2,heavy,true,102,1) == Decision::rejected, "non-boolean original cannot be rewritten");
        expect(cache.consider(player,1,1,heavy,false,103,1) == Decision::natural, "original game-visible player is never suppressed");
    }
    cache.clear();
    expect(cache.consider(player,1,0,false,std::nullopt,100,1) == Decision::unchanged, "unknown HMD view cannot force an unproven body");
    cache.consider(player,1,0,false,true,101,1);
    expect(cache.consider(player,1,0,false,std::nullopt,102,1) == Decision::unchanged, "our own override cannot train visible history");
    cache.consider(player,1,1,false,false,200,1);
    expect(cache.consider(player,1,0,false,std::nullopt,300,2) == Decision::restore, "genuine previous-shot visibility covers the first view after a cut");
    expect(cache.consider(player,1,0,false,std::nullopt,701,2) == Decision::unchanged, "previous-shot handoff expires after 500 ms");
    auto reused = player; reused.model = 23;
    expect(cache.consider(reused,1,0,false,std::nullopt,301,1) == Decision::unchanged, "model-ID reuse cannot inherit genuine-visible history");
    cache.consider(player,1,1,false,false,400,1);
    reused = player; reused.transform += 8;
    expect(cache.consider(reused,1,0,false,std::nullopt,401,1) == Decision::unchanged, "transform allocation reuse invalidates history");
    cache.clear();
    expect(cache.consider(player,1,0,false,std::nullopt,500,1) == Decision::unchanged, "mode/world/instance retirement discards all history");
    expect(cache.consider(player,1,0,true,false,500,1) == Decision::restore, "Heavy remains the explicit all-enabled fallback");
    expect(cache.consider(player,1,0,false,false,500,1) == Decision::unchanged, "Balanced does not render players outside the corrected HMD view");
    cache.clear();
    auto collision = player; collision.state += 0x400; collision.model = 3;
    cache.consider(player,1,1,false,false,600,1);
    cache.consider(collision,1,1,false,false,600,1);
    cache.consider(player,0,0,false,false,601,1);
    expect(cache.consider(collision,1,0,false,std::nullopt,602,1) == Decision::restore,
        "an empty colliding slot cannot hide the existing player's genuine history");

    size_t enabled{}, restored{}, disabled{}, dummy{};
    for (size_t pass{}; pass < 120; ++pass) {
        cache.clear();
        for (size_t i{}; i < roster.at("players").size(); ++i) {
            const auto& row = roster.at("players")[i];
            const auto model = row.at("model").get<uint64_t>();
            Identity id{0x40000+i*0x100,0x140008000,0x500000+i*0x100,model};
            const auto decision = cache.consider(id,row.at("enabled").get<uint8_t>(),0,false,true,1000+pass,1);
            if (!pass) {
                dummy += model == UINT64_MAX;
                disabled += row.at("enabled") == 0;
                enabled += model != UINT64_MAX && row.at("enabled") == 1;
                restored += decision == Decision::restore;
                if (model != UINT64_MAX && row.at("enabled") == 1) {
                    expect(player_bounds(row.at("bounds").get<std::array<float,6>>(), row.at("matrix").get<Matrix>()).has_value(),
                        "all enabled real players in the live roster have validated native bounds/transforms");
                }
            }
        }
    }
    expect(roster.at("players").size() == 48 && enabled == 40 && dummy == 1 && disabled == 7 && restored == enabled,
        "captured field/bench roster restores all 40 relevant enabled real models, not four, without enabling seven off states or the dummy");
    for (size_t i{}; i < 512; ++i) {
        auto id = player; id.state += i*0x400; id.model = i; id.transform += i*0x100;
        expect(cache.consider(id,1,0,false,true,2000,1) == Decision::restore, "cache pressure never imposes a player count limit on current HMD relevance");
    }
    expect(cache.count(2000) <= Cache::capacity, "crowded shots keep fixed bounded storage");

    const auto p = projection();
    expect(packet_current(1000,3,1250,3) && !packet_current(0,3,1000,3) && !packet_current(1000,2,1000,3) &&
        !packet_current(1000,3,1251,3) && !packet_current(1001,3,1000,3),
        "projection and pose packets require the same current camera generation and a bounded monotonic age");
    const auto view = make_view({0,0,0},{0,0,0},p,p,1000,3);
    expect(view.has_value(), "actual Unreal positive-Z projection supported");
    if (view) {
        expect(intersects(*view,{{2000,0,0},100}), "forward player intersects HMD");
        expect(!intersects(*view,{{-2000,0,0},100}), "behind-HMD player not restored by Balanced");
        expect(!intersects(*view,{{2000,10000,0},100}), "distant lateral bench outside HMD excluded");
        expect(intersects(*view,{{2000,3000,0},400}), "large body and motion margin retained at view edge");
        std::array<View,2> views{*view,{}};
        expect(relevance(views,{{2000,0,0},100},1250,3) == true, "bounded one-frame/low-FPS view age accepted");
        expect(!relevance(views,{{2000,0,0},100},1251,3).has_value(), "stale HMD pose falls back to genuine history");
        expect(!relevance(views,{{2000,0,0},100},999,3).has_value(), "backwards clock rejected");
        expect(!relevance(views,{{2000,0,0},100},1000,4).has_value(), "new camera generation cannot use old HMD geometry");
        views[1] = *make_view({0,0,0},{0,90,0},p,p,1000,3);
        expect(relevance(views,{{0,2000,0},100},1000,3) == true, "either corrected eye can authorize a player, including canted orientations");
    }
    const auto pitched = make_view({0,0,0},{90,0,0},p,p,1000,3);
    expect(pitched && intersects(*pitched,{{0,0,2000},100}), "UE pitch sign looks up, not along the old broadcast view");
    auto asym = p; asym[8] = 0.6f; asym[9] = -0.4f;
    expect(make_view({0,0,0},{0,0,0},p,asym,1000,3)->horizontal > view->horizontal,
        "projection union covers asymmetric outer eye FOV");
    auto bad = p; bad[0] = 0;
    expect(!make_view({0,0,0},{0,0,0},p,bad,1000,3), "uninitialized projection does not authorize player restoration");
    bad = p; bad[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!projection_extent(bad), "non-finite projection rejected");
    auto matrix = identity_matrix(); matrix[12] = 10; matrix[13] = 20; matrix[14] = 30;
    const std::array<float,6> bounds{-20,-1,-20,20,25,20};
    const auto sphere = player_bounds(bounds,matrix);
    expect(sphere && sphere->center == Vector{100,300,320}, "native player center converts X/Z/Y * 10 including local vertical center");
    matrix[15] = 0;
    expect(!player_bounds(bounds,matrix), "non-affine transform rejected");
    matrix = identity_matrix(); matrix[0] = std::numeric_limits<float>::infinity();
    expect(!player_bounds(bounds,matrix), "non-finite transform rejected");
    auto inverted = bounds; inverted[0] = 30;
    expect(!player_bounds(inverted,identity_matrix()), "inverted local AABB rejected");
}
}
