#pragma once

#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include <sdk/AbzuCapture.hpp>

namespace abzu_tests {
namespace a = sdk::abzu;
struct Fixture {
    struct Block { uintptr_t address{}; std::vector<uint8_t> bytes; bool code{}; };
    uintptr_t base{0x140000000};
    std::vector<Block> blocks;
    bool readable{true}, executable{true};
    static constexpr uintptr_t object = 0x300000000, klass = 0x300001000, outer = 0x300002000;
    static constexpr uintptr_t resource = 0x300003000, rhi = 0x300004000, native = 0x300005000;
    static constexpr uintptr_t options = 0x300006000, family = 0x300007000, state = 0x300008000;
    static constexpr uintptr_t left = 0x300009000, right = 0x30000a000;
    static constexpr uintptr_t renderer_module = 0x30000b000;

    void add(uintptr_t p, size_t n, bool code = false) { blocks.push_back({p, std::vector<uint8_t>(n), code}); }
    template<class T> void put(uintptr_t p, const T& value) {
        for (auto& b : blocks) {
            if (p >= b.address && p - b.address <= b.bytes.size() && sizeof(T) <= b.bytes.size() - (p - b.address)) {
                std::memcpy(b.bytes.data() + p - b.address, &value, sizeof(T)); return;
            }
        }
        throw std::runtime_error("ABZU fixture write out of bounds");
    }
    sdk::discovery::Memory memory() {
        return {this, [](void* c, uintptr_t p, void* out, size_t n) {
            const auto& f = *static_cast<Fixture*>(c);
            if (!f.readable) { return false; }
            for (const auto& b : f.blocks) {
                if (p >= b.address && p - b.address <= b.bytes.size() && n <= b.bytes.size() - (p - b.address)) {
                    std::memcpy(out, b.bytes.data() + p - b.address, n); return true;
                }
            }
            return false;
        }, [](void* c, uintptr_t p, size_t n) {
            const auto& f = *static_cast<Fixture*>(c);
            if (!f.executable) { return false; }
            for (const auto& b : f.blocks) {
                if (b.code && p >= b.address && p - b.address <= b.bytes.size() && n <= b.bytes.size() - (p - b.address)) { return true; }
            }
            return false;
        }};
    }
    explicit Fixture(const nlohmann::json& captured, uintptr_t load_base = 0x140000000) : base{load_base} {
        add(base, 0x400);
        put(base, uint16_t{0x5a4d}); put(base + 0x3c, uint32_t{0x80}); put(base + 0x80, uint32_t{0x4550});
        put(base + 0x84, uint16_t{0x8664}); put(base + 0x86, uint16_t{9});
        put(base + 0x88, captured.at("timestamp").get<uint32_t>()); put(base + 0x98, uint16_t{0x20b});
        put(base + 0xd0, captured.at("image_size").get<uint32_t>());
        for (const auto& b : captured.at("blocks")) {
            const auto hex = b.at("hex").get<std::string>();
            add(base + b.at("rva").get<uint32_t>(), hex.size()/2, true);
            for (size_t i = 0; i < hex.size(); i += 2) { blocks.back().bytes[i/2] = uint8_t(std::stoul(hex.substr(i,2), nullptr,16)); }
        }
        add(base + a::texture_vtable, 0x200); add(base + a::target_vtable, 0x30); add(base + a::rhi_vtable, 0x38);
        put(base + a::texture_vtable + 0x1e8, base + a::code::update_resource_rva);
        put(base + a::texture_vtable + 0x1f0, base + a::code::create_resource_rva);
        put(base + a::target_vtable + 8, base + a::code::render_texture_getter_rva);
        put(base + a::target_vtable + 24, base + a::code::capture_gamma_rva);
        put(base + a::target_vtable, base + a::code::update_resource_rva);
        put(base + a::rhi_vtable + 48, base + a::code::native_texture_getter_rva);
        add(base+a::renderer_global,8); add(base+a::renderer_vtable,0x48); add(renderer_module,8);
        put(base+a::renderer_global,renderer_module); put(renderer_module,base+a::renderer_vtable);
        put(base+a::renderer_vtable+0x40,base+a::code::renderer_prefix_rva);
        add(object, a::texture_size); add(resource, a::resource_size); add(rhi, 0xa8);
        put(object, base + a::texture_vtable); put(object + 0x10, klass); put(object + 0x20, outer);
        add(options, 0x108); add(family, 0x30); add(state, 8);
        put(options + 0xb0, family); put(options + 0xb8, state);
        put(options + 0x100, uint32_t{2}); put(options + 0x104, 100.0f);
        put(family + 0x18, resource + a::render_target); put(family + a::family_scene, outer);
        put(family + 0x20, klass); // Deliberate plausible decoy; the renderer consumes +0x28.
        put(state, base + a::target_vtable); put(resource + a::render_target, base + a::target_vtable);
        add(left, 0x480); add(right, 0x480);
    }
    void initialized() {
        put(object + 0xc0, uint32_t{2472}); put(object + 0xc4, uint32_t{2416});
        put(object + 0xdc, uint32_t{3}); put(object + 0xe0, uint8_t{2}); put(object + 0xe4, uint32_t{1});
        put(object + a::private_resource, resource); put(resource, base + a::resource_vtable);
        put(resource + 0xa0, object); put(resource + a::render_target, base + a::target_vtable);
        put(resource + 0xc0, uint32_t{2}); put(resource + 0xc4, uint32_t{2472}); put(resource + 0xc8, uint32_t{2416});
        put(resource + a::texture_rhi, rhi); put(resource + 0x70, rhi); put(resource + 0xa8, rhi);
        put(rhi, base + a::rhi_vtable); put(rhi + a::native_texture, native);
    }
};

template<class Expect> void rects(Fixture fixture, Expect&& expect) {
    const a::ViewRects left{{0,0,2472,2416},{0,0,2472,2416},{0,0,2472,2416}};
    const a::ViewRects right{{2472,0,4944,2416},{2472,0,4944,2416},{2472,0,4944,2416}};
    fixture.put(Fixture::left + a::rects_offset, left); fixture.put(Fixture::right + a::rects_offset, right);
    fixture.put(Fixture::right + 0x30, uint64_t{0x12345678});
    fixture.put(Fixture::right + 0x430, uint32_t{2});
    const auto black = [](const a::Rect& x, const a::Rect& y) {
        const auto area = [](const a::Rect& r) { return int64_t(r[2]-r[0])*(r[3]-r[1]); };
        return area(x) + area(y) != int64_t(std::max(x[2],y[2])-std::min(x[0],y[0]))*
            (std::max(x[3],y[3])-std::min(x[1],y[1]));
    };
    expect(black(left.unscaled,left.unscaled) && !black(left.unscaled,right.unscaled),
        "ABZU Draw rejects overlapping eyes but accepts the original packed viewport coverage");
    struct Writer {
        Fixture* fixture{};
        unsigned calls{}, failure{}, partial{};
        static bool write(void* c, uintptr_t p, const a::ViewRects& v) {
            auto& w = *static_cast<Writer*>(c);
            if (++w.calls == w.failure) { return false; }
            if (w.calls == w.partial) { w.fixture->put(p + a::rects_offset, v.render); return false; }
            w.fixture->put(p + a::rects_offset, v); return true;
        }
    } writer{&fixture};
    auto start = [&](a::RightRectOverride& t) {
        return t.initialize(fixture.memory(), Fixture::left, Fixture::right, left.unscaled, left.unconstrained,
            right.unscaled,right.unconstrained,2472,2416,&writer,Writer::write);
    };
    auto read = [&] { a::ViewRects r{}; expect(a::read_rects(fixture.memory(),Fixture::right,r), "ABZU right rectangle read succeeds"); return r; };
    {
        a::RightRectOverride t;
        expect(start(t) && read() == left && !start(t), "ABZU right singleton rebases exactly once inside the renderer transaction");
        expect(t.restore() && read() == right, "ABZU restores all packed rectangles before viewport/canvas code resumes");
    }
    uint64_t before{}; uint32_t pass{};
    fixture.memory().load(Fixture::right + 0x30,before); fixture.memory().load(Fixture::right + 0x430,pass);
    expect(before == 0x12345678 && pass == 2, "ABZU never writes Finch's +0x30 layout or changes the original eye identity");
    writer.calls = 0; writer.partial = 1;
    { a::RightRectOverride t; expect(!start(t) && t.restore() && read() == right, "ABZU partially written rectangles roll back before ordinary fallback"); }
    writer.calls = writer.partial = 0; writer.failure = 2;
    { a::RightRectOverride t; expect(start(t) && !t.restore(), "ABZU reports failed restoration instead of publishing a successful frame"); }
    expect(read() == right, "ABZU scope cleanup retries a failed restoration");
    writer.calls = 0; writer.failure = 1;
    { a::RightRectOverride t; expect(!start(t) && t.restore() && read() == right,
        "ABZU a rejected first write leaves the original viewport rectangles intact"); }
    writer.calls = writer.failure = 0;
    fixture.readable = false;
    { a::RightRectOverride t; expect(!start(t) && !writer.calls, "ABZU inaccessible rectangles are rejected without writes"); }
    expect(!a::right_singleton(left,left,2472,2416) && !a::right_singleton(left,right,0,2416),
        "ABZU rejects already-overlapping pairs and invalid dimensions");
    auto unequal = right; --unequal.unscaled[2];
    expect(!a::right_singleton(left,unequal,2472,2416), "ABZU refuses mismatched per-eye constrained rectangles");
    auto boxed_left = left, boxed_right = right;
    boxed_left.render = boxed_left.unscaled = {10,20,2462,2396};
    boxed_right.render = boxed_right.unscaled = {2482,20,4934,2396};
    expect(a::right_singleton(boxed_left,boxed_right,2472,2416) == boxed_left,
        "ABZU preserves matching letterbox borders without assuming desktop resolution");
}

template<class Expect> void run(Expect&& expect) {
    std::ifstream stream{std::filesystem::path{__FILE__}.parent_path()/"fixtures/abzu-20201114-native-contracts.json"};
    expect(stream.good(), "ABZU original instruction fixture is present");
    if (!stream.good()) { return; }
    const auto captured = nlohmann::json::parse(stream);
    Fixture f{captured};
    expect(a::factory(f.memory(),f.base) && a::constructor(f.memory(),f.base) == f.base + a::code::constructor_prefix_rva &&
        a::viewport_rect_contract(f.memory(),f.base), "ABZU factory, constructor and viewport contracts validate independently");
    const auto resolve=[&](Fixture& image, uintptr_t callback = 0, uintptr_t draw = 0) {
        return a::renderer_entry(image.memory(),image.base,callback ? callback : image.base+a::renderer_callback_return,
            draw ? draw : image.base+0x11196f0);
    };
    expect(resolve(f) == f.base+a::code::renderer_prefix_rva && a::code::renderer_prefix.size() == 19,
        "ABZU's real native-vtable renderer is callable despite its 19-byte first unwind fragment");
    expect(!resolve(f,f.base+0x111a2f3) && !resolve(f,f.base+a::renderer_callback_return,f.base+a::code::renderer_prefix_rva),
        "ABZU cannot resolve Draw, a different callback, or the excluded entry as the renderer");
    Fixture relocated{captured,0x7ff600000000};
    expect(a::factory(relocated.memory(),relocated.base) && a::viewport_rect_contract(relocated.memory(),relocated.base) &&
        resolve(relocated) == relocated.base+a::code::renderer_prefix_rva,
        "ABZU code and native vtable relationships are ASLR-safe");
    for (auto name : {L"AbzuGame-Win64-Shipping.exe",L"D:/Games/ABZU/ABZUGAME-WIN64-SHIPPING.EXE"}) {
        expect(a::target(name), "ABZU exact executable matching accepts case/path variants");
    }
    for (auto name : {L"",L"AbzuGame-Win64-Shipping.exe.old",L"NotAbzuGame-Win64-Shipping.exe",L"FinchGame.exe",L"Other.exe",
            L"D:/AbzuGame-Win64-Shipping.exe/Other.exe"}) { expect(!a::target(name), "ABZU does not alter other games or similarly named files"); }
    for (const auto& b : f.blocks) {
        if (!b.code) { continue; }
        for (size_t i = 0; i < b.bytes.size(); ++i) {
            auto bad = f; bad.put(b.address+i,uint8_t(b.bytes[i]^1));
            const bool view = b.address >= f.base+a::code::constructor_prefix_rva && b.address < f.base+0x1302c00;
            const bool viewport = b.address >= f.base+a::code::viewport_area_rva && b.address < f.base+0x111a300;
            const bool renderer = b.address == f.base+a::code::renderer_getter_rva ||
                (b.address >= f.base+a::code::renderer_prefix_rva && b.address < f.base+0xb1b8c0);
            expect(view ? !a::constructor(bad.memory(),bad.base) : renderer ? !resolve(bad) : viewport ? !a::viewport_rect_contract(bad.memory(),bad.base) :
                !a::factory(bad.memory(),bad.base), "ABZU rejects every changed code-witness byte instead of invoking an unverified ABI");
        }
    }
    for (const auto& [where,value] : std::array<std::pair<uintptr_t,uint32_t>,3>{{
            {0x88,a::timestamp+1},{0xd0,a::image_size+0x1000},{0x80,0x1234}}}) {
        auto bad=f; bad.put(bad.base+where,value);
        expect(!a::factory(bad.memory(),bad.base), "ABZU changed PE identities fail closed");
    }
    auto bad=f; bad.executable=false;
    expect(!a::factory(bad.memory(),bad.base), "ABZU readable non-executable witness pages are rejected");
    bad=f; bad.put(bad.base+a::texture_vtable+0x1e8,bad.base+a::code::create_resource_rva);
    expect(!a::factory(bad.memory(),bad.base), "ABZU rejects swapped UpdateResource/CreateResource virtuals");
    bad=f; bad.put(bad.base+a::renderer_vtable+0x40,bad.base+0x11196f0);
    expect(!resolve(bad), "ABZU rejects a Draw pointer placed in the renderer slot instead of reusing a stack-size guess");
    bad=f; bad.put(Fixture::renderer_module,bad.base+a::texture_vtable);
    expect(!resolve(bad), "ABZU renderer module must own the native renderer vtable");
    bad=f; bad.put(bad.base+a::renderer_global,uintptr_t{});
    expect(!resolve(bad), "ABZU waits for the real renderer module without calling its lazy-initializing getter");
    bad=f; bad.put(bad.base+a::code::renderer_prefix_rva,uint8_t{0xe9});
    expect(a::family_contract(bad.memory(),bad.base) && !resolve(bad),
        "ABZU family-member proof survives our installed prologue hook, while fresh renderer discovery rejects modified entries");
    expect(a::fresh_target(f.memory(),Fixture::object,f.base,Fixture::klass,Fixture::outer), "ABZU accepts only a fresh owned native TextureRenderTarget2D");
    for (const auto flags : {0x10u,0x20u,0x31u}) {
        bad=f; bad.put(Fixture::object+8,flags);
        expect(!a::fresh_target(bad.memory(),Fixture::object,bad.base,Fixture::klass,Fixture::outer), "ABZU never initializes CDOs or archetypes");
    }
    bad=f; bad.put(Fixture::object+0x20,Fixture::klass);
    expect(!a::fresh_target(bad.memory(),Fixture::object,bad.base,Fixture::klass,Fixture::outer), "ABZU rejects foreign outer ownership");
    auto initialized=f; initialized.initialized();
    expect(!a::fresh_target(initialized.memory(),Fixture::object,initialized.base,Fixture::klass,Fixture::outer),
        "ABZU never reinitializes an already allocated texture");
    const auto chain=a::capture_chain(initialized.memory(),initialized.base,Fixture::object,2472,2416);
    expect(chain && chain->resource == Fixture::resource && chain->target == Fixture::resource+0x68 &&
        chain->rhi == Fixture::rhi && chain->native == Fixture::native, "ABZU validates owner/resource/FRenderTarget/render-SRV/native identity before publication");
    a::StableCapture stable;
    expect(!stable.observe(1,42,chain) && !stable.observe(1,42,chain) && stable.observe(1,43,chain),
        "ABZU requires an unchanged complete chain on distinct engine frames, not two families in one frame");
    expect(!stable.observe(2,43,chain) && stable.observe(2,44,chain),
        "ABZU new generations cannot inherit prior capture readiness even if addresses are reused");
    expect(!stable.observe(2,45,std::nullopt) && !stable.observe(2,46,chain) && stable.observe(2,47,chain),
        "ABZU losing the owned resource resets readiness before a later observation");
    auto replacement=chain; replacement->native+=0x100;
    expect(!stable.observe(2,48,replacement) && stable.observe(2,49,replacement) && !stable.observe(0,50,replacement),
        "ABZU replacement resources and retired generations cannot reuse stale publication readiness");
    for (auto offset : {a::private_resource}) {
        bad=initialized; bad.put(Fixture::object+offset,uintptr_t{});
        expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU pending resource allocation waits without speculative scanning");
    }
    for (auto offset : {0x30u,0x70u,0xa0u,0xa8u}) {
        bad=initialized; bad.put(Fixture::resource+offset,uintptr_t{});
        expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU incomplete, mismatched or foreign resource references cannot publish");
    }
    bad=initialized; bad.put(Fixture::rhi,initialized.base+a::texture_vtable);
    expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU does not confuse a UObject or wrapper with its native D3D11 RHI texture");
    bad=initialized; bad.put(Fixture::rhi+a::native_texture,uintptr_t{});
    expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU waits for the actual native texture pointer");
    expect(a::target_accessors(initialized.memory(),initialized.base,Fixture::resource+a::render_target),
        "ABZU capture getter/gamma indices are committed only from the proven native FRenderTarget");
    bad=initialized; bad.put(bad.base+a::target_vtable+24,bad.base+a::code::native_texture_getter_rva);
    expect(!a::target_accessors(bad.memory(),bad.base,Fixture::resource+a::render_target),
        "ABZU never probes a non-gamma virtual with a float-returning ABI");
    for (auto flags : {0u,2u,5u,7u}) {
        bad=initialized; bad.put(Fixture::object+0xdc,flags);
        expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU rejects sRGB-creating or mip-generating target flags");
    }
    bad=initialized; bad.put(Fixture::object+0xbc,uint8_t{1});
    expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU refuses two-copy targets that would need a separate deferred resolve");
    bad=initialized; bad.put(Fixture::object+0xb8,2.2f);
    expect(!a::capture_chain(bad.memory(),bad.base,Fixture::object,2472,2416), "ABZU RHI creation cannot double-encode the viewport-matched output");
    expect(!a::capture_chain(initialized.memory(),initialized.base,Fixture::object,2473,2416), "ABZU stale resize extents cannot publish");
    const auto layout=a::view_layout(f.memory(),f.base,Fixture::options);
    expect(layout && layout->view_family == 0xb0 && layout->scene_state == 0xb8 && !layout->player_index &&
        layout->stereo_pass == 0x100 && layout->world_to_meters == 0x104, "ABZU preserves the pre-4.16 options ABI without inventing a player index");
    bad=f; bad.put(Fixture::family+a::family_scene,uintptr_t{});
    expect(!a::view_layout(bad.memory(),bad.base,Fixture::options),
        "ABZU does not substitute a plausible +0x20 pointer for the native renderer's +0x28 Scene member");
    bad=f; bad.put(Fixture::options+0x104,std::numeric_limits<float>::quiet_NaN());
    expect(!a::view_layout(bad.memory(),bad.base,Fixture::options), "ABZU rejects nonfinite world scale");
    bad=f; bad.put(Fixture::options+0x100,uint32_t{3});
    expect(!a::view_layout(bad.memory(),bad.base,Fixture::options), "ABZU rejects invalid original eye identity");
    expect(!a::fresh_target(f.memory(),0x7ffffffffff8,f.base,Fixture::klass,Fixture::outer) &&
        !a::constructor(f.memory(),0xffffffffffff0000) && !a::view_layout(f.memory(),f.base,0), "ABZU null and overflowing addresses fail closed");
    rects(f,expect);
}

template<class Expect> void shipping_image(const char* path, Expect&& expect) {
    std::ifstream stream{std::filesystem::path{path},std::ios::binary};
    std::vector<uint8_t> file{std::istreambuf_iterator<char>{stream},std::istreambuf_iterator<char>{}};
    if (file.size()<0x400) { expect(false,"ABZU shipping image is readable"); return; }
    const auto load=[&]<class T>(size_t p,T& out) {
        if (p>file.size() || sizeof(T)>file.size()-p) { return false; }
        std::memcpy(&out,file.data()+p,sizeof(T)); return true;
    };
    uint32_t nt{},headers{}; uint16_t sections{},optional{}; uintptr_t base{};
    if (!load(0x3c,nt) || nt>0x300 || !load(nt+6,sections) || sections!=9 || !load(nt+20,optional) ||
        !load(nt+24+24,base) || !load(nt+24+60,headers) || headers>file.size() || headers>a::image_size) {
        expect(false,"ABZU shipping image header is bounded"); return;
    }
    Fixture f{nlohmann::json{{"timestamp",a::timestamp},{"image_size",a::image_size},{"blocks",nlohmann::json::array()}},base};
    f.blocks.clear(); f.add(base,headers); std::memcpy(f.blocks.back().bytes.data(),file.data(),headers);
    for (size_t i=0;i<sections;++i) {
        const size_t p=nt+24+optional+i*40;
        uint32_t rva{},raw{},offset{},flags{};
        if (!load(p+12,rva) || !load(p+16,raw) || !load(p+20,offset) || !load(p+36,flags) ||
            rva>a::image_size || raw>a::image_size-rva || offset>file.size() || raw>file.size()-offset) {
            expect(false,"ABZU section table has bounded raw/RVA ranges"); return;
        }
        f.add(base+rva,raw,(flags&0x20000000)!=0);
        std::memcpy(f.blocks.back().bytes.data(),file.data()+offset,raw);
    }
    uintptr_t renderer{};
    expect(f.memory().load(base+a::renderer_vtable+a::renderer_slot*8,renderer) && renderer == base+a::code::renderer_prefix_rva &&
        a::family_contract(f.memory(),base) && a::exact(f.memory(),base,a::code::renderer_getter_rva,a::code::renderer_getter) &&
        a::exact(f.memory(),base,a::code::renderer_prefix_rva,a::code::renderer_prefix) &&
        a::constructor(f.memory(),base).has_value() && a::factory(f.memory(),base) && a::viewport_rect_contract(f.memory(),base),
        "ABZU unchanged shipping EXE satisfies every constructor, capture, gamma, native-resource and viewport contract");
}
} // namespace abzu_tests
