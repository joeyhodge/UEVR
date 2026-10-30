#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <sdk/PhantomBladeZeroRenderTarget.hpp>

#include "mods/vr/PhantomBladeZeroUI.hpp"

namespace pbz_ui_tests {

struct Memory {
    static constexpr uintptr_t base = 0x10000, owner = 0x11000, resource = 0x12000, rhi = 0x14000;
    std::array<uint8_t, 0x6000> bytes{};

    template <typename T>
    void put(uintptr_t address, T value) { std::memcpy(bytes.data() + address - base, &value, sizeof(value)); }

    template <typename T>
    bool read(uintptr_t address, T& value) const {
        if (address < base || address - base > bytes.size() || sizeof(T) > bytes.size() - (address - base)) {
            return false;
        }
        std::memcpy(&value, bytes.data() + address - base, sizeof(T));
        return true;
    }

    Memory() {
        put(owner + 0x30, resource);
        put(owner + 0x38, resource);
        put(resource + uevr::pbz_ui::owner_offset, owner);
        put(resource + uevr::ue58_owned_ui::texture_rhi_offset, rhi);
        put(resource + uevr::pbz_ui::render_target_texture_offset, rhi);
        put(resource + uevr::pbz_ui::width_offset, uint32_t{1920});
        put(resource + uevr::pbz_ui::height_offset, uint32_t{1080});
    }

    auto find(bool code_proven = true, size_t owner_size = 0x100) const {
        return uevr::pbz_ui::find_owned_resource(owner, owner_size, 1920, 1080,
            [&](uintptr_t address, auto& value) { return read(address, value); },
            [&](uintptr_t actual_resource, uintptr_t actual_rhi) {
                return code_proven && actual_resource == resource && actual_rhi == rhi;
            });
    }
};

template <typename Expect>
void run(const Expect& expect) {
    using namespace uevr;
    pbz_ui::Creation state{true, true, true, true, true, true, true, 1920, 1080};
    expect(pbz_ui::can_request(state), "PBZ menu UI does not require a scene family");
    std::array flags{&state.exact_game, &state.dx12, &state.inputs_proven, &state.game_data,
        &state.engine, &state.stable_slate, &state.main_target};
    for (auto* flag : flags) {
        *flag = false;
        expect(!pbz_ui::can_request(state), "PBZ UI requires every independent startup proof");
        *flag = true;
    }
    expect(!pbz_ui::valid_extent(0, 1080) && !pbz_ui::valid_extent(1920, 0) &&
        !pbz_ui::valid_extent(100, 100) && !pbz_ui::valid_extent(16385, 1080) &&
        !pbz_ui::valid_extent(16384, 16384), "PBZ UI rejects absent, tiny and excessive extents");

    Memory memory;
    const auto valid = memory.find();
    expect(valid && valid->private_resource_offset == 0x30 && valid->resource == Memory::resource &&
        valid->rhi_texture == Memory::rhi, "PBZ resource uses its own verified secondary-base layout");
    if (!valid) { return; }
    expect(!memory.find(false), "PBZ resource requires accessor-code proof");
    expect(!memory.find(true, 0x301), "PBZ resource scan is bounded by the reflected owner size");

    auto changed = memory;
    changed.put(Memory::owner + 0x38, uintptr_t{0});
    expect(!changed.find(), "PBZ waits for matching GT and RT resource mirrors");
    changed = memory;
    changed.put(Memory::resource + pbz_ui::owner_offset, Memory::owner + 8);
    expect(!changed.find(), "PBZ rejects another texture's render resource");
    changed = memory;
    changed.put(Memory::resource + pbz_ui::render_target_texture_offset, Memory::rhi + 8);
    expect(!changed.find(), "PBZ requires FTexture and FRenderTarget RHI agreement");
    changed = memory;
    changed.put(Memory::resource + pbz_ui::height_offset, uint32_t{720});
    expect(!changed.find(), "PBZ rejects stale or wrongly sized resources");
    expect(!pbz_ui::resource_matches(Memory::owner, *valid, 1920, 1080,
        [&](uintptr_t address, auto& value) { return changed.read(address, value); }), "PBZ publication rechecks the exact owned chain");
    expect(pbz_ui::resource_matches(Memory::owner, *valid, 1920, 1080,
        [&](uintptr_t address, auto& value) { return memory.read(address, value); }), "PBZ retained capture resource can be revalidated without scanning");
    changed = memory;
    changed.put(Memory::owner + 0x60, Memory::resource);
    changed.put(Memory::owner + 0x68, Memory::resource);
    expect(!changed.find(), "PBZ rejects ambiguous owner fields");
    changed = memory;
    changed.put(Memory::owner + 0x30, (std::numeric_limits<uintptr_t>::max)() - 7);
    changed.put(Memory::owner + 0x38, (std::numeric_limits<uintptr_t>::max)() - 7);
    expect(!changed.find(), "PBZ rejects overflowing resource addresses");
    expect(!ue58_owned_ui::find_resource(Memory::owner, 0x100, 1920, 1080,
        [&](uintptr_t address, auto& value) { return memory.read(address, value); },
        [](uintptr_t, uintptr_t) { return true; }), "PBZ does not masquerade as the UE5.8 resource layout");

    ue58_owned_ui::StableResource stable;
    const ue58_owned_ui::Observation observation{1, *valid, 0x15000};
    expect(!stable.observe(observation) && stable.observe(observation), "PBZ requires two identical owned-resource samples");
    expect(!stable.observe(std::nullopt) && !stable.observe(observation), "PBZ loses stability on an incomplete resource");
    auto code = pbz_ui::slate_inputs_contract;
    expect(ue58_owned_ui::matches_accessor(code, pbz_ui::slate_inputs_contract), "PBZ input ABI matches its contract");
    code[3] ^= 8;
    expect(!ue58_owned_ui::matches_accessor(code, pbz_ui::slate_inputs_contract), "PBZ rejects a changed Slate input offset");
    constexpr std::array<uint8_t, 27> original_native{
        0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xd0, 0, 0, 0,
        0x33, 0xdb, 0x48, 0x85, 0xc0, 0x74, 0x09, 0x48, 0x8b, 0x58, 0x20, 0x48, 0x85, 0xdb};
    expect(ue58_owned_ui::matches_accessor(original_native, pbz_ui::native_resource_prefix),
        "PBZ native accessor preserves the original instruction prefixes");

    constexpr uintptr_t table = 0x14800, texture_function = 0x15000, gamma_function = 0x15100;
    memory.put(table + 2 * sizeof(uintptr_t), texture_function);
    memory.put(table + 6 * sizeof(uintptr_t), gamma_function);
    memory.put(texture_function, sdk::pbz_render_target::texture_code);
    constexpr std::array<uint8_t, 14> original_gamma{
        0x48, 0x8b, 0x49, 0x40, 0x48, 0x8b, 0x01, 0x48, 0xff, 0xa0, 0xe0, 0x03, 0, 0};
    memory.put(gamma_function, original_gamma);
    sdk::discovery::Memory backend{&memory,
        [](void* context, uintptr_t address, void* out, size_t count) {
            const auto& fixture = *static_cast<Memory*>(context);
            if (address < Memory::base || address - Memory::base > fixture.bytes.size() ||
                count > fixture.bytes.size() - (address - Memory::base)) { return false; }
            std::memcpy(out, fixture.bytes.data() + address - Memory::base, count);
            return true;
        },
        [](void*, uintptr_t address, size_t count) {
            return address >= 0x15000 && address <= 0x16000 && count <= 0x16000 - address;
        }};
    expect(sdk::pbz_render_target::texture_accessor(backend, table), "PBZ validates texture slot 2 without invoking it");
    expect(sdk::pbz_render_target::gamma_accessor(backend, table), "PBZ validates gamma slot 6 without invoking other slots");
    memory.put(gamma_function, uint8_t{0xc3});
    expect(!sdk::pbz_render_target::gamma_accessor(backend, table), "PBZ refuses an unrelated slot-6 function");
    expect(!sdk::pbz_render_target::texture_accessor(backend, UINTPTR_MAX - 7), "PBZ rejects overflowing vtable slots");
}

} // namespace pbz_ui_tests
