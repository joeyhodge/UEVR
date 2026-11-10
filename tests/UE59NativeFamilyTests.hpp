#pragma once

#include "mods/vr/UE59NativeFix.hpp"

namespace ue59_family_tests {
namespace n=uevr::ue58_native;

template<bool Wide> struct Native {
    static constexpr size_t size=Wide?0x1b8:0x1a8, interfaces=Wide?0x178:0x168;
    inline static Native* active{};
    uint64_t before{0x1122334455667788};
    alignas(16) std::array<uint8_t,size> source{};
    uint64_t after{0x8877665544332211};
    std::array<uintptr_t,2> views{0x20000,0x30000};
    n::Interfaces destroyed{};
    int copies{},destroys{};
    bool alias{};
    template<class T> static void put(void* p,size_t o,const T& v) { std::memcpy(static_cast<uint8_t*>(p)+o,&v,sizeof(v)); }
    Native() {
        active=this;
        put(source.data(),0,uintptr_t{0x40000});
        put(source.data(),8,n::ViewArray{reinterpret_cast<uintptr_t>(views.data()),2,2});
        if constexpr(Wide) { put(source.data(),0x160,uintptr_t{0x12345678}); }
        put(source.data(),interfaces,n::Interfaces{0x70000,0,0,0,0});
        source[0xb7]=1;
    }
    ~Native() { active=nullptr; }
    static void* copy(void* to,const void* from) {
        ++active->copies;
        std::memcpy(to,from,size);
        put(to,0,uintptr_t{0x80000});
        auto* entries=active->alias?active->views.data():new uintptr_t[2]{active->views[0],active->views[1]};
        put(to,8,n::ViewArray{reinterpret_cast<uintptr_t>(entries),2,2});
        return to;
    }
    static void destroy(void* family) {
        ++active->destroys;
        std::memcpy(active->destroyed.data(),static_cast<uint8_t*>(family)+interfaces,sizeof(active->destroyed));
        n::ViewArray entries{};
        std::memcpy(&entries,static_cast<uint8_t*>(family)+8,sizeof(entries));
        delete[] reinterpret_cast<uintptr_t*>(entries.data);
    }
    static sdk::discovery::Memory memory() {
        return {nullptr,[](void*,uintptr_t p,void* out,size_t size) {
            if(p<0x10000) { return false; }
            std::memcpy(out,reinterpret_cast<const void*>(p),size); return true;
        },nullptr};
    }
    static n::FamilyFunctions functions() { return {reinterpret_cast<uintptr_t>(&copy),reinterpret_cast<uintptr_t>(&destroy),0x80000,Wide}; }
};

template<bool Wide,class Expect> void run_layout(Expect&& expect) {
    Native<Wide> native;
    const auto before=native.source;
    const char* reason{};
    {
        uevr::ue59_native::FamilyClone clone;
        expect(clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason),
            "5.9 family transaction selects proven Shipping/Development storage, not a version-guessed size");
        expect(clone.address() && clone.finish(native.memory(),reinterpret_cast<uintptr_t>(native.source.data())),
            "both family layouts preserve five independently checked interface owners");
        expect(!clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason),
            "family storage cannot be reinitialized across a running transaction");
    }
    expect(native.source==before && native.copies==1 && native.destroys==1 && native.destroyed[0]==0 &&
        native.before==0x1122334455667788 && native.after==0x8877665544332211,
        "family cleanup preserves source bytes/canaries and detaches the borrowed interface before destruction");
    native.alias=true;
    {
        uevr::ue59_native::FamilyClone clone;
        expect(!clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason),
            "a shallow array cannot authorize either family layout");
    }
    expect(native.views==std::array<uintptr_t,2>{0x20000,0x30000} && native.destroys==2 && native.source==before,
        "rejected family cleanup never frees the original viewport array");
    Native<Wide>::put(native.source.data(),Native<Wide>::interfaces-sizeof(uintptr_t),uintptr_t{0x99999});
    const auto consumed=native.source;
    {
        uevr::ue59_native::FamilyClone clone;
        expect(!clone.initialize(native.memory(),reinterpret_cast<uintptr_t>(native.source.data()),0x40000,native.functions(),reason) &&
            !clone.original_render_safe(),"already-consumed family rejects using its own renderer-field offset");
    }
    expect(native.source==consumed && native.copies==2,"consumed family never calls the native copy constructor");
}
struct CodeMemory {
    std::array<uint8_t,0x6000> bytes{};
    static constexpr uintptr_t base=0x100000;
    template<class T> void put(uintptr_t p,const T& v) { std::memcpy(bytes.data()+p-base,&v,sizeof(v)); }
    sdk::discovery::Memory view() {
        return {this,[](void* c,uintptr_t p,void* out,size_t n) {
            const auto& m=*static_cast<CodeMemory*>(c);
            if(p<base || p-base>m.bytes.size() || n>m.bytes.size()-(p-base)) { return false; }
            std::memcpy(out,m.bytes.data()+p-base,n); return true;
        },[](void* c,uintptr_t p,size_t n) {
            const auto& m=*static_cast<CodeMemory*>(c);
            return p>=base && p-base<=m.bytes.size() && n<=m.bytes.size()-(p-base);
        }};
    }
};
template<class Expect> void run(Expect&& expect) {
    run_layout<false>(expect); run_layout<true>(expect);
    namespace c=uevr::ue59_native::contract;
    CodeMemory memory;
    const auto copy=memory.base+0x100, deleting=memory.base+0x1000, destroy=memory.base+0x1100;
    const auto renderer=memory.base+0x3000, table=memory.base+0x5000;
    memory.put(copy,c::development_family_copy_code);
    memory.put(deleting,c::development_family_delete_code);
    memory.put(destroy,c::development_family_destroy_code);
    memory.put(renderer+0x30,c::development_renderer_code);
    memory.put(table,deleting);
    memory.put(copy+0x25,int32_t(table-(copy+0x29)));
    memory.put(deleting+0x10,int32_t(destroy-(deleting+0x14)));
    memory.put(destroy+9,int32_t(table-(destroy+0xd)));
    const auto size=[&](uintptr_t p)->size_t {
        return p==copy ? c::development_family_copy_code.size() : p==deleting ? c::development_family_delete_code.size() :
            p==destroy ? c::development_family_destroy_code.size() : 0;
    };
    const auto resolve=[&] { return uevr::ue59_native::family_functions(memory.view(),copy,memory.base,memory.bytes.size(),size); };
    const auto functions=resolve();
    expect(functions && functions->development_layout && functions->vtable==table && functions->destroy==destroy,
        "Development family requires complete copy/delete/destroy contracts and a common vtable");
    const auto mutate=[&](uintptr_t p,const auto& mask,auto&& validate) {
        for(size_t i=0;i<mask.size();++i) {
            if(!mask[i]) { continue; }
            memory.bytes[p-memory.base+i]^=1;
            expect(!validate(),"every unmasked ownership/member/control-flow byte is required in the Development contract");
            memory.bytes[p-memory.base+i]^=1;
        }
    };
    mutate(copy,c::development_family_copy_mask,resolve);
    mutate(deleting,c::development_family_delete_mask,resolve);
    mutate(destroy,c::development_family_destroy_mask,resolve);
    const auto render=[&] { return uevr::ue59_native::renderer_layout(memory.view(),renderer); };
    expect(render(),"Development renderer independently proves span, scene, frame and virtual-call ABI");
    memory.bytes[renderer-memory.base]=0xe9;
    expect(render(),"installed entry hooks cannot invalidate the unmodified body contract");
    mutate(renderer+0x30,c::development_renderer_mask,render);
    memory.put(destroy+9,int32_t(table+8-(destroy+0xd)));
    expect(!resolve(),"a relocated destructor must still reference the same proven family vtable");
}
} // namespace ue59_family_tests
