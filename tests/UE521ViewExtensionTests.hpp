#pragma once

#include "mods/vr/UE521ViewExtensionCode.hpp"
#include <array>
#include <cstring>

namespace ue521_view_extension_tests {
namespace v = uevr::ue521::view_extension;
struct Memory {
    static constexpr uintptr_t base = 0x10000, family = 0x10100, views = 0x10500;
    static constexpr uintptr_t target = 0x10600, scene = 0x10700, view = 0x10800;
    std::array<uint8_t, 0x1000> bytes{};
    uintptr_t denied{};
    bool bad_poly{}, change_frame{};
    size_t family_reads{};
    template<class T> void put(uintptr_t address, T value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
    Memory() {
        put<uintptr_t>(family, 0x20000); put<uintptr_t>(family + 8, views);
        put<int32_t>(family + 16, 1); put<int32_t>(family + 20, 1);
        put<uintptr_t>(family + 0x20, target); put<uintptr_t>(family + 0x28, scene);
        put<uint32_t>(family + v::frame_offset, 100); put<uintptr_t>(views, view);
    }
    bool read(uintptr_t address, void* out, size_t size) {
        if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base) ||
            address == denied) { return false; }
        if (address == family && ++family_reads == 2 && change_frame) { put<uint32_t>(family + v::frame_offset, 101); }
        std::memcpy(out, bytes.data() + address - base, size); return true;
    }
    auto observe(uintptr_t at = family) {
        return v::read_family(at, [&](auto a, auto* o, auto n) { return read(a, o, n); },
            [&](uintptr_t a) { return !bad_poly && (a == family || a == target || a == scene); });
    }
};

template<class Expect> void run(const Expect& expect) {
    const std::array<uint8_t, 9> begin_code{0x89, 0x83, 0x84, 0, 0, 0, 0xff, 0x50, 0x28};
    const std::array<uint8_t, 7> pre_code{0xff, 0x50, 0x38, 0x41, 0xff, 0x51, 0x48};
    expect(v::caller_contract(begin_code, begin_code.size(), v::Stage::begin),
        "Begin caller has a decoded dword frame write before the exact slot-5 call");
    expect(v::caller_contract(pre_code, 3, v::Stage::pre_render),
        "RDG family caller at slot 7 has the subsequent slot-9 view loop");
    expect(!v::caller_contract(begin_code, 8, v::Stage::begin) &&
        !v::caller_contract(begin_code, 0, v::Stage::begin) &&
        !v::caller_contract(begin_code, 10, v::Stage::begin) &&
        !v::caller_contract(begin_code, begin_code.size(), v::Stage::pre_render),
        "return address must end the source-verified callback instruction for that stage");
    auto wrong_frame = begin_code; wrong_frame[2] = 0x80;
    auto frame_read = begin_code; frame_read[0] = 0x8b;
    const std::array<uint8_t, 10> stack_frame{0x89, 0x84, 0x24, 0x84, 0, 0, 0, 0xff, 0x50, 0x28};
    auto wrong_begin = begin_code; wrong_begin[8] = 0x30;
    expect(!v::caller_contract(wrong_frame, 9, v::Stage::begin) &&
        !v::caller_contract(frame_read, 9, v::Stage::begin) &&
        !v::caller_contract(stack_frame, stack_frame.size(), v::Stage::begin) &&
        !v::caller_contract(wrong_begin, 9, v::Stage::begin),
        "wrong member, reads, stack storage and wrong virtual call cannot prove Begin");
    const std::array<uint8_t, 10> wide_frame{0x48, 0x89, 0x83, 0x84, 0, 0, 0, 0xff, 0x50, 0x28};
    const std::array<uint8_t, 13> embedded{0x48, 0xb8, 0x89, 0x83, 0x84, 0, 0, 0, 0, 0, 0xff, 0x50, 0x28};
    expect(!v::caller_contract(wide_frame, wide_frame.size(), v::Stage::begin) &&
        !v::caller_contract(embedded, embedded.size(), v::Stage::begin),
        "64-bit stores and instruction-shaped immediate bytes are not a frame write");
    auto wrong_pre = pre_code; wrong_pre[2] = 0x30;
    auto wrong_view = pre_code; wrong_view[6] = 0x40;
    auto after_return = pre_code; after_return[3] = 0xc3;
    expect(!v::caller_contract(wrong_pre, 3, v::Stage::pre_render) &&
        !v::caller_contract(wrong_view, 3, v::Stage::pre_render) &&
        !v::caller_contract(after_return, 3, v::Stage::pre_render) &&
        !v::caller_contract(std::span<const uint8_t>{pre_code}.first(3), 3, v::Stage::pre_render),
        "wrong overload, missing view loop and bytes past a return cannot authorize RDG PreFamily");
    std::array<uint8_t, v::max_caller_bytes + 1> oversized{};
    expect(!v::caller_contract(oversized, 1, v::Stage::begin), "caller-code discovery is bounded");
    expect(v::eligible(5, 2, 1, 20, false), "stock 5.2.1 topology is a candidate, not automatic hook authorization");
    expect(v::eligible(5, 2, 0, 20, false), "source-identical 5.2.0 still requires native caller and family proof");
    for (const auto active : {14u, 19u, 21u, 22u}) {
        expect(!v::eligible(5, 2, 1, active, false), "different/custom extension topology keeps its existing path");
    }
    expect(!v::eligible(4, 27, 2, 20, false) && !v::eligible(5, 1, 1, 20, false) &&
        !v::eligible(5, 3, 1, 20, false) && !v::eligible(5, 2, 2, 20, false) &&
        !v::eligible(5, 2, 1, 20, true), "unverified versions and destructor-less layouts never take the fallback");
    Memory good;
    const auto family = good.observe();
    expect(family && family->target == Memory::target && family->scene == Memory::scene && family->frame == 100,
        "reads the source/PDB family prefix and frame at 0x84");
    for (auto denied : {Memory::family, Memory::views, Memory::view}) {
        Memory m; m.denied = denied;
        expect(!m.observe(), "unreadable family, view-array and view pointers fail closed");
    }
    Memory poly; poly.bad_poly = true;
    expect(!poly.observe(), "family/target/scene ownership is mandatory");
    Memory changed; changed.change_frame = true;
    expect(!changed.observe(), "changing callback arguments cannot authorize a mixed-frame layout");
    for (auto count : {-1, 0, 9}) {
        Memory m; m.put<int32_t>(Memory::family + 16, count);
        expect(!m.observe(), "empty/negative/oversized view arrays are not proof");
    }
    Memory capacity; capacity.put<int32_t>(Memory::family + 20, 0);
    expect(!capacity.observe(), "view count must fit capacity");
    Memory no_scene; no_scene.put<uintptr_t>(Memory::family + 0x28, 0);
    expect(!no_scene.observe(), "null scene is not a main renderer witness");
    expect(!good.observe(0) && !good.observe(UINTPTR_MAX - 7) && !good.observe(Memory::family + 1),
        "null/overflow/unaligned candidate pointers are rejected");

    const auto sample = [](v::Evidence& e, v::Stage stage, uint32_t frame,
        uintptr_t target = Memory::target, uintptr_t scene = Memory::scene) {
        e.observe(stage, v::Family{target, scene, frame});
    };
    for (auto stride : {1u, 2u, 4u, 16u}) {
        v::Evidence e;
        for (uint32_t i{}; i < 8; ++i) {
            sample(e, v::Stage::begin, 100 + stride*i);
            sample(e, v::Stage::pre_render, 100 + stride*i);
            if (i < 7) { expect(!e.ready(), "partial observations never install callbacks"); }
        }
        expect(e.ready().has_value(), "source-frame proof accepts real advancing scene counters, not only exact +1");
    }
    v::Evidence repeated, frozen, wrong_key, gap, sparse, reset;
    for (uint32_t i{}; i < 10; ++i) {
        for (unsigned repeat{}; repeat < 2; ++repeat) {
            sample(repeated, v::Stage::begin, 100+i); sample(repeated, v::Stage::pre_render, 100+i);
        }
        sample(frozen, v::Stage::begin, 100); sample(frozen, v::Stage::pre_render, 100);
        sample(wrong_key, v::Stage::begin, 100+i); sample(wrong_key, v::Stage::pre_render, 100+i, Memory::target+8);
        sample(gap, v::Stage::begin, 100+i); sample(gap, v::Stage::pre_render, 500+i);
        sample(sparse, v::Stage::begin, 100+i*65); sample(sparse, v::Stage::pre_render, 100+i*65);
        sample(reset, v::Stage::begin, 100+i); sample(reset, v::Stage::pre_render, 100+i);
    }
    expect(repeated.ready().has_value(), "multiple callbacks for one scene frame do not prevent convergence");
    expect(!frozen.ready() && !wrong_key.ready() && !gap.ready() && !sparse.ready(),
        "constant clocks, unrelated targets, excessive lag and unbounded counter jumps do not authorize slots");
    sample(reset, v::Stage::begin, 10);
    expect(!reset.ready(), "a large backwards clock reset revokes both stages of that scope");
    sample(repeated, v::Stage::begin, 1000);
    expect(!repeated.ready(), "an excessive forward jump also requires fresh source observations");
    v::Evidence mixed;
    for (uint32_t i{}; i < 8; ++i) for (uintptr_t extra{}; extra < 3; ++extra) {
        sample(mixed, v::Stage::begin, 100+i, Memory::target+extra*8);
        sample(mixed, v::Stage::pre_render, 100+i, Memory::target+extra*8);
    }
    expect(mixed.ready().has_value(), "bounded interleaved auxiliary families do not reset the main source proof");
}
}
