#pragma once

#include "utility/ValidationThreshold.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>

namespace validation_threshold_tests {
template<class Expect> void run(const Expect& expect) {
    for (uint32_t mask{}; mask < (1u << 12); ++mask) {
        size_t calls{};
        const auto result = utility::validation::at_least<12, 6>([&](size_t slot) {
            expect(slot == calls && slot < 12, "vtable probes remain ordered and bounded");
            ++calls;
            return (mask & (1u << slot)) != 0;
        });
        expect(result == (std::popcount(mask) >= 6),
            "all 4096 vtable-entry outcomes retain the original six-of-twelve acceptance rule");
        expect(calls <= 12, "validation never exceeds the old probe budget");
    }
    size_t calls{};
    bool executable = true;
    const auto probe = [&](size_t) { ++calls; return executable; };
    expect(utility::validation::at_least<12, 6>(probe) && calls == 6,
        "a valid vtable stops after six freshly executable entries, rather than checking all twelve");
    executable = false;
    calls = 0;
    expect(!utility::validation::at_least<12, 6>(probe) && calls == 7,
        "a later invalid vtable cannot reuse an earlier acceptance or cached page authority");
    executable = true;
    calls = 0;
    expect(utility::validation::at_least<12, 6>(probe) && calls == 6,
        "a fresh invocation reevaluates permissions after a prior failure");

    using utility::validation::ExecutableRegion;
    size_t queries{};
    bool current_executable = true;
    const auto query = [&](uintptr_t address) -> std::optional<ExecutableRegion> {
        ++queries;
        if (address >= 0x1000 && address < 0x2000) { return ExecutableRegion{0x1000, 0x1000, current_executable}; }
        if (address >= 0x2000 && address < 0x3000) { return ExecutableRegion{0x2000, 0x1000, false}; }
        return std::nullopt;
    };
    {
        auto regions = utility::validation::executable_regions<12>(query);
        for (size_t i{}; i < 12; ++i) { expect(regions.check(0x1100 + i*8, 1), "same executable region covers all probed functions"); }
        expect(queries == 1, "a single validator queries a homogeneous executable region once");
        expect(!regions.check(0x2100, 1) && queries == 2, "a different protection region requires a fresh query");
        expect(!regions.check(0x1fff, 2), "region reuse cannot authorize a range crossing a protection boundary");
    }
    current_executable = false;
    {
        auto fresh = utility::validation::executable_regions<12>(query);
        expect(!fresh.check(0x1100, 1), "the next validator sees revoked executable permissions without a cross-call cache");
    }
    {
        auto invalid = utility::validation::executable_regions<1>([](uintptr_t) -> std::optional<ExecutableRegion> {
            return ExecutableRegion{(std::numeric_limits<uintptr_t>::max)()-1, 8, true};
        });
        expect(!invalid.check(0, 1) && !invalid.check(0x1000, 0) && !invalid.check(0x1000, SIZE_MAX) &&
            !invalid.check((std::numeric_limits<uintptr_t>::max)()-1, 1),
            "invalid pointers, zero sizes, overflow and malformed regions fail closed");
    }
}
}
