#pragma once

#include "mods/vr/GalacticRacerBink.hpp"

void test_swgr_bink() {
    namespace b = uevr::swgr_bink;
    swgr_tests::Memory m;
    const auto base = m.base, overlay = base + 0x200, writer = base + 0x1000;
    const auto scheduled = base + 0x1100, draw = base + 0x1200, frame = base + 0x5000;
    m.put(overlay, b::overlay_code); m.put(writer, b::writer_code);
    m.put(scheduled, b::scheduled_code); m.put(draw, b::draw_code);
    const auto rel32 = [&](uintptr_t displacement, uintptr_t after, uintptr_t target) {
        m.put(displacement, static_cast<int32_t>(target - after));
    };
    rel32(overlay + 0x100, overlay + 0x104, writer);
    rel32(overlay + 0x105, overlay + 0x109, scheduled);
    rel32(overlay + 0x111, overlay + 0x115, draw);
    rel32(writer + 6, writer + 10, frame);
    rel32(writer + 17, writer + 21, frame + 16);
    expect(b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()),
        "Bink viewport callback proves packet and three distinct consumers");
    const auto good = m;
    const auto mutate = [&](uintptr_t address) {
        m = good; m.bytes[address - base] ^= 1;
        expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()),
            "modified Bink ABI/control flow preserves original output");
    };
    for (size_t i = 0; i < b::overlay_code.size(); ++i) { if (b::overlay_mask[i]) { mutate(overlay + i); } }
    for (size_t i = 0; i < b::writer_code.size(); ++i) { if (b::writer_mask[i]) { mutate(writer + i); } }
    for (size_t i = 0; i < b::scheduled_code.size(); ++i) { if (b::scheduled_mask[i]) { mutate(scheduled + i); } }
    for (size_t i = 0; i < b::draw_code.size(); ++i) { if (b::draw_mask[i]) { mutate(draw + i); } }
    m = good; rel32(writer + 17, writer + 21, frame + 24);
    expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()), "split global packet must be contiguous");
    m = good; rel32(overlay + 0x100, overlay + 0x104, base + m.bytes.size());
    expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()), "out-of-image consumer rejected");
    m = good; m.readable = false;
    expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()), "unreadable Bink code rejected");
    m = good; m.executable = false;
    expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size(), base, m.bytes.size()), "nonexecutable callback rejected");
    m = good;
    expect(!b::overlay_contract(m.view(), overlay + 1, b::overlay_code.size() - 1, base, m.bytes.size()), "interior helper is not the viewport callback");
    expect(!b::overlay_contract(m.view(), overlay, b::overlay_code.size() + 1, base, m.bytes.size()), "changed unwind contract rejected");

    b::Target scene{0x200000, 0x210000, 0x220000, 7, {4944, 2416, 3, 24, 1, 1, 0, 1, 1}};
    b::Target ui{0x300000, 0x310000, scene.device, 2, {1920, 1080, 3, 87, 1, 1, 0, 1, 1}};
    const uintptr_t command = 0x400000;
    const b::Packet packet{command, scene.texture, 4, 1920, 1080, 1};
    auto redirected = b::redirected_packet(packet, command, scene, ui);
    expect(redirected && redirected->texture == ui.texture && redirected->hdr == 0 &&
        redirected->command_list == packet.command_list && redirected->width == packet.width &&
        redirected->height == packet.height && redirected->resource_state == packet.resource_state,
        "change only viewport destination/color representation; preserve command, extent and playback UVs");
    auto high_res = ui; high_res.desc.width = 3840; high_res.desc.height = 2160;
    auto high_packet = packet; high_packet.width = 3840; high_packet.height = 2160;
    expect(b::redirected_packet(high_packet, command, scene, high_res).has_value(), "movie extent is not hard-coded to 1080p");
    for (auto format : {87u, 90u}) {
        auto alternate = ui; alternate.desc.format = format;
        expect(b::redirected_packet(packet, command, scene, alternate).has_value(), "validated BGRA family supported");
    }
    for (auto field : {&b::Target::texture, &b::Target::resource, &b::Target::device}) {
        auto invalid = ui; invalid.*field = 0;
        expect(!b::redirected_packet(packet, command, scene, invalid), "incomplete UI target keeps game output");
    }
    for (auto field : {&b::Target::texture, &b::Target::resource}) {
        auto invalid = ui; invalid.*field = scene.*field;
        expect(!b::redirected_packet(packet, command, scene, invalid), "aliased UI/scene cannot redirect");
    }
    auto invalid = ui; invalid.device += 8;
    expect(!b::redirected_packet(packet, command, scene, invalid), "different device keeps original movie");
    invalid = ui; invalid.generation = 0;
    expect(!b::redirected_packet(packet, command, scene, invalid), "unpublished UI generation cannot redirect");
    auto invalid_scene = scene; invalid_scene.generation = 0;
    expect(!b::redirected_packet(packet, command, invalid_scene, ui), "unpublished scene cannot redirect");
    for (auto format : {24u, 28u, 10u, 40u}) {
        invalid = ui; invalid.desc.format = format;
        expect(!b::redirected_packet(packet, command, scene, invalid), "non-BGRA target not a movie UI capability");
    }
    for (const auto descriptor : {
            uevr::swgr::NativeDescription{1280, 720, 3, 87, 1, 1, 0, 1, 1},
            uevr::swgr::NativeDescription{1920, 1080, 3, 87, 1, 2, 0, 1, 1},
            uevr::swgr::NativeDescription{1920, 1080, 3, 87, 9, 1, 0, 1, 1},
            uevr::swgr::NativeDescription{1920, 1080, 3, 87, 1, 1, 0, 2, 1}}) {
        invalid = ui; invalid.desc = descriptor;
        expect(!b::redirected_packet(packet, command, scene, invalid), "resize/MSAA/SRV/array incompatibility retains movie fallback");
    }
    for (auto field : {&b::Packet::resource_state, &b::Packet::width, &b::Packet::height}) {
        auto changed = packet; changed.*field = 0;
        expect(!b::redirected_packet(changed, command, scene, ui), "incomplete packet is never altered");
    }
    auto changed = packet; changed.command_list += 8;
    expect(!b::redirected_packet(changed, command, scene, ui), "wrong command cannot borrow outer callback lifetime");
    changed = packet; changed.texture += 8;
    expect(!b::redirected_packet(changed, command, scene, ui), "texture-movie/unrelated target unchanged");
    changed = packet; changed.hdr = 2;
    expect(!b::redirected_packet(changed, command, scene, ui), "unknown color representation unchanged");

    int outer{}, inner{};
    int* current{};
    {
        b::Scope active{current, &outer};
        expect(current == &outer, "callback publishes only its own lifetime scope");
        {
            b::Scope isolate{current, static_cast<int*>(nullptr)};
            expect(current == nullptr, "recursive callback does not inherit outer resources");
            { b::Scope nested{current, &inner}; expect(current == &inner, "independent scope supported"); }
            expect(current == nullptr, "inner callback restores isolated state");
        }
        expect(current == &outer, "outer callback restored after nesting");
        try { b::Scope isolate{current, static_cast<int*>(nullptr)}; throw 1; } catch (int) {}
        expect(current == &outer, "exception unwinding restores outer scope");
    }
    expect(current == nullptr, "no persistent media or UI state after movie callback");
}
