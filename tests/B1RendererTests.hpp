#pragma once

#include <limits>
#include <vector>

#include "mods/vr/B1RendererEntry.hpp"
#include "B1RendererFixture.hpp"

namespace b1_renderer_tests {

template <typename Expect>
void run(const Expect& expect) {
    namespace b = uevr::b1;
    namespace f = b1_renderer_fixture;
    for (const auto path : {L"b1-Win64-Shipping.exe", L"D:\\Games\\B1-WIN64-SHIPPING.EXE",
                            L"D:/Games/b1-Win64-Shipping.exe"}) {
        for (const uint32_t ms : {0u, 0x0004001Bu, 0x00050000u, 0x00050001u, 0x00050008u}) {
            for (const uint32_t ls : {0u, 1u, 0x00010000u}) {
                for (const bool dx12 : {false, true}) {
                    expect(b::is_supported_runtime(path, ms, ls, dx12) == (ms == 0x00050000 && ls == 0 && dx12),
                        "B1 adapter is limited to the validated executable, UE5.0.0 and DX12");
                }
            }
        }
    }
    for (const auto path : {L"", L"Other.exe", L"b1-Win64-Shipping.exe.bak", L"not-b1-Win64-Shipping.exe",
                            L"b1-WinGDK-Shipping.exe", L"D:\\b1-Win64-Shipping.exe\\Other.exe",
                            L"prospi-Win64-Shipping.exe", L"FinchGame.exe", L"PBZ.exe"}) {
        expect(!b::is_supported_runtime(path, 0x00050000, 0, true), "B1 does not alter other games");
    }
    sdk::detail::FamilyLayout layout{};
    expect(!b::matches_family_layout(layout), "B1 cannot authorize an undiscovered family layout");
    layout.has_vtable = true;
    layout.views = 8;
    layout.render_target = 0x20;
    layout.scene_interface = 0x28;
    layout.frame_count = 0x7C;
    expect(!b::matches_family_layout(layout), "B1 requires a discovered family vtable");
    layout.vtable = 0;
    expect(!b::matches_family_layout(layout), "B1 rejects a null family vtable");
    layout.vtable = 0x150001000;
    const auto original = layout;
    expect(b::matches_family_layout(layout) && layout == original, "B1 only corroborates SDK offsets");
    for (const auto member : {&sdk::detail::FamilyLayout::views, &sdk::detail::FamilyLayout::render_target,
                              &sdk::detail::FamilyLayout::scene_interface, &sdk::detail::FamilyLayout::frame_count}) {
        auto bad = layout;
        bad.*member = 0x1234;
        expect(!b::matches_family_layout(bad), "B1 rejects mismatching family members");
    }
    layout.has_vtable = false;
    expect(!b::matches_family_layout(layout), "B1 requires the source-confirmed vtable layout");
    expect(!b::is_distinct_renderer_entry(0, 0x2000) && !b::is_distinct_renderer_entry(0x1000, 0) &&
           !b::is_distinct_renderer_entry(0x2000, 0x2000), "missing or identical Draw/renderer identity is rejected");
    expect(b::is_distinct_renderer_entry(0x1000, 0x2000), "a distinct callable renderer remains eligible");

    expect(b::has_callable_renderer_entry(f::renderer, f::unwind, f::callback_return),
        "captured B1 short root plus chained callback is accepted without lowering generic thresholds");
    expect(!b::has_callable_renderer_entry(f::wrong_viewport, f::unwind, f::callback_return),
        "the captured FViewport::Draw false positive cannot satisfy the renderer ABI");
    expect(!b::has_callable_renderer_entry(std::span{f::renderer}.subspan(0x22), f::unwind, f::callback_return - 0x22),
        "a CHAININFO child is not a callable entry");
    for (size_t i = 0; i < f::renderer.size(); ++i) {
        if (i == f::callback_return) { continue; }
        expect(!b::has_callable_renderer_entry(f::renderer, f::unwind, i),
            "B1 only accepts the observed slot-5 callback in the validated family loop");
    }
    for (const auto callback : {std::numeric_limits<size_t>::max(), f::renderer.size(), size_t{0}}) {
        expect(!b::has_callable_renderer_entry(f::renderer, f::unwind, callback), "B1 rejects out-of-range callback offsets");
    }
    for (size_t i = 0; i < 0x182; ++i) {
        expect(!b::has_callable_renderer_entry(std::span{f::renderer}.first(i), f::unwind, f::callback_return),
            "B1 rejects every truncated entry, scene access or callback loop");
    }
    for (size_t i = 0; i < f::unwind.size(); ++i) {
        expect(!b::has_callable_renderer_entry(f::renderer, std::span{f::unwind}.first(i), f::callback_return),
            "B1 rejects truncated root unwind information");
        auto bad = f::unwind;
        bad[i] ^= 1;
        expect(!b::has_callable_renderer_entry(f::renderer, bad, f::callback_return), "B1 validates every root unwind byte");
    }
    for (const auto [start, count] : {std::pair<size_t, size_t>{0, 16}, {0x16, 3}, {0x1F, 3}, {0x4B, 16}, {0x14C, 54}}) {
        for (size_t i = start; i < start + count; ++i) {
            auto bad = f::renderer;
            bad[i] ^= 1;
            expect(!b::has_callable_renderer_entry(bad, f::unwind, f::callback_return),
                "B1 rejects altered prologue, argument flow, scene, frame count and extension loop");
        }
    }
    auto relocated_globals = f::renderer;
    for (size_t i : {0x12, 0x13, 0x14, 0x15, 0x1B, 0x1C, 0x1D, 0x1E}) { relocated_globals[i] ^= 0x55; }
    expect(b::has_callable_renderer_entry(relocated_globals, f::unwind, f::callback_return),
        "B1 does not depend on absolute addresses or unrelated RIP-relative globals");
    std::vector<uint8_t> oversized(0x4001);
    std::copy(f::renderer.begin(), f::renderer.end(), oversized.begin());
    expect(!b::has_callable_renderer_entry(oversized, f::unwind, f::callback_return), "B1 bounds renderer inspection");
    expect(!b::matches_at(std::span{f::renderer}, std::numeric_limits<size_t>::max(), b::save_family),
        "pattern checks reject overflowing offsets");
}

} // namespace b1_renderer_tests
