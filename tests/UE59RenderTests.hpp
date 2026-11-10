#pragma once

#include <array>
#include <cstring>
#include <span>
#include "mods/vr/UE59Slate.hpp"
#include "mods/vr/CompatibilityPolicy.hpp"
#include "mods/vr/UE59ViewExtension.hpp"
#include "mods/vr/UE59NativeFix.hpp"

namespace ue59_render_tests {
template<class Expect> void run(Expect&& expect) {
    namespace ui = uevr::ue59_slate;
    using namespace uevr::vr_compatibility;
    {
        const bool lazy=sdk::ue59::lazy_viewstates(sdk::ue59::Identity::ReviewedSnapshot);
        expect(lazy && is_ghosting_bootstrap_allocator_ready(false,lazy,true,true) &&
            !is_ghosting_bootstrap_allocator_ready(true,lazy,false,true) &&
            !is_ghosting_bootstrap_allocator_ready(true,lazy,true,false),
            "UE5.9 lazy bootstrap requires main Draw and the live view-count hook, not a PostInit flag");
        expect(should_enable_ghosting_remap(true,true,false,false,false) &&
            !should_enable_ghosting_remap(false,true,false,false,false) &&
            !should_enable_ghosting_remap(true,false,false,false,false) &&
            !should_enable_ghosting_remap(true,true,true,false,false) &&
            !should_enable_ghosting_remap(true,true,false,true,false) &&
            !should_enable_ghosting_remap(true,true,false,false,true),
            "5.9 Synced/AFR Ghost is opt-in and remains separate from Native/Fix and other compatibility modes");
        expect(is_native_stereo_fix_active(true,RenderingMethod::NativeStereo,false) &&
            !is_native_stereo_fix_active(false,RenderingMethod::NativeStereo,false) &&
            !is_native_stereo_fix_active(true,RenderingMethod::NativeStereo,true),
            "UE5.9 Native and Native Fix do not enable each other's transaction implicitly");
    }
    {
        std::array<uint8_t,0x200> code{};
        const auto base=reinterpret_cast<uintptr_t>(code.data());
        std::memcpy(code.data()+0x2a,uevr::ue58_native::contract::renderer_span_code.data(),uevr::ue58_native::contract::renderer_span_code.size());
        std::memcpy(code.data()+0x19c,uevr::ue59_native::renderer_frame_code.data(),uevr::ue59_native::renderer_frame_code.size());
        sdk::discovery::Memory memory{&code,[](void* c,uintptr_t p,void* out,size_t n) {
            const auto& data=*static_cast<std::array<uint8_t,0x200>*>(c);
            const auto b=reinterpret_cast<uintptr_t>(data.data());
            if(p<b || p-b>data.size() || n>data.size()-(p-b)) { return false; }
            std::memcpy(out,data.data()+p-b,n); return true;
        },[](void* c,uintptr_t p,size_t n) {
            const auto& data=*static_cast<std::array<uint8_t,0x200>*>(c);
            const auto b=reinterpret_cast<uintptr_t>(data.data());
            return p>=b && p-b<=data.size() && n<=data.size()-(p-b);
        }};
        expect(uevr::ue59_native::renderer_layout(memory,base) && !uevr::ue58_native::renderer_layout(memory,base),
            "5.9 scene-interface call has a separate contract, not a widened 5.8 gate");
        for(size_t i=0;i<uevr::ue59_native::renderer_frame_code.size();++i) {
            code[0x19c+i]^=1;
            expect(!uevr::ue59_native::renderer_layout(memory,base),"every renderer member/control-flow/virtual-slot byte remains required");
            code[0x19c+i]^=1;
        }
        for(size_t i=0;i<uevr::ue58_native::contract::renderer_span_code.size();++i) {
            code[0x2a+i]^=1;
            expect(!uevr::ue59_native::renderer_layout(memory,base),"changed plural renderer span ABI cannot authorize a second submission");
            code[0x2a+i]^=1;
        }
    }
    expect(!is_validated_ue58_slate_source_version(0x50009,0) &&
        is_validated_modern_slate_source_version(0x50009,0), "5.9 gets its own explicit reviewed Slate contract");
    for (uint32_t patch=0; patch<=4; ++patch) {
        expect(is_validated_modern_slate_source_version(0x50008,patch<<16) &&
            is_validated_ue58_slate_source_version(0x50008,patch<<16), "all previously reviewed UE5.8 patches remain admitted");
    }
    for (const auto version : {std::array<uint32_t,2>{0x50009,0x10000}, {0x5000a,0}, {0x5005a,0}, {0x60000,0}, {0x50008,0x50000}}) {
        expect(!is_validated_modern_slate_source_version(version[0],version[1]), "future minor/patch Slate layouts are not implicitly enabled");
    }
    ui::DrawInputs input{};
    input.renderer=0x110000; input.elements=0x120000; input.window=0x130000; input.viewport=0x140000;
    // Poisoned screenshot pointers must remain opaque to the UI reader.
    input.screenshot={{-100,-200,3,4},UINTPTR_MAX,UINTPTR_MAX-1,UINTPTR_MAX-2};
    input.cursor={123,456}; input.scene={0,0,2000,1000}; input.scale=1.25f;
    const auto bytes=std::span{reinterpret_cast<const uint8_t*>(&input),sizeof(input)};
    const auto decoded=ui::decode(bytes,input.renderer);
    expect(decoded && decoded->cursor==input.cursor && decoded->scene.right==2000 &&
        decoded->scene.bottom==1000 && decoded->scale==1.25f && decoded->screenshot.colors==UINTPTR_MAX-1,
        "5.9 reader skips screenshot fields and preserves correct cursor/scene rect/scale");
    for (size_t size=0; size<sizeof(input); ++size) {
        expect(!ui::decode(bytes.first(size),input.renderer), "truncated new Slate prefix is never read as an older layout");
    }
    expect(!ui::decode(bytes,0) && !ui::decode(bytes,0x220000), "wrong renderer and missing owner are rejected");
    auto bad=input; bad.window=0;
    expect(!ui::decode({reinterpret_cast<const uint8_t*>(&bad),sizeof(bad)},input.renderer), "missing live window cannot publish UI metadata");
    bad=input; bad.viewport=0;
    expect(!ui::decode({reinterpret_cast<const uint8_t*>(&bad),sizeof(bad)},input.renderer), "missing viewport cannot publish UI metadata");
    // A decoder cannot establish actual window/resource ownership; production
    // still performs the independent checked-pointer and texture validations.
    expect(input.renderer==0x110000 && input.screenshot.viewport==UINTPTR_MAX,
        "read-only decoding never mutates engine input or screenshots");

    const nlohmann::json cached{{"ue59_schema",1},{"index_0_called",true},{"is_active_this_frame_index",21},
        {"begin_render_viewfamily_index",4},{"pre_render_viewfamily_renderthread_index",6},{"frame_count_offset",0xa0}};
    expect(uevr::ue59_extension::valid_cache(cached), "only versioned 5.9 cache topology is admitted");
    expect(uevr::ue59_extension::source_discovery_eligible(true,false,true,21,true,false),
        "live 5.9 stage 2 admits observed slot 21 so validated render callbacks can publish real frames");
    expect(!uevr::ue59_extension::source_discovery_eligible(true,false,true,20,true,false),
        "live 5.9 discovery cannot reuse the old active slot 20");
    for (const auto active_slot : {20u,21u,UINT32_MAX}) {
        for (uint32_t flags=0; flags<32; ++flags) {
            const bool reviewed59=(flags&1)!=0, legacy=(flags&2)!=0, active=(flags&4)!=0;
            const bool setup=(flags&8)!=0, mapped=(flags&16)!=0;
            const bool expected=(reviewed59 || legacy) && active && setup && !mapped &&
                active_slot==(reviewed59 ? 21u : 20u);
            expect(uevr::ue59_extension::source_discovery_eligible(reviewed59,legacy,active,active_slot,setup,mapped)==expected,
                "stage 2 preserves legacy slot 20, rejects unknown profiles and requires active/setup evidence before mapping");
        }
    }
    for (const auto& [key, value] : cached.items()) {
        auto stale=cached; stale.erase(key);
        expect(!uevr::ue59_extension::valid_cache(stale), "incomplete view-extension caches are refused");
        stale=cached; stale[key]="invalid";
        expect(!uevr::ue59_extension::valid_cache(stale), "malformed cache fields fail closed without throwing");
    }
    for (const auto value : {nlohmann::json(-1), nlohmann::json(0x100000000ull), nlohmann::json(UINT64_MAX), nlohmann::json(4.0)}) {
        auto stale=cached; stale["begin_render_viewfamily_index"]=value;
        expect(!uevr::ue59_extension::valid_cache(stale), "negative, overflowing and floating-point slots cannot wrap into a valid index");
    }
}
} // namespace ue59_render_tests
