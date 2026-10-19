#define NOMINMAX
#include <windows.h>
#include <safetyhook.hpp>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "mods/vr/GalacticRacerLetterboxingPolicy.hpp"

namespace {
namespace p = uevr::swgr_letterbox;
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

struct Fixture {
    static constexpr uintptr_t base = 0x140000000;
    static constexpr size_t size = 0x10000;
    static constexpr uintptr_t mid = base+0x1000, cls = base+0x2000, asset = base+0x3000,
        asset_cls = base+0x4000, view = base+0x5000, family = base+0x8000, views = base+0x9000,
        other = base+0xa000, other_cls = base+0xb000, other_asset = base+0xc000, replacement = base+0xd000;
    std::array<uint8_t, size> bytes{};
    uintptr_t denied{};
    p::Names names{100, 200, 300};
    p::Memory memory{this, read};

    static bool read(void* context, uintptr_t address, void* output, size_t count) {
        auto& f = *static_cast<Fixture*>(context);
        if (address == f.denied || address < base || address-base > size || count > size-(address-base)) { return false; }
        std::memcpy(output, f.bytes.data()+address-base, count);
        return true;
    }
    template<class T> void put(uintptr_t address, const T& value) {
        std::memcpy(bytes.data()+address-base, &value, sizeof(value));
    }
    void object(uintptr_t address, uintptr_t uclass, int name, int number = 0) {
        const p::ObjectHeader h{base+0x100, 0, 123, uclass, name, number, 0};
        put(address, h);
    }
    Fixture() {
        object(mid, cls, 400, 1); object(cls, cls, names.dynamic_class);
        object(asset, asset_cls, names.parent); object(asset_cls, asset_cls, names.material_class);
        object(other, other_cls, 401); object(other_cls, other_cls, 500);
        object(other_asset, asset_cls, 301); object(replacement, cls, 402, 42);
        put(mid+0x128, asset); put(replacement+0x128, asset);
        put(view+8, family); put(family+8, views); put(family+0x10, int32_t{1}); put(views, view);
        put(view+0xdf0, p::StereoIdentity{1, 1, 0});
        put(view+0x1f10, std::array<int32_t, 4>{0, 0, 2472, 2416});
    }
    p::Chain chain() const { p::Chain c{}; c.capacity = 10; c.count = 1; c.elements[0] = mid; return c; }
};

void test_code_contract() {
    expect(p::code_matches(p::getter_code), "original full code contract");
    expect(!p::code_matches(std::span{p::getter_code}.first(1004)), "truncated function refused");
    for (size_t i = 0; i < p::getter_code.size(); ++i) {
        auto bytes = p::getter_code; bytes[i] ^= 1;
        expect(p::code_matches(bytes) == (p::getter_mask[i] == 0), "only external relocation bytes may change");
    }
}

void test_mode_and_eye() {
    expect(p::supported_mode(true, true, true, true, false, false, false), "Native mode allowed");
    for (unsigned bit = 0; bit < 7; ++bit) {
        std::array<bool, 7> mode{true, true, true, true, false, false, false};
        mode[bit] = !mode[bit];
        expect(!p::supported_mode(mode[0],mode[1],mode[2],mode[3],mode[4],mode[5],mode[6]), "other game/RHI/mode/focus paths excluded");
    }
    Fixture f;
    for (int eye : {0, 1}) {
        f.put(Fixture::view+0xdf0, p::StereoIdentity{1, eye, 0});
        expect(p::eye_of_view(f.memory, Fixture::view) == unsigned(eye), "singleton Native Fix logical eye retained");
        f.put(Fixture::family+0x10, int32_t{2});
        f.put(Fixture::view+0xdf0, p::StereoIdentity{eye+1, eye, 0});
        expect(p::eye_of_view(f.memory, Fixture::view) == unsigned(eye), "ordinary Native PRIMARY/SECONDARY allowed");
        f.put(Fixture::family+0x10, int32_t{1});
    }
    for (auto identity : {p::StereoIdentity{0,0,0}, {1,-1,0}, {1,2,0}, {1,1,1}, {2,1,0}}) {
        f.put(Fixture::view+0xdf0, identity);
        expect(!p::eye_of_view(f.memory, Fixture::view), "flat/non-primary/foreign identity untouched");
    }
    f.put(Fixture::view+0xdf0, p::StereoIdentity{1,1,0});
    f.put(Fixture::family+0x10, int32_t{3});
    expect(!p::eye_of_view(f.memory, Fixture::view), "extra-player family untouched");
    f.put(Fixture::family+0x10, int32_t{1});
    f.put(Fixture::view+0x1f10, std::array<int32_t,4>{0,0,0,2416});
    expect(!p::eye_of_view(f.memory, Fixture::view), "invalid render rectangle untouched");
    expect(!p::eye_of_view(f.memory, 0), "null view untouched");
}

void test_filter_and_game_rearm() {
    Fixture f;
    const auto apply = [&](const p::Chain& c, int location = 0) {
        return p::filtered_chain(f.memory,c,location,f.names,Fixture::base,Fixture::size);
    };
    const auto original = f.chain();
    auto result = apply(original);
    expect(result && result->count == 0 && result->capacity == 10 && !result->heap, "only letterboxing removed");
    expect(original.count == 1 && original.elements[0] == Fixture::mid, "input value/material ownership not changed");
    for (int location : {-1,1,2,3,4,5,6,7}) { expect(!apply(original,location), "other blendable locations untouched"); }
    for (int count : {-1,0,11,INT32_MAX}) {
        auto c = original; c.count = count; expect(!apply(c), "inline count bounds");
    }
    auto heap = original; heap.heap = Fixture::views;
    expect(!apply(heap), "heap storage never mutated");
    auto capacity = original; capacity.capacity = 12;
    expect(!apply(capacity), "unsupported inline capacity untouched");
    auto mix = original; mix.count = 4;
    mix.elements = {Fixture::other, Fixture::mid, Fixture::other, Fixture::replacement};
    result = apply(mix);
    expect(result && result->count == 2 && result->elements[0] == Fixture::other &&
        result->elements[1] == Fixture::other && result->elements[2] == 0, "other materials retain order and duplicates");
    f.put(Fixture::mid+0x128, Fixture::other_asset);
    expect(!apply(original), "unrelated dynamic material untouched");
    f.put(Fixture::mid+0x128, Fixture::asset);
    f.denied = Fixture::asset;
    expect(!apply(original), "unreadable parent means complete pass-through");
    f.denied = 0;
    f.put(Fixture::asset+0x1c, int32_t{1});
    expect(!apply(original), "numbered lookalike asset untouched");
    f.put(Fixture::asset+0x1c, int32_t{0});
    f.put(Fixture::mid+8, uint32_t{0x8000});
    expect(!apply(original), "destroying material untouched");
    f.put(Fixture::mid+8, uint32_t{0});
    for (int frame = 0; frame < 1000; ++frame) {
        auto fresh = f.chain();
        fresh.elements[0] = frame%2 ? Fixture::replacement : Fixture::mid;
        const auto filtered = apply(fresh);
        expect(filtered && !filtered->count && fresh.count == 1, "fresh game chains and replacement MIDs handled without pointer cache or CVar re-arm");
    }
    expect(!p::filtered_chain(f.memory,original,0,{},Fixture::base,Fixture::size), "unvalidated names never applied");
    expect(!p::filtered_chain(f.memory,original,0,f.names,0,Fixture::size), "unowned vtable never applied");
}

void test_dispatch_and_stack() {
    p::Chain output{}, different{};
    int calls{}, applied{};
    const auto original = [&](p::Chain* out, const void*, int32_t) { ++calls; return out; };
    const auto apply = [&](p::Chain*, const void*, int32_t) { ++applied; };
    expect(p::dispatch(&output,nullptr,0,original,[]{return false;},apply)==&output && calls==1 && !applied,
        "disabled dispatch calls original exactly once");
    expect(p::dispatch(&output,nullptr,0,original,[]{return true;},apply)==&output && calls==2 && applied==1,
        "enabled dispatch preserves original return");
    const auto wrong = [&](p::Chain*,const void*,int32_t){ ++calls; return &different; };
    expect(p::dispatch(&output,nullptr,0,wrong,[]{return true;},apply)==&different && applied==1,
        "unexpected sret never filtered");
    p::dispatch(nullptr,nullptr,0,original,[]{return true;},apply);
    expect(applied==1, "null sret not filtered");
    ULONG_PTR low{}, high{}; GetCurrentThreadStackLimits(&low,&high);
    expect(p::stack_return_buffer(reinterpret_cast<uintptr_t>(&output),low,high), "real caller stack allowed");
    expect(!p::stack_return_buffer(low-8,low,high) && !p::stack_return_buffer(high-8,low,high) &&
        !p::stack_return_buffer(low+1,low,high) && !p::stack_return_buffer(low,high,low), "stack bounds/alignment guarded");
    auto heap = std::make_unique<p::Chain>();
    expect(!p::stack_return_buffer(reinterpret_cast<uintptr_t>(heap.get()),low,high), "shared/heap return buffer refused");
}

// A non-trivial by-value return mirrors Unreal's TArray sret convention, not
// merely a mock three-argument C function with the same apparent pointer type.
struct NativeArray : p::Chain { ~NativeArray() {} };
static_assert(sizeof(NativeArray) == sizeof(p::Chain));
safetyhook::InlineHook native_hook{};
Fixture* fixture{};
bool enabled{};
int native_calls{};
const void* expected_view{};

__declspec(noinline) NativeArray native_original(const void* view, int32_t location) {
    ++native_calls;
    expect(view == expected_view && location == 0, "real native sret view/location registers");
    NativeArray result{};
    static_cast<p::Chain&>(result) = fixture->chain();
    volatile int preserve_body = result.count+location;
    result.count = preserve_body;
    return result;
}
__declspec(noinline) p::Chain* native_replacement(p::Chain* out,const void* view,int32_t location) {
    return p::dispatch(out,view,location,
        [](p::Chain* buffer,const void* v,int32_t l){return native_hook.call<p::Chain*>(buffer,v,l);},
        []{return enabled;}, [](p::Chain* buffer,const void* v,int32_t l){
            if (!p::eye_of_view(fixture->memory,reinterpret_cast<uintptr_t>(v))) { return; }
            const auto next = p::filtered_chain(fixture->memory,*buffer,l,fixture->names,Fixture::base,Fixture::size);
            ULONG_PTR low{},high{}; GetCurrentThreadStackLimits(&low,&high);
            if (next && p::stack_return_buffer(reinterpret_cast<uintptr_t>(buffer),low,high)) { *buffer = *next; }
        });
}
void test_real_native_abi() {
    Fixture f; fixture=&f; expected_view=reinterpret_cast<void*>(Fixture::view);
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(&native_original),
        reinterpret_cast<void*>(&native_replacement),safetyhook::InlineHook::StartDisabled);
    expect(bool(hook), "install isolated real-ABI fixture hook");
    if (!hook) { return; }
    native_hook=std::move(*hook);
    expect(bool(native_hook.enable()), "enable real-ABI fixture");
    for(int i=0;i<24;++i) {
        enabled=(i%2)==0;
        const auto before=native_calls;
        auto result=native_original(expected_view,0);
        expect(native_calls==before+1 && result.capacity==10 && result.count==(enabled?0:1),
            "real MSVC sret original/filter/reenable ABI and ownership");
    }
    native_hook.reset();
    auto result=native_original(expected_view,0);
    expect(result.count==1, "original restored after fixture hook removal");
    fixture=nullptr;
}
}

int main() {
    test_code_contract(); test_mode_and_eye(); test_filter_and_game_rearm();
    test_dispatch_and_stack(); test_real_native_abi();
    if (!failures) { std::cout << "SWGR letterboxing policy, game re-arm and native sret ABI checks passed\n"; }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
