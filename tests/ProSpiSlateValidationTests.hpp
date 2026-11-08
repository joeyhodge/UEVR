#pragma once

#include "mods/vr/ProSpiSlateValidation.hpp"

namespace prospi_slate_validation_tests {
template<class Expect> void run(const Expect& expect) {
    using namespace uevr::prospi::slate_validation;
    for (unsigned flags{}; flags < 16; ++flags) {
        for (uintptr_t table : {uintptr_t{0}, uintptr_t{0x1000}, uintptr_t{0x2000}}) {
            for (uintptr_t selected : {uintptr_t{0}, uintptr_t{0x1000}, uintptr_t{0x2000}}) {
                const Inputs in{bool(flags & 1), bool(flags & 2), bool(flags & 4), bool(flags & 8), table, selected};
                const auto deferred = may_defer_unused_proof(in);
                expect(deferred == (flags == 15 && table != 0 && table == selected),
                    "unused Slate proof defers only for the target mode with both destinations and a fresh matching table");
                expect(!needs_late_proof(deferred, true, 0x3000, 0x4000),
                    "an existing UI destination never needs to adopt the unused engine texture");
                expect(needs_late_proof(deferred, false, 0x3000, 0x4000) == deferred,
                    "losing the UI destination requires the full proof before engine-texture adoption");
            }
        }
    }
    expect(!needs_late_proof(true, false, 0, 0x4000) && !needs_late_proof(true, false, 0x4000, 0x4000),
        "null and scene-alias textures cannot be late UI fallback candidates");
    Inputs frame{true, true, true, true, 0x1000, 0x1000};
    expect(may_defer_unused_proof(frame), "steady draw needs no unused full type proof");
    frame.current_table = 0x2000;
    expect(!may_defer_unused_proof(frame), "a new RHI table takes the full original validation path");
    frame.current_table = 0x1000; frame.scene_ready = false;
    expect(!may_defer_unused_proof(frame), "scene retirement/reallocation cannot reuse a previous frame's eligibility");
    frame.scene_ready = true; frame.ui_ready = false;
    expect(!may_defer_unused_proof(frame), "UI retirement/reallocation restores full startup validation");
    frame.ui_ready = true; frame.stable_draw = false;
    expect(!may_defer_unused_proof(frame), "a reset stable-draw witness requires full validation again");
    frame.stable_draw = true; frame.targeted_native_fix = false;
    expect(!may_defer_unused_proof(frame), "Native Fix exit, other titles/engines/RHIs retain the old predicate");

    struct Output {
        uintptr_t scene{}, ui{}, selected{};
        bool stable{};
        bool operator==(const Output&) const = default;
    };
    // Model the actual early-adoption/provider/late-fallback order. Validate
    // routing equality, not merely the fast path's boolean decision.
    for (unsigned flags{}; flags < 256; ++flags) {
        for (const bool same_table : {false, true}) {
            const bool scope = flags & 1, stable = flags & 2, scene = flags & 4, ui = flags & 8;
            const bool valid = flags & 16, provider = flags & 32, retire_ui = flags & 64, alias_scene = flags & 128;
            const uintptr_t texture = alias_scene ? 0x1000 : 0x3000;
            const uintptr_t current = same_table ? 0x4000 : 0x5000;
            const auto route = [&](bool optimized) {
                Output out{scene ? uintptr_t{0x1000} : 0, ui ? uintptr_t{0x2000} : 0, 0x4000, stable};
                bool proven{};
                const auto full_proof = [&] {
                    proven = valid;
                    if (proven) { out.selected = current; out.stable = true; }
                };
                const bool deferred = optimized && may_defer_unused_proof({
                    scope, out.stable, out.scene != 0, out.ui != 0, current, out.selected});
                if (!deferred) { full_proof(); }
                if (proven && !out.scene) { out.scene = texture; }
                if (retire_ui) { out.ui = 0; }
                if (!out.ui && provider && texture != out.scene) { out.ui = 0x6000; }
                if (needs_late_proof(deferred, out.ui != 0, texture, out.scene)) { full_proof(); }
                if (!out.ui && proven && texture != out.scene) { out.ui = texture; }
                if (!out.ui && out.scene) { out.ui = out.scene; }
                return out;
            };
            expect(route(false) == route(true),
                "deferred proof preserves texture adoption, table selection and stable-draw state including late UI retirement");
        }
    }
}
}
