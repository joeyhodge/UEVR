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
}
}
