#pragma once

#include <fstream>
#include "mods/vr/GalacticRacerRenderTargets.hpp"
#include "mods/vr/GalacticRacerOwnedTexture.hpp"
#include "mods/vr/GalacticRacerNativeFix.hpp"
#include "mods/vr/GalacticRacerBink.hpp"
#include "mods/vr/GalacticRacerBinkSeek.hpp"

namespace swgr_tests {
struct Memory {
    uintptr_t base{0x100000};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x7000);
    bool readable{true}, executable{true};
    template<class T> void put(uintptr_t p, const T& value) {
        std::memcpy(bytes.data() + p - base, &value, sizeof(value));
    }
    sdk::discovery::Memory view() {
        return {this, [](void* context, uintptr_t p, void* out, size_t n) {
            auto& m = *static_cast<Memory*>(context);
            if (!m.readable || p < m.base || p - m.base > m.bytes.size() || n > m.bytes.size() - (p - m.base)) { return false; }
            std::memcpy(out, m.bytes.data() + p - m.base, n); return true;
        }, [](void* context, uintptr_t p, size_t n) {
            auto& m = *static_cast<Memory*>(context);
            return m.executable && p >= m.base && p - m.base <= 0x2000 && n <= 0x2000 - (p - m.base);
        }};
    }
};
}

void test_swgr_render_targets() {
    namespace s = sdk::galactic_racer;
    namespace u = uevr::swgr;
    expect(s::runtime(L"D:/Game/SWGR-Win64-Shipping.exe", 0x50007, 0x40000, true), "SWGR exact title/engine/RHI gate");
    for (auto path : {L"SWGR-Win64-Shipping.exe.bak", L"Other-Win64-Shipping.exe", L"xSWGR-Win64-Shipping.exe", L"D:/SWGR-Win64-Shipping.exe/Other.exe"}) {
        expect(!s::runtime(path, 0x50007, 0x40000, true), "other executables do not use SWGR paths");
    }
    for (auto version : {0x50005u, 0x50006u, 0x50008u}) {
        expect(!s::runtime(L"SWGR-Win64-Shipping.exe", version, 0x40000, true), "other engine families unchanged");
    }
    expect(!s::runtime(L"SWGR-Win64-Shipping.exe", 0x50007, 0x30000, true), "unvalidated SWGR patch unchanged");
    expect(!s::runtime(L"SWGR-Win64-Shipping.exe", 0x50007, 0x40000, false), "DX11 does not select DX12 adoption");
    expect(s::runtime(L"d:\\GAME\\swgr-WIN64-shipping.EXE", 0x50007, 0x40000, true), "case-insensitive executable gate");
    expect(!u::requires_scene_conversion(87) && !u::requires_scene_conversion(90), "BGRA family permits a compatible scene copy");
    for (auto format : {23u, 24u, 27u, 28u, 9u, 10u}) {
        expect(u::requires_scene_conversion(format), "R10/RGBA/FP16 require conversion, never an incompatible XR copy");
        expect(u::valid_scene({4944, 2416, 3, format, 1, 1, 0, 1, 1}, 4944, 2416), "validated color contract");
    }
    for (auto format : {0u, 40u, 45u}) {
        expect(!u::valid_scene({4944, 2416, 3, format, 1, 1, 0, 1, 1}, 4944, 2416), "noncolor resources rejected");
    }
    expect(!u::valid_scene({2472, 2416, 3, 24, 1, 1, 0, 1, 1}, 4944, 2416), "single eye is not a packed viewport");
    expect(!u::valid_scene({4944, 2416, 3, 24, 9, 1, 0, 1, 1}, 4944, 2416), "shader-inaccessible scene rejected");
    expect(!u::valid_scene({4944, 2416, 3, 24, 1, 2, 0, 1, 1}, 4944, 2416), "MSAA scene rejected");

    swgr_tests::Memory m;
    const auto b = m.base, viewport = b + 0x3100, table = b + 0x3200, texture = b + 0x3400;
    const auto texture_table = b + 0x3600, wrapper = b + 0x3700, native = b + 0x3800;
    m.put(viewport, table); m.put(table + 16, b + 0x100); m.put(b + 0x100, s::viewport_getter_code);
    m.put(viewport + 8, texture); m.put(viewport + 0x2d8, b + 0x3998);
    m.put(texture, texture_table); m.put(texture_table + 40, b + 0x200); m.put(b + 0x200, s::native_getter_code);
    m.put(texture + 0xd0, wrapper); m.put(wrapper + 0x20, native);
    m.put(table + 48, b + 0x300); m.put(b + 0x300, s::viewport_gamma_code);
    expect(s::viewport_texture(m.view(), viewport) == texture, "completed Draw reads the GT ref, never RT storage");
    expect(s::native_resource(m.view(), texture) == native, "read-only direct native path; no virtual execution");
    expect(s::viewport_gamma_accessor(m.view(), table), "viewport gamma slot 6 has the actual float ABI");
    uintptr_t observed_table{};
    expect(s::native_resource(m.view(), texture, 0, &observed_table) == native && observed_table == texture_table,
        "publish only the vtable used by the validated native read");
    expect(s::native_resource(m.view(), texture, observed_table) == native, "same validated native vtable can be reused");
    auto original = m;
    for (size_t i = 0; i < s::viewport_getter_code.size(); ++i) {
        if (!s::viewport_getter_mask[i]) { continue; }
        m = original; m.bytes[0x100 + i] ^= 1;
        expect(!s::viewport_texture(m.view(), viewport), "mutated viewport accessor rejects adoption");
    }
    for (size_t i = 0; i < s::native_getter_code.size(); ++i) {
        m = original; m.bytes[0x200 + i] ^= 1;
        expect(!s::native_resource(m.view(), texture), "mutated native getter rejects wrapper access");
    }
    for (size_t i = 0; i < s::viewport_gamma_code.size(); ++i) {
        if (!s::viewport_gamma_mask[i]) { continue; }
        m = original; m.bytes[0x300 + i] ^= 1;
        expect(!s::viewport_gamma_accessor(m.view(), table), "mutated gamma ABI rejected without probing a virtual");
    }
    m = original; m.put(table + 48, b + 0x200);
    expect(!s::viewport_gamma_accessor(m.view(), table), "native getter is not callable as a gamma getter");
    m = original; m.put(texture, table);
    observed_table = 0;
    expect(!s::native_resource(m.view(), texture, texture_table, &observed_table) && observed_table == 0,
        "changed native vtable must validate again, and failure publishes no cache entry");
    m = original; m.put(texture + 0xd0, uintptr_t{});
    expect(!s::native_resource(m.view(), texture), "uninitialized direct resource does not invoke fallback getter");
    m = original; m.put(wrapper + 0x20, uintptr_t{0xffffffffffffffff});
    expect(!s::native_resource(m.view(), texture), "noncanonical native pointer rejected");
    m = original; m.readable = false;
    expect(!s::viewport_texture(m.view(), viewport) && !s::native_resource(m.view(), texture), "unreadable storage fails closed");
    m = original; m.executable = false;
    expect(!s::viewport_texture(m.view(), viewport) && !s::native_resource(m.view(), texture), "nonexecutable accessors rejected");

    m = original;
    const auto begin = b + 0x500, join = begin + 0x4c2, end = begin + 0x2000;
    m.put(begin, u::slate_entry_code); m.put(begin + 0x1e5, u::slate_array_code);
    m.put(begin + 0x2b5, u::slate_inputs_code); m.put(join - 0x1a, u::slate_select_code);
    m.put(join, u::slate_hash_code); m.put(join + 0xbe, u::slate_register_code);
    m.put(join, u::slate_registration_code);
    m.put(join + u::slate_registration_code.size(), u::slate_restore_code);
    m.put(begin + 0x1a6, u::slate_graph_code);
    const auto name = b + 0x3a00, graph = b + 0x1200;
    m.put(join + 0xcf, int32_t(name - (join + 0xd3)));
    m.put(begin + 0x1b3, int32_t(graph - (begin + 0x1b7)));
    m.put(name, std::array<wchar_t, 19>{L'S',L'l',L'a',L't',L'e',L'O',L'u',L't',L'p',L'u',L't',L'T',L'e',L'x',L't',L'u',L'r',L'e',0});
    m.put(graph + 0x1d, std::array<uint8_t, 6>{0x49,0x89,0xd6,0x48,0x89,0xce});
    m.put(graph + 0x118, std::array<uint8_t, 7>{0x4c,0x89,0xb6,0xc0,0,0,0});
    expect(u::slate_join(m.view(), join, begin, end), "inlined Slate contract includes raw output hash and graph cmdlist ownership");
    auto slate = m;
    const auto mutate = [&](uintptr_t p) { m = slate; m.bytes[p - b] ^= 1; expect(!u::slate_join(m.view(), join, begin, end), "changed Slate contract preserves engine output"); };
    for (const auto p : {begin, begin + 0x1e5, begin + 0x2b5, join - 0x1a, join, join + 0xbe, name,
            graph + 0x1d, graph + 0x118}) { mutate(p); }
    expect(!u::slate_join(slate.view(), join, begin - 1, end), "small array-growth helper cannot own Slate hook");
    for (size_t i = 0; i < u::slate_registration_code.size(); ++i) {
        if (!u::slate_registration_mask[i]) { continue; }
        mutate(join + i);
    }
    for (size_t i = 0; i < u::slate_restore_code.size(); ++i) { mutate(join + u::slate_registration_code.size() + i); }
    m = slate; m.put(join + 0xcf, int32_t{INT32_MAX});
    expect(!u::slate_join(m.view(), join, begin, end), "bad anchor displacement fails closed");

    m = original;
    const auto inputs = b + 0x4000, info = b + 0x4200, stack = b + 0x4300, commands = b + 0x5000;
    u::SlateInputs in{b + 0x5100, b + 0x5200, b + 0x5300, info, 5, 9, 0, 0, 2560, 1440, 1.0f, 0};
    m.put(inputs, in); m.put(stack + 0x250, commands); m.put(stack + 0x7c, uint8_t{1});
    const auto object = [&](uintptr_t p) { return p == in.renderer || p == in.window || p == info; };
    auto frame = u::slate_frame(m.view(), inputs, info, stack, object);
    expect(frame && frame->width == 2560 && frame->height == 1440 && frame->command_list == commands,
        "UI dimensions come from current Slate rect, not a fixed desktop size");
    expect(!u::slate_frame(m.view(), inputs, info + 8, stack, object), "wrong window input cannot redirect UI");
    for (const auto scale : {0.0f, -1.0f, 9.0f, (std::numeric_limits<float>::quiet_NaN)()}) {
        auto invalid = in; invalid.scale = scale; m.put(inputs, invalid);
        expect(!u::slate_frame(m.view(), inputs, info, stack, object), "bad Slate scale rejected");
    }
    for (const auto extent : {0, -1, 16385}) {
        auto invalid = in; invalid.max_x = extent; m.put(inputs, invalid);
        expect(!u::slate_frame(m.view(), inputs, info, stack, object), "bad UI extent rejected");
    }
    m.put(inputs, in); m.put(stack + 0x250, stack + 0x190);
    expect(!u::slate_frame(m.view(), inputs, info, stack, object), "GraphBuilder is never passed as a command list");
    m.put(stack + 0x250, b + 0x10000);
    expect(!u::slate_frame(m.view(), inputs, info, stack, object), "unreadable command list rejected");
    m.put(stack + 0x250, commands); m.put(stack + 0x7c, uint8_t{0});
    expect(!u::slate_frame(m.view(), inputs, info, stack, object), "nonstereo output retains its original Present resource");

    u::SlateOutputTransaction tx;
    uintptr_t output = texture;
    expect(tx.begin(stack, inputs, output, native), "exact window arms one bounded substitution");
    output = native;
    expect(!tx.begin(stack + 8, inputs, texture, wrapper), "recursive Slate cannot overwrite the outer restoration");
    expect(!tx.complete(stack + 8, inputs, output) && output == native, "another stack cannot restore the transaction");
    expect(!tx.complete(stack, inputs + 8, output) && output == native, "another window cannot restore the transaction");
    expect(tx.complete(stack, inputs, output) && output == texture && !tx.active(), "raw scene output is restored before presentation");
    expect(!tx.complete(stack, inputs, output), "nonredirected Slate invocation changes nothing");
    expect(!tx.begin(stack, inputs, texture, texture) && !tx.begin(stack, inputs, 0, native), "aliased or missing scene cannot arm");
}

void test_swgr_memory_image(const char* path) {
    struct FileMemory {
        std::ifstream file;
        uint64_t size{};
        explicit FileMemory(const char* p) : file(p, std::ios::binary | std::ios::ate) {
            if (file) { size = static_cast<uint64_t>(file.tellg()); }
        }
    } fixture{path};
    expect(fixture.file && fixture.size == 0x1d61f000, "SWGR runtime-image fixture is complete");
    if (!fixture.file || fixture.size != 0x1d61f000) { return; }
    constexpr uintptr_t base = 0x140000000;
    sdk::discovery::Memory memory{&fixture, [](void* context, uintptr_t p, void* out, size_t n) {
        auto& f = *static_cast<FileMemory*>(context);
        if (p < base || p - base > f.size || n > f.size - (p - base)) { return false; }
        f.file.clear(); f.file.seekg(p - base);
        return static_cast<bool>(f.file.read(static_cast<char*>(out), n));
    }, [](void*, uintptr_t p, size_t n) {
        // Executable ranges from this fixture's PE section table, not guessed .text names.
        return p >= base + 0x1000 && p < base + 0x834f000 && n <= base + 0x834f000 - p;
    }};
    expect(sdk::galactic_racer::viewport_accessor(memory, 0x149275d00), "actual FViewport getter contract");
    expect(sdk::galactic_racer::viewport_gamma_accessor(memory, 0x149275d00), "actual viewport gamma contract");
    expect(sdk::galactic_racer::native_accessor(memory, 0x148ddfd50), "actual FD3D12Texture direct resource contract");
    const auto owned_accessor = [&](uintptr_t table, uint32_t slot, const auto& code) {
        uintptr_t fn{};
        auto actual = code;
        return memory.load(table + slot * sizeof(uintptr_t), fn) &&
            memory.executable(memory.context, fn, actual.size()) && memory.load(fn, actual) && actual == code;
    };
    expect(owned_accessor(0x1492a3530, 6, uevr::swgr_owned::size_x_code), "actual owned resource width contract");
    expect(owned_accessor(0x1492a3530, 7, uevr::swgr_owned::size_y_code), "actual owned resource height contract");
    expect(owned_accessor(0x1492a3600, 2, uevr::swgr_owned::texture_code), "actual owned FRenderTarget accessor contract");
    expect(uevr::swgr::slate_join(memory, 0x14283e160, 0x14283dc9e, 0x14283ff84), "actual inlined Slate registration/restoration contract");
    expect(!uevr::swgr::slate_join(memory, 0x14283e160, 0x14283dc1f, 0x14283dc9e), "actual array helper cannot be selected as DrawWindow");

    std::array<uint32_t, 2> directory{};
    uint32_t pe_offset{};
    expect(memory.load(base + 0x3c, pe_offset) &&
        memory.load(base + pe_offset + 24 + 112 + 3 * 8, directory), "fixture exposes native exception table");
    std::vector<std::array<uint32_t, 3>> functions;
    if (directory[1] && directory[1] <= 8 * 1024 * 1024 && directory[1] % 12 == 0) {
        functions.resize(directory[1] / 12);
        expect(memory.read(memory.context, base + directory[0], functions.data(), directory[1]), "read fixture unwind boundaries");
    }
    const auto function_size = [&](uintptr_t p) -> size_t {
        if (p < base || p - base > UINT32_MAX) { return 0; }
        const auto rva = static_cast<uint32_t>(p - base);
        const auto it = std::lower_bound(functions.begin(), functions.end(), rva,
            [](const auto& entry, uint32_t address) { return entry[0] < address; });
        return it != functions.end() && (*it)[0] == rva && (*it)[1] > rva ? (*it)[1] - rva : 0;
    };
    namespace s = uevr::swgr_native;
    expect(uevr::swgr_bink::seek_contract(memory, {0x146cbd900, 0x142d71e0a, 0x143dd9990},
        base, fixture.size, function_size), "actual Bink seek budget, overlay continuation and decoder-completion contracts");
    expect(uevr::swgr_bink::overlay_contract(memory, 0x140380236, function_size(0x140380236), base, fixture.size),
        "actual Bink viewport callback/packet/consumer contract; no injected execution");
    const auto family = s::family_functions(memory, 0x14384d17c, function_size(0x14384d17c), base, fixture.size, function_size);
    expect(family && family->table == 0x149269380 && family->scalar_destructor == 0x146393686,
        "actual SWGR copy/destructor contract is accepted without executing either function");
    expect(s::view_layout(memory, 0x14384eaac, function_size(0x14384eaac), base, fixture.size),
        "actual SWGR constructor proves the offsets used for singleton/rectangle adaptation");
    expect(!s::family_functions(memory, 0x14384cd86, function_size(0x14384cd86), base, fixture.size, function_size),
        "actual ConstructionValues constructor is not mistaken for a copy");
}
