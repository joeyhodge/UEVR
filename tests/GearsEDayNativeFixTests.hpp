#pragma once

#include "mods/vr/GearsEDayNativeFix.hpp"

namespace gears_native_tests {
struct Memory {
    uintptr_t base{0x100000};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x8000);
    bool readable{true}, executable{true};
    template<class T> void put(uintptr_t address, const T& value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
    sdk::discovery::Memory view() {
        return {this,
            [](void* p, uintptr_t address, void* out, size_t size) {
                auto& m = *static_cast<Memory*>(p);
                if (!m.readable || address < m.base || address - m.base > m.bytes.size() ||
                    size > m.bytes.size() - (address - m.base)) { return false; }
                std::memcpy(out, m.bytes.data() + address - m.base, size); return true;
            },
            [](void* p, uintptr_t address, size_t size) {
                auto& m = *static_cast<Memory*>(p);
                return m.executable && address >= m.base && address - m.base <= 0x5000 &&
                    size <= 0x5000 - (address - m.base);
            }};
    }
};
}

void test_goweday_native_family() {
    namespace g = uevr::gears_native;
    namespace s = sdk::gears_eday;
    {
        auto first = g::make_family_storage(), second = g::make_family_storage();
        expect(first && second && first.get() != second.get(), "Gears placement storage has independent UEVR ownership without a GMalloc lookup");
        if (first && second) {
            expect((reinterpret_cast<uintptr_t>(first->bytes.data()) & 15) == 0 &&
                first->bytes.size() >= g::family_size, "Gears clone storage has the proven size/alignment");
            expect(std::all_of(first->bytes.begin(), first->bytes.end(), [](auto byte) { return byte == 0; }),
                "Gears placement storage is initialized before native construction");
            first->bytes[0x197] = 0xa5;
            expect(second->bytes[0x197] == 0, "separate frame-family placement buffers cannot alias");
            first.reset();
            expect(second->bytes[0x197] == 0, "releasing one outer buffer does not affect another");
        }
    }
    gears_native_tests::Memory m;
    const auto b = m.base, copy = b + 0x100, deleting = b + 0x800, destroy = b + 0x900;
    const auto array_copy = b + 0xc00, table = b + 0x6000, ctor = b + 0x2000, family = b + 0x7000;
    m.put(copy, g::family_copy_code); m.put(deleting, g::family_delete_code);
    m.put(destroy, g::family_destroy_code); m.put(array_copy, g::view_array_copy_code);
    m.put(table, deleting);
    m.put(copy + 0x0e, static_cast<int32_t>(table - (copy + 0x12)));
    m.put(destroy + 9, static_cast<int32_t>(table - (destroy + 0x0d)));
    const auto patch_call = [&](uintptr_t ip, uintptr_t target) { m.put(ip + 1, static_cast<int32_t>(target - (ip + 5))); };
    patch_call(copy + 0x31, array_copy); patch_call(copy + 0x4c, array_copy); patch_call(deleting + 0x0f, destroy);
    const auto function_size = [&](uintptr_t address) -> size_t {
        if (address == copy) { return g::family_copy_code.size(); }
        if (address == deleting) { return g::family_delete_code.size(); }
        if (address == destroy) { return g::family_destroy_code.size(); }
        if (address == array_copy) { return g::view_array_copy_code.size(); }
        return 0;
    };
    const auto resolve = [&] { return g::family_functions(m.view(), copy, b, m.bytes.size(), function_size); };
    const auto result = resolve();
    expect(result && result->copy == copy && result->vtable == table && result->deleting_destructor == deleting &&
        result->complete_destructor == destroy, "Gears resolves its own native copy, deep arrays and base-only destructor ABI");
    const auto good = m;
    const auto mutate = [&](uintptr_t address, const auto& mask) {
        for (size_t i = 0; i < mask.size(); ++i) {
            if (mask[i] == 0) { continue; }
            m = good; m.bytes[address - b + i] ^= 1;
            expect(!resolve(), "Gears rejects any changed family layout, ownership operation or destructor control flow");
        }
    };
    mutate(copy, g::family_copy_mask); mutate(deleting, g::family_delete_mask);
    mutate(destroy, g::family_destroy_mask); mutate(array_copy, g::view_array_copy_mask);
    m = good; patch_call(copy + 0x4c, destroy);
    expect(!resolve(), "Gears Views and AllViews must share the same proven deep-copy helper");
    m = good; m.put(destroy + 9, static_cast<int32_t>(table + 8 - (destroy + 0x0d)));
    expect(!resolve(), "Gears copy and destructor must agree on the base-family vtable");
    m = good; m.put(table, deleting + 1);
    expect(!resolve(), "Gears does not accept a derived or mid-function deleting destructor");
    m = good;
    expect(!g::family_functions(m.view(), copy, b, m.bytes.size(), [](uintptr_t) -> size_t { return 0; }),
        "Gears requires exact original unwind boundaries before calling engine copy/destruction code");
    m.executable = false;
    expect(!resolve(), "Gears family contracts must be executable");
    m = good; m.readable = false;
    expect(!resolve(), "Gears family contracts must be readable");
    m = good;
    m.put(ctor + 0x30, s::view_identity_code); m.put(ctor + 0x9c, s::view_player_code);
    m.put(ctor + 0xc4, g::view_rects_code);
    m.put(ctor + 0x214, g::view_singleton_code);
    expect(g::singleton_layout(m.view(), ctor), "Gears singleton fields come from the real view constructor, not its engine version");
    for (size_t i = 0; i < g::view_singleton_mask.size(); ++i) {
        auto changed = m; changed.bytes[ctor + 0x214 - b + i] ^= 1;
        expect(!g::singleton_layout(changed.view(), ctor), "Gears rejects changed pass/primary-index metadata offsets or initialization");
    }
    for (size_t i = 0; i < g::view_rects_mask.size(); ++i) {
        auto changed = m; changed.bytes[ctor + 0xc4 - b + i] ^= 1;
        expect(!g::singleton_layout(changed.view(), ctor), "Gears rejects changed InitOptions or output rectangle-copy offsets");
    }
    const auto renderer = b + 0x3000;
    m.put(renderer + 0x2a, g::renderer_array_code);
    expect(g::renderer_layout(m.view(), renderer, 19), "Gears proves the plural renderer's family-list/scene ABI beyond its entry hook");
    for (size_t i = 0; i < g::renderer_array_mask.size(); ++i) {
        auto changed = m; changed.bytes[renderer + 0x2a - b + i] ^= 1;
        expect(g::renderer_layout(changed.view(), renderer, 19) == (g::renderer_array_mask[i] == 0),
            "Gears tolerates renderer relocation but rejects changed argument transport or calls");
    }
    expect(!g::renderer_layout(m.view(), renderer, 0) && !g::renderer_layout(m.view(), renderer, 0x2b),
        "Gears does not inspect renderer bytes overwritten by an unknown hook");
    m.put(family, table);
    expect(g::fresh_family(m.view(), family, table), "Gears accepts a fresh original family before rendering");
    for (const auto offset : {0x158, 0x168, 0x170, 0x178}) {
        auto consumed = m; consumed.put(family + offset, uintptr_t(0x12345678));
        expect(!g::fresh_family(consumed.view(), family, table), "Gears refuses to submit a consumed renderer/upscaler family twice");
    }
    m.put(family + 0x160, uintptr_t(0x12345678));
    expect(g::fresh_family(m.view(), family, table), "Gears permits the intentionally borrowed screen-percentage interface");
    expect(!g::fresh_family(m.view(), family, table + 8), "Gears requires the exact selected source-family vtable");
}

void test_goweday_native_rects() {
    namespace g = uevr::gears_native;
    struct Fixture : gears_native_tests::Memory {
        bool writable{true}, partial{};
        int writes{};
        static bool write(void* context, uintptr_t view, const g::ViewRects& rects) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.writes;
            if (!f.writable) { return false; }
            f.put(view + g::constrained_rect_offset, rects.constrained);
            if (f.partial) { f.partial = false; return false; }
            f.put(view + g::unconstrained_rect_offset, rects.unconstrained);
            return true;
        }
    } f;
    const auto left = f.base + 0x1000, right = f.base + 0x2000;
    const g::ViewRects l{{0, 12, 2472, 2404}, {0, 0, 2472, 2416}};
    const g::ViewRects r{{2472, 12, 4944, 2404}, {2472, 0, 4944, 2416}};
    Fixture::write(&f, left, l); Fixture::write(&f, right, r);
    f.put(right + 0x378, uintptr_t(0x1122334455667788));
    f.put(right + 0x3a0, uintptr_t(0x8877665544332211));
    const auto original = f.bytes;
    g::ViewRects read{};
    {
        g::RectOverride tx;
        expect(tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write),
            "Gears transaction validates exact live and constructor packed rectangles before moving the right eye");
        expect(g::read_rects(f.view(), left, read) && read == l && g::read_rects(f.view(), right, read) && read == l,
            "only the linked right-eye capture temporarily has a zero-origin rectangle");
        expect(!tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write), "active rectangle transaction cannot be overwritten");
        expect(tx.restore() && f.bytes == original, "explicit restoration preserves every byte including neighboring projection/color fields");
        expect(tx.restore(), "rectangle restoration is idempotent");
    }
    {
        g::RectOverride tx;
        expect(tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write), "early-return restoration fixture initializes");
    }
    expect(f.bytes == original, "early return restores the packed ordinary-render fallback");
    {
        f.partial = true;
        g::RectOverride tx;
        expect(!tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write) && f.bytes == original,
            "partial rectangle writes roll back both fields before fallback");
    }
    {
        g::RectOverride tx;
        expect(tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write), "failed restoration fixture initializes");
        f.writable = false;
        expect(!tx.restore(), "failed restoration cannot authorize fallback or packet publication");
        f.writable = true;
        expect(tx.restore() && f.bytes == original, "restoration retries retain the exact saved packed pair");
    }
    auto stale = r; ++stale.constrained[1];
    {
        g::RectOverride tx;
        const auto writes = f.writes;
        expect(!tx.initialize(f.view(), left, right, l, stale, 2472, 2416, &f, Fixture::write), "stale constructor metadata is rejected");
        expect(!tx.initialize(f.view(), left, left, l, l, 2472, 2416, &f, Fixture::write), "aliased views are rejected");
        expect(!tx.initialize(f.view(), left, right, l, r, 1236, 1208, &f, Fixture::write), "resolution-generation mismatch is rejected");
        expect(f.writes == writes && f.bytes == original, "rejected transactions perform no writes");
    }
    auto invalid = r; invalid.constrained[0] = (std::numeric_limits<int32_t>::min)();
    expect(!g::rebase_right(l, invalid, 2472, 2416), "malformed coordinates cannot overflow rebasing arithmetic");
    expect(!g::rebase_right(l, l, 2472, 2416), "overlapping eyes cannot authorize a capture transaction");
    expect(!g::rebase_right(l, r, 0, 2416) && !g::rebase_right(l, r, 16385, 2416), "invalid capture dimensions are rejected");
    f.readable = false;
    g::RectOverride tx;
    expect(!tx.initialize(f.view(), left, right, l, r, 2472, 2416, &f, Fixture::write), "unreadable views cannot authorize writes");
}
