#pragma once

#include "mods/vr/GalacticRacerNativeFix.hpp"

namespace swgr_native_tests {
struct Memory {
    uintptr_t base{0x100000};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x9000);
    bool readable{true}, executable{true}, writable{true}, corrupt_write{}, partial_write{};
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
            return m.executable && p >= m.base && p - m.base <= 0x5000 && n <= 0x5000 - (p - m.base);
        }};
    }
    static bool write(void* context, uintptr_t view, const uevr::swgr_native::ViewRects& rects) {
        auto& m = *static_cast<Memory*>(context);
        if (!m.writable) { return false; }
        m.put(view + uevr::swgr_native::unscaled_rect_offset, rects.constrained);
        if (m.partial_write) { m.partial_write = false; return false; }
        auto raw = rects.unconstrained;
        if (m.corrupt_write) { raw[0] += 1; m.corrupt_write = false; }
        m.put(view + uevr::swgr_native::unconstrained_rect_offset, raw);
        return true;
    }
};
}

void test_swgr_native_fix() {
    namespace s = uevr::swgr_native;
    swgr_native_tests::Memory m;
    const auto b = m.base, copy = b + 0x100, scalar = b + 0x600, complete = b + 0x700;
    const auto allocator = b + 0xa00, origins = b + 0xb00, flags = b + 0xc00;
    const auto shared = b + 0xe00, release = b + 0xf00, memcpy = b + 0x1000;
    const auto extensions = b + 0x1100, view_extensions = b + 0x1200, free = b + 0x1300;
    const auto table = b + 0x6000, ctor = b + 0x2000;
    const auto patch_call = [&](uintptr_t ip, uintptr_t target) { m.put(ip + 1, int32_t(target - (ip + 5))); };
    m.put(copy, s::family_copy_code); m.put(scalar, s::scalar_destructor_code); m.put(complete, s::complete_destructor_code);
    m.put(allocator, s::array_allocate_code); m.put(origins, s::origins_allocate_code); m.put(flags, s::flags_allocate_code);
    m.put(shared, s::shared_copy_code); m.put(extensions, s::extensions_destroy_code);
    m.put(view_extensions, s::view_extensions_destroy_code);
    m.put(release, std::array<uint8_t, 17>{0x56,0x48,0x83,0xec,0x20,0x48,0x85,0xc9,0x74,0x09,0x48,0x89,0xce,0xf0,0xff,0x49,0x08});
    m.put(table, scalar); m.put(copy + 0x14, int32_t(table - (copy + 0x18)));
    m.put(complete + 0xb, int32_t(table - (complete + 0xf)));
    patch_call(scalar + 0xb, complete); patch_call(scalar + 0x1c, free);
    for (auto off : {0x2d8, 0x315, 0x386, 0x3c4, 0x405}) { patch_call(copy + off, allocator); }
    for (auto off : {0x1ed, 0x2a9, 0x326, 0x398, 0x3d6}) { patch_call(copy + off, memcpy); }
    patch_call(copy + 0x1cf, origins); patch_call(copy + 0x353, flags);
    for (auto off : {0x2e6, 0x413}) { patch_call(copy + off, shared); }
    for (auto off : {0x22, 0x56}) { patch_call(allocator + off, free); }
    for (auto off : {0x61, 0x74}) { patch_call(origins + off, free); }
    for (auto off : {0x60, 0x72, 0x7b, 0x8b}) { patch_call(flags + off, free); }
    patch_call(complete + 0x5f, extensions); patch_call(complete + 0x9d, view_extensions);
    patch_call(extensions + 0x1d, release); patch_call(view_extensions + 0x16, release);
    for (auto off : {0xd2, 0xed, 0xf4, 0xfb, 0x132, 0x13c, 0x146}) { patch_call(complete + off, free); }
    const auto function_size = [&](uintptr_t p) -> size_t {
        if (p == scalar) { return s::scalar_destructor_code.size(); }
        if (p == complete) { return s::complete_destructor_code.size(); }
        if (p == allocator) { return s::array_allocate_code.size(); }
        if (p == origins) { return s::origins_allocate_code.size(); }
        if (p == flags) { return s::flags_allocate_code.size(); }
        return 0;
    };
    const auto resolve = [&] { return s::family_functions(m.view(), copy, s::family_copy_code.size(), b, m.bytes.size(), function_size); };
    const auto f = resolve();
    expect(f && f->copy == copy && f->table == table && f->scalar_destructor == scalar,
        "SWGR Clang copy proves arrays, interface offsets, refcounts and base-only cleanup");
    const auto good = m;
    const auto check_mutations = [&](uintptr_t p, const auto& mask) {
        for (size_t i = 0; i < mask.size(); ++i) {
            if (!mask[i]) { continue; }
            m = good; m.bytes[p - b + i] ^= 1;
            expect(!resolve(), "changed family/ownership/control-flow byte cannot enable Native Fix");
        }
    };
    check_mutations(copy, s::family_copy_mask); check_mutations(scalar, s::scalar_destructor_mask);
    check_mutations(complete, s::complete_destructor_mask); check_mutations(allocator, s::array_allocate_mask);
    check_mutations(origins, s::origins_allocate_mask); check_mutations(flags, s::flags_allocate_mask);
    check_mutations(shared, s::shared_copy_mask); check_mutations(extensions, s::extensions_destroy_mask);
    check_mutations(view_extensions, s::view_extensions_destroy_mask);
    m = good; m.put(table, scalar + 0x2c);
    expect(!resolve(), "derived context destructor cannot delete borrowed engine views");
    m = good; m.put(complete + 0xb, int32_t(table + 8 - (complete + 0xf)));
    expect(!resolve(), "copy and destructor must agree on the base vtable");
    m = good; patch_call(copy + 0x398, b + 0x9800);
    expect(!resolve(), "external copy target cannot leave its owning image");
    m = good; patch_call(copy + 0x3c4, free);
    expect(!resolve(), "Views and AllViews must use the same validated deep allocator");
    m = good; patch_call(complete + 0x5f, shared);
    expect(!resolve(), "a refcount increment is not valid shared-array destruction");
    m = good; m.executable = false;
    expect(!resolve(), "nonexecutable contracts fail closed");
    m = good; m.readable = false;
    expect(!resolve(), "unreadable contracts fail closed");
    m = good;
    expect(!s::family_functions(m.view(), copy, s::family_copy_code.size() + 1, b, m.bytes.size(), function_size),
        "changed unwind boundaries cannot authorize a copy");

    m.put(ctor + 0x74, s::view_arguments_code); m.put(ctor + 0x96, s::view_family_code);
    m.put(ctor + 0x2e4, s::view_rects_code); m.put(ctor + 0x4ce, s::view_stereo_code);
    m.put(ctor + 0x132d, s::view_primary_code);
    const auto layout = [&] { return s::view_layout(m.view(), ctor, s::view_constructor_size, b, m.bytes.size()); };
    expect(layout(), "SWGR constructor proves pass/index and both rectangle fields beyond the inline hook");
    const auto layout_good = m;
    const auto check_layout = [&](uintptr_t p, const auto& code) {
        for (size_t i = 0; i < code.size(); ++i) {
            m = layout_good; m.bytes[p - b + i] ^= 1;
            expect(!layout(), "constructor metadata changes cannot reuse Venice's projection-overlapping offsets");
        }
    };
    check_layout(ctor + 0x74, s::view_arguments_code); check_layout(ctor + 0x96, s::view_family_code);
    check_layout(ctor + 0x2e4, s::view_rects_code); check_layout(ctor + 0x4ce, s::view_stereo_code);
    check_layout(ctor + 0x132d, s::view_primary_code);
    m = layout_good; m.put(ctor, uint64_t{0xffffffffffff25ff});
    expect(layout(), "an installed prologue hook does not invalidate untouched metadata proof blocks");
    expect(!s::view_layout(m.view(), ctor, s::view_constructor_size - 1, b, m.bytes.size()), "wrong constructor boundary rejected");

    const auto left = b + 0x7000, right = b + 0x7800;
    const s::ViewRects l{{0, 80, 2472, 2336}, {0, 0, 2472, 2416}};
    const s::ViewRects r{{2472, 80, 4944, 2336}, {2472, 0, 4944, 2416}};
    m = layout_good;
    swgr_native_tests::Memory::write(&m, left, l); swgr_native_tests::Memory::write(&m, right, r);
    s::ViewRects read{};
    {
        s::RectOverride tx;
        expect(tx.initialize(m.view(), left, right, l, r, 2472, 2416, &m, swgr_native_tests::Memory::write),
            "letterboxed packed pair rebases only the validated right capture");
        expect(s::read_rects(m.view(), right, read) && read == l, "right capture has the same zero-origin footprint as primary");
        expect(s::read_rects(m.view(), left, read) && read == l, "primary rectangle never changes");
        expect(!tx.initialize(m.view(), left, right, l, r, 2472, 2416, &m, swgr_native_tests::Memory::write),
            "nested transactions cannot overwrite the saved fallback rectangles");
    }
    expect(s::read_rects(m.view(), right, read) && read == r, "scope exit restores the original packed family before fallback");
    for (const auto partial : {false, true}) {
        m.corrupt_write = !partial; m.partial_write = partial;
        s::RectOverride tx;
        expect(!tx.initialize(m.view(), left, right, l, r, 2472, 2416, &m, swgr_native_tests::Memory::write),
            "partial/incorrect writes cannot publish a capture");
        expect(s::read_rects(m.view(), right, read) && read == r, "failed application rolls back before original rendering");
    }
    {
        s::RectOverride tx;
        expect(tx.initialize(m.view(), left, right, l, r, 2472, 2416, &m, swgr_native_tests::Memory::write), "transaction can start again");
        m.writable = false;
        expect(!tx.restore(), "failed restoration cannot authorize original rendering or packet publication");
        m.writable = true;
        expect(tx.restore() && s::read_rects(m.view(), right, read) && read == r, "retry can restore the exact saved rectangle");
    }
    auto invalid = r; invalid.constrained[0] = (std::numeric_limits<int32_t>::min)();
    expect(!s::rebase_right(l, invalid, 2472, 2416), "malformed rectangle cannot overflow rebasing arithmetic");
    expect(!s::rebase_right(l, l, 2472, 2416), "already overlapping eyes are not a normal packed pair");
    expect(!s::rebase_right(l, r, 1236, 1208), "resolution generation mismatch rejects capture without moving views");
    expect(!s::rebase_right(l, r, 0, 2416) && !s::rebase_right(l, r, 16385, 2416), "invalid capture extent rejected");
    {
        s::RectOverride tx;
        expect(!tx.initialize(m.view(), left, left, l, l, 2472, 2416, &m, swgr_native_tests::Memory::write), "aliased views rejected");
        invalid = r; invalid.constrained[1] += 1;
        expect(!tx.initialize(m.view(), left, right, l, invalid, 2472, 2416, &m, swgr_native_tests::Memory::write),
            "stale constructor metadata rejected before any write");
        expect(s::read_rects(m.view(), right, read) && read == r, "rejected metadata retains the engine's original rectangles");
    }
}
