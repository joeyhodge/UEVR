#pragma once

#include "mods/vr/GalacticRacerBinkSeek.hpp"

void test_swgr_bink_seek() {
    namespace b = uevr::swgr_bink;
    swgr_tests::Memory m;
    const auto base = m.base, info = base + 0x1400, go = base + 0x1600, initialize = base + 0x1800;
    const b::SeekFunctions f{base + 0x100, base + 0x300, base + 0xb00};
    m.put(f.seek, b::seek_code); m.put(f.tick, b::tick_code); m.put(f.process, b::process_code);
    m.put(info, b::info_code); m.put(go, b::goto_code);
    const auto rel32 = [&](uintptr_t ip, uintptr_t target) { m.put(ip + 1, static_cast<int32_t>(target - ip - 5)); };
    rel32(f.seek + 0x5d, info); rel32(f.seek + b::seek_boundary, go);
    for (size_t offset : {0xf3, 0x138, 0x17d, 0x1cb, 0x22a, 0x26b, 0x399}) { rel32(f.tick + offset, info); }
    rel32(info + 0x10, initialize); rel32(go + 0x17, initialize);
    const auto size = [&](uintptr_t p) -> size_t {
        return p == f.seek ? b::seek_code.size() : p == f.tick ? b::tick_code.size() :
            p == f.process ? b::process_entry_size : p == info ? b::info_code.size() : p == go ? b::goto_code.size() : 0;
    };
    const auto contract = [&] { return b::seek_contract(m.view(), f, base, m.bytes.size(), size); };
    expect(contract(), "linked Seek/Tick/Info/Goto/bounded decode contracts accepted");
    const auto good = m;
    const auto mutations = [&](uintptr_t address, const auto& mask) {
        for (size_t i = 0; i < mask.size(); ++i) {
            if (!mask[i]) { continue; }
            m = good; m.bytes[address - base + i] ^= 1;
            expect(!contract(), "changed Bink layout/branch/decoder preserves original playback");
        }
    };
    mutations(f.seek, b::seek_mask); mutations(f.tick, b::tick_mask); mutations(f.process, b::process_mask);
    mutations(info, b::info_mask); mutations(go, b::goto_mask);
    m = good; rel32(f.tick + 0x26b, go);
    expect(!contract(), "Tick cannot borrow an unrelated Info consumer");
    m = good; rel32(go + 0x17, initialize + 8);
    expect(!contract(), "Info and Goto must share SDK initialization");
    m = good; rel32(f.seek + 0x5d, base + m.bytes.size());
    expect(!contract(), "out-of-module SDK consumer rejected");
    m = good; m.readable = false; expect(!contract(), "unreadable code retains original playback");
    m = good; m.executable = false; expect(!contract(), "nonexecutable code retains original playback");
    m = good;
    expect(!b::seek_contract(m.view(), f, base, m.bytes.size(), [&](uintptr_t p) { return size(p) + 1; }),
        "changed unwind boundaries rejected");

    b::Player p{base + 0x4500, 1, 0, 0};
    b::MovieInfo i{};
    i.width = 3840; i.height = 1606; i.frames = 2948; i.frame = 70; i.rate = 60; i.rate_div = 1;
    const auto forward = [&](const auto& player, const auto& movie, uint64_t target = 185, uint64_t budget = UINT32_MAX) {
        return b::budget_forward_seek(player, movie, target, budget);
    };
    expect(forward(p, i), "captured playing forward seek replaces only the unlimited budget");
    for (uint8_t style : {1, 2, 3, 4}) { auto changed = p; changed.style = style; expect(forward(changed, i), "overlay styles supported"); }
    for (uint8_t style : {0, 5, 255}) { auto changed = p; changed.style = style; expect(!forward(changed, i), "texture/unknown styles unchanged"); }
    auto player = p; player.paused = 1; expect(!forward(player, i), "paused seek unchanged");
    player = p; player.ended = 1; expect(!forward(player, i), "end/restart behavior unchanged");
    player = p; player.handle = 0; expect(!forward(player, i), "missing handle unchanged");
    for (uint32_t state : {1u, 2u, 3u, UINT32_MAX}) { auto changed = i; changed.playback_state = state; expect(!forward(p, changed), "only playing seeks budgeted"); }
    for (uint64_t target : {0ull, 1ull, 69ull, 70ull, 2949ull, UINT64_MAX}) { expect(!forward(p, i, target), "rewind/cancel/equal/invalid target unchanged"); }
    for (uint64_t budget : {0ull, 6ull, 30ull}) { expect(!forward(p, i, 185, budget), "existing finite SDK budget preserved"); }
    for (auto field : {&b::MovieInfo::width, &b::MovieInfo::height, &b::MovieInfo::frames, &b::MovieInfo::frame, &b::MovieInfo::rate, &b::MovieInfo::rate_div}) {
        auto changed = i; changed.*field = 0; expect(!forward(p, changed), "incomplete movie info unchanged");
    }
    auto movie = i; movie.read_error = 1; expect(!forward(p, movie), "read error is not hidden by a budget");
    movie = i; movie.texture_error = 1; expect(!forward(p, movie), "texture error is not hidden by a budget");
    movie = i; movie.frames = UINT32_MAX; expect(!forward(p, movie), "SDK signed frame domain respected");

    movie = i; movie.playback_state = 2;
    b::GotoProgress progress{0, 185, 0, b::seek_budget_ms};
    expect(b::display_budgeted_seek(p, movie, progress), "pending bounded seek keeps original completed-image scheduling");
    auto pending = progress; pending.pending = 1;
    expect(b::display_budgeted_seek(p, movie, pending), "newly queued bounded seek also schedules the completed image");
    movie.frame = 185;
    expect(b::display_budgeted_seek(p, movie, progress), "target reached but SDK completion not yet published still displays");
    movie.frame = 186;
    expect(!b::display_budgeted_seek(p, movie, progress), "stale/backward target never borrows scheduling");
    movie = i; movie.playback_state = 2;
    for (auto field : {&b::GotoProgress::budget, &b::GotoProgress::target}) {
        auto changed = progress; changed.*field = -1;
        expect(!b::display_budgeted_seek(p, movie, changed), "unlimited/cancelled seek retains original Tick");
    }
    for (int32_t target : {0, 1, 69, 2949}) { auto changed = progress; changed.target = target; expect(!b::display_budgeted_seek(p, movie, changed), "invalid/backward progress retains Tick"); }
    for (uint32_t state : {0, 1, 3}) { auto changed = movie; changed.playback_state = state; expect(!b::display_budgeted_seek(p, changed, progress), "normal/paused/ended Tick unchanged"); }
    player = p; player.paused = 1; expect(!b::display_budgeted_seek(player, movie, progress), "pause flag never overridden");

    const auto object = base + 0x3000, stack = base + 0x5000;
    m.put(object + 0xb0, p.style); m.put(object + 0x108, p.handle);
    m.put(object + 0x110, p.paused); m.put(object + 0x111, p.ended);
    m.put(stack + 0x20, i);
    expect(b::seek_invocation(m.view(), true, object, p.handle, p.handle, stack, stack + 0x20, 185, UINT32_MAX), "exact Seek stack/caller context accepted");
    expect(!b::seek_invocation(m.view(), false, object, p.handle, p.handle, stack, stack + 0x20, 185, UINT32_MAX), "default-off Seek is a no-op");
    expect(!b::seek_invocation(m.view(), true, object, p.handle + 8, p.handle, stack, stack + 0x20, 185, UINT32_MAX), "saved/call handle mismatch rejected");
    expect(!b::seek_invocation(m.view(), true, object, p.handle + 8, p.handle + 8, stack, stack + 0x20, 185, UINT32_MAX), "player/call handle mismatch rejected");
    expect(!b::seek_invocation(m.view(), true, object, p.handle, p.handle, stack, stack + 0x28, 185, UINT32_MAX), "another stack local cannot supply Info");
    m.put(stack + 0x30, movie); m.put(p.handle + 0x118, progress);
    expect(b::tick_invocation(m.view(), true, object + 0x28, stack, stack + 0x30), "Tick uses its own adjusted player and current pending SDK handle");
    expect(!b::tick_invocation(m.view(), false, object + 0x28, stack, stack + 0x30), "default-off Tick is a no-op");
    expect(!b::tick_invocation(m.view(), true, object + 0x28, stack, stack + 0x38), "nested/different stack cannot borrow Info");
    expect(!b::tick_invocation(m.view(), true, 0x20, stack, stack + 0x30), "adjusted-this underflow rejected");
    expect(!b::tick_invocation(m.view(), true, object + 0x28, UINTPTR_MAX - 16, stack + 0x30), "stack arithmetic overflow rejected");
    m.readable = false;
    expect(!b::tick_invocation(m.view(), true, object + 0x28, stack, stack + 0x30), "unreadable caller keeps original Tick");
    for (uintptr_t flags : {0u, 1u, 0x40u, 0x202u, 0xad5u}) {
        expect(b::overlay_branch_flags(flags, false) == flags, "fallback preserves every CPU flag");
        expect(b::overlay_branch_flags(flags, true) == (flags | 0x40), "completed-image continuation changes only ZF");
    }
}
