#include "mods/vr/NativeOpenXRArrayPolicy.hpp"
#include "mods/vr/NativeOpenXRArrayD3D12.hpp"
#include "NativeOpenXRAsyncTests.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace p = uevr::native_array;
static void expect(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
static void hr(HRESULT result) { expect(SUCCEEDED(result), "D3D12/WARP call failed"); }

static void policy_tests() {
    for (unsigned bits = 0; bits < 256; ++bits) {
        p::Scope scope{!!(bits&1),!!(bits&2),!!(bits&4),!!(bits&8),!!(bits&16),!!(bits&32),!!(bits&64),!!(bits&128)};
        expect(scope.enabled() == (bits == 59), "the opt-in/backend/mode/pass/title gates changed");
    }
    p::Retry retry; const p::Key key{1,2185,2135};
    unsigned attempts = 0;
    for (uint64_t tick = 0; tick < 100'000; ++tick) { attempts += retry.attempt(true,key,false,tick*1'000'000); }
    expect(attempts == 3 && retry.exhausted(), "failed runtime allocation can loop every frame");
    retry.attempt(false,key,false,200'000'000'000);
    expect(retry.attempt(true,key,false,200'000'000'000), "off/on must re-arm a failed allocation");
    expect(!retry.attempt(true,key,true,200'000'000'001), "a ready array must never request a rebuild");
    expect(retry.attempt(true,key,false,200'000'000'002), "a successful allocation restores budget after real destruction");
    expect(retry.attempt(true,{2,2185,2135},false,200'000'000'003), "a new session must have its own budget");
    expect(retry.attempt(true,{2,2400,2135},false,200'000'000'004), "a new extent must have its own budget");
    for (const p::Key invalid : {p::Key{},p::Key{1,0,5},p::Key{1,65536,5}}) {
        expect(!retry.attempt(true,invalid,false,300'000'000'000), "invalid session/extent cannot allocate");
    }
    expect(p::current_copy(true,false,false,40,40), "a current, complete array copy should submit");
    expect(!p::current_copy(true,false,false,39,40) && !p::current_copy(false,false,false,40,40) &&
        !p::current_copy(true,true,false,40,40) && !p::current_copy(true,false,true,40,40) &&
        !p::current_copy(true,false,false,0,0), "stale/unready/AFR/Mono or absent copy cannot select an array");
    p::ImageWait wait;
    expect(!wait.pending() && !wait.writable() && !wait.waited(true), "an unacquired image cannot become writable");
    wait.acquired();
    for (int timeout = 0; timeout < 1000; ++timeout) {
        expect(!wait.waited(false) && wait.pending() && !wait.writable(), "timeout cannot authorize a copy or release");
    }
    expect(wait.waited(true) && wait.writable() && !wait.pending(), "a later successful wait must resume the same acquisition");
    expect(!wait.waited(true) && wait.writable(), "a second wait cannot consume another image before release");
    wait.released();
    expect(!wait.writable() && !wait.pending(), "release must clear image ownership");
    wait.acquired();
    expect(wait.waited(true), "the next acquisition must wait independently");
    for (unsigned bits = 0; bits < 8; ++bits) {
        expect(p::copy_enqueued(bits & 1, bits & 2, bits & 4, 10, 11) == (bits == 7),
            "unrecorded, poisoned or unfenced work must not authorize array submission");
    }
    expect(!p::copy_enqueued(true, true, true, 10, 10) && !p::copy_enqueued(true, true, true, UINT64_MAX, 0),
        "an unchanged or wrapped fence cannot authorize array submission");
}

static D3D12_RESOURCE_DESC desc(uint32_t width, uint16_t layers = 1) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=8;
    d.DepthOrArraySize=layers;d.MipLevels=1;d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}
static void descriptor_tests() {
    const auto packed=desc(16),right=desc(8),array=desc(8,2);
    const D3D12_BOX left_box{0,0,0,8,8,1},right_box{8,0,0,16,8,1},capture_box{0,0,0,8,8,1};
    expect(p::pair_copy(packed,packed,array,left_box,right_box), "packed eye pair rejected");
    expect(p::pair_copy(packed,right,array,left_box,capture_box), "separate Native Fix right eye rejected");
    auto typeless=packed;typeless.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;
    auto srgb=array;srgb.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    expect(p::pair_copy(typeless,typeless,srgb,left_box,right_box), "ProSpi typeless/SRGB copy family rejected");
    for(int bad=0;bad<12;++bad) {
        auto src=packed,dst=array;auto box=right_box;
        switch(bad) {
        case 0:dst.DepthOrArraySize=1;break;case 1:dst.MipLevels=2;break;
        case 2:dst.SampleDesc.Count=4;break;case 3:src.SampleDesc.Count=4;break;
        case 4:src.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;break;
        case 5:box.right=17;break;case 6:box.left=9;break;case 7:box.bottom=9;break;
        case 8:box.back=2;break;case 9:src.DepthOrArraySize=2;break;
        case 10:dst.Format=DXGI_FORMAT_R8G8B8A8_UNORM;break;case 11:dst.Width=9;break;
        }
        expect(!p::pair_copy(src,src,dst,left_box,box), "unsafe array shape/format/copy bounds accepted");
    }
}

static void warp_test(bool separate) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    std::cout << "WARP " << (separate ? "separate" : "packed") << " sources; debug layer " << (debug ? "enabled" : "unavailable") << '\n';
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;device.As(&messages);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
    const auto resource=[&](const D3D12_RESOURCE_DESC& d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> result;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result)));return result;
    };
    const auto buffer=[&](uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return resource(d,heap,state);
    };
    auto source_desc=desc(16);source_desc.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;source_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto array_desc=desc(8,2);array_desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;array_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto source=resource(source_desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto array=resource(array_desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_layout{};UINT64 upload_bytes{};
    device->GetCopyableFootprints(&source_desc,0,1,0,&upload_layout,nullptr,nullptr,&upload_bytes);
    auto upload=buffer(upload_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{};D3D12_RANGE empty{};hr(upload->Map(0,&empty,&mapped));
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<16;++x) {
        const uint32_t pixel=x<8 ? 0xff332211 : 0xffccbbaa;
        std::memcpy(static_cast<char*>(mapped)+upload_layout.Offset+y*upload_layout.Footprint.RowPitch+x*4,&pixel,4);
    }
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=upload_layout;
    D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    const auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=before;b.Transition.StateAfter=after;
        commands->ResourceBarrier(1,&b);
    };
    transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_BOX boxes[2]{{0,0,0,8,8,1},{8,0,0,16,8,1}};
    ComPtr<ID3D12Resource> right=source;
    if(separate) {
        auto right_desc=source_desc;right_desc.Width=8;
        right=resource(right_desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        from={};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to={};to.pResource=right.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to,0,0,0,&from,&boxes[1]);
        transition(right.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        boxes[1]=boxes[0];
    }
    transition(source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto right_state=separate ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const auto plan=p::array_copy_plan(source.Get(),right.Get(),array.Get(),boxes[0],boxes[1],D3D12_RESOURCE_STATE_RENDER_TARGET,right_state);
    expect(plan.has_value() && plan->count==(separate ? 3u : 2u), "paired-copy barriers did not deduplicate sources");
    struct Recorder {
        std::array<std::vector<D3D12_RESOURCE_BARRIER>,2> barriers;
        unsigned barrier_calls{}, copies{};
        void ResourceBarrier(UINT count,const D3D12_RESOURCE_BARRIER* list) {
            expect(barrier_calls<2,"more than two barrier batches");barriers[barrier_calls++].assign(list,list+count);
        }
        void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* to,UINT x,UINT y,UINT z,
            const D3D12_TEXTURE_COPY_LOCATION* from,const D3D12_BOX* box) {
            expect(barrier_calls==1 && copies<2 && to->SubresourceIndex==copies && from->SubresourceIndex==0 &&
                x==0 && y==0 && z==0 && box,"eye copy has wrong slice/origin/order");++copies;
        }
    } recorder;
    const auto ignore=[](bool,UINT,const D3D12_RESOURCE_BARRIER*) {};
    plan->record(recorder,ignore);
    expect(recorder.barrier_calls==2 && recorder.copies==2,"paired-copy did not record both eyes as one transaction");
    for(UINT i=0;i<plan->count;++i) {
        const auto& before=recorder.barriers[0][i].Transition;
        const auto& after=recorder.barriers[1][i].Transition;
        expect(before.pResource==after.pResource && before.StateBefore==after.StateAfter && before.StateAfter==after.StateBefore &&
            before.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,"paired-copy did not restore original resource states");
    }
    expect(!p::array_copy_plan(nullptr,right.Get(),array.Get(),boxes[0],boxes[1],D3D12_RESOURCE_STATE_RENDER_TARGET,right_state),"null source accepted");
    expect(!p::array_copy_plan(source.Get(),right.Get(),source.Get(),boxes[0],boxes[1],D3D12_RESOURCE_STATE_RENDER_TARGET,right_state),"source/destination alias accepted");
    expect(!p::array_copy_plan(source.Get(),source.Get(),array.Get(),boxes[0],boxes[1],D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),"aliased source with contradictory states accepted");
    auto bad_box=boxes[0];++bad_box.right;
    expect(!p::array_copy_plan(source.Get(),right.Get(),array.Get(),bad_box,boxes[1],D3D12_RESOURCE_STATE_RENDER_TARGET,right_state),"invalid copy extent accepted");
    for (const auto left_state : {D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}) {
        for (const auto tested_right : {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}) {
            const auto candidate=p::array_copy_plan(source.Get(),right.Get(),array.Get(),boxes[0],boxes[1],left_state,tested_right);
            expect(candidate.has_value()==(separate || left_state==tested_right),"aliased-state validation differs from independent-state validation");
            if(candidate) {
                const auto sources=UINT(left_state!=D3D12_RESOURCE_STATE_COPY_SOURCE)+
                    UINT(separate && tested_right!=D3D12_RESOURCE_STATE_COPY_SOURCE);
                expect(candidate->count==sources+1,"unnecessary or missing source/array transition");
            }
        }
    }
    plan->record(*commands.Get(),ignore);
    transition(array.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layouts{};UINT64 bytes{};
    device->GetCopyableFootprints(&array_desc,0,2,0,layouts.data(),nullptr,nullptr,&bytes);
    auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    for(unsigned eye=0;eye<2;++eye) {
        from={};from.pResource=array.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=eye;
        to={};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layouts[eye];
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    hr(commands->Close());ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    hr(queue->Signal(fence.Get(),1));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    expect(event!=nullptr,"fence event");hr(fence->SetEventOnCompletion(1,event));
    const auto waited=WaitForSingleObject(event,5000);CloseHandle(event);expect(waited==WAIT_OBJECT_0,"bounded WARP fence wait");
    D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};hr(readback->Map(0,&range,&mapped));
    for(unsigned eye=0;eye<2;++eye) for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        uint32_t pixel{};std::memcpy(&pixel,static_cast<char*>(mapped)+layouts[eye].Offset+y*layouts[eye].Footprint.RowPitch+x*4,4);
        expect(pixel==(eye==0 ? 0xff332211 : 0xffccbbaa),"array eye slices are mixed, clipped or gamma-transformed during copy");
    }
    readback->Unmap(0,&empty);
    if(messages) for(UINT64 n=0;n<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++n) {
        SIZE_T size{};messages->GetMessage(n,nullptr,&size);std::vector<char> data(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(messages->GetMessage(n,message,&size));
        expect(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,
            "D3D12 debug layer rejected the two-slice copy");
    }
}

int main(int argc,char** argv) try {
    policy_tests();descriptor_tests();native_async_tests::run();
    if(argc==2 && std::string_view{argv[1]}=="--worker-only") {
        std::cout<<"Native OpenXR worker/cancellation/drain/retirement tests passed\n";return 0;
    }
    expect(argc==1,"unrecognized test option");
    warp_test(false);warp_test(true);
    std::cout<<"Native OpenXR array: scope/retry/frame/worker/drain/retirement and production paired WARP copies passed\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
