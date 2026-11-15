#pragma once

#include <span>
#include "mods/vr/ParadoxDriveRHI.hpp"
#include "fixtures/UE584StockABI.hpp"
#include "fixtures/UE584DevelopmentABI.hpp"

namespace paradoxdrive_rhi_tests {
namespace p = uevr::paradoxdrive_rhi;

struct Memory {
    uintptr_t base{0x100000};
    std::array<uint8_t, 0x6000> bytes{};
    bool readable{true}, executable{true};
    size_t reads{};
    sdk::discovery::Memory view() {
        return {this, [](void* context, uintptr_t address, void* output, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            ++m.reads;
            if (!m.readable || address < m.base || address - m.base > m.bytes.size() ||
                size > m.bytes.size() - (address - m.base)) { return false; }
            std::memcpy(output, m.bytes.data() + address - m.base, size);
            return true;
        }, [](void* context, uintptr_t address, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            return m.executable && address >= m.base && address - m.base < 0x4000 &&
                size <= 0x4000 - (address - m.base);
        }};
    }
    template<class T> void put(uintptr_t address, const T& value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
};

template<class Expect> void run(Expect&& expect) {
    expect(p::target(L"D:/Games/ParadoxDrive-Win64-Shipping.exe", 0x50008) &&
        p::target(L"PARADOXDRIVE-WIN64-SHIPPING.EXE", 0x50008), "the RDG bridge is scoped to ParadoxDrive UE5.8");
    for (const auto name : {L"Other.exe", L"ParadoxDrive.exe", L"xParadoxDrive-Win64-Shipping.exe",
            L"ParadoxDrive-Win64-Shipping.exe.old", L"ParadoxDrive-Win64-Shipping.exe/Other.exe"}) {
        expect(!p::target(name, 0x50008), "other executables cannot enable the RDG bridge");
    }
    for (const auto version : {0u, 0x4001bu, 0x50007u, 0x50009u, 0x60000u}) {
        expect(!p::target(L"ParadoxDrive-Win64-Shipping.exe", version), "other engine intervals retain their original callback");
    }
    Memory memory;
    const auto render = memory.base + 0x400, builder = memory.base + 0x1000;
    const auto other = memory.base + 0x2000;
    memory.put(render, p::contract::render_texture_code);
    memory.put(builder, p::contract::builder_prefix_code);
    memory.put(other, p::contract::builder_prefix_code);
    const auto size = [&](uintptr_t address) -> size_t {
        return address == render ? p::contract::render_texture_code.size() :
            address == builder || address == other ? p::builder_function_size : 0;
    };
    const auto discover = [&](auto&& scan) {
        return p::discover(memory.view(), memory.base, 0x4000, render, scan, size);
    };
    const auto unique = [&](uintptr_t cursor, size_t bytes, const char*) -> std::optional<uintptr_t> {
        expect(cursor >= memory.base && bytes <= 0x4000 && cursor + bytes == memory.base + 0x4000,
            "discovery stays within the current module");
        return cursor <= builder ? std::optional<uintptr_t>{builder} : std::nullopt;
    };
    expect(discover(unique), "the native RDG overload and unique constructor reference store authorize unwrapping");
    const auto classify = [&](std::wstring_view path, uint32_t version = 0x50008u) {
        return p::classify_input(memory.view(), path, version, memory.base, 0x4000, render, unique, size);
    };
    for (const auto name : {L"ParadoxDrive-Win64-Shipping.exe", L"UnrealGame-Win64-Shipping.exe",
            L"D:/Games/Other-Win64-Shipping.exe"}) {
        expect(classify(name) == p::Input::VerifiedRDG,
            "the same fully proven Shipping RDG ABI is reusable without a title-name allowlist");
    }
    for (const auto version : {0u, 0x4001bu, 0x50007u, 0x50009u, 0x60000u}) {
        memory.reads = 0;
        expect(classify(L"UnrealGame-Win64-Shipping.exe", version) == p::Input::LegacyRHI && memory.reads == 0,
            "other engine intervals retain their original argument path with no new memory reads");
    }
    memory.bytes[render - memory.base] ^= 1;
    expect(classify(L"UnrealGame-Win64-Shipping.exe") == p::Input::LegacyRHI,
        "an unrecognized callback cannot authorize new stock argument handling");
    expect(classify(L"ParadoxDrive-Win64-Shipping.exe") == p::Input::UnverifiedRDG,
        "a changed known RDG title preserves its existing fail-closed protection");
    memory.bytes[render - memory.base] ^= 1;
    memory.bytes[builder - memory.base] ^= 1;
    expect(classify(L"UnrealGame-Win64-Shipping.exe") == p::Input::UnverifiedRDG,
        "a proven RDG callback with an unproven builder is never reinterpreted as a legacy command list");
    memory.bytes[builder - memory.base] ^= 1;
    {
        const auto saved = memory;
        memory.put(render, uevr::tests::ue584_stock::render_texture);
        memory.put(builder, uevr::tests::ue584_stock::builder_prefix);
        expect(classify(L"UnrealGame-Win64-Shipping.exe") == p::Input::VerifiedRDG,
            "independent stock UE5.8.4 EXE/PDB bytes satisfy the unchanged callback/constructor proof");
        memory.bytes[builder - memory.base + p::contract::builder_prefix_code.size() - 4] ^= 1;
        expect(classify(L"UnrealGame-Win64-Shipping.exe") == p::Input::UnverifiedRDG,
            "changing the independently captured builder reference store rejects stock unwrapping");
        memory = saved;
    }
    {
        namespace d = uevr::ue584_development::contract;
        namespace f = uevr::tests::ue584_development;
        const auto saved = memory;
        memory.put(render, f::render_texture); memory.put(builder, f::builder_prefix); memory.put(other, f::builder_prefix);
        const auto development_size = [&](uintptr_t address) -> size_t {
            return address == render ? f::render_texture.size() :
                address == builder || address == other ? p::development_builder_function_size : 0;
        };
        const auto classify_development = [&] {
            return p::classify_input(memory.view(), L"UnrealGame.exe", 0x50008,
                memory.base, 0x4000, render, unique, development_size);
        };
        expect(classify_development() == p::Input::VerifiedRDG,
            "independent Development callback and constructor bytes authorize the separately proven immediate-list reference");
        const auto before = memory;
        for (const auto part : {0, 1}) {
            const auto address = part ? builder : render;
            const auto mask = part ? std::span<const uint8_t>{d::builder_prefix_mask} : std::span<const uint8_t>{d::render_texture_mask};
            for (size_t i = 0; i < mask.size(); ++i) {
                if (!mask[i]) { continue; }
                memory = before; memory.bytes[address - memory.base + i] ^= 1;
                expect(classify_development() != p::Input::VerifiedRDG,
                    "changed Development argument flow, member offset or control flow cannot authorize RDG unwrapping");
            }
        }
        memory = before; memory.bytes[builder-memory.base] ^= 1;
        expect(classify_development() == p::Input::UnverifiedRDG,
            "recognized Development RDG input never falls back to interpreting the graph as a command list");
        memory = before;
        expect(!p::discover_development(memory.view(), memory.base, 0x4000, render,
            [&](uintptr_t cursor, size_t, const char*) -> std::optional<uintptr_t> {
                return cursor <= builder ? std::optional<uintptr_t>{builder} :
                    cursor <= other ? std::optional<uintptr_t>{other} : std::nullopt;
            }, development_size), "ambiguous Development constructor ownership rejects");
        size_t candidates{};
        expect(!p::discover_development(memory.view(), memory.base, 0x4000, render,
            [&](uintptr_t, size_t, const char*) { ++candidates; return std::optional<uintptr_t>{render+candidates}; }, development_size) &&
            candidates == 4, "Development discovery also has a strict bounded candidate count");
        memory = saved;
    }
    const auto mutate = [&](uintptr_t address, const auto& code, const auto& mask, auto&& check) {
        for (size_t i = 0; i < code.size(); ++i) {
            memory.bytes[address - memory.base + i] ^= 1;
            expect(check() == (mask[i] == 0), "only image-relative operands are ignored; ABI and control-flow changes reject");
            memory.bytes[address - memory.base + i] ^= 1;
        }
    };
    mutate(render, p::contract::render_texture_code, p::contract::render_texture_mask,
        [&] { return discover(unique); });
    mutate(builder, p::contract::builder_prefix_code, p::contract::builder_prefix_mask,
        [&] { return discover(unique); });
    memory.readable = false;
    expect(!discover(unique), "unreadable original code rejects");
    memory.readable = true;
    memory.executable = false;
    expect(!discover(unique), "non-executable original code rejects");
    memory.executable = true;
    expect(!p::builder_layout(memory.view(), memory.base, 0x4000, builder,
        [](uintptr_t) { return p::builder_function_size - 1; }), "a changed constructor unwind extent rejects");
    expect(!p::render_texture_layout(memory.view(), memory.base, 0x4000, render + 1, size),
        "an interior callback address cannot authorize a layout");
    expect(!discover([&](uintptr_t cursor, size_t, const char*) -> std::optional<uintptr_t> {
        return cursor <= builder ? std::optional<uintptr_t>{builder} :
            cursor <= other ? std::optional<uintptr_t>{other} : std::nullopt;
    }), "ambiguous original constructors reject");
    size_t calls{};
    expect(!discover([&](uintptr_t, size_t, const char*) { ++calls; return std::optional<uintptr_t>{render + calls}; }) && calls == 4,
        "discovery has a strict candidate bound even when every candidate is invalid");
    expect(!discover([&](uintptr_t cursor, size_t, const char*) { return std::optional<uintptr_t>{cursor - 1}; }),
        "a regressing scanner result rejects");
    expect(!discover([&](uintptr_t, size_t, const char*) { return std::optional<uintptr_t>{memory.base + 0x4000}; }),
        "an out-of-module scanner result rejects");
    expect(!p::contains((std::numeric_limits<uintptr_t>::max)() - 7, 16,
        (std::numeric_limits<uintptr_t>::max)() - 7, 1), "overflowing module bounds reject");

    const auto graph = memory.base + 0x4000, immediate = memory.base + 0x5000;
    const auto field = graph + p::command_list_offset;
    memory.put(field, immediate);
    memory.put(graph + 0x28, builder);
    memory.put(immediate + 0x28, uintptr_t{0});
    const auto unwrap = [&](uintptr_t input = 0) {
        return p::command_list(memory.view(), p::Input::VerifiedRDG, input ? input : graph);
    };
    expect(unwrap() == immediate, "RDG +0xc0 supplies the immediate list, not RDG allocator +0x28");
    expect(unwrap() == immediate, "an empty RHI Root is left for the unchanged worker checks, not guessed from RDG memory");
    memory.reads = 0;
    expect(p::command_list(memory.view(), p::Input::LegacyRHI, graph) == graph && memory.reads == 0,
        "ordinary legacy callbacks are unchanged and perform no additional memory reads");
    expect(p::command_list(memory.view(), p::Input::UnverifiedRDG, graph) == 0 && memory.reads == 0,
        "an unverified RDG argument never reaches the worker as a command list");
    for (const auto invalid : {uintptr_t{0}, uintptr_t{1}, graph, immediate + 1, memory.base + memory.bytes.size()}) {
        memory.put(field, invalid);
        expect(unwrap() == 0, "null, low, self, unaligned and unreadable command-list references reject");
    }
    memory.put(field, immediate);
    memory.readable = false;
    expect(unwrap() == 0, "each callback revalidates the borrowed reference without retaining it across frames");
    memory.readable = true;
    for (const auto invalid : {uintptr_t{0}, uintptr_t{1}, graph + 1, (std::numeric_limits<uintptr_t>::max)() - 7}) {
        expect(p::command_list(memory.view(), p::Input::VerifiedRDG, invalid) == 0, "invalid or overflowing RDG inputs reject");
    }
}
} // namespace paradoxdrive_rhi_tests
