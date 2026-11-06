#pragma once
#include "mods/vr/ProSpiPlayerVisibility.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace prospi_player_cut_tests {
using namespace uevr::prospi::players;

template<class Expect> void test_handoff(const Expect& expect) {
    Cache cache;
    const Identity player{0x20000,0x140008000,0x30000,0};
    const auto seed = [&](uint32_t tick = 100) {
        cache.clear();
        expect(cache.consider(player,1,0,false,true,1000,7,tick) == Decision::restore,
            "HMD geometry, not the game's overridden visibility, proves the handoff source");
    };
    seed();
    expect(cache.consider(player,1,0,false,std::nullopt,1016,8,101) == Decision::restore &&
        cache.consider(player,1,0,false,std::nullopt,1033,8,102) == Decision::restore,
        "geometry-proven player does not cycle off/on while the new cut publishes its first two views");
    expect(cache.consider(player,1,0,false,std::nullopt,1040,8,103) == Decision::unchanged,
        "handoff cannot renew itself or last more than two engine ticks");
    seed();
    expect(cache.consider(player,1,0,false,std::nullopt,1100,8,102) == Decision::restore &&
        cache.consider(player,1,0,false,std::nullopt,1101,8,102) == Decision::unchanged,
        "low FPS or a stalled engine cannot extend the handoff beyond 100 ms");
    seed();
    expect(cache.consider(player,1,0,false,std::nullopt,1016,9,101) == Decision::unchanged &&
        cache.consider(player,1,0,false,std::nullopt,1016,6,101) == Decision::unchanged &&
        cache.consider(player,1,0,false,std::nullopt,999,8,101) == Decision::unchanged &&
        cache.consider(player,1,0,false,std::nullopt,1016,8,99) == Decision::unchanged,
        "multiple cuts, backwards generations, clocks and ticks do not authorize a handoff");
    seed(UINT32_MAX);
    expect(cache.consider(player,1,0,false,std::nullopt,1016,8,0) == Decision::restore &&
        cache.consider(player,1,0,false,std::nullopt,1033,8,1) == Decision::restore &&
        cache.consider(player,1,0,false,std::nullopt,1040,8,2) == Decision::unchanged,
        "two-tick bound remains correct across engine-tick counter wrap");
    seed();
    expect(cache.consider(player,1,0,false,false,1016,8,101) == Decision::unchanged &&
        cache.consider(player,1,0,false,std::nullopt,1033,8,102) == Decision::unchanged,
        "a fresh outside-HMD result immediately revokes the geometry handoff");
    seed();
    expect(cache.consider(player,0,0,false,std::nullopt,1016,8,101) == Decision::intentional_off &&
        cache.consider(player,1,0,false,std::nullopt,1033,8,102) == Decision::unchanged,
        "PLAYEROFF clears the handoff before a later re-enable");
    for (size_t field{}; field < 3; ++field) {
        seed();
        auto reused = player;
        if (field == 0) { ++reused.model; }
        else if (field == 1) { reused.transform += 8; }
        else { reused.vtable += 8; }
        expect(cache.consider(reused,1,0,false,std::nullopt,1016,8,101) == Decision::unchanged,
            "model, transform or vtable reuse cannot inherit an HMD handoff");
    }
    seed(); cache.clear();
    expect(cache.consider(player,1,0,false,std::nullopt,1016,8,101) == Decision::unchanged,
        "world/mode/owner retirement clears HMD proof as well as genuine visibility");
    seed();
    expect(cache.consider(player,1,0,false,std::nullopt,1016,8) == Decision::unchanged,
        "missing engine-tick proof cannot authorize the new handoff");
    cache.clear();
    cache.consider(player,1,0,true,std::nullopt,1000,7,100);
    expect(cache.consider(player,1,0,false,std::nullopt,1016,8,101) == Decision::unchanged,
        "a Heavy override alone never trains HMD geometry");
    cache.clear();
    cache.consider(player,1,1,false,std::nullopt,1000,7,100);
    expect(cache.consider(player,1,0,false,std::nullopt,1500,8,120) == Decision::restore &&
        cache.consider(player,1,0,false,std::nullopt,1501,8,120) == Decision::unchanged,
        "the separate genuine game-visible 500 ms cut history stays unchanged");
}

template<class Expect> void test_geometry(const Expect& expect) {
    nlohmann::json capture;
    try {
        const auto file = std::filesystem::path{__FILE__}.parent_path()/"fixtures/prospi-player-cut-geometry-20261106.json";
        std::ifstream input{file}; input >> capture;
    } catch (const std::exception&) { expect(false, "read-only cut geometry fixture loads"); return; }
    expect(capture.at("read_only") == true && capture.at("samples").size() == 19,
        "fixture retains actual player geometry before and after nine paused camera cuts");
    size_t checked{}, restored{}, held{}, disabled{}, missing{};
    for (const auto& sample : capture.at("samples")) {
        std::array<View,2> views{};
        for (size_t i{}; i < views.size(); ++i) {
            const auto& row = sample.at("views").at(i);
            views[i] = {row.at("origin").get<Vector>(), row.at("forward").get<Vector>(),
                row.at("right").get<Vector>(), row.at("up").get<Vector>(),
                row.at("horizontal").get<float>(), row.at("vertical").get<float>(),
                row.at("time_ms").get<int64_t>(), row.at("generation").get<uint64_t>()};
        }
        const auto now = sample.at("now_ms").get<int64_t>();
        const auto generation = sample.at("generation").get<uint64_t>();
        size_t index{};
        Cache cache, batch;
        std::vector<Identity> relevant_batch;
        for (const auto& row : sample.at("players")) {
            const auto model = row.at("model").get<uint64_t>();
            const Identity id{0x40000+index*0x108,0x140008000,0x500000+index*0x100,model};
            ++index;
            const auto enabled = row.at("enabled").get<uint8_t>();
            if (!enabled || model == UINT64_MAX) {
                expect(cache.consider(id,enabled,0,false,true,now,generation,100) != Decision::restore,
                    "cut replay never enables an intentional off state or the invalid model dummy");
                ++disabled; continue;
            }
            const auto sphere = player_bounds(row.at("bounds").get<std::array<float,6>>(), row.at("matrix").get<Matrix>());
            expect(sphere.has_value(), "cut replay native bounds remain valid");
            if (!sphere) { continue; }
            const auto actual = relevance(views,*sphere,now,generation);
            const auto expected = row.at("expected_relevance").is_null() ? std::optional<bool>{} :
                std::optional{row.at("expected_relevance").get<bool>()};
            expect(actual == expected, "all captured inside/outside/unavailable classifications are unchanged");
            const auto decision = cache.consider(id,enabled,0,false,actual,now,generation,100);
            batch.consider(id,enabled,0,false,actual,now,generation,100);
            ++checked;
            if (actual == true) {
                expect(decision == Decision::restore, "every captured HMD-relevant player is still restored");
                relevant_batch.push_back(id);
                ++restored;
                // Simulated publication gap, not a measured visibility bit or first-tick dropout.
                expect(cache.consider(id,enabled,0,false,std::nullopt,now+16,generation+1,101) == Decision::restore &&
                    cache.consider(id,enabled,0,false,std::nullopt,now+33,generation+1,102) == Decision::restore,
                    "captured relevant bodies survive the bounded first-view cut handoff");
                expect(cache.consider(id,enabled,0,false,std::nullopt,now+50,generation+1,103) == Decision::unchanged,
                    "cut replay cannot turn a synthetic handoff into permanent preservation");
                ++held;
            } else {
                expect(decision == Decision::unchanged, "current outside/unproven bodies receive no new blanket override");
                missing += !actual.has_value();
            }
        }
        for (const auto& id : relevant_batch) {
            expect(batch.consider(id,1,0,false,std::nullopt,now+16,generation+1,101) == Decision::restore,
                "the whole relevant field/bench set survives the cut, not only the last player visited");
        }
    }
    expect(checked > 600 && restored > 150 && held == restored && disabled > 100 && missing > 0,
        "replay exercises field/bench players, disabled states, multiple cuts and missing current geometry");
}

template<class Expect> void run(const Expect& expect) {
    test_handoff(expect);
    test_geometry(expect);
    for (const bool match : {false,true}) {
        for (const bool dolly : {false,true}) {
            expect(uses_headset_projection(match,dolly) == !(match && !dolly),
                "headset-optics reuse is limited to the exact unscaled get_projection_matrix branch");
        }
    }
    const Matrix p{1,0,0,0, 0,1,0,0, 0,0,0,1, 0,0,1,0};
    expect(!packet_current(1000,3,1016,4) && make_view({0,0,0},{0,0,0},p,p,1016,4).has_value(),
        "a new corrected pose may use unchanged physical optics, not an old cut's pose or matched-FOV packet");
}
}
