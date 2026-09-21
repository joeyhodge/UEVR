#pragma once

#include <fstream>
#include <iterator>
#include "mods/vr/KtjLCloudResources.hpp"

void test_ktjl_cloud_memory_image(const char* path) {
    // A flat module snapshot uses RVAs as file offsets. This proves the actual
    // instruction/vtable contracts, not live page permissions or GPU contents.
    std::ifstream input{path, std::ios::binary};
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    expect(bytes.size() == sdk::ktjl::image_size, "KTJL flat image has the expected extent");
    const sdk::discovery::Memory memory{&bytes,
        [](void* context, uintptr_t address, void* out, size_t size) {
            const auto& data = *static_cast<std::vector<uint8_t>*>(context);
            constexpr uintptr_t base = 0x140000000;
            if (address < base || address - base > data.size() || size > data.size() - (address - base)) { return false; }
            std::memcpy(out, data.data() + address - base, size); return true;
        },
        [](void* context, uintptr_t address, size_t size) {
            const auto& data = *static_cast<std::vector<uint8_t>*>(context);
            constexpr uintptr_t base = 0x140000000;
            return address >= base && address - base <= data.size() && size <= data.size() - (address - base);
        }};
    expect(uevr::ktjl::cloud::validate_code(memory, 0x140000000), "cloud/pool/cleanup contracts match the captured KTJL executable");
    expect(uevr::ktjl::hooks::validates_texture(memory, 0x140000000) &&
        uevr::ktjl::hooks::validates_native_texture(memory, 0x140000000),
        "typed allocator, C0->20 getter and typeless mapping match the captured executable");
    namespace r = uevr::ktjl::renderer;
    expect(r::resolve(memory, 0x140000000, 0x140000000 + r::callback_return_rva,
        {0x140000000, r::entry_rva, r::end_rva, r::unwind_rva}, ktjl_renderer_test_layout()) == 0x140000000 + r::entry_rva,
        "callable renderer, +0x40 frame provenance, extension loop and unwind match the captured executable");
}

void test_ktjl_cloud_resources() {
    namespace c = uevr::ktjl::cloud;
    namespace f = uevr::ktjl::fog;
    namespace k = sdk::ktjl;
    namespace h = uevr::ktjl::hooks;
    constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
    constexpr uintptr_t left = 0x62000000, right = 0x62001000, states = 0x63000000;
    constexpr uintptr_t right_state = states + 0x10000;
    constexpr uintptr_t slot = right_state + c::sky_ao_offset;
    struct Fixture {
        struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool executable{}; };
        std::vector<Block> blocks;
        size_t reads{};
        void add(uintptr_t address, const void* p, size_t n, bool code = false) {
            const auto first = static_cast<const uint8_t*>(p);
            blocks.push_back({address, {first, first + n}, code});
        }
        void put(uintptr_t address, const void* p, size_t n) {
            for (auto& b : blocks) {
                if (address >= b.address && address - b.address <= b.bytes.size() && n <= b.bytes.size() - (address - b.address)) {
                    std::memcpy(b.bytes.data() + address - b.address, p, n); return;
                }
            }
            expect(false, "KTJL cloud fixture writes stay in mapped storage");
        }
        sdk::discovery::Memory memory() {
            return {this,
                [](void* c, uintptr_t a, void* p, size_t n) {
                    auto& self = *static_cast<Fixture*>(c); ++self.reads;
                    for (const auto& b : self.blocks) {
                        if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                            std::memcpy(p, b.bytes.data() + a - b.address, n); return true;
                        }
                    }
                    return false;
                },
                [](void* c, uintptr_t a, size_t n) {
                    for (const auto& b : static_cast<Fixture*>(c)->blocks) {
                        if (b.executable && a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) { return true; }
                    }
                    return false;
                }};
        }
    };
    const auto resource = [&](uintptr_t texture, int32_t width = 512) {
        std::array<uint8_t, 0xF0> data{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(data.data() + offset, &value, sizeof(value)); };
        put(0, base + f::pool_vtable_rva); put(8, texture); put(0x10, texture); put(0x18, texture + 0x100);
        put(0x88, int32_t{2}); put(0xE8, base + f::pool_rva);
        put(0x90 + 0x14, width); put(0x90 + 0x18, width); put(0x90 + 0x20, int32_t{1});
        put(0x90 + 0x26, uint16_t{1}); put(0x90 + 0x28, uint16_t{1});
        put(0x90 + 0x2C, uint32_t{26}); put(0x90 + 0x34, uint32_t{0x10009});
        put(0x90 + 0x48, uint8_t{1});
        return data;
    };
    const auto fixture = [&] {
        Fixture x;
        std::array<uint8_t, 512> pe{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(pe.data() + offset, &value, sizeof(value)); };
        put(0, uint16_t{0x5A4D}); put(0x3C, uint32_t{0x80}); put(0x80, uint32_t{0x4550});
        put(0x84, uint16_t{0x8664}); put(0x88, k::image_timestamp); put(0x98, uint16_t{0x20B}); put(0xD0, k::image_size);
        x.add(base, pe.data(), pe.size());
        const auto code = [&](uintptr_t rva, const auto& bytes) { x.add(base + rva, bytes.data(), bytes.size(), true); };
        code(0xAFFF76, k::class_name_access); code(0xA1DC2F, k::object_iteration); code(0x58DC4E, k::name_entry_access);
        for (const auto& e : f::code_evidence) { code(e.rva, e.bytes); }
        for (const auto& e : c::code_evidence) { code(e.rva, e.bytes); }
        const auto accessor = base + f::get_desc_rva, release = base + 0x55AAE0, destructor = base + 0x57836D0;
        const auto state_destructor = base + 0x1986F94;
        x.add(base + f::pool_vtable_rva + 0x10, &accessor, 8); x.add(base + f::pool_vtable_rva + 0x38, &release, 8);
        x.add(base + f::view_vtable_rva, &destructor, 8); x.add(base + k::stereo::state_vtable_rva, &state_destructor, 8);
        const f::Header header{views, 2, 2}; x.add(renderer + f::views_offset, &header, sizeof(header));
        for (size_t i = 0; i < 2; ++i) {
            const auto state = i == 0 ? states : right_state;
            const f::ViewPrefix view{base + f::view_vtable_rva, 0, renderer + 0x10, state};
            const auto vt = base + k::stereo::state_vtable_rva, ref = i == 0 ? left : uintptr_t{};
            x.add(views + f::view_stride * i, &view, sizeof(view)); x.add(state, &vt, 8);
            x.add(views + f::view_stride * i + c::state_offset, &state, 8);
            x.add(state + c::sky_ao_offset, &ref, 8);
        }
        const auto a = resource(0x64000000), b = resource(0x65000000);
        x.add(left, a.data(), a.size()); x.add(right, b.data(), b.size());
        return x;
    };
    const auto mutate = [](Fixture& x, uintptr_t address, auto value) { x.put(address, &value, sizeof(value)); };
    auto x = fixture();
    expect(c::validate_code(x.memory(), base), "KTJL sky-AO producer/consumer, RHI context and cleanup all validate");
    for (const auto& e : c::code_evidence) {
        for (size_t i = 0; i < e.bytes.size(); ++i) {
            x = fixture(); mutate(x, base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!c::validate_code(x.memory(), base), "changed cloud contract rejects installation");
        }
        for (int mode = 0; mode < 2; ++mode) {
            x = fixture();
            for (auto& b : x.blocks) {
                if (b.address == base + e.rva) {
                    if (mode == 0) { b.bytes.pop_back(); } else { b.executable = false; }
                }
            }
            expect(!c::validate_code(x.memory(), base), "truncated or non-executable cloud contract rejects installation");
        }
    }
    for (auto address : {base + 0x88, base + 0xD0, base + k::stereo::state_vtable_rva}) {
        x = fixture(); mutate(x, address, uint32_t{});
        expect(!c::validate_code(x.memory(), base), "wrong image or cleanup contract cannot bootstrap stereo");
    }
    x = fixture();
    auto plan = c::prepare(x.memory(), base, renderer, views + f::view_stride, true);
    expect(plan.decision == f::Decision::allocate && plan.right_slot == slot && x.reads < 24,
        "crash fixture resolves the missing right state+1f28 with bounded reads");
    const auto before = x.blocks;
    size_t calls{};
    auto allocate = [&](const c::Plan& p) { ++calls; mutate(x, p.right_slot, right); return true; };
    auto run = [&](bool enabled = true, uintptr_t view = 0) {
        return c::ensure(x.memory(), base, renderer, view == 0 ? views : view, enabled, allocate);
    };
    expect(run() == f::Outcome::allocated && calls == 1, "initialize the missing resource before the first per-eye import");
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i].address != slot) { expect(before[i].bytes == x.blocks[i].bytes, "only the right owned reference may change"); }
    }
    for (size_t i = 0; i < 100; ++i) {
        expect(run(true, views + f::view_stride) == f::Outcome::existing && calls == 1,
            "subsequent eyes/frames reuse the validated independent resource");
    }
    for (int cycle = 0; cycle < 5; ++cycle) {
        mutate(x, slot, uintptr_t{});
        expect(run() == f::Outcome::allocated, "a new world/view-state lifetime is not blocked by a stale cache");
    }
    x = fixture(); calls = 0;
    mutate(x, left + 0x90 + 0x30, uint32_t{0x80000000});
    mutate(x, right + 0x90 + 0x30, uint32_t{0x80000000});
    expect(run() == f::Outcome::allocated && calls == 1,
        "the pool's Transient flag is preserved without accepting unrelated flags");
    x = fixture(); calls = 0;
    expect(run(false) == f::Outcome::passthrough && x.reads == 0 && calls == 0, "disabled sky AO remains untouched");
    for (int count : {0, 1, 3, 4}) {
        x = fixture(); mutate(x, renderer + f::views_offset + 8, count);
        expect(run() == f::Outcome::passthrough && calls == 0, "mono, AFR and non-pair captures remain untouched");
    }
    x = fixture();
    expect(run(true, views + 8) == f::Outcome::rejected && calls == 0, "a view outside the exact pair cannot authorize writes");
    for (auto address : {views, views + f::view_stride, views + 0x10, views + f::view_stride + 0x10,
                        views + 0x18, states, right_state, views + c::state_offset,
                        views + f::view_stride + c::state_offset, states + c::sky_ao_offset,
                        left, left + 8, left + 0x10, left + 0x18, left + 0x88, left + 0xE8}) {
        x = fixture(); mutate(x, address, uintptr_t{});
        expect(run() == f::Outcome::rejected && calls == 0, "invalid owner/resource fails before any allocator call");
    }
    x = fixture(); mutate(x, slot, left);
    expect(run() == f::Outcome::rejected && calls == 0, "never alias the left pooled target into the right state");
    x = fixture(); mutate(x, right + 8, uintptr_t{0x64000000});
    expect(run() == f::Outcome::rejected, "allocator outputs may not alias the left native texture");
    for (auto offset : {0x00, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x25, 0x26, 0x28, 0x2C, 0x30, 0x34, 0x38}) {
        x = fixture();
        for (auto& block : x.blocks) {
            if (block.address == left) { block.bytes[0x90 + offset] ^= 0x10; }
        }
        expect(run() == f::Outcome::rejected, "unknown extent/clear/format/flag/sample layouts cannot call the pool");
    }
    x = fixture();
    expect(c::ensure(x.memory(), base, renderer, views, true, [](const c::Plan&) { return false; }) == f::Outcome::rejected,
        "allocation refusal skips the unsafe import");
    expect(c::ensure(x.memory(), base, renderer, views, true, [](const c::Plan&) { return true; }) == f::Outcome::rejected,
        "an allocator return alone does not validate the owned reference");
    x = fixture(); mutate(x, slot, right);
    auto small = resource(0x65000000, 256); x.put(right, small.data(), small.size());
    expect(c::prepare(x.memory(), base, renderer, views, true).decision == f::Decision::allocate,
        "a validated stale-resolution target is replaced through the engine pool");
    expect(c::ensure(x.memory(), base, renderer, views, true, [&](const c::Plan&) {
        const auto replacement = resource(0x66000000); x.put(right, replacement.data(), replacement.size()); return true;
    }) == f::Outcome::allocated, "resolution replacement requires full matching postconditions");
    x = fixture(); plan = c::prepare(x.memory(), base, renderer, views, true); mutate(x, slot, right);
    mutate(x, renderer + f::views_offset, views + 0x100000);
    expect(!c::allocation_completed(x.memory(), base, plan), "changed view storage cannot publish a stale allocation");

    // Native texture validation uses the actual getter contract, not a guessed virtual call.
    Fixture texture;
    texture.add(base + h::native_resource_rva, h::native_resource_code.data(), h::native_resource_code.size(), true);
    texture.add(base + h::bgra_format_rva, h::bgra_format_code.data(), h::bgra_format_code.size(), true);
    const auto accessor = base + h::native_resource_rva, table = base + h::texture_vtable_rva;
    constexpr uintptr_t tex = 0x68000000, wrapper = 0x68010000, native = 0x68020000;
    texture.add(table + 8 * 8, &accessor, 8); texture.add(tex, &table, 8);
    texture.add(tex + 0xC0, &wrapper, 8); texture.add(wrapper + 0x20, &native, 8);
    expect(h::validates_native_texture(texture.memory(), base), "KTJL texture getter and BGRA typeless mapping validate");
    expect(h::read_native_texture(texture.memory(), base, tex) == native, "read exact C0->20 chain without virtual calls");
    for (auto address : {tex, tex + 0xC0, wrapper + 0x20}) {
        auto changed = texture; mutate(changed, address, uintptr_t{});
        expect(!h::read_native_texture(changed.memory(), base, tex), "incomplete native resource chains fail closed");
    }
    for (auto address : {base + h::native_resource_rva, base + h::bgra_format_rva, table + 8 * 8}) {
        auto changed = texture; mutate(changed, address, uint8_t{});
        expect(!h::validates_native_texture(changed.memory(), base), "changed getter/format/vtable fails closed");
    }
    const h::TextureDescription desc{3, 90, 1, 1080, 1920, 1, 1, 1, 0};
    expect(h::valid_texture_description(desc, 1920, 1080), "BGRA TYPELESS backing storage is valid, not falsely rejected");
    for (uint32_t format : {0, 2, 24, 28, 87, 90, 91}) {
        auto d = desc; d.format = format;
        expect(h::valid_texture_description(d, 1920, 1080) == (format == 87 || format == 90), "accept only the proven linear BGRA format family");
    }
    for (int field = 0; field < 10; ++field) {
        auto d = desc;
        switch (field) {
        case 0: d.dimension = 4; break;
        case 1: d.width = 1919; break;
        case 2: d.height = 1079; break;
        case 3: d.array_size = 2; break;
        case 4: d.mips = 2; break;
        case 5: d.samples = 2; break;
        case 6: d.quality = 1; break;
        case 7: d.flags = 0; break;
        case 8: d.flags |= 8; break;
        case 9: d.flags |= 2; break;
        }
        expect(!h::valid_texture_description(d, 1920, 1080), "typeless acceptance never weakens other descriptor validation");
    }
}
