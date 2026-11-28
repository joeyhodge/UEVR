#pragma once

#include <limits>
#include <memory>

#include "mods/vr/MibChaseHud.hpp"
#include "mods/vr/UE58UIInitialization.hpp"

namespace mib_chase_hud_tests {

template <typename Expect>
void run(Expect&& expect) {
    using namespace uevr::mib_chase_hud;
    expect(executable_matches(L"D:\\Games\\MIBChase\\MIBChase-Win64-Shipping.exe") &&
        executable_matches(L"MIBCHASE-WIN64-SHIPPING.EXE"), "MIB fixes the exact shipping image name");
    expect(!executable_matches(L"NotMIBChase-Win64-Shipping.exe") &&
        !executable_matches(L"MIBChase-Win64-Shipping.exe.bak") &&
        !executable_matches(L"D:/MIBChase-Win64-Shipping.exe/UnrealGame-Win64-Shipping.exe"),
        "MIB leaves unrelated images, even the same engine version, alone");

    constexpr Extent eye{2472, 2416};
    constexpr Rect view{0, 0, 2472, 2416};
    for (const auto window : {Extent{1920, 1080}, Extent{1600, 900}, Extent{2560, 1440}}) {
        const Rect area{0, 0, window.width, window.height};
        expect(can_redirect(window, eye, eye, area, {}, view, 0, false),
            "MIB accepts changed window sizes independent of the headset");
        expect(!can_redirect(window, eye, eye, area, {}, view, 1, false) &&
            !can_redirect(window, eye, eye, area, {}, view, 0, true),
            "MIB leaves already queued scene work and hit-test passes on their original target");
        expect(!can_redirect(window, eye, eye, area, {}, Rect{2472, 0, 4944, 2416}, 0, false) &&
            !can_redirect(window, eye, eye, area, Rect{0, 40, window.width, window.height}, view, 0, false) &&
            !can_redirect(window, window, eye, area, {}, view, 0, false) &&
            !can_redirect({}, eye, eye, area, {}, view, 0, false),
            "MIB rejects secondary-eye, offset, clipped, and minimized draw geometry");

        const Matrix projection{2.0 / window.width, 0, 0, 0, 0, -2.0 / window.height, 0, 0,
            0, 0, 1, 0, -1, 1, 0, 1};
        auto shifted = projection;
        shifted[12] += 0.25;
        auto malformed = projection;
        malformed[0] = std::numeric_limits<double>::quiet_NaN();
        expect(window_projection(projection, projection, window) &&
            !window_projection(projection, shifted, window) &&
            !window_projection(projection, projection, eye) &&
            !window_projection(malformed, malformed, window),
            "MIB requires the original centered window-space projection");
    }
    expect(tile_blend(2, true) == 11 && tile_blend(2, false) == 2 &&
        tile_blend(0, true) == 0 && tile_blend(3, true) == 3,
        "MIB writes sprite opacity only in the scoped HUD draw");

    constexpr uintptr_t image = 0x140000000;
    expect(render_target_rdg_slot == 1 &&
        owned_canvas_target(image, image + texture_resource_vtable_rva,
            image + render_target_vtable_rva, image + render_target_rdg_rva),
        "MIB uses the compiled owned target's actual RDG slot, not the declaration order");
    expect(!owned_canvas_target(image, image + texture_resource_vtable_rva,
            image + render_target_vtable_rva, image + 0x133eb20) &&
        !owned_canvas_target(image, image + texture_resource_vtable_rva,
            image + render_target_vtable_rva + 8, image + render_target_rdg_rva),
        "MIB rejects unrelated slots and adjacent vtables");

    using namespace uevr::ue58_ui;
    Initialization<int> state{};
    auto first = state.begin(1920, 1080, Source::Slate, 1);
    expect(state.acquire_creation(2) == first && state.created(first, std::make_shared<int>(1), 3),
        "MIB waits for an owned Slate target");
    expect(!state.try_retain_published(1920, 1080), "MIB cannot borrow an unpublished target");
    expect(state.acquire(Source::Slate, 4) == first && state.publish(first, 5, [] { return true; }),
        "MIB can borrow the validated target after publication");
    auto lease = state.try_retain_published(1920, 1080);
    expect(lease == first && !state.try_retain_published(1600, 900),
        "MIB requires the current exact-size UI allocation");
    state.cancel("resize", [] {});
    first.reset();
    expect(lease && lease->owner && *lease->owner == 1 && !state.try_retain_published(1920, 1080),
        "MIB can keep a borrowed owner alive without accepting it for subsequent draws");
}

} // namespace mib_chase_hud_tests
