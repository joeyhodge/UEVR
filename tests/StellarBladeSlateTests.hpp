#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <mods/vr/StellarBladeSlate.hpp>

template <typename Expect>
void test_stellar_blade_slate_contract(Expect&& expect) {
    using namespace uevr::stellar_blade;
    expect(provider_field(viewport_texture_getter) == 0xC0,
        "Stellar Blade's original getter proves provider +C0 and resource +8 without a call");
    for (size_t i = 0; i < viewport_texture_getter.size(); ++i) {
        if (i >= 9 && i < 13) { continue; }
        auto changed = viewport_texture_getter;
        changed[i] ^= 1;
        expect(!provider_field(changed), "changed getter behavior is not authorized");
    }
    for (uint32_t offset : {0u, 8u, 0x41u, 0x308u, 0xFFFFFFC0u}) {
        auto changed = viewport_texture_getter;
        std::memcpy(changed.data() + 9, &offset, sizeof(offset));
        expect(!provider_field(changed), "invalid provider displacement is rejected");
    }
    for (size_t length = 0; length < viewport_texture_getter.size(); ++length) {
        expect(!provider_field(std::span{viewport_texture_getter}.first(length)),
            "truncated getter is never used");
    }
    constexpr uintptr_t base = 0x140000000, draw_address = base + 0x2000, getter = base + 0x1000;
    std::array<uint8_t, 0x200> draw{};
    const std::array<uint8_t, 15> entry{0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    std::copy(entry.begin(), entry.end(), draw.begin());
    std::copy(slate_arguments.begin(), slate_arguments.end(), draw.begin() + slate_argument_offset);
    const auto put_call = [&](size_t at, uintptr_t target, bool flag) {
        const std::array<uint8_t, 7> args{0x48,0x8D,0x55,0x80,0x49,0x8B,0xCD};
        std::copy(args.begin(), args.end(), draw.begin() + at);
        size_t call = at + args.size();
        if (flag) {
            const std::array<uint8_t, 5> bits{0x41,0x0F,0xBA,0xEC,0x07};
            std::copy(bits.begin(), bits.end(), draw.begin() + call);
            call += bits.size();
        }
        draw[call] = 0xE8;
        const auto relative = static_cast<int32_t>(static_cast<int64_t>(target) -
            static_cast<int64_t>(draw_address + call + 5));
        std::memcpy(draw.data() + call + 1, &relative, sizeof(relative));
    };
    size_t reads{};
    auto reader = [&](uintptr_t at, void* output, size_t bytes) {
        ++reads;
        if (at != getter || bytes != viewport_texture_getter.size()) { return false; }
        std::memcpy(output, viewport_texture_getter.data(), bytes);
        return true;
    };
    const auto discover = [&](const auto& source, auto&& read) {
        return discover_provider_field(source, draw_address, base, base + 0x10000, read);
    };
    put_call(0x80, getter, false);
    expect(!discover(draw, reader), "one plausible getter call is insufficient");
    put_call(0xE0, getter, true);
    expect(discover(draw, reader) == 0xC0, "both original sret call variants establish the same provider field");
    auto invalid_args = draw;
    invalid_args[0x80 + 6] = 0xC9;
    expect(!discover(invalid_args, reader), "a different viewport receiver is not accepted");
    auto truncated = std::span{draw}.first(0xE0 + 14);
    expect(!discover(truncated, reader), "partial second call cannot authorize the layout");
    auto invalid_entry = draw;
    invalid_entry[0] ^= 1;
    reads = 0;
    expect(!discover(invalid_entry, reader) && reads == 0, "unknown Slate entry causes no getter reads or calls");
    auto invalid_binding = draw;
    invalid_binding[slate_argument_offset + 9] ^= 1;
    reads = 0;
    expect(!discover(invalid_binding, reader) && reads == 0, "R13 must contain the original R8 viewport argument");
    expect(!discover(draw, [](uintptr_t, void*, size_t) { return false; }),
        "unreadable or non-executable getter fails closed");
    put_call(0xE0, getter + 0x100, true);
    expect(!discover(draw, [&](uintptr_t, void* out, size_t size) {
        std::memcpy(out, viewport_texture_getter.data(), size); return true;
    }), "two distinct getter candidates are ambiguous even if their layouts match");
    put_call(0x80, base - 0x1000, false);
    put_call(0xE0, base - 0x1000, true);
    reads = 0;
    expect(!discover(draw, reader) && reads == 0, "out-of-image call targets are never read");
    expect(!discover_provider_field(draw, draw_address, base, draw_address + 0x20, reader),
        "caller window must remain inside the image");
    expect(!discover_provider_field(draw, std::numeric_limits<uintptr_t>::max() - 10,
        base, std::numeric_limits<uintptr_t>::max(), reader), "overflowing caller span is rejected");
    for (bool target : {false, true}) {
        for (bool other : {false, true}) {
            for (bool failed : {false, true}) {
                const auto expected = (target ? !failed : other) ? 2u : 1u;
                expect(warming_view_count(target, other, failed) == expected,
                    "Stellar Blade preserves its pair while warming, but retains one view after failed state allocation");
            }
        }
    }
}
