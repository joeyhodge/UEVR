#pragma once

#include <algorithm>
#include <array>
#include <vector>
#include "mods/vr/Subnautica2BetaSlate.hpp"

template<class Expect> void test_subnautica2_beta_slate(const Expect& expect) {
    namespace s = uevr::subnautica2_beta::slate;
    constexpr uintptr_t object = 0x100000, scene = 0x200000, table = 0x140100000;
    std::array<uint8_t, s::resource_bytes> bytes{};
    const uint32_t width = 4944, height = 2416;
    std::memcpy(bytes.data(), &table, sizeof(table));
    std::memcpy(bytes.data() + 0x10, &scene, sizeof(scene));
    std::memcpy(bytes.data() + 0x28, &width, sizeof(width));
    std::memcpy(bytes.data() + 0x2c, &height, sizeof(height));
    bytes[8] = 1;
    const auto found = s::reference(object, bytes, scene);
    expect(found && found->cell == object + 0x10 && found->texture == scene && found->vtable == table,
        "beta Slate redirects the proven texture cell, not DestroyState at +8");
    expect(found && found->width == width && found->height == height, "source-matched dimension fields are readable");
    for (size_t n = 0; n < bytes.size(); ++n) {
        expect(!s::reference(object, std::span{bytes}.first(n), scene), "short Slate resources reject before accessing their texture");
    }
    expect(!s::reference(object, bytes, scene + 8) && !s::reference(object, bytes, 0),
        "another or absent scene cannot authorize a texture-cell write");
    for (auto address : {uintptr_t{0}, uintptr_t{0xfff8}, object + 1, uintptr_t{0x0000800000000000ull}, UINTPTR_MAX - 7}) {
        expect(!s::reference(address, bytes, scene), "invalid, unaligned or wrapped Slate objects reject");
    }
    auto legacy_only = bytes;
    std::memcpy(legacy_only.data() + 8, &scene, sizeof(scene));
    std::fill_n(legacy_only.data() + 0x10, sizeof(scene), uint8_t{});
    expect(!s::reference(object, legacy_only, scene), "a plausible pointer at the old +8 offset is never a fallback");
    for (size_t field : {size_t{0}, size_t{0x28}, size_t{0x2c}}) {
        auto invalid = bytes; std::fill_n(invalid.data() + field, field == 0 ? sizeof(uintptr_t) : sizeof(uint32_t), uint8_t{});
        expect(!s::reference(object, invalid, scene), "empty vtable or dimensions reject");
    }
    constexpr std::array<uint8_t, 4> width_code{0x8b,0x41,0x28,0xc3};
    constexpr std::array<uint8_t, 4> height_code{0x8b,0x41,0x2c,0xc3};
    expect(s::dimension_getter(width_code,0x28) && s::dimension_getter(height_code,0x2c),
        "captured read-only Width/Height virtuals prove the beta Slate layout without executing them");
    expect(!s::dimension_getter(width_code,0x2c) && !s::dimension_getter(height_code,0x28), "swapped getter slots reject");
    for (size_t n = 0; n < width_code.size(); ++n) {
        expect(!s::dimension_getter(std::span{width_code}.first(n),0x28), "truncated getter proof rejects");
    }
    for (const auto code : {std::vector<uint8_t>{0x89,0x41,0x28,0xc3}, std::vector<uint8_t>{0x48,0x8b,0x41,0x28,0xc3},
            std::vector<uint8_t>{0x8b,0x42,0x28,0xc3}, std::vector<uint8_t>{0x67,0x8b,0x41,0x28,0xc3},
            std::vector<uint8_t>{0x8b,0x41,0x28,0xc2,8,0}, std::vector<uint8_t>{0xe8,0,0,0,0,0xc3},
            std::vector<uint8_t>{0x8b,0x41,0x28,0x31,0xc0,0xc3}, std::vector<uint8_t>{0xc3}}) {
        expect(!s::dimension_getter(code,0x28), "writes, wrong widths/bases, calls, clobbers and cleanup returns cannot prove the layout");
    }
    const std::array<uint8_t, 6> padded{0x90,0x8b,0x41,0x28,0x90,0xc3};
    expect(s::dimension_getter(padded,0x28), "harmless NOPs do not change the read-only getter proof");
    auto oversized = std::vector<uint8_t>(width_code.begin(),width_code.end()); oversized.resize(17,0x90);
    expect(!s::dimension_getter(oversized,0x28), "getter validation cannot expand into an unbounded scan");
    expect(bytes[8] == 1, "read-only validation leaves DestroyState untouched");
}
