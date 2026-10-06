#pragma once

#include <utility>
#include "mods/vr/GalacticRacerOwnedTexture.hpp"

void test_swgr_owned_texture() {
    namespace o = uevr::swgr_owned;
    swgr_tests::Memory m;
    const auto b = m.base, owner = b + 0x3100, resource = b + 0x4000;
    const auto rhi = b + 0x5000, table = b + 0x5200, rt_table = b + 0x5400;
    m.put(owner + 0x130, resource); m.put(owner + 0x138, resource);
    m.put(resource, table); m.put(resource + o::render_target_offset, rt_table);
    m.put(table + 6 * 8, b + 0x100); m.put(b + 0x100, o::size_x_code);
    m.put(table + 7 * 8, b + 0x200); m.put(b + 0x200, o::size_y_code);
    m.put(rt_table + 2 * 8, b + 0x300); m.put(b + 0x300, o::texture_code);
    m.put(resource + o::owner_offset, owner);
    m.put(resource + 0x10, rhi); m.put(resource + o::render_target_offset + 8, rhi);
    m.put(resource + o::width_offset, uint32_t{2472}); m.put(resource + o::height_offset, uint32_t{2416});
    const auto original = m;
    const auto identity = o::find_resource(m.view(), owner, 0x200, 2472, 2416);
    expect(m.bytes == original.bytes, "fixture resource validation is read-only");
    expect(identity && identity->private_resource_offset == 0x130 && identity->resource == resource &&
        identity->rhi_texture == rhi, "SWGR proves an owned target without global UTexture offsets or virtual calls");
    if (!identity) { return; }
    const auto reflected_size_identity = o::find_resource(m.view(), owner, 0x180, 2472, 2416);
    expect(reflected_size_identity && reflected_size_identity->private_resource_offset == 0x130 &&
        reflected_size_identity->owner_size == 0x180, "owned scan fits SWGR's reflected TextureRenderTarget2D size");
    expect(o::resource_matches(m.view(), owner, *identity, 2472, 2416), "SWGR capture consumers revalidate the published chain");
    m.put(owner + 0x1a8, uintptr_t{0x10100000007});
    expect(o::find_resource(m.view(), owner, 0x200, 2472, 2416) == identity &&
        o::resource_matches(m.view(), owner, *identity, 2472, 2416),
        "SWGR ignores the exact crash-causing unrelated field at +0x1a8");
    m.put(owner + 0x130, uintptr_t{}); m.put(owner + 0x138, uintptr_t{});
    expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416) &&
        !o::resource_matches(m.view(), owner, *identity, 2472, 2416), "pending owner refs never adopt the decoy RHI field");
    for (auto p : {owner + 0x130, owner + 0x138, resource + o::owner_offset,
                  resource + 0x10, resource + o::render_target_offset + 8}) {
        m = original; m.put(p, uintptr_t{});
        expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416) &&
            !o::resource_matches(m.view(), owner, *identity, 2472, 2416), "changed owner/GT/RT/RHI references fail closed");
    }
    for (auto p : {resource + o::width_offset, resource + o::height_offset}) {
        m = original; m.put(p, uint32_t{1});
        expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416), "resize cannot reuse an old target identity");
    }
    for (const auto [p, size] : {std::pair{b + 0x100, o::size_x_code.size()},
            std::pair{b + 0x200, o::size_y_code.size()}, std::pair{b + 0x300, o::texture_code.size()}}) {
        for (size_t i = 0; i < size; ++i) {
            m = original; m.bytes[p - b + i] ^= 1;
            expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416), "unproved resource accessors never publish an offset");
        }
    }
    m = original; m.executable = false;
    expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416), "resource accessors require executable-image proof");
    m = original; m.readable = false;
    expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416), "unreadable resource chains remain pending");
    m = original; m.put(owner + 0x170, resource); m.put(owner + 0x178, resource);
    expect(!o::find_resource(m.view(), owner, 0x200, 2472, 2416), "ambiguous owner fields reject discovery");
    for (auto size : {size_t{0}, size_t{8}, size_t{0x301}}) {
        expect(!o::find_resource(m.view(), owner, size, 2472, 2416), "owned target scans are strictly size-bounded");
    }
    m = original;
    for (auto p : {uintptr_t{}, uintptr_t{0xffff}, uintptr_t{0x800000000000}, UINTPTR_MAX}) {
        expect(!o::find_resource(m.view(), p, 0x200, 2472, 2416), "noncanonical/unmapped owners fail closed");
        auto invalid = *identity; invalid.resource = p;
        expect(!o::resource_matches(m.view(), owner, invalid, 2472, 2416), "invalid published resource is never dereferenced");
        invalid = *identity; invalid.rhi_texture = p;
        expect(!o::resource_matches(m.view(), owner, invalid, 2472, 2416), "invalid published RHI is never adopted");
    }
    for (auto extent : {uint32_t{0}, uint32_t{65537}, UINT32_MAX}) {
        expect(!o::find_resource(m.view(), owner, 0x200, extent, 2416) &&
            !o::find_resource(m.view(), owner, 0x200, 2472, extent), "invalid requested extents reject discovery");
        expect(!o::resource_matches(m.view(), owner, *identity, extent, 2416) &&
            !o::resource_matches(m.view(), owner, *identity, 2472, extent), "invalid extents reject published identities");
    }
    for (auto offset : {uintptr_t{0}, uintptr_t{0x131}, uintptr_t{0x1f8}, UINTPTR_MAX}) {
        auto invalid = *identity; invalid.private_resource_offset = offset;
        expect(!o::resource_matches(m.view(), owner, invalid, 2472, 2416), "invalid published offsets fail closed");
    }
    expect(!o::find_resource(m.view(), 0x10100000007, 0x200, 2472, 2416), "the unaligned crash pointer is rejected before dereference");
    m = original;
    m.put(owner + 0x130, uintptr_t{}); m.put(owner + 0x138, uintptr_t{});
    m.put(owner + 0x150, resource); m.put(owner + 0x158, resource);
    const auto relocated = o::find_resource(m.view(), owner, 0x200, 2472, 2416);
    expect(relocated && relocated->private_resource_offset == 0x150,
        "SWGR does not hard-code the observed +0x130 owner field");
}
