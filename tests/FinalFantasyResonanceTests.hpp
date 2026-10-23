#pragma once

#include <fstream>
#include <sdk/FinalFantasyResonance.hpp>

namespace ffrs_tests {
struct Memory {
    static constexpr uintptr_t base = 0x100000;
    static constexpr uintptr_t viewport = base + 0x3000, viewport_table = base + 0x3400;
    static constexpr uintptr_t rhi = base + 0x4000, rhi_table = base + 0x4400, wrapper = base + 0x4600, native = base + 0x4800;
    static constexpr uintptr_t background = base + 0x5000, background_table = base + 0x5400;
    static constexpr uintptr_t proxy = base + 0x6000, proxy_table = base + 0x6400, proxy_viewport = base + 0x6800, storage = base + 0x6a00;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x8000);
    bool readable{true}, executable{true};
    uintptr_t blocked{};
    size_t legacy_reads{};
    template<class T> void put(uintptr_t p, const T& value) { std::memcpy(bytes.data() + p - base, &value, sizeof(value)); }
    Memory() {
        namespace f = sdk::ffrs;
        put(base + 0x100, f::viewport_code); put(base + 0x200, f::gamma_code);
        put(base + 0x300, std::array<uint8_t, 5>{0x48,0x8d,0x41,0x20,0xc3}); put(base + 0x400, f::native_code);
        put(base + 0x500, f::proxy_code);
        put(base + 0x600, uint8_t{0xe9}); put(base + 0x601, int32_t{-0x105});
        put(viewport, viewport_table); put(viewport_table + 16, base + 0x100); put(viewport_table + 48, base + 0x200);
        put(viewport + 8, rhi); put(viewport + 0x2d8, background); put(viewport + 0x2e0, background);
        put(rhi, rhi_table); put(rhi_table + 16, base + 0x300); put(rhi_table + 40, base + 0x400);
        put(rhi_table + 56, base + 0x900); put(base + 0x900, std::array<uint8_t, 4>{0x48,0x8b,0xc1,0xc3});
        put(rhi + 0xb8, wrapper); put(wrapper + 0x20, native);
        // The actual crash called the FBackgroundView deleting destructor in slot 13.
        put(background, background_table); put(background_table + 13 * 8, base + 0xa00);
        put(base + 0xa00, std::array<uint8_t, 2>{0x0f,0x0b});
        put(proxy, proxy_table); put(proxy_table + 16, base + 0x300); put(proxy_table + 40, base + 0x400);
        put(proxy_table + 56, base + 0x600); put(proxy + 0x240, proxy_viewport);
        put(proxy_viewport + 0x98, storage); put(storage, rhi);
    }
    sdk::discovery::Memory view() {
        return {this, [](void* context, uintptr_t p, void* out, size_t n) {
            auto& m = *static_cast<Memory*>(context);
            if (!m.readable || p < base || p - base > m.bytes.size() || n > m.bytes.size() - (p - base)) { return false; }
            if (p <= viewport + 0x2e0 && viewport + 0x2e0 - p < n) { ++m.legacy_reads; }
            if (m.blocked && p <= m.blocked && m.blocked - p < n) { return false; }
            std::memcpy(out, m.bytes.data() + p - base, n); return true;
        }, [](void* context, uintptr_t p, size_t n) {
            const auto& m = *static_cast<Memory*>(context);
            return m.executable && p >= base && p - base <= 0x2000 && n <= 0x2000 - (p - base);
        }};
    }
};
}

void test_ffrs_texture_selection() {
    namespace f = sdk::ffrs;
    using M = ffrs_tests::Memory;
    expect(f::matches_executable(L"D:/Game/FFRS-Win64-Shipping.exe") &&
        f::matches_executable(L"d:\\GAME\\ffrs-WIN64-shipping.EXE"), "FFRS gate ignores case and misleading file-version metadata");
    for (auto p : {L"Other.exe", L"xFFRS-Win64-Shipping.exe", L"FFRS-Win64-Shipping.exe.bak", L"D:/FFRS-Win64-Shipping.exe/Other.exe", L""}) {
        expect(!f::matches_executable(p), "only the exact FFRS basename enables its ABI guard");
    }
    expect(f::completed_game_draw("UGameViewportClient::Draw post"), "FFRS observes only completed game Draw");
    for (auto p : {"", "UGameViewportClient::Draw viewport", "FSceneViewFamily::RenderTarget", "BeginRenderingViewFamily RenderTarget"}) {
        expect(!f::completed_game_draw(p), "render-thread and pre-Draw targets cannot become FFRS scene sources");
    }

    M m;
    const auto original = m;
    expect(f::completed_draw_texture(m.view(), M::viewport) == M::rhi &&
        f::native_resource(m.view(), M::rhi) == M::native, "FFRS selects the real +8 texture and its b8/20 native resource");
    expect(m.bytes == original.bytes && m.legacy_reads == 0, "FFRS selection never reads +2e0 or writes any game memory");
    expect(!f::texture(m.view(), M::background) && !f::native_resource(m.view(), M::background),
        "FBackgroundView cannot reach a destructive virtual probe");
    m.put(M::viewport + 8, M::background);
    expect(!f::completed_draw_texture(m.view(), M::viewport), "a module-owned vtable alone is not a texture type proof");
    m = original; m.put(M::viewport + 8, uintptr_t{});
    expect(!f::completed_draw_texture(m.view(), M::viewport), "no stale RT-field or legacy-field fallback when GT target is absent");

    const auto corrupt = [&](uintptr_t code, const auto& mask, bool native) {
        for (size_t i = 0; i < mask.size(); ++i) {
            m = original; m.bytes[code - M::base + i] ^= 1;
            const auto accepted = native ? f::native_resource(m.view(), M::rhi).has_value()
                : f::completed_draw_texture(m.view(), M::viewport).has_value();
            expect(accepted == (mask[i] == 0), "every unmasked accessor byte is required; only external displacements may vary");
        }
        m = original; m.blocked = code + mask.size() - 1;
        expect(native ? !f::native_resource(m.view(), M::rhi) : !f::completed_draw_texture(m.view(), M::viewport),
            "truncated accessor bodies fail closed");
    };
    corrupt(M::base + 0x100, f::viewport_code_mask, false);
    corrupt(M::base + 0x200, f::gamma_code_mask, false);
    corrupt(M::base + 0x400, f::native_code_mask, true);
    constexpr std::array<uint8_t, 5> descriptor_mask{0xff,0xff,0xff,0xff,0xff};
    corrupt(M::base + 0x300, descriptor_mask, true);

    constexpr uintptr_t hook = M::base + 0x800, external_hook = M::base + 0x810;
    m = original; m.put(M::viewport_table + 16, hook);
    expect(!f::completed_draw_texture(m.view(), M::viewport), "unknown viewport replacement never inherits a cached slot proof");
    expect(f::completed_draw_texture(m.view(), M::viewport, hook, M::base + 0x100) == M::rhi,
        "only our own viewport hook can use its validated original accessor");
    expect(!f::completed_draw_texture(m.view(), M::viewport, hook, 0) &&
        !f::completed_draw_texture(m.view(), M::viewport, external_hook, M::base + 0x100) &&
        !f::completed_draw_texture(m.view(), M::viewport, hook, M::base + 0x200), "missing, unrelated or wrong-ABI original accessor rejects");

    for (auto p : {uintptr_t{}, uintptr_t{0xffff}, uintptr_t{0x101001}, uintptr_t{0x800000000000}, UINTPTR_MAX}) {
        m = original;
        expect(!f::completed_draw_texture(m.view(), p) && !f::native_resource(m.view(), p), "invalid object pointers reject before dereference");
        for (auto slot : {M::viewport, M::viewport + 8, M::rhi, M::rhi + 0xb8, M::wrapper + 0x20}) {
            m = original; m.put(slot, p);
            expect(!f::native_resource(m.view(), M::rhi) || (slot == M::viewport || slot == M::viewport + 8),
                "invalid texture table/location/native pointers cannot produce a native resource");
            if (slot == M::viewport || slot == M::viewport + 8 || slot == M::rhi) {
                expect(!f::completed_draw_texture(m.view(), M::viewport), "invalid viewport/texture pointers cannot be adopted");
            }
        }
    }
    for (auto p : {M::viewport, M::viewport + 8, M::viewport_table + 16, M::viewport_table + 48, M::rhi}) {
        m = original; m.blocked = p;
        expect(!f::completed_draw_texture(m.view(), M::viewport), "each viewport/type-chain read is checked");
    }
    for (auto p : {M::rhi_table + 16, M::rhi_table + 40, M::rhi + 0xb8, M::wrapper + 0x20}) {
        m = original; m.blocked = p;
        expect(!f::native_resource(m.view(), M::rhi), "each direct native-resource read is checked");
    }
    m = original; m.readable = false;
    expect(!f::completed_draw_texture(m.view(), M::viewport) && !f::native_resource(m.view(), M::rhi), "unreadable memory rejects");
    m = original; m.executable = false;
    expect(!f::completed_draw_texture(m.view(), M::viewport) && !f::native_resource(m.view(), M::rhi), "non-executable accessors reject");
    m = original; m.put(M::rhi_table + 40, M::base + 0x7000); m.put(M::base + 0x7000, f::native_code);
    expect(!f::native_resource(m.view(), M::rhi), "data bytes that look like a getter are not executable-image proof");

    m = original;
    expect(f::native_resource(m.view(), M::proxy) == M::native, "the validated backbuffer proxy follows only its bounded pointer chain");
    m.put(M::proxy_table + 56, M::base + 0x500);
    expect(f::native_resource(m.view(), M::proxy) == M::native, "direct and E9-thunked proxy accessors are supported without calls");
    const auto direct_proxy = m;
    for (size_t i = 0; i < f::proxy_code.size(); ++i) {
        m = direct_proxy; m.bytes[0x500 + i] ^= 1;
        expect(!f::native_resource(m.view(), M::proxy), "every proxy pointer-chain instruction is required");
    }
    for (auto p : {M::proxy_table + 56, M::proxy + 0x240, M::proxy_viewport + 0x98, M::storage}) {
        m = original; m.blocked = p;
        expect(!f::native_resource(m.view(), M::proxy), "proxy chain unreadability never invokes a fallback virtual");
        for (auto invalid : {uintptr_t{}, uintptr_t{1}, uintptr_t{0x800000000000}, UINTPTR_MAX}) {
            m = original; m.put(p, invalid);
            expect(!f::native_resource(m.view(), M::proxy), "invalid proxy chain pointers fail closed");
        }
    }
    m = original; m.put(M::storage, M::proxy);
    expect(!f::native_resource(m.view(), M::proxy), "a cyclic proxy cannot recurse");
    m = original; m.put(M::rhi + 0xb8, uintptr_t{});
    expect(!f::native_resource(m.view(), M::proxy), "a second unavailable native location cannot recurse or execute the getter");
    m = original; m.put(M::base + 0x601, INT32_MAX);
    expect(!f::native_resource(m.view(), M::proxy), "an out-of-range proxy thunk target rejects");
    m = original; m.put(M::base + 0x600, uint8_t{0xe8});
    expect(!f::native_resource(m.view(), M::proxy), "a call is not a supported proxy thunk");
    m = original; m.blocked = M::base + 0x604;
    expect(!f::native_resource(m.view(), M::proxy), "a truncated thunk cannot authorize a proxy");
}

// Optional read-only check against the exact shipping PE. No code is mapped or executed.
void test_ffrs_image(const char* path) {
    struct Image {
        std::ifstream file;
        uint64_t size{}, base{};
        struct Section { uint32_t rva, raw_size, raw, flags; };
        std::vector<Section> sections;
        std::optional<uint64_t> offset(uintptr_t address, size_t n, bool code) const {
            if (address < base) { return {}; }
            const auto rva = address - base;
            for (const auto& s : sections) {
                if (rva < s.rva || rva - s.rva > s.raw_size || n > s.raw_size - (rva - s.rva) ||
                    (code && !(s.flags & 0x20000000))) { continue; }
                const auto offset = uint64_t(s.raw) + rva - s.rva;
                if (offset <= size && n <= size - offset) { return offset; }
            }
            return {};
        }
    } image;
    image.file.open(path, std::ios::binary | std::ios::ate);
    if (!image.file) { expect(false, "open FFRS PE fixture"); return; }
    image.size = static_cast<uint64_t>(image.file.tellg());
    std::array<uint8_t, 65536> header{};
    image.file.seekg(0);
    if (!image.file.read(reinterpret_cast<char*>(header.data()), header.size())) { expect(false, "read FFRS PE header"); return; }
    const auto u16 = [&](size_t o) { uint16_t v{}; if (o <= header.size() - 2) { std::memcpy(&v, header.data() + o, 2); } return v; };
    const auto u32 = [&](size_t o) { uint32_t v{}; if (o <= header.size() - 4) { std::memcpy(&v, header.data() + o, 4); } return v; };
    const auto nt = u32(0x3c);
    if (nt > 4096 || u16(0) != 0x5a4d || u32(nt) != 0x4550 || u16(nt + 4) != 0x8664 || u16(nt + 24) != 0x20b) {
        expect(false, "validate FFRS x64 PE fixture"); return;
    }
    const auto count = u16(nt + 6), opt_size = u16(nt + 20);
    const auto section_start = nt + 24 + opt_size;
    if (opt_size < 112 || !count || count > 96 || uint64_t(section_start) + uint64_t(count) * 40 > header.size()) {
        expect(false, "validate FFRS section table"); return;
    }
    std::memcpy(&image.base, header.data() + nt + 24 + 24, sizeof(image.base));
    for (size_t i = 0; i < count; ++i) {
        const auto o = section_start + i * 40;
        image.sections.push_back({u32(o + 12), u32(o + 16), u32(o + 20), u32(o + 36)});
    }
    sdk::discovery::Memory memory{&image, [](void* context, uintptr_t p, void* out, size_t n) {
        auto& image = *static_cast<Image*>(context);
        const auto offset = image.offset(p, n, false);
        if (!offset) { return false; }
        image.file.clear(); image.file.seekg(*offset);
        return static_cast<bool>(image.file.read(static_cast<char*>(out), n));
    }, [](void* context, uintptr_t p, size_t n) { return static_cast<Image*>(context)->offset(p, n, true).has_value(); }};
    namespace f = sdk::ffrs;
    expect(image.base == 0x140000000 && image.size == 495708416, "FFRS evidence matches the analyzed shipping image");
    expect(f::viewport_accessor(memory, 0x14829bfe8) && f::gamma_accessor(memory, 0x14829bfe8), "actual FFRS viewport/gamma contract");
    expect(f::native_accessor(memory, 0x147c4bad0) && f::native_accessor(memory, 0x147c4f698), "actual texture and backbuffer-proxy tables are recognized");
    expect(!f::native_accessor(memory, 0x147e70b38), "the actual FBackgroundView table that caused the crash is rejected");
    expect(f::code_matches(memory, 0x142e2a360, f::proxy_code, f::proxy_code_mask), "actual proxy getter is the proven read-only chain");
    std::array<uint8_t, 5> jump{};
    int32_t displacement{};
    const auto read_jump = memory.load(uintptr_t{0x142e51df0}, jump);
    std::memcpy(&displacement, jump.data() + 1, sizeof(displacement));
    expect(read_jump && jump[0] == 0xe9 && sdk::discovery::relative_address(0x142e51df5, displacement) == 0x142e2a360,
        "actual proxy thunk reaches the validated getter");
}
