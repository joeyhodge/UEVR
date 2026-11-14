#pragma once

#include <sdk/UE6Contracts.hpp>
#include "mods/vr/UE60ViewExtension.hpp"

namespace ue6_november_tests {
template<size_t Words> struct ConsoleModel {
    uint32_t flags;
    std::array<uint32_t, Words> tag, anchor;
};
static_assert(sizeof(ConsoleModel<1>) == 12 && sizeof(ConsoleModel<2>) == 20 && sizeof(ConsoleModel<3>) == 28);

template<size_t Words> struct FamilyModel {
    uintptr_t vtable;
    std::array<uint8_t, 16> views, all_views;
    uint8_t mode;
    uintptr_t target, depth;
    std::array<uint8_t, 16> requests;
    std::array<uint32_t, Words> context;
    float maximum_nits, paper_white_nits;
    uintptr_t scene;
};
static_assert(offsetof(FamilyModel<1>, target) == sdk::ue6::family_render_target);
static_assert(offsetof(FamilyModel<1>, scene) == 0x60 && offsetof(FamilyModel<2>, scene) == 0x60 &&
    offsetof(FamilyModel<3>, scene) == 0x68);

template<class Expect>
void run(Expect&& expect) {
    expect(sdk::ue6::never_replay_viewstate_postinit(6) && !sdk::ue6::never_replay_viewstate_postinit(5),
        "UE6 histories use original engine view setup, never PostInitProperties replay; UE5 policy is unchanged");
    const sdk::ue6::ConsoleSetContext context{0x09000000};
    for (const size_t words : {1, 2, 3}) {
        const size_t used = 4 + 2 * words * 4;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&context);
        expect(used <= sizeof(context) && context.flags == 0x09000000,
            "resolved context has room for Tag and AnchorTag at all reviewed name widths");
        for (size_t i = 4; i < used; ++i) { expect(bytes[i] == 0, "both name tags remain NAME_None"); }
    }
    expect(sdk::ue6::family_prefix(8, 0x30, 0x48) && sdk::ue6::family_prefix(8, 0x30, 0x60) &&
        sdk::ue6::family_prefix(8, 0x30, 0x68), "old and new independently validated family prefixes");
    expect(!sdk::ue6::family_prefix(8, 0x30, 0x40) && !sdk::ue6::family_prefix(0, 0x30, 0x60),
        "request array and legacy view offset cannot become the scene");
    expect(sdk::ue6::extension_mapping(true, 20, 4, 6, 0xb8) &&
        sdk::ue6::extension_mapping(true, 22, 4, 8, 0xd0), "both source callback topologies need live frame proof");
    expect(!sdk::ue6::extension_mapping(true, 22, 4, 6, 0xd0) &&
        !sdk::ue6::extension_mapping(true, 21, 4, 8, 0xd0) &&
        !sdk::ue6::extension_mapping(false, 22, 4, 8, 0xd0) &&
        !sdk::ue6::extension_mapping(true, 22, 4, 8, 0x100), "mixed, UE5.9 and out-of-prefix mappings rejected");

    uevr::ue60::SlateInputPrefix input{};
    input.renderer = input.window_element_list = input.window = input.viewport_info = reinterpret_cast<void*>(0x10000);
    input.hdr.maximum_nits = input.hdr.full_frame_nits = 100;
    input.hdr.paper_white_nits = 203;
    input.scene_rect.max = {1920,1080}; input.ui_scale = 1;
    input.screenshot.colors = reinterpret_cast<void*>(UINTPTR_MAX);
    std::array<uint8_t, sizeof(input)> bytes{};
    std::memcpy(bytes.data(), &input, sizeof(input));
    const auto saved = bytes;
    auto decoded = uevr::ue60::decode_sdr_input(bytes, input.renderer);
    expect(decoded && decoded->layout == uevr::ue60::InputLayout::november && decoded->prefix.scene_rect.max.x == 1920 &&
        bytes == saved, "screenshot-aware decoding never touches screenshot pointers or engine storage");
    if (!decoded) { return; }
    const uevr::ue60::IntRect old_rect{{0,0},{800,600}};
    const float scale = 1;
    std::memcpy(bytes.data() + offsetof(uevr::ue60::SlateInputPrefixOctober, scene_rect), &old_rect, sizeof(old_rect));
    std::memcpy(bytes.data() + offsetof(uevr::ue60::SlateInputPrefixOctober, ui_scale), &scale, sizeof(scale));
    expect(!uevr::ue60::decode_sdr_input(bytes, input.renderer), "ambiguous same-version Slate inputs fail closed");
    bytes = {};
    auto october = decoded->prefix;
    std::memcpy(bytes.data(), &october, sizeof(october));
    decoded = uevr::ue60::decode_sdr_input(bytes, input.renderer);
    expect(decoded && decoded->layout == uevr::ue60::InputLayout::october,
        "November refresh preserves the uniquely validated October input");
    expect(!uevr::ue60::decode_sdr_input(std::span{bytes}.first(0x100), input.renderer), "truncated Slate header rejected");

    constexpr uintptr_t base = 0x10000;
    std::array<uint8_t, 0x1000> memory{};
    const auto put = [&]<class T>(uintptr_t address, const T& value) { std::memcpy(memory.data() + address - base, &value, sizeof(value)); };
    const auto read = [&](uintptr_t address, void* out, size_t size) {
        if (address < base || address - base > memory.size() || size > memory.size() - (address - base)) { return false; }
        std::memcpy(out, memory.data() + address - base, size); return true;
    };
    const auto polymorphic = [](uintptr_t address) { return address == base || address == base + 0x300 ||
        address == base + 0x400 || address == base + 0x500; };
    put(base + 8, base + 0x200); put(base + 16, int32_t{1}); put(base + 20, int32_t{1});
    put(base + 0x30, base + 0x400); put(base + 0x60, base + 0x500); put(base + 0xd0, uint32_t{45});
    put(base + 0x200, base + 0x300); put(base + 0x308, base);
    expect(uevr::ue60::valid_family(base, 0xd0, read, polymorphic), "new family identity and exact view backlinks validate");
    put(base + 0x308, base + 8);
    expect(!uevr::ue60::valid_family(base, 0xd0, read, polymorphic), "a different family backlink rejects an RDG or auxiliary candidate");
    put(base + 0x308, base); put(base + 0x48, base + 0x500);
    expect(!uevr::ue60::valid_family(base, 0xd0, read, polymorphic), "ambiguous scene owners cannot qualify a callback");
    put(base + 0x48, uintptr_t{});
    expect(!uevr::ue60::valid_family(UINTPTR_MAX - 16, 0xd0, read, polymorphic), "family read cannot wrap");
    int reads{};
    const auto changing = [&](uintptr_t address, void* out, size_t size) {
        if (!read(address, out, size)) { return false; }
        if (address == base && ++reads == 2) { static_cast<uint8_t*>(out)[0xd0] ^= 1; }
        return true;
    };
    expect(!uevr::ue60::valid_family(base, 0xd0, changing, polymorphic), "racing frame snapshots are discarded");
}
} // namespace ue6_november_tests
