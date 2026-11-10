#pragma once

#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>
#include <sdk/UE59ViewLayout.hpp>
#include "mods/vr/UE59NativeFix.hpp"

namespace ue59_binary_tests {

// Optional read-only integration test. The manifest comes from the matching
// PDB public symbols; no game/engine code is executed or redistributed.
template<class Expect> void image(const char* path, const char* manifest_path, Expect&& expect) {
    struct Image {
        std::ifstream file;
        uint64_t size{},base{};
        uint32_t module_size{};
        struct Section { uint32_t rva,size,offset,flags; };
        std::vector<Section> sections;
        std::optional<uint64_t> offset(uintptr_t p,size_t n,bool code) const {
            if (p<base) { return {}; }
            const auto rva=p-base;
            for (const auto& s:sections) {
                if (rva<s.rva || rva-s.rva>s.size || n>s.size-(rva-s.rva) ||
                    (code && !(s.flags&0x20000000))) { continue; }
                const auto o=uint64_t{s.offset}+rva-s.rva;
                if (o<=size && n<=size-o) { return o; }
            }
            return {};
        }
    } image;
    image.file.open(path,std::ios::binary|std::ios::ate);
    if (!image.file) { expect(false,"open UE5.9 engine image"); return; }
    image.size=static_cast<uint64_t>(image.file.tellg());
    std::array<uint8_t,65536> header{};
    image.file.seekg(0);
    if (!image.file.read(reinterpret_cast<char*>(header.data()),header.size())) { expect(false,"read UE5.9 PE header"); return; }
    const auto u16=[&](size_t o) { uint16_t v{}; if(o<=header.size()-2) { std::memcpy(&v,header.data()+o,2); } return v; };
    const auto u32=[&](size_t o) { uint32_t v{}; if(o<=header.size()-4) { std::memcpy(&v,header.data()+o,4); } return v; };
    const auto nt=u32(0x3c);
    if (nt>4096 || u16(0)!=0x5a4d || u32(nt)!=0x4550 || u16(nt+4)!=0x8664 || u16(nt+24)!=0x20b) {
        expect(false,"validate UE5.9 Win64 PE headers"); return;
    }
    const auto opt=nt+24, section_start=opt+u16(nt+20);
    const auto count=u16(nt+6);
    if (u16(nt+20)<144 || !count || count>96 || uint64_t{section_start}+uint64_t{count}*40>header.size()) {
        expect(false,"bound UE5.9 PE directories"); return;
    }
    std::memcpy(&image.base,header.data()+opt+24,8);
    image.module_size=u32(opt+56);
    for(size_t i=0;i<count;++i) {
        const auto o=section_start+i*40;
        image.sections.push_back({u32(o+12),u32(o+16),u32(o+20),u32(o+36)});
    }
    sdk::discovery::Memory memory{&image,[](void* c,uintptr_t p,void* out,size_t n) {
        auto& image=*static_cast<Image*>(c);
        const auto offset=image.offset(p,n,false);
        if(!offset) { return false; }
        image.file.clear(); image.file.seekg(*offset);
        return static_cast<bool>(image.file.read(static_cast<char*>(out),n));
    },[](void* c,uintptr_t p,size_t n) { return static_cast<Image*>(c)->offset(p,n,true).has_value(); }};
    const auto pdata=u32(opt+112+3*8), pdata_size=u32(opt+112+3*8+4);
    if(!pdata_size || pdata_size%12 || pdata_size>16*1024*1024) { expect(false,"bound UE5.9 unwind table"); return; }
    std::vector<std::array<uint32_t,3>> entries(pdata_size/12);
    if(!memory.read(memory.context,image.base+pdata,entries.data(),pdata_size) ||
        !std::is_sorted(entries.begin(),entries.end(),[](auto a,auto b) { return a[0]<b[0]; })) {
        expect(false,"read sorted UE5.9 unwind records"); return;
    }
    const auto size=[&](uintptr_t p)->size_t {
        if(p<image.base || p-image.base>UINT32_MAX) { return 0; }
        const auto rva=static_cast<uint32_t>(p-image.base);
        const auto e=std::lower_bound(entries.begin(),entries.end(),rva,[](auto e,uint32_t r) { return e[0]<r; });
        return e!=entries.end() && (*e)[0]==rva && (*e)[1]>(*e)[0] ? (*e)[1]-(*e)[0] : 0;
    };
    nlohmann::json manifest;
    std::ifstream input{manifest_path};
    if(!input || !nlohmann::json::accept(input)) { expect(false,"read UE5.9 symbol manifest"); return; }
    input.clear(); input.seekg(0); input>>manifest;
    if(!manifest.contains("image") || manifest["image"]!=image.base || !manifest.contains("symbols") || !manifest["symbols"].is_array()) {
        expect(false,"match UE5.9 symbol image base"); return;
    }
    const auto symbol=[&](std::string_view name)->uintptr_t {
        for(const auto& s:manifest["symbols"]) {
            if(s.value("name",std::string{})==name && s.contains("rva") && s["rva"].is_number_unsigned()) {
                const auto rva=s["rva"].get<uint64_t>();
                if(rva<image.module_size) { return image.base+rva; }
            }
        }
        return 0;
    };
    const auto constructor=symbol("??0FSceneView@@QEAA@AEBUFSceneViewInitOptions@@@Z");
    const auto renderer=symbol("?BeginRenderingViewFamilies@FRendererModule@@UEAAXPEAVFCanvas@@V?$TArrayView@QEAVFSceneViewFamily@@H@@@Z");
    const auto copy=symbol("??0FSceneViewFamily@@QEAA@AEBV0@@Z");
    const auto table=symbol("??_7FSceneViewFamily@@6B@");
    const auto constructor_size=size(constructor);
    if(!constructor || !renderer || !copy || !table || !constructor_size || constructor_size>0x3000) {
        expect(false,"resolve UE5.9 constructor/renderer/family public symbols"); return;
    }
    std::vector<uint8_t> code(constructor_size);
    sdk::detail::InitOptionsLayout layout;
    layout.view_family=0x158; layout.scene_state=0x160; layout.stereo_pass=0x1d0;
    expect(memory.read(memory.context,constructor,code.data(),code.size()) && sdk::ue59::constructor_proves_view(code,layout),
        "actual UE5.9 constructor proves family/state, both rects and stereo members");
    const auto original=code;
    std::fill_n(code.begin(),14,uint8_t{0xcc});
    expect(!sdk::ue59::constructor_proves_view(code,layout) &&
        sdk::ue59::overlay_saved_entry(code,{original.data(),14}) && sdk::ue59::constructor_proves_view(code,layout),
        "real constructor remains provable after hook installation without rewriting live code");
    expect(!sdk::ue59::constructor_proves_view({code.data(),std::min<size_t>(code.size(),64)},layout),
        "incomplete real constructor rejects");
    expect(uevr::ue59_native::renderer_layout(memory,renderer),"actual UE5.9 plural renderer proves span and new scene-interface ABI");
    expect(!uevr::ue58_native::renderer_layout(memory,renderer),"UE5.9 renderer cannot silently authorize the old UE5.8 ABI");
    const auto family=uevr::ue59_native::family_functions(memory,copy,image.base,image.module_size,size);
    expect(family && family->vtable==table,"actual UE5.9 family copy/destructor proves the exact build's storage/ownership contract");
    expect(!uevr::ue59_native::family_functions(memory,copy+1,image.base,image.module_size,size),"interior family addresses reject");
    const auto extension=symbol("??_7FSceneViewExtensionBase@@6B@");
    const auto active=symbol("?IsActiveThisFrame@FSceneViewExtensionBase@@UEBA_NAEBUFSceneViewExtensionContext@@@Z");
    uintptr_t target{};
    expect(extension && active && memory.load(extension+sdk::ue59::extension_active_slot*8,target) && target==active,
        "actual extension-active vtable slot matches compiler-overload ordering");
    for(const auto& [class_name,method] : {std::pair{"??_7UObject@@6B@","?PostInitProperties@UObject@@UEAAXXZ"},
            std::pair{"??_7UClass@@6B@","?PostInitProperties@UClass@@UEAAXXZ"}}) {
        const auto vt=symbol(class_name), fn=symbol(method);
        expect(vt && fn && memory.load(vt+10*8,target) && target==fn,"actual UObject/UClass PostInitProperties stays at slot 10");
    }
    std::cout<<"UE5.9 binary contracts checked: "<<path<<'\n';
}
} // namespace ue59_binary_tests
