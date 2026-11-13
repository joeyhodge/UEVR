#pragma once
#include "utility/SupportHealth.hpp"
#include "utility/VirtualSlotValidation.hpp"

namespace support_health_tests {
template<class Expect> void run(const Expect& expect) {
    const auto check_code = [](const nlohmann::json& rows, const char* code) {
        for (const auto& row : rows) { if (row["code"] == code) { return true; } }
        return false;
    };
    for (uint32_t bits{}; bits < 256; ++bits) {
        const nlohmann::json original{{"features", {{"wait_only_requested", bool(bits & 1)},
            {"array_requested", bool(bits & 2)}, {"preacquire_requested", bool(bits & 4)},
            {"same_pass", bool(bits & 8)}, {"native_fix_requested", bool(bits & 16)},
            {"native_stereo", bool(bits & 32)}, {"hitch_diagnostics", bool(bits & 64)},
            {"pacing_recorder", bool(bits & 128)}}}};
        auto input = original;
        const auto rows = utility::support::health_findings(input);
        expect(input == original, "health findings cannot change settings or observations");
        expect(check_code(rows, "wait_options_conflict") == bool((bits & 1) && (bits & 6)),
            "health explains actual wait-option conflicts without changing priority");
        expect(check_code(rows, "array_same_pass") == bool((bits & 2) && (bits & 8)),
            "health preserves array Same Pass restriction");
        expect(check_code(rows, "native_fix_mode") == bool((bits & 16) && !(bits & 32)),
            "requested Native Fix is distinguished from eligible Native mode");
        expect(check_code(rows, "optional_diagnostics") == bool(bits & 192),
            "optional diagnostics are disclosed for fair performance comparisons");
    }
    expect(check_code(utility::support::health_findings({{"openxr", {{"sampled", false}}}}), "xr_snapshot_unavailable"),
        "contention is unavailable telemetry, not a rendering error");
    expect(!check_code(utility::support::health_findings({{"features", {{"array_requested", "true"}, {"same_pass", true}}}}), "array_same_pass"),
        "unknown/malformed option types do not become positive capabilities");

    for (unsigned mask{}; mask < 64; ++mask) {
        size_t object_reads{}, slot_reads{}, proofs{}, execs{};
        const auto read = [&](uintptr_t address, uintptr_t& value) {
            if (address == 0x1000) { ++object_reads; value = mask & 2 ? 0x3000 : 0; return bool(mask & 1); }
            ++slot_reads; value = mask & 16 ? 0x5000 : 0; return address == 0x3018 && bool(mask & 8);
        };
        const auto proof = [&](uintptr_t) { ++proofs; return bool(mask & 4); };
        const auto exec = [&](uintptr_t) { ++execs; return bool(mask & 32); };
        const auto result = utility::validation::virtual_slot(0x1000, 3, read, proof, exec);
        expect(result.has_value() == (mask == 63), "immediate slot validation retains every existing proof");
        expect(object_reads == 1 && slot_reads <= 1 && proofs <= 1 && execs <= 1,
            "object and selected slot are read once, with one complete table proof");
    }
    uintptr_t live_table = 0x3000, live_function = 0x5000;
    bool allowed = true;
    size_t reads{};
    const auto read = [&](uintptr_t address, uintptr_t& value) {
        ++reads;
        if (address == 0x1000) { value = live_table; return true; }
        value = live_function; return address == 0x3018;
    };
    const auto validate = [&](size_t slot = 3) {
        return utility::validation::virtual_slot(0x1000, slot, read,
            [&](uintptr_t table) { return allowed && table == 0x3000; },
            [&](uintptr_t function) { return function == 0x5000; });
    };
    expect(validate() == 0x5000, "valid immediate slot is available to its original caller");
    allowed = false;
    expect(!validate(), "later operation rechecks ownership/type proof");
    allowed = true; live_function = 0x6000;
    expect(!validate(), "rewritten live slot cannot borrow old executable proof");
    live_function = 0x5000; live_table = 0x4000;
    expect(!validate(), "rewritten live table is not a persistent cached identity");
    live_table = 0x3000; reads = 0;
    expect(!validate(SIZE_MAX) && reads == 1, "slot arithmetic overflow rejects before a derived address read");
    expect(!utility::validation::virtual_slot(0, 3, read, [](uintptr_t) { return true; }, [](uintptr_t) { return true; }),
        "null objects cannot authorize a virtual call");
}
}
