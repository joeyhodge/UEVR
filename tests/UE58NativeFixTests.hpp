#pragma once

#include <fstream>
#include <utility>
#include <vector>
#include "mods/vr/UE58NativeFix.hpp"

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

template<class Expect> void run(Expect&& expect) {
    using namespace n::contract;
    expect(n::needs_guarded_fallback(L"D:/Games/SLASHER-Win64-Shipping.exe") &&
        n::needs_guarded_fallback(L"d:\\Games\\slasher-win64-shipping.EXE"), "changed Fatekeeper code must fail closed rather than return to the crashing path");
    for (auto name : {L"", L"Other.exe", L"xSLASHER-Win64-Shipping.exe", L"SLASHER-Win64-Shipping.exe.old",
            L"D:/SLASHER-Win64-Shipping.exe/Other.exe"}) {
        expect(!n::needs_guarded_fallback(name), "other titles require actual code capability rather than a substring or patch-number guess");
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
    if (!image.file) { expect(false,"open exact Fatekeeper shipping image"); return; }
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
    const bool roadtruckers = image.size==186809344;
    expect((image.size==195425280 || roadtruckers) && image.base==0x140000000,
        "UE5.8 fixture is an analyzed complete Fatekeeper or RoadTruckers build");
    const uintptr_t constructor=roadtruckers ? 0x142f9c3e0 : 0x142f43710;
    const uintptr_t renderer=roadtruckers ? 0x143befbb0 : 0x143b8c630;
    const uintptr_t copy=roadtruckers ? 0x142f9dd20 : 0x142f45050;
    const uintptr_t destroy=roadtruckers ? 0x142fa0180 : 0x142f47530;
    const uintptr_t table=roadtruckers ? 0x148967ea0 : 0x148eba550;
    expect(n::view_layout(memory,constructor) && n::renderer_layout(memory,renderer),
        "actual unmodified game code proves projection/rect/stereo identity and linked renderer argument flow");
    const auto f=n::family_functions(memory,copy,image.base,image.module_size,size);
    expect(f && f->destroy==destroy && f->vtable==table,
        "actual game copy and base destructor resolve through original unwind boundaries and shared native vtable");
    expect(!n::family_functions(memory,copy+1,image.base,image.module_size,size),
        "actual interior function addresses cannot authorize engine calls");
}
} // namespace ue58_native_tests
