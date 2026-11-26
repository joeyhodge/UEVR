#pragma once

#include <array>
#include <cstring>

#include "mods/vr/UE426RendererExtent.hpp"

namespace ue426_renderer_tests {

using namespace uevr::ue426_renderer;

struct SanAndreasUnwind {
    static constexpr uint32_t unwind_rva = 0x4D46170;
    static constexpr uint32_t callback_return = 0x24179F4;
    static constexpr uint32_t image_size = 0x5CA4400;
    std::array<FunctionRecord, 3> table{{
        {0x2417830, 0x24179CB, unwind_rva},
        {0x24179CB, 0x2417A0A, unwind_rva + 0x18},
        {0x2417A0A, 0x2417D72, unwind_rva + 0x2C},
    }};
    std::array<uint8_t, 60> unwind{
        0x01, 0x13, 0x09, 0x00, 0x13, 0x34, 0x21, 0x00,
        0x13, 0x01, 0x1A, 0x00, 0x0C, 0xF0, 0x0A, 0xC0,
        0x08, 0x70, 0x07, 0x60, 0x06, 0x50, 0x00, 0x00,
        0x21, 0x08, 0x02, 0x00, 0x08, 0xE4, 0x20, 0x00,
        0x30, 0x78, 0x41, 0x02, 0xCB, 0x79, 0x41, 0x02, 0x70, 0x61, 0xD4, 0x04,
        0x21, 0x00, 0x00, 0x00,
        0x30, 0x78, 0x41, 0x02, 0xCB, 0x79, 0x41, 0x02, 0x70, 0x61, 0xD4, 0x04,
    };
    size_t readable_size{unwind.size()};

    std::optional<Extent> resolve(uint32_t pc = callback_return, uint32_t size = image_size) const {
        return extend_short_renderer(pc, size, [&](uint32_t rva) -> std::optional<FunctionRecord> {
            for (const auto& entry : table) {
                if (rva >= entry.begin && rva < entry.end) {
                    return entry;
                }
            }
            return {};
        }, [&](size_t rva, void* output, size_t count) {
            if (rva < unwind_rva || rva - unwind_rva > readable_size ||
                count > readable_size - (rva - unwind_rva))
            {
                return false;
            }
            std::memcpy(output, unwind.data() + (rva - unwind_rva), count);
            return true;
        });
    }
};

template <typename Expect>
void run(Expect&& expect) {
    const SanAndreasUnwind original{};
    expect(original.table[1].end - original.table[0].begin < minimum_size &&
        original.resolve() == Extent{original.table[0].begin, original.table[2].end},
        "San Andreas resolves the full renderer extent, returning the root rather than the callback fragment");

    expect(!original.resolve(original.table[1].begin + 2), "callback call must fit inside the chained part");
    expect(!original.resolve(original.table[0].begin + 0x30), "ordinary callable ranges do not use the chained fallback");
    expect(!original.resolve(original.callback_return, original.unwind_rva + 0x3B),
        "chained record must fit inside the main image");

    auto changed = original;
    ++changed.table[2].begin;
    expect(!changed.resolve(), "do not skip a gap looking for a continuation");
    changed = original;
    changed.table[2].end = changed.table[0].begin + minimum_size - 1;
    expect(!changed.resolve(), "do not lower the renderer size floor");
    changed.table[2].end = changed.table[0].begin + maximum_size + 1;
    expect(!changed.resolve(), "do not expand past the renderer size limit");

    changed = original;
    changed.table[0].unwind += 4;
    expect(!changed.resolve(), "embedded root must match the registered unwind record");
    changed = original;
    changed.table[1].unwind += 1;
    expect(!changed.resolve(), "reject misaligned unwind metadata");
    changed = original;
    changed.readable_size = changed.unwind.size() - 1;
    expect(!changed.resolve(), "reject an unreadable continuation record");

    changed = original;
    changed.unwind[0x2C] = 1;
    expect(!changed.resolve(), "do not count an adjacent independent function");
    changed.unwind[0x2C] = 0x29;
    expect(!changed.resolve(), "reject conflicting handler flags on a chained continuation");
    changed = original;
    changed.unwind[0x2F] = 0x5;
    expect(!changed.resolve(), "the continuation frame register must agree with the root");
    for (const size_t offset : {size_t{0x30}, size_t{0x34}, size_t{0x38}}) {
        changed = original;
        changed.unwind[offset] ^= 1;
        expect(!changed.resolve(), "compare all three fields of the continuation's owner");
    }
}

} // namespace ue426_renderer_tests
