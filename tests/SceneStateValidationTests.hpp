#pragma once

#include "utility/SceneStateValidation.hpp"
#include <array>
#include <cstdint>

namespace scene_state_validation_tests {
template<class Expect> void run(const Expect& expect) {
    // Exhaust all stable readable/type combinations against the old predicate.
    for (unsigned mask{}; mask < 256; ++mask) {
        for (const bool common_table : {false, true}) {
            size_t reads{}, proofs{};
            const auto read = [&](uintptr_t address, uintptr_t& out) {
                ++reads;
                if (address == 0x1000) { out = 0x3000; return (mask & 1) != 0; }
                if (address == 0x2000) { out = common_table ? 0x3000 : 0x4000; return (mask & 2) != 0; }
                if (address == 0x3000) { out = 0x5000; return (mask & 4) != 0; }
                if (address == 0x4000) { out = 0x6000; return (mask & 8) != 0; }
                return false;
            };
            const auto probe = [&](uintptr_t table, uintptr_t first) {
                ++proofs;
                return table == 0x3000 ? (mask & 0x30) == 0x30 && first == 0x5000
                                      : (mask & 0xc0) == 0xc0 && first == 0x6000;
            };
            const auto original = [&](uintptr_t state) {
                uintptr_t table{}, first{};
                return state && read(state, table) && table && read(table, first) && first && probe(table, first);
            };
            const auto expected = original(0x1000) && original(0x2000);
            reads = proofs = 0;
            auto checks = utility::scene_state_validation::batch<2>(read, probe);
            const auto actual = checks.check(0x1000) && checks.check(0x2000);
            expect(actual == expected, "paired state checks retain the original complete predicate");
            expect(!actual || (reads == 4 && proofs == (common_table ? 1 : 2)),
                "both live states and slots are read; only a common successful type proof is shared");
        }
    }

    uintptr_t table = 0x3000, first = 0x5000;
    bool state_readable = true, slot_readable = true, type_ok = true;
    size_t reads{}, proofs{};
    const auto read = [&](uintptr_t address, uintptr_t& out) {
        ++reads;
        out = address == 0x1000 || address == 0x2000 ? table : first;
        return address == 0x1000 || address == 0x2000 ? state_readable : slot_readable;
    };
    const auto probe = [&](uintptr_t, uintptr_t) { ++proofs; return type_ok; };
    auto make = [&] { return utility::scene_state_validation::batch<2>(read, probe); };
    auto checks = make();
    expect(checks.check(0x1000) && checks.check(0x2000) && proofs == 1 && reads == 4,
        "a common eye-state type saves one proof, not the live pointer reads");
    state_readable = false;
    expect(!checks.check(0x1000), "an unreadable or freed state cannot borrow the other eye's type proof");
    state_readable = true; slot_readable = false;
    const auto before_failed_read = proofs;
    expect(!checks.check(0x1000) && proofs == before_failed_read,
        "a failed/guarded slot read fails closed without a second read consuming its guard");
    slot_readable = true; table = 0;
    expect(!checks.check(0x1000), "a current null table cannot borrow a past proof");
    table = 0x3000; first = 0;
    expect(!checks.check(0x1000), "a null slot remains invalid");
    first = 0x6000; type_ok = false;
    expect(!checks.check(0x1000), "a rewritten non-executable or unowned slot needs a new successful proof");
    first = 0x5000;
    expect(!checks.check(0x1000), "rewriting a rejected slot back cannot revive an earlier proof");
    first = 0x5000; table = 0x4000;
    expect(!checks.check(0x1000), "a different table with the same code cannot borrow module ownership");
    table = 0x3000;
    expect(!make().check(0x1000), "a later operation rechecks executable protection and module ownership");
    type_ok = true; proofs = 0;
    auto outer = make();
    expect(outer.check(0x1000) && make().check(0x2000) && proofs == 2,
        "nested operations do not share proof storage");
    expect(!make().check(0), "null state remains rejected");

    size_t capacity_proofs{};
    auto limited = utility::scene_state_validation::batch<1>(
        [](uintptr_t address, uintptr_t& out) { out = address < 0x3000 ? address + 0x2000 : 0x5000; return true; },
        [&](uintptr_t, uintptr_t) { ++capacity_proofs; return true; });
    expect(limited.check(0x1000) && limited.check(0x2000) && limited.check(0x2000) && capacity_proofs == 3,
        "capacity overflow performs complete validation instead of extending the proof's lifetime");
    expect(limited.check(0x1000) && capacity_proofs == 3, "capacity fallback leaves the existing common proof intact");
}
}
