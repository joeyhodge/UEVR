#pragma once
#include "mods/vr/DispatchMoviePolicy.hpp"
#include <cstring>
#include <limits>

namespace dispatch_movie_tests {
template<class Expect> void run(Expect&& expect) {
    namespace movie=uevr::dispatch_movie;
    for (unsigned mask=0;mask<256;++mask) {
        movie::Scope s{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),bool(mask&16),bool(mask&32),bool(mask&64),bool(mask&128)};
        expect(s.enabled() == ((mask&15)==15 && !(mask&48) && ((mask&192)==64 || (mask&192)==128)),
            "Dispatch movies require explicit opt-in Native OpenXR and exactly one reviewed RHI");
    }
    expect(movie::contract(movie::image_timestamp,movie::image_size,movie::setter_code,movie::draw_code),"exact Dispatch callback contract accepted");
    expect(!movie::contract(movie::image_timestamp+1,movie::image_size,movie::setter_code,movie::draw_code) &&
        !movie::contract(movie::image_timestamp,movie::image_size-1,movie::setter_code,movie::draw_code),"other executable revisions fail closed");
    for (size_t i=0;i<24;++i) {
        auto setter=movie::setter_code, draw=movie::draw_code;
        setter[i]^=1; draw[i]^=1;
        expect(!movie::contract(movie::image_timestamp,movie::image_size,setter,movie::draw_code) &&
            !movie::contract(movie::image_timestamp,movie::image_size,movie::setter_code,draw),"changed native callback instructions rejected");
        expect(!movie::contract(movie::image_timestamp,movie::image_size,std::span{movie::setter_code}.first(i),movie::draw_code),"truncated callback proof rejected");
    }
    movie::Owner owner{1,2,3,4,uint64_t{7}<<32|42,5,6};
    expect(owner.valid(),"complete world/media/weak identity accepted");
    auto replacement=owner; replacement.weak_texture+=uint64_t{1}<<32;
    expect(replacement != owner,"recycled UObject index retires the old movie");
    replacement=owner; replacement.world=5;
    expect(replacement != owner,"world replacement retires the old movie");
    replacement=owner; replacement.source=7;
    expect(replacement != owner,"a new movie source cannot inherit the prior movie epoch even if MediaTexture is reused");
    owner.weak_texture=42;
    expect(!owner.valid(),"uninitialized weak serial rejected");
    expect(movie::current(3,3,1000,1000) && movie::current(3,3,1000,1500),"paused decoder can republish the same valid source");
    expect(!movie::current(3,4,1000,1000) && !movie::current(0,0,1000,1000) &&
        !movie::current(3,3,1000,1501) && !movie::current(3,3,1000,999) && !movie::current(3,3,0,1),"exit, staleness and time rollback cannot reuse a movie");
    const auto geometry=movie::geometry(2560,1440,2);
    expect(std::abs(geometry.width/geometry.height-16.0f/9.0f)<0.0001f && geometry.height==2,"decoded movie keeps its full aspect ratio, not an eye crop");
    expect(!movie::geometry(0,1440,2).height && !movie::geometry(2560,1440,std::numeric_limits<float>::quiet_NaN()).height &&
        !movie::geometry(2560,1440,0).height && !movie::geometry(2560,1440,11).height,"invalid screen geometry rejected");
    for (uint32_t value=0;value<1024;++value) {
        expect(movie::shader_read_state(value) == (value==0x40 || value==0x80 || value==0xc0),"DX12 accepts only a proven shader-read state, never a write/TBD/common guess");
    }
    constexpr uintptr_t base=0x1000;
    std::array<uint8_t,512> memory{};
    const auto put=[&]<class T>(size_t at,T value) { std::memcpy(memory.data()+at,&value,sizeof(value)); };
    const auto read=[&](uintptr_t at,void* out,size_t size) {
        if (at<base || at-base>memory.size() || size>memory.size()-(at-base)) { return false; }
        std::memcpy(out,memory.data()+(at-base),size); return true;
    };
    put(0xfc,uint8_t{1}); put(0xc8,uint32_t{0x800000c0});
    expect(movie::shader_state(base,3,read)==0xc0,"uniform tracked state decoded without treating the validity bit as a D3D12 state");
    put(0xc8,uint32_t{0}); put(0xe8,int32_t{3}); put(0xd0,uint32_t{0x80}); put(0xd4,uint32_t{4});
    expect(movie::shader_state(base,3,read)==0x80,"top-mip state does not assume other media mip states match");
    put(0xe8,int32_t{2});
    expect(!movie::shader_state(base,3,read),"inconsistent per-subresource state array rejected");
    put(0xe8,int32_t{6});
    expect(!movie::shader_state(base,6,read),"more than four inline states require a real allocation");
    put(0xe0,base+0x180); put(0x180,uint32_t{0xc0});
    expect(movie::shader_state(base,6,read)==0xc0,"allocated subresource states decoded from their actual allocation");
    put(0xe0,uintptr_t{0x7000});
    expect(!movie::shader_state(base,6,read),"unreadable state allocation cannot fall back to a guessed state");
    put(0xfc,uint8_t{0}); put(0xf0,uint32_t{0xc0});
    expect(movie::shader_state(base,3,read)==0xc0,"untracked resources use their validated immutable default state");
    put(0xf0,uint32_t{0x7fffffff});
    expect(!movie::shader_state(base,3,read) && !movie::shader_state(base,0,read) && !movie::shader_state(base,15,read),"TBD state and invalid mip counts rejected");
}
}
