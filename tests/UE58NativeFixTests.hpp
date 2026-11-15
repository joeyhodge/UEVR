#pragma once

#include <fstream>
#include <utility>
#include <vector>
#include "mods/vr/UE58NativeFix.hpp"
#include "mods/vr/NativeStereoCopyExtent.hpp"
#include "mods/vr/ParadoxDriveRHI.hpp"
#include "mods/vr/UE584Development.hpp"
#include "fixtures/UE584StockABI.hpp"
#include "fixtures/UE584DevelopmentABI.hpp"

namespace ue58_native_tests {
namespace n = uevr::ue58_native;

struct Memory {
    uintptr_t base{0x100000};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0xc000);
    bool readable{true}, executable{true}, partial_write{};
    template<class T> void put(uintptr_t p, const T& value) {
        std::memcpy(bytes.data() + p - base, &value, sizeof(value));
    }
    sdk::discovery::Memory view() {
        return {this, [](void* context, uintptr_t p, void* out, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            if (!m.readable || p < m.base || p - m.base > m.bytes.size() || size > m.bytes.size() - (p - m.base)) { return false; }
            std::memcpy(out, m.bytes.data() + p - m.base, size); return true;
        }, [](void* context, uintptr_t p, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            return m.executable && p >= m.base && p - m.base <= 0x5000 && size <= 0x5000 - (p - m.base);
        }};
    }
    static bool write_eye(void* context, uintptr_t view, const n::Eye& eye) {
        auto& m = *static_cast<Memory*>(context);
        m.put(view + n::constrained_offset, eye.constrained);
        if (m.partial_write) { m.partial_write = false; return false; }
        m.put(view + n::unconstrained_offset, eye.unconstrained);
        m.put(view + n::pass_offset, eye.pass);
        return true;
    }
};

struct NativeCalls {
    inline static NativeCalls* active{};
    alignas(16) std::array<uint8_t, n::family_size> source{};
    std::array<uintptr_t, 2> entries{0x20000, 0x30000};
    std::array<uintptr_t, 2> empty_all{};
    n::Interfaces deleted{};
    int copies{}, destroys{};
    bool alias_views{}, alias_all{};
    NativeCalls() {
        active = this;
        put(source.data(), 0, uintptr_t{0x40000});
        put(source.data(), 8, n::ViewArray{reinterpret_cast<uintptr_t>(entries.data()), 2, 2});
        put(source.data(), 0x18, n::ViewArray{reinterpret_cast<uintptr_t>(empty_all.data()), 0, 2});
        put(source.data(), 0x30, uintptr_t{0x50000});
        put(source.data(), 0x40, uintptr_t{0x60000});
        put(source.data(), n::interfaces_offset, n::Interfaces{0x70000,0,0,0,0});
        source[0xb7] = 1;
    }
    ~NativeCalls() { active = nullptr; }
    template<class T> static void put(void* storage, size_t offset, const T& value) {
        std::memcpy(static_cast<uint8_t*>(storage) + offset, &value, sizeof(value));
    }
    static void* copy(void* to, const void* from) {
        ++active->copies;
        std::memcpy(to, from, n::family_size);
        put(to, 0, uintptr_t{0x80000});
        auto* entries = active->alias_views ? active->entries.data() : new uintptr_t[2]{active->entries[0], active->entries[1]};
        put(to, 8, n::ViewArray{reinterpret_cast<uintptr_t>(entries),2,2});
        put(to, 0x18, active->alias_all ? n::ViewArray{reinterpret_cast<uintptr_t>(active->empty_all.data()),0,2} : n::ViewArray{});
        return to;
    }
    static void destroy(void* family) {
        ++active->destroys;
        std::memcpy(active->deleted.data(), static_cast<uint8_t*>(family) + n::interfaces_offset, sizeof(active->deleted));
        n::ViewArray views{};
        std::memcpy(&views, static_cast<uint8_t*>(family) + 8, sizeof(views));
        delete[] reinterpret_cast<uintptr_t*>(views.data);
        n::ViewArray all{};
        std::memcpy(&all, static_cast<uint8_t*>(family) + 0x18, sizeof(all));
        delete[] reinterpret_cast<uintptr_t*>(all.data);
    }
    static sdk::discovery::Memory memory() {
        return {nullptr, [](void*, uintptr_t p, void* out, size_t size) {
            if (p < 0x10000) { return false; }
            std::memcpy(out, reinterpret_cast<const void*>(p), size); return true;
        }, nullptr};
    }
    static n::FamilyFunctions functions() {
        return {reinterpret_cast<uintptr_t>(&copy), reinterpret_cast<uintptr_t>(&destroy), 0x80000};
    }
};

template<class Expect> void test_copy_extent(Expect&& expect) {
    namespace c = uevr::native_stereo_copy;
    expect(n::needs_copy_extent_revalidation(L"D:/Games/ParadoxDrive-Win64-Shipping.exe", 0x50008),
        "ParadoxDrive UE5.8 revalidates Native Fix copy extents after deferred scene adoption");
    for (const auto version : {0x4001bu, 0x50007u, 0x50009u, 0x60000u, 0u}) {
        expect(!n::needs_copy_extent_revalidation(L"ParadoxDrive-Win64-Shipping.exe", version),
            "The ParadoxDrive copy follow-up does not change other engine intervals");
    }
    for (const auto name : {L"ParadoxDrive.exe", L"Other-Win64-Shipping.exe",
        L"ParadoxDrive-Win64-Shipping.exe.old", L"D:/ParadoxDrive-Win64-Shipping.exe/Other.exe"}) {
        expect(!n::needs_copy_extent_revalidation(name, 0x50008),
            "The ParadoxDrive copy follow-up requires the exact game basename");
    }

    // The live resize rebuilt XR before the engine published its smaller target.
    c::CopyExtent extent{true, true, true, 2734, 1336, 3708, 1812, 1367, 1336};
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::rebuild,
        "ParadoxDrive rejects the recorded stale 3708x1812 copy against 2734x1336 resources");
    extent.source_width = 3708; extent.source_height = 1812;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::wait_for_scene,
        "ParadoxDrive waits while the old scene target is still published, even if its cache matches");
    extent.source_width = 2734; extent.source_height = 1336;
    extent.cached_width = 2734; extent.cached_height = 1336;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::unchanged,
        "ParadoxDrive resumes only after ordinary setup has refreshed both cached dimensions");
    extent.cached_height = 1812;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::rebuild,
        "A matching width cannot hide a stale height or permit an out-of-bounds source box");
    extent.cached_height = 1336; extent.cached_width = 3708;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::rebuild,
        "A matching height cannot hide a stale right-eye destination offset");
    extent.external_scene = false;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::wait_for_scene,
        "A desktop backbuffer cannot stand in for the ParadoxDrive packed engine target");
    extent.external_scene = true; extent.descriptor_valid = false;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::wait_for_scene,
        "ParadoxDrive does not rebuild from an incompatible native resource");
    extent.scoped = false;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::unchanged,
        "The copy policy is inert outside the existing Native Fix OpenXR title guard");

    extent = {true, true, true, 4944, 2416, 2734, 1336, 2472, 2416};
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::rebuild,
        "Growing resolution also refreshes copy and spectator resources instead of clipping the new eye pair");
    extent.cached_width = 4944; extent.cached_height = 2416;
    expect(c::copy_extent_action(extent) == c::CopyExtentAction::unchanged,
        "The corrected resolution does not cause a steady-state rebuild loop");
}

template<class Expect> void test_development(Expect&& expect) {
    namespace d = uevr::ue584_development;
    namespace f = uevr::tests::ue584_development;
    namespace s = uevr::tests::ue584_stock;
    Memory memory;
    const auto base = memory.base, copy = base + 0x100, deleting = base + 0x700, destroy = base + 0x800;
    const auto table = base + 0x6000, constructor = base + 0x2000, renderer = base + 0x3000;
    memory.put(constructor + 0x39, s::view_identity);
    memory.put(constructor + 0x3af, s::view_rects);
    memory.put(constructor + 0x495, s::view_projection);
    memory.put(constructor + 0x53d, f::view_stereo);
    memory.put(renderer + 0x30, f::renderer);
    memory.put(copy, f::family_copy); memory.put(deleting, f::family_delete); memory.put(destroy, f::family_destroy);
    memory.put(table, deleting);
    memory.put(copy + 0x25, int32_t(table - (copy + 0x29)));
    memory.put(deleting + 0x10, int32_t(destroy - (deleting + 0x14)));
    memory.put(destroy + 9, int32_t(table - (destroy + 0xd)));
    const auto size = [&](uintptr_t address) -> size_t {
        return address == copy ? f::family_copy.size() : address == deleting ? f::family_delete.size() :
            address == destroy ? f::family_destroy.size() : 0;
    };
    const auto resolve = [&] { return d::family_functions(memory.view(), copy, base, memory.bytes.size(), size); };
    const auto family = resolve();
    expect(d::view_layout(memory.view(), constructor) && d::renderer_layout(memory.view(), renderer) &&
        family && family->development_layout && family->copy == copy && family->destroy == destroy && family->vtable == table,
        "independent Development bytes prove eye fields, the linked renderer and the larger family's native ownership");
    expect(!n::view_layout(memory.view(), constructor) && !n::renderer_layout(memory.view(), renderer) &&
        !n::family_functions(memory.view(), copy, base, memory.bytes.size(), size),
        "Development cannot inherit Shipping constructor, renderer or placement-storage authorization");
    const auto original = memory;
    const auto mutate = [&](uintptr_t address, const auto& mask, auto&& check) {
        for (size_t i = 0; i < mask.size(); ++i) {
            if (!mask[i]) { continue; }
            memory = original;
            memory.bytes[address - base + i] ^= 1;
            expect(!check(), "changed Development identity, ownership or control flow rejects before engine calls");
        }
    };
    const auto view = [&] { return d::view_layout(memory.view(), constructor); };
    const auto render = [&] { return d::renderer_layout(memory.view(), renderer); };
    mutate(constructor + 0x39, n::contract::view_identity_mask, view);
    mutate(constructor + 0x3af, n::contract::view_rects_mask, view);
    mutate(constructor + 0x495, n::contract::view_projection_mask, view);
    mutate(constructor + 0x53d, d::contract::view_stereo_mask, view);
    mutate(renderer + 0x30, d::contract::renderer_mask, render);
    mutate(copy, uevr::ue59_native::contract::development_family_copy_mask, resolve);
    mutate(deleting, uevr::ue59_native::contract::development_family_delete_mask, resolve);
    mutate(destroy, d::contract::family_destroy_mask, resolve);
    memory = original; memory.put(destroy + 9, int32_t(table + 8 - (destroy + 0xd)));
    expect(!resolve(), "the Development base destructor must restore the same vtable as its copy constructor");
    memory = original; memory.put(table, deleting + 1);
    expect(!resolve(), "a Development interior/deleting/context-destructor mismatch rejects");
    memory = original; memory.readable = false;
    expect(!view() && !render() && !resolve(), "unreadable Development code cannot authorize a write or native call");
    memory = original; memory.executable = false;
    expect(!view() && !render() && !resolve(), "non-executable Development candidates reject");
    memory = original;
    expect(!d::view_layout(memory.view(), UINTPTR_MAX - 16) &&
        !d::family_functions(memory.view(), copy, base, memory.bytes.size(), [](uintptr_t) -> size_t { return 0; }),
        "Development requires bounded addresses and original function starts/sizes");
}

template<class Expect> void run(Expect&& expect) {
    test_copy_extent(expect);
    test_development(expect);
    using namespace n::contract;
    expect(n::needs_guarded_fallback(L"D:/Games/SLASHER-Win64-Shipping.exe") &&
        n::needs_guarded_fallback(L"d:\\Games\\slasher-win64-shipping.EXE"), "changed Fatekeeper code must fail closed rather than return to the crashing path");
    for (auto name : {L"ParadoxDrive-Win64-Shipping.exe", L"D:/Games/ParadoxDrive/ParadoxDrive-Win64-Shipping.exe",
            L"d:\\Games\\PARADOXDRIVE-WIN64-SHIPPING.EXE"}) {
        expect(n::is_paradox_drive(name) && n::needs_guarded_fallback(name),
            "ParadoxDrive keeps its code-validated linked route or fails closed after an update");
    }
    for (auto name : {L"", L"Other.exe", L"xSLASHER-Win64-Shipping.exe", L"SLASHER-Win64-Shipping.exe.old",
            L"D:/SLASHER-Win64-Shipping.exe/Other.exe", L"ParadoxDrive.exe", L"xParadoxDrive-Win64-Shipping.exe",
            L"ParadoxDrive-Win64-Shipping.exe.old", L"D:/ParadoxDrive-Win64-Shipping.exe/Other.exe"}) {
        expect(!n::needs_guarded_fallback(name) && !n::is_paradox_drive(name),
            "other titles and launchers require actual code capability rather than a substring or patch-number guess");
    }
    Memory m;
    const auto b = m.base, copy = b + 0x100, deleting = b + 0x700, destroy = b + 0x800;
    const auto table = b + 0x6000, ctor = b + 0x2000, renderer = b + 0x3000;
    m.put(copy, family_copy_code); m.put(deleting, family_delete_code); m.put(destroy, family_destroy_code);
    m.put(table, deleting);
    m.put(copy + 0x2b, int32_t(table - (copy + 0x2f)));
    m.put(deleting + 0x10, int32_t(destroy - (deleting + 0x14)));
    m.put(destroy + 9, int32_t(table - (destroy + 0xd)));
    m.put(ctor + 0x30, view_identity_code); m.put(ctor + 0x3b9, view_rects_code);
    m.put(ctor + 0x49f, view_projection_code); m.put(ctor + 0x547, view_stereo_code);
    m.put(renderer + 0x2a, renderer_span_code); m.put(renderer + 0x19c, renderer_frame_code);
    const auto size = [&](uintptr_t p) -> size_t {
        if (p == copy) { return family_copy_code.size(); }
        if (p == deleting) { return family_delete_code.size(); }
        if (p == destroy) { return family_destroy_code.size(); }
        return 0;
    };
    const auto resolve = [&]() { return n::family_functions(m.view(), copy, b, m.bytes.size(), size); };
    const auto f = resolve();
    expect(f && f->copy == copy && f->destroy == destroy && f->vtable == table,
        "UE5.8 proves native deep-copy and base-only destructor through their shared vtable");
    expect(n::view_layout(m.view(), ctor) && n::renderer_layout(m.view(), renderer),
        "constructor and plural renderer independently corroborate UE5.8 eye/family fields");
    const auto good = m;
    const auto mutate = [&](uintptr_t p, const auto& mask, auto&& validate) {
        for (size_t i = 0; i < mask.size(); ++i) {
            if (!mask[i]) { continue; }
            m = good; m.bytes[p - b + i] ^= 1;
            expect(!validate(), "changed UE5.8 members, ownership operations and control flow reject before native calls");
        }
    };
    mutate(copy, family_copy_mask, resolve); mutate(deleting, family_delete_mask, resolve); mutate(destroy, family_destroy_mask, resolve);
    const auto view = [&] { return n::view_layout(m.view(), ctor); };
    mutate(ctor + 0x30, view_identity_mask, view); mutate(ctor + 0x3b9, view_rects_mask, view);
    mutate(ctor + 0x49f, view_projection_mask, view); mutate(ctor + 0x547, view_stereo_mask, view);
    const auto render = [&] { return n::renderer_layout(m.view(), renderer); };
    mutate(renderer + 0x2a, renderer_span_mask, render); mutate(renderer + 0x19c, renderer_frame_mask, render);
    m = good; m.put(destroy + 9, int32_t(table + 8 - (destroy + 0xd)));
    expect(!resolve(), "copy and destruction cannot use different family bases");
    m = good; m.put(table, deleting + 1);
    expect(!resolve(), "mid-function or context destructor candidates reject");
    m = good; m.readable = false;
    expect(!resolve() && !view() && !render(), "unreadable immutable code never authorizes writes or calls");
    m = good; m.executable = false;
    expect(!resolve() && !view() && !render(), "data masquerading as constructor/renderer code rejects");
    m = good;
    expect(!n::family_functions(m.view(), copy, b, m.bytes.size(), [](uintptr_t) -> size_t { return 0; }),
        "native family calls require original function boundaries, not interior pattern matches");

    {
        Memory shifted;
        shifted.put(ctor + 0x39, view_identity_code);
        shifted.put(ctor + 0x3af, view_rects_code);
        shifted.put(ctor + 0x495, view_projection_code);
        shifted.put(ctor + 0x53d, view_stereo_code);
        expect(n::view_layout(shifted.view(), ctor),
            "independently verified stock UE5.8.4 Shipping and ParadoxDrive share the shifted constructor proof");
        const auto before = shifted;
        const auto mutate_shifted = [&](uintptr_t address, const auto& mask) {
            for (size_t i = 0; i < mask.size(); ++i) {
                if (!mask[i]) { continue; }
                shifted = before;
                shifted.bytes[address - b + i] ^= 1;
                expect(!n::view_layout(shifted.view(), ctor),
                    "changed stock UE5.8.4 member access or control flow rejects before eye writes or engine calls");
            }
        };
        mutate_shifted(ctor + 0x39, view_identity_mask);
        mutate_shifted(ctor + 0x3af, view_rects_mask);
        mutate_shifted(ctor + 0x495, view_projection_mask);
        mutate_shifted(ctor + 0x53d, view_stereo_mask);
        shifted = before;
        shifted.readable = false;
        expect(!n::view_layout(shifted.view(), ctor), "unreadable stock Shipping constructor code rejects");
        shifted = before;
        shifted.executable = false;
        expect(!n::view_layout(shifted.view(), ctor), "non-executable stock Shipping code rejects");
        shifted = before;
        expect(!n::view_layout(shifted.view(), 0) &&
            !n::view_layout(shifted.view(), UINTPTR_MAX - 0x10) &&
            !n::view_layout(shifted.view(), ctor + 1),
            "null, overflowing and interior stock Shipping constructor candidates cannot authorize the layout");
    }

    {
        namespace stock = uevr::tests::ue584_stock;
        Memory captured;
        captured.put(ctor + 0x39, stock::view_identity);
        captured.put(ctor + 0x3af, stock::view_rects);
        captured.put(ctor + 0x495, stock::view_projection);
        captured.put(ctor + 0x53d, stock::view_stereo);
        captured.put(renderer + 0x2a, stock::renderer_span);
        captured.put(renderer + 0x19c, stock::renderer_frame);
        captured.put(copy, stock::family_copy);
        captured.put(deleting, stock::family_delete);
        captured.put(destroy, stock::family_destroy);
        captured.put(table, deleting);
        captured.put(copy + 0x2b, int32_t(table - (copy + 0x2f)));
        captured.put(deleting + 0x10, int32_t(destroy - (deleting + 0x14)));
        captured.put(destroy + 9, int32_t(table - (destroy + 0xd)));
        const auto family = n::family_functions(captured.view(), copy, b, captured.bytes.size(), size);
        expect(n::view_layout(captured.view(), ctor) && n::renderer_layout(captured.view(), renderer) &&
            family && family->copy == copy && family->destroy == destroy && family->vtable == table,
            "independent stock UE5.8.4 Shipping bytes prove the linked view/renderer and family ownership contracts");
        captured.bytes[ctor - b + 0x53d] ^= 1;
        expect(!n::view_layout(captured.view(), ctor),
            "a changed stock constructor cannot use the confirmed Shipping eye layout");
    }

    for (auto dims : {std::pair{1024,1024}, std::pair{2472,2416}, std::pair{3500,3100}, std::pair{8192,4096}}) {
        for (int letterbox : {0, 20, 100}) {
            const auto [w,h] = dims;
            const auto left = b + 0x7000, right = b + 0x9000;
            n::Eye l{{0,letterbox,w,h-letterbox},{0,0,w,h},1,0};
            n::Eye r{{w,letterbox,2*w,h-letterbox},{w,0,2*w,h},2,0};
            Memory::write_eye(&m,left,l); Memory::write_eye(&m,right,r);
            m.put(left + n::primary_index_offset,int32_t{0}); m.put(right + n::primary_index_offset,int32_t{0});
            m.put(right + n::eye_index_offset,int32_t{1}); m.put(right + 0x10,uintptr_t{0x123456});
            const auto before = m.bytes;
            {
                n::EyeOverride eye;
                expect(eye.initialize(m.view(),left,right,l,r,w,h,&m,Memory::write_eye),
                    "right singleton is temporarily primary at origin for arbitrary eye sizes and letterboxing");
                n::Eye adapted{};
                expect(sdk::sinking_city_2::native::read_eye(m.view(),right,adapted) && adapted == l,
                    "only physical classification and destination-relative rectangles change");
                int32_t logical{}; uintptr_t state{};
                expect(m.view().load(right+n::eye_index_offset,logical) && logical == 1 &&
                    m.view().load(right+0x10,state) && state == 0x123456, "logical eye and temporal/exposure state remain authored");
            }
            expect(m.bytes == before, "renderer transaction restores every original byte for the viewport and spectator");
            m.partial_write = true;
            n::EyeOverride partial;
            expect(!partial.initialize(m.view(),left,right,l,r,w,h,&m,Memory::write_eye) && m.bytes == before,
                "a partial rectangle write rolls back before ordinary Native fallback");
        }
    }

    {
        NativeCalls native;
        const auto before = native.source;
        const char* reason{};
        {
            n::FamilyClone clone;
            expect(clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason),
                "fresh family is copied once with native member ownership");
            auto* copied = reinterpret_cast<uint8_t*>(clone.address());
            expect(copied && copied[0xb0] == 1 && copied[0xb7] == 0 && copied[0xb8] == 0,
                "additional base-family storage is never misclassified as a larger renderer-owned FViewFamilyInfo");
            expect(clone.finish(native.memory(),reinterpret_cast<uintptr_t>(native.source.data())),
                "the original borrowed screen-percentage interface is allowed without sharing temporal upscalers");
        }
        expect(native.source == before && native.entries == std::array<uintptr_t,2>{0x20000,0x30000} &&
            native.copies == 1 && native.destroys == 1 && native.deleted[0] == 0,
            "clone cleanup releases its arrays once, never deletes the source interface/views or placement storage");
        NativeCalls::put(native.source.data(),0x170,uintptr_t{0x90000});
        n::FamilyClone consumed;
        expect(!consumed.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason) &&
            !consumed.original_render_safe() && native.copies == 1,
            "an already-consumed family is neither cloned nor submitted to BeginRenderingViewFamilies twice");
    }
    {
        for (const bool alias_all : {false, true}) {
            NativeCalls native;
            native.alias_views = !alias_all;
            native.alias_all = alias_all;
            const auto before = native.source;
            const char* reason{};
            {
                n::FamilyClone clone;
                expect(!clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason),
                    "a shallow Views or empty-but-allocated AllViews copy cannot authorize a renderer transaction");
            }
            expect(native.source == before && native.destroys == 1,
                "rejected borrowed array storage is detached before native cleanup and the original family remains untouched");
        }
    }
    {
        n::Interfaces original{1,2,3,4,5}, clone{1,6,7,8,9}, borrowed{};
        expect(n::retain_shared_interfaces(original,clone,borrowed) && borrowed[0] == 1,
            "independent renderer interfaces retain only the borrowed screen-percentage owner");
        clone[3] = original[1];
        expect(!n::retain_shared_interfaces(original,clone,borrowed) && borrowed[3] == 2,
            "cross-slot source alias is rejected and excluded from clone deletion");
        clone = {10,11,12,11,14}; borrowed = {};
        expect(!n::retain_shared_interfaces(original,clone,borrowed) && borrowed[3] == 11,
            "duplicate clone-owned interfaces cannot be deleted twice");
    }
}

template<class Expect> void shipping_image(const char* path, Expect&& expect) {
    struct Image {
        std::ifstream file;
        uint64_t size{}, base{};
        uint32_t module_size{};
        struct Section { uint32_t rva, size, offset, flags; };
        std::vector<Section> sections;
        std::optional<uint64_t> offset(uintptr_t p, size_t n, bool code) const {
            if (p < base) { return {}; }
            const auto rva = p - base;
            for (auto s : sections) {
                if (rva < s.rva || rva - s.rva > s.size || n > s.size - (rva - s.rva) ||
                    (code && !(s.flags & 0x20000000))) { continue; }
                const auto o = uint64_t(s.offset) + rva - s.rva;
                if (o <= size && n <= size - o) { return o; }
            }
            return {};
        }
    } image;
    image.file.open(path, std::ios::binary | std::ios::ate);
    if (!image.file) { expect(false,"open exact analyzed UE5.8 shipping image"); return; }
    image.size = static_cast<uint64_t>(image.file.tellg());
    std::array<uint8_t,65536> header{};
    image.file.seekg(0);
    if (!image.file.read(reinterpret_cast<char*>(header.data()),header.size())) { expect(false,"read shipping PE header"); return; }
    const auto u16 = [&](size_t o) { uint16_t v{}; if (o <= header.size()-2) { std::memcpy(&v,header.data()+o,2); } return v; };
    const auto u32 = [&](size_t o) { uint32_t v{}; if (o <= header.size()-4) { std::memcpy(&v,header.data()+o,4); } return v; };
    const auto nt = u32(0x3c);
    if (nt > 4096 || u16(0) != 0x5a4d || u32(nt) != 0x4550 || u16(nt+4) != 0x8664 || u16(nt+24) != 0x20b) {
        expect(false,"validate x64 shipping PE headers"); return;
    }
    const auto opt = nt+24;
    const auto count = u16(nt+6), opt_size = u16(nt+20);
    const auto section_start = opt + opt_size;
    if (opt_size < 144 || !count || count > 96 || uint64_t(section_start)+uint64_t(count)*40 > header.size()) {
        expect(false,"validate shipping section and exception-directory bounds"); return;
    }
    std::memcpy(&image.base,header.data()+opt+24,sizeof(image.base));
    image.module_size = u32(opt+56);
    for (size_t i=0;i<count;++i) {
        const auto o=section_start+i*40;
        image.sections.push_back({u32(o+12),u32(o+16),u32(o+20),u32(o+36)});
    }
    sdk::discovery::Memory memory{&image,[](void* context,uintptr_t p,void* out,size_t n) {
        auto& image=*static_cast<Image*>(context);
        const auto offset=image.offset(p,n,false);
        if (!offset) { return false; }
        image.file.clear(); image.file.seekg(*offset);
        return static_cast<bool>(image.file.read(static_cast<char*>(out),n));
    },[](void* context,uintptr_t p,size_t n) { return static_cast<Image*>(context)->offset(p,n,true).has_value(); }};
    const auto pdata=u32(opt+112+3*8), pdata_size=u32(opt+112+3*8+4);
    if (!pdata_size || pdata_size%12 || pdata_size>16*1024*1024) { expect(false,"bound original unwind table"); return; }
    std::vector<std::array<uint32_t,3>> entries(pdata_size/12);
    if (!memory.read(memory.context,image.base+pdata,entries.data(),pdata_size) ||
        !std::is_sorted(entries.begin(),entries.end(),[](auto a,auto b) { return a[0]<b[0]; })) {
        expect(false,"read sorted original unwind records"); return;
    }
    const auto size=[&](uintptr_t p) -> size_t {
        if (p<image.base || p-image.base>UINT32_MAX) { return 0; }
        const auto rva=static_cast<uint32_t>(p-image.base);
        const auto e=std::lower_bound(entries.begin(),entries.end(),rva,[](auto e,uint32_t rva) { return e[0]<rva; });
        return e!=entries.end() && (*e)[0]==rva && (*e)[1]>(*e)[0] ? (*e)[1]-(*e)[0] : 0;
    };
    if (image.size == 331843000 && image.base == 0x140000000) {
        namespace d = uevr::ue584_development;
        constexpr uintptr_t constructor = 0x145e36770, renderer = 0x147973b60;
        constexpr uintptr_t copy = 0x145e38330, destroy = 0x145e3c9e0, table = 0x14fdc9398;
        const auto family = d::family_functions(memory, copy, image.base, image.module_size, size);
        expect(d::view_layout(memory, constructor) && d::renderer_layout(memory, renderer) &&
            family && family->development_layout && family->destroy == destroy && family->vtable == table,
            "actual Development EXE/PDB code proves its independent linked renderer and full native family ownership");
        expect(!n::view_layout(memory, constructor) && !n::renderer_layout(memory, renderer) &&
            !n::family_functions(memory, copy, image.base, image.module_size, size),
            "the actual Development binary is never authorized by Shipping layout contracts");
        namespace p = uevr::paradoxdrive_rhi;
        constexpr uintptr_t callback = 0x146546610, builder = 0x146607330;
        const auto scan = [&](uintptr_t cursor, size_t, const char*) -> std::optional<uintptr_t> {
            return cursor <= builder ? std::optional<uintptr_t>{builder} : std::nullopt;
        };
        expect(p::classify_input(memory, L"UnrealGame.exe", 0x50008, image.base, image.module_size,
            callback, scan, size) == p::Input::VerifiedRDG,
            "actual Development callback and constructor prove RDG +0xc0 rather than the legacy allocator root");
        expect(!d::view_layout(memory, constructor+1) &&
            !d::family_functions(memory, copy+1, image.base, image.module_size, size),
            "interior Development addresses cannot authorize private calls");
        return;
    }
    const bool roadtruckers = image.size==186809344;
    const bool paradoxdrive = image.size==167882752;
    const bool stock584 = image.size==165862328;
    const bool analyzed = (image.size==195425280 || roadtruckers || paradoxdrive || stock584) && image.base==0x140000000;
    expect(analyzed, "UE5.8 fixture is an analyzed complete stock Shipping, Fatekeeper, RoadTruckers or ParadoxDrive build");
    if (!analyzed) { return; }
    const uintptr_t constructor=stock584 ? 0x142f02bb0 : paradoxdrive ? 0x142f098f0 : roadtruckers ? 0x142f9c3e0 : 0x142f43710;
    const uintptr_t renderer=stock584 ? 0x143b4cc80 : paradoxdrive ? 0x143b53a40 : roadtruckers ? 0x143befbb0 : 0x143b8c630;
    const uintptr_t copy=stock584 ? 0x142f042d0 : paradoxdrive ? 0x142f0b010 : roadtruckers ? 0x142f9dd20 : 0x142f45050;
    const uintptr_t destroy=stock584 ? 0x142f06690 : paradoxdrive ? 0x142f0d3d0 : roadtruckers ? 0x142fa0180 : 0x142f47530;
    const uintptr_t table=stock584 ? 0x147cb4820 : paradoxdrive ? 0x147df9c90 : roadtruckers ? 0x148967ea0 : 0x148eba550;
    expect(n::view_layout(memory,constructor) && n::renderer_layout(memory,renderer),
        "actual unmodified game code proves projection/rect/stereo identity and linked renderer argument flow");
    if (paradoxdrive || stock584) {
        namespace p = uevr::paradoxdrive_rhi;
        const auto callback=stock584 ? uintptr_t{0x143202870} : uintptr_t{0x143209630};
        const auto builder=stock584 ? uintptr_t{0x14325a3d0} : uintptr_t{0x143261190};
        expect(p::render_texture_layout(memory,image.base,image.module_size,callback,size) &&
            p::builder_layout(memory,image.base,image.module_size,builder,size),
            "actual stock/ParadoxDrive RDG overload and constructor prove the immediate-list reference before unwrapping");
        expect(!p::render_texture_layout(memory,image.base,image.module_size,callback+1,size) &&
            !p::builder_layout(memory,image.base,image.module_size,builder+1,size),
            "actual interior RDG code addresses cannot authorize the borrowed reference");
    }
    const auto f=n::family_functions(memory,copy,image.base,image.module_size,size);
    expect(f && f->destroy==destroy && f->vtable==table,
        "actual game copy and base destructor resolve through original unwind boundaries and shared native vtable");
    expect(!n::family_functions(memory,copy+1,image.base,image.module_size,size),
        "actual interior function addresses cannot authorize engine calls");
}
} // namespace ue58_native_tests
