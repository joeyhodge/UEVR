#pragma once

#include <sdk/FinalFantasyResonanceVersion.hpp>
#include <sdk/ShippingSceneViewLayout.hpp>

void test_shipping_scene_view_layouts() {
    namespace s = sdk::shipping_view;
    using M = ffrs_tests::Memory;
    constexpr uintptr_t constructor = M::base + 0x1000, options = M::base + 0x7000;
    for (const bool ffrs : {false, true}) {
        const std::wstring_view path = ffrs ? L"D:/FFRS-Win64-Shipping.exe" : L"D:/Hotel188-Win64-Shipping.exe";
        M m;
        const auto put_code = [&](auto& memory) {
            if (ffrs) {
                memory.put(constructor + 0x3f, s::ffrs_0); memory.put(constructor + 0x3a6, s::ffrs_1);
                memory.put(constructor + 0x493, s::ffrs_2); memory.put(constructor + 0x524, s::ffrs_3);
            } else {
                memory.put(constructor + 0x46, s::hotel188_0); memory.put(constructor + 0x1d8, s::hotel188_1);
                memory.put(constructor + 0x2cf, s::hotel188_2); memory.put(constructor + 0x365, s::hotel188_3);
            }
        };
        put_code(m);
        m.put(M::viewport_table, M::base + 0x100); m.put(M::rhi_table, M::base + 0x400);
        // The earlier projection-tail pair can look just as callable as the real objects.
        m.put(options + 0x130, M::viewport); m.put(options + 0x138, M::rhi);
        m.put(options + 0x158, M::viewport); m.put(options + 0x160, M::rhi);
        const uint32_t player = ffrs ? 0x180 : 0x170, pass = ffrs ? 0x1c0 : 0x1b0, world = pass + 8;
        m.put(options + player, int32_t{0});
        m.put(options + pass - 0x10, std::array<float, 4>{1, 1, 1, 1});
        m.put(options + pass, uint32_t{1}); m.put(options + world, 100.f);
        const auto original = m;
        const sdk::detail::InitOptionsLayout expected{0x158, 0x160, player, pass, world};
        expect(s::resolve(m.view(), path, constructor, options) == expected,
            "shipping constructor proof overrides misleading version/projection-pointer heuristics");
        expect(m.bytes == original.bytes, "layout discovery does not write colors, poses, or game objects");
        m.put(constructor, std::array<uint8_t, 14>{0xff, 0x25});
        expect(s::resolve(m.view(), path, constructor, options) == expected, "our constructor entry hook does not invalidate original field-copy evidence");

        const auto corrupt = [&](size_t offset, const auto& bytes) {
            for (size_t i = 0; i < bytes.size(); ++i) {
                m = original; m.bytes[constructor - M::base + offset + i] ^= 1;
                expect(!s::resolve(m.view(), path, constructor, options), "every constructor-contract instruction byte is mandatory");
            }
            m = original; m.blocked = constructor + offset + bytes.size() - 1;
            expect(!s::resolve(m.view(), path, constructor, options), "a truncated constructor contract cannot publish a layout");
        };
        if (ffrs) { corrupt(0x3f, s::ffrs_0); corrupt(0x3a6, s::ffrs_1); corrupt(0x493, s::ffrs_2); corrupt(0x524, s::ffrs_3); }
        else { corrupt(0x46, s::hotel188_0); corrupt(0x1d8, s::hotel188_1); corrupt(0x2cf, s::hotel188_2); corrupt(0x365, s::hotel188_3); }
        for (const auto offset : {0x158u, 0x160u, player, pass, world}) {
            m = original; m.blocked = options + offset;
            expect(!s::resolve(m.view(), path, constructor, options), "each live identity-field read must succeed");
        }
        for (const auto value : {uint32_t{3}, UINT32_MAX}) {
            m = original; m.put(options + pass, value);
            expect(!s::resolve(m.view(), path, constructor, options), "invalid stereo passes are not accepted as an eye");
        }
        for (const auto value : {-2, 256}) {
            m = original; m.put(options + player, value);
            expect(!s::resolve(m.view(), path, constructor, options), "invalid player identity fails closed");
        }
        for (const auto value : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            m = original; m.put(options + world, value);
            expect(!s::resolve(m.view(), path, constructor, options), "invalid world scale is not treated as layout proof");
        }
        for (const auto p : {uintptr_t{}, uintptr_t{1}, uintptr_t{0x800000000000}, UINTPTR_MAX}) {
            m = original;
            expect(!s::resolve(m.view(), path, p, options) && !s::resolve(m.view(), path, constructor, p), "invalid constructor/options addresses reject");
            m.put(options + 0x160, p);
            expect(!s::resolve(m.view(), path, constructor, options), "invalid state pointers cannot publish metadata");
        }
        m = original; m.executable = false;
        expect(!s::resolve(m.view(), path, constructor, options), "data copies alone are not executable constructor evidence");
        m = original;
        expect(!s::resolve(m.view(), L"Other.exe", constructor, options), "unrelated games keep their existing layout handling");
    }
    expect(s::hotel188(L"D:\\Game\\HOTEL188-WIN64-SHIPPING.EXE"), "Hotel188 identity is case-insensitive");
    for (const auto p : {L"Hotel188.exe", L"Hotel188-Win64-Shipping.exe.bak", L"xHotel188-Win64-Shipping.exe", L""}) {
        expect(!s::hotel188(p), "Hotel188 layout proof requires the exact basename");
    }
}

void test_ffrs_engine_version() {
    namespace f = sdk::ffrs;
    using M = ffrs_tests::Memory;
    constexpr uintptr_t getter = M::base + 0xb00, object = M::base + 0x7000;
    constexpr uintptr_t guard = M::base + 0x7080, cell = M::base + 0x7090, tls = M::base + 0x70a0, branch = M::base + 0x7100;
    const f::VersionImage image{M::base, 0x8000};
    M m;
    m.put(getter, f::version_getter_code);
    const auto relative = [&](size_t displacement, size_t next, uintptr_t target) {
        m.put(getter + displacement, static_cast<int32_t>(target - (getter + next)));
    };
    relative(6, 0xa, tls);
    for (auto pair : {std::pair{0x21u, 0x25u}, {0x36u, 0x3au}, {0x41u, 0x46u}, {0x72u, 0x76u}}) { relative(pair.first, pair.second, guard); }
    relative(0x2a, 0x2e, cell); relative(0x66, 0x6a, cell); relative(0x50, 0x54, object);
    for (auto pair : {std::pair{0x3bu, 0x3fu}, {0x58u, 0x5cu}, {0x5fu, 0x63u}, {0x6bu, 0x6fu}, {0x77u, 0x7bu}}) {
        relative(pair.first, pair.second, M::base + 0xc00);
    }
    m.put(guard, int32_t{-2147483647}); m.put(cell, object);
    m.put(object, std::array<uint16_t, 4>{5, 6, 1, 0}); m.put(object + 16, branch);
    m.put(object + 24, int32_t{4}); m.put(object + 28, int32_t{8});
    m.put(branch, std::array<char16_t, 4>{u'U', u'E', u'5', 0});
    const auto original = m;
    const auto source = f::version_source(m.view(), image, getter);
    expect(source && source->object == object && source->published == cell && source->guard == guard,
        "native version source is decoded from coherent relative operands, not a fixed RVA");
    if (!source) { return; }
    const std::array sources{*source};
    expect(f::agreed_version(m.view(), sources) == f::EngineVersion{5, 6, 1}, "initialized native engine version recovers misleading product metadata");
    expect(m.bytes == original.bytes, "engine version discovery never calls or writes to the game");
    for (size_t i = 0; i < f::version_getter_mask.size(); ++i) {
        if (!f::version_getter_mask[i]) { continue; }
        m = original; m.bytes[getter - M::base + i] ^= 1;
        expect(!f::version_source(m.view(), image, getter), "all unmasked native getter instructions are required");
    }
    for (auto offset : {0x21u, 0x36u, 0x41u, 0x72u, 0x2au, 0x66u}) {
        m = original; m.bytes[getter - M::base + offset] ^= 4;
        expect(!f::version_source(m.view(), image, getter), "incoherent publication/init-guard operands reject");
    }
    for (const auto offset : {6u, 0x50u, 0x3bu, 0x58u, 0x5fu, 0x6bu, 0x77u}) {
        m = original; m.put(getter + offset, INT32_MAX);
        expect(!f::version_source(m.view(), image, getter), "external data or helper references are not accepted as same-image proof");
    }
    for (const auto value : {0, -1, 1}) {
        m = original; m.put(guard, value);
        expect(!f::agreed_version(m.view(), sources), "uninitialized or in-progress native static objects are not used");
    }
    for (const auto p : {cell, guard, object, object + 16, branch, getter + f::version_getter_code.size() - 1}) {
        m = original; m.blocked = p;
        if (p >= getter && p < getter + f::version_getter_code.size()) {
            expect(!f::version_source(m.view(), image, getter), "unreadable native getter fails closed");
        } else { expect(!f::agreed_version(m.view(), sources), "every native object/branch read is checked"); }
    }
    for (const auto value : {uintptr_t{}, object + 8, uintptr_t{UINTPTR_MAX}}) {
        m = original; m.put(cell, value);
        expect(!f::agreed_version(m.view(), sources), "only the original getter's published object is trusted");
    }
    for (const auto offset : {0u, 2u, 4u}) {
        m = original; m.put(object + offset, uint16_t{7});
        expect(!f::agreed_version(m.view(), sources), "unverified engine versions cannot inherit the FFRS fallback");
    }
    for (const auto offset : {24u, 28u}) {
        for (const auto value : {-1, 0, 3, 65}) {
            m = original; m.put(object + offset, value);
            expect(!f::agreed_version(m.view(), sources), "invalid branch string extents reject");
        }
    }
    m = original; m.put(branch, std::array<char16_t, 4>{u'U', u'E', u'4', 0});
    expect(!f::agreed_version(m.view(), sources), "native branch text must corroborate the engine family");
    m = original; m.executable = false;
    expect(!f::version_source(m.view(), image, getter), "a byte pattern in non-code memory is not a native getter");
    m = original;
    expect(!f::version_source(m.view(), {M::base, 0x40000001}, getter) &&
        !f::version_source(m.view(), {UINTPTR_MAX - 7, 16}, getter), "invalid/oversized image ranges reject");
    expect(!f::agreed_version(m.view(), {}), "missing native sources do not invent a version");
    const f::VersionSource other{object + 0x200, cell + 0x200, guard + 0x200};
    const std::array both{*source, other};
    m.put(other.object, std::array<uint16_t, 4>{5, 6, 1, 0}); m.put(other.object + 16, branch);
    m.put(other.object + 24, int32_t{4}); m.put(other.object + 28, int32_t{8});
    m.put(other.published, other.object); m.put(other.guard, int32_t{-2147483646});
    expect(f::agreed_version(m.view(), both) == f::EngineVersion{5, 6, 1}, "multiple coherent native version objects may agree");
    m.put(other.object + 2, uint16_t{5});
    expect(!f::agreed_version(m.view(), both), "conflicting native version authorities fail closed");
    m.put(other.guard, int32_t{-1});
    expect(f::agreed_version(m.view(), both) == f::EngineVersion{5, 6, 1}, "uninitialized unrelated lazy objects cannot override a proved version");
    std::array<f::VersionSource, 33> excessive{};
    excessive.fill(*source);
    expect(!f::agreed_version(m.view(), excessive), "native version candidate processing is bounded");

    struct Changing {
        M memory;
        uintptr_t address{};
        size_t reads{};
        sdk::discovery::Memory view() {
            return {this, [](void* context, uintptr_t p, void* out, size_t n) {
                auto& self = *static_cast<Changing*>(context);
                auto underlying = self.memory.view();
                if (!underlying.read(underlying.context, p, out, n)) { return false; }
                if (p == self.address && ++self.reads == 2) { static_cast<uint8_t*>(out)[0] ^= 1; }
                return true;
            }, nullptr};
        }
    };
    for (auto p : {object, cell, guard}) {
        Changing changing{original, p};
        expect(!f::agreed_version(changing.view(), sources), "concurrent object/publication/guard changes reject instead of mixing observations");
    }
}
