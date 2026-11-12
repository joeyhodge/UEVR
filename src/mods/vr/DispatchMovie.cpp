#define NOMINMAX
#include "DispatchMovie.hpp"
#include "DispatchMovieD3D12.hpp"
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace uevr::dispatch_movie {
namespace {
struct State {
    std::mutex control_mutex;
    std::mutex mutex;
    std::atomic_bool enabled{};
    std::atomic_bool stopping{};
    std::atomic<const char*> message{"Off"};
    std::atomic<const char*> refusal{};
    std::atomic<uint64_t> epoch{1};
    std::atomic_bool presenting{};
    Owner owner;
    Frame frame;
    uintptr_t game_world{}, base{}, klass{}, observed_subsystem{};
    bool attempted{}, supported{}, previously_enabled{}, dx12_supported{};
    int32_t bootstrap_cursor{}, bootstrap_limit{};
    bool bootstrap_done{};
    safetyhook::InlineHook setter, draw, present;
};
// Entry patches are disabled explicitly, but trampolines stay immutable and
// alive through CRT teardown. Never free one underneath a native game call.
State& state() { static auto* value=new State; return *value; }
thread_local uintptr_t present_viewport{};
uint64_t clock_ms() { return GetTickCount64(); }

// Leaf guards contain no C++ objects requiring unwinding. A failed observation
// must never skip the original game callback.
bool bytes(uintptr_t address, void* out, size_t size) noexcept {
    constexpr uintptr_t highest=0x7fffffffffff;
    if (address < 0x10000 || address > highest || size > highest-address) { return false; }
    __try { std::memcpy(out,reinterpret_cast<void*>(address),size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> T load(uintptr_t address) { T v{}; bytes(address,&v,sizeof(v)); return v; }
bool live(uintptr_t address, uint64_t weak=0) noexcept {
    __try {
        if (!address) { return false; }
        const auto index=load<int32_t>(address+0xc);
        const auto array=sdk::FUObjectArray::get();
        if (!array || index < 0 || index >= array->get_object_count()) { return false; }
        const auto item=array->get_object(index);
        return item && !(item->get_flags() & ((1u<<28)|(1u<<29)|(1u<<31))) &&
            !(load<uint32_t>(address+8) & (0x8000u|0x10000u)) &&
            reinterpret_cast<uintptr_t>(item->get_object()) == address &&
            (!weak || (int32_t(weak) == index && uint32_t(weak >> 32) == uint32_t(item->get_serial_number())));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uintptr_t object_at(sdk::FUObjectArray* array, int32_t index) noexcept {
    __try {
        if (!array || index < 0 || index >= array->get_object_count()) { return 0; }
        const auto item=array->get_object(index);
        return item ? reinterpret_cast<uintptr_t>(item->get_object()) : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool pin11(uintptr_t rhi, ID3D11Texture2D** out, D3D11_TEXTURE2D_DESC* desc) noexcept {
    __try {
        // Reviewed Dispatch getter: mov rax,[rcx+0x90]; ret. No SDK scan or
        // virtual-slot probing is needed inside the movie callback.
        auto* native=reinterpret_cast<IUnknown*>(load<uintptr_t>(rhi+0x90));
        if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(out)))) { return false; }
        (*out)->GetDesc(desc);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { *out=nullptr; return false; }
}
bool pin12(uintptr_t resource, ID3D12Resource** out, D3D12_RESOURCE_DESC* desc) noexcept {
    __try {
        auto* native=reinterpret_cast<IUnknown*>(load<uintptr_t>(resource+0x20));
        if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(out)))) { return false; }
        *desc=(*out)->GetDesc();
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { *out=nullptr; return false; }
}
bool native_path_ready(const Owner& owner) {
    if (!owner.valid()) { return false; }
    uint8_t material_path{};
    if (!bytes(load<uintptr_t>(state().base+0x422ba10)+1,&material_path,sizeof(material_path))) { return false; }
    return material_path ? live(load<uintptr_t>(load<uintptr_t>(owner.extension+0x40)+0x10))
        : load<uintptr_t>(owner.extension+0x50) != 0;
}
bool owner_current(const Owner& owner) {
    return owner.valid() && live(owner.subsystem) && live(owner.world) && live(owner.texture,owner.weak_texture) &&
        load<uintptr_t>(owner.subsystem+0x10) == state().klass && load<uintptr_t>(owner.subsystem+0x20) == owner.world &&
        load<uintptr_t>(owner.subsystem+0x40) == owner.source &&
        load<uintptr_t>(owner.subsystem+0x48) == owner.player && load<uintptr_t>(owner.subsystem+0x50) == owner.texture &&
        load<uintptr_t>(owner.subsystem+0xa0) == owner.extension &&
        load<uintptr_t>(owner.extension) == state().base+extension_vtable_rva &&
        load<uint64_t>(owner.extension+0x30) == owner.weak_texture && native_path_ready(owner);
}
void select_owner(uintptr_t subsystem) {
    auto& s=state();
    if (!s.enabled.load(std::memory_order_acquire) || s.stopping.load() || !live(subsystem) || load<uintptr_t>(subsystem+0x10) != s.klass) { return; }
    Owner next{subsystem,load<uintptr_t>(subsystem+0xa0),load<uintptr_t>(subsystem+0x50),load<uintptr_t>(subsystem+0x20)};
    const auto world=next.world;
    next.weak_texture=load<uint64_t>(next.extension+0x30);
    next.source=load<uintptr_t>(subsystem+0x40); next.player=load<uintptr_t>(subsystem+0x48);
    if (!next.valid() || !live(next.source) || !live(next.player) || !live(next.texture,next.weak_texture) || !live(next.world) ||
        load<uintptr_t>(next.extension) != s.base+extension_vtable_rva) { next={}; }
    std::scoped_lock lock{s.mutex};
    if (!world || world != s.game_world) { return; }
    s.observed_subsystem=subsystem;
    if (next != s.owner) {
        s.owner=next; s.frame={}; s.refusal.store(nullptr); s.epoch.fetch_add(1,std::memory_order_acq_rel);
        s.message.store(next.valid() ? "Waiting for the movie post-process source" : "No active story movie");
        SPDLOG_INFO("[Dispatch][Movie] Media ownership active={} world={:#x} source={:#x} player={:#x} texture={:#x}",
            next.valid(),next.world,next.source,next.player,next.texture);
    }
}
void* setter_hook(void* self, void* objects) {
    auto& s=state();
    auto* result=s.setter.unsafe_call<void*>(self,objects);
    try { select_owner(reinterpret_cast<uintptr_t>(self)); } catch (...) {}
    return result;
}
// Member/sret ABI: the native callback returns a 24-byte screen-pass value
// through its second argument. Preserve all five arguments and the game draw.
void* draw_hook(void* self, void* out, void* graph, void* view, void* inputs) {
    auto& s=state();
    if (!s.enabled.load(std::memory_order_acquire) || s.stopping.load()) { return s.draw.unsafe_call<void*>(self,out,graph,view,inputs); }
    Owner owner{};
    Frame frame{};
    try {
        if (s.enabled.load(std::memory_order_acquire)) {
            std::unique_lock lock{s.mutex,std::try_to_lock};
            if (lock && s.owner.extension == reinterpret_cast<uintptr_t>(self)) { owner=s.owner; frame.epoch=s.epoch.load(); }
        }
        if (owner_current(owner)) {
            frame.rhi=load<uintptr_t>(owner.extension+0x48);
            const auto vtable=load<uintptr_t>(frame.rhi);
            if (vtable == s.base+dx11_texture_vtable_rva) {
                ID3D11Texture2D* native{};
                D3D11_TEXTURE2D_DESC desc{};
                if (pin11(frame.rhi,&native,&desc)) {
                    frame.texture.Attach(native);
                    if (!extent(desc.Width,desc.Height) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
                        (desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                         desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) { frame.texture.Reset(); }
                    frame.width=desc.Width; frame.height=desc.Height;
                }
            } else if (s.dx12_supported && vtable == s.base+dx12_texture_vtable_rva) {
                frame.resource12=load<uintptr_t>(frame.rhi+0xc0);
                if (load<uintptr_t>(frame.resource12) == s.base+dx12_resource_vtable_rva && !load<uintptr_t>(frame.resource12+0x28)) {
                    ID3D12Resource* native{};
                    D3D12_RESOURCE_DESC desc{};
                    if (pin12(frame.resource12,&native,&desc)) {
                        frame.texture12.Attach(native);
                        if (!movie_desc(desc)) { frame.texture12.Reset(); }
                        frame.width=static_cast<uint32_t>(desc.Width); frame.height=desc.Height;
                    }
                }
            }
        }
    } catch (...) { frame={}; }
    auto* result=s.draw.unsafe_call<void*>(self,out,graph,view,inputs);
    try {
        if ((frame.texture || frame.texture12) && s.enabled.load(std::memory_order_acquire) && !s.stopping.load()) {
            std::unique_lock lock{s.mutex,std::try_to_lock};
            if (lock && frame.epoch == s.epoch.load() && owner == s.owner) {
                if (s.frame.epoch != frame.epoch) {
                    SPDLOG_INFO("[Dispatch][Movie] First native movie source epoch={} RHI={:#x} DX12={} extent={}x{}",
                        frame.epoch,frame.rhi,bool(frame.texture12),frame.width,frame.height);
                }
                frame.sampled_ms=clock_ms(); s.frame=std::move(frame);
                if (!s.presenting.load()) { s.message.store(s.frame.texture ? "Validated D3D11 story movie source" : "Validated D3D12 story movie source"); }
            }
        }
    } catch (...) {}
    return result;
}
int32_t present_hook(void* viewport, int32_t interval) {
    if (!state().enabled.load(std::memory_order_acquire) || state().stopping.load()) { return state().present.unsafe_call<int32_t>(viewport,interval); }
    struct Restore { uintptr_t previous; ~Restore() { present_viewport=previous; } } restore{present_viewport};
    present_viewport=reinterpret_cast<uintptr_t>(viewport);
    // The original Present may wait in the runtime; do not hold a hook mutex
    // across it. This hook's trampoline is never reset during the game.
    return state().present.unsafe_call<int32_t>(viewport,interval);
}
bool dx12_contract(uintptr_t base) {
    struct Proof { uintptr_t rva; std::array<uint8_t,24> code; };
    constexpr Proof proofs[]{
        {present_rva,present_code},
        {0x1454a00,{0x8b,1,0x4c,0x63,0xc2,0x85,0xc0,0x79,5,0x0f,0xba,0xf0,0x1f,0xc3,0x48,0x8d,0x51,8,0x48,0x8b,0x49,0x18,0x48,0x85}},
        {0x1407370,{0x80,0xb9,0x3e,1,0,0,0,0x48,0x8b,0x81,0x40,1,0,0,0x74,5,0x48,0x8b,0x40,0x48,0xc3,0x48,0x8b,0x40}},
        {0x14071f0,{0x40,0x53,0x57,0x48,0x83,0xec,0x48,0x48,0x8b,0xd9,0x48,0x8b,0xfa,0x48,0x8b,0x49,0x28,0x48,0x8d,0x54,0x24,0x60,0x48,0x8b}},
        {0x1454210,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0xc0,0,0,0,0x33,0xdb,0x48,0x85,0xc0,0x74,9,0x48,0x8b,0x58,0x20}}
    };
    for (const auto& p:proofs) {
        std::array<uint8_t,24> code{};
        if (!bytes(base+p.rva,code.data(),code.size()) || code != p.code) { return false; }
    }
    return true;
}
bool install() {
    auto& s=state();
    s.base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    std::array<uint8_t,24> setter{},draw{};
    if (!bytes(s.base,&dos,sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
        !bytes(s.base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !bytes(s.base+setter_rva,setter.data(),setter.size()) || !bytes(s.base+draw_rva,draw.data(),draw.size()) ||
        !contract(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage,setter,draw)) { return false; }
    constexpr std::array<uint8_t,8> getter{0x48,0x8b,0x81,0x90,0,0,0,0xc3};
    std::array<uint8_t,8> code{};
    if (!bytes(s.base+0xc1c8e0,code.data(),code.size()) || code != getter ||
        load<uintptr_t>(s.base+dx11_texture_vtable_rva+0x38) != s.base+0xc1c8e0) { return false; }
    s.klass=reinterpret_cast<uintptr_t>(sdk::find_uobject<sdk::UClass>(L"Class /Script/AdHocMedia.AdHocMediaSubsystem"));
    if (!s.klass) { return false; }
    auto setter_value=safetyhook::create_inline(reinterpret_cast<void*>(s.base+setter_rva),reinterpret_cast<void*>(&setter_hook),safetyhook::InlineHook::StartDisabled);
    auto draw_value=safetyhook::create_inline(reinterpret_cast<void*>(s.base+draw_rva),reinterpret_cast<void*>(&draw_hook),safetyhook::InlineHook::StartDisabled);
    if (!setter_value || !draw_value) { return false; }
    s.setter=std::move(setter_value); s.draw=std::move(draw_value);
    if (!s.setter.enable() || !s.draw.enable()) {
        (void)s.setter.disable(); (void)s.draw.disable();
        return false;
    }
    if (dx12_contract(s.base)) {
        s.present=safetyhook::create_inline(reinterpret_cast<void*>(s.base+present_rva),reinterpret_cast<void*>(&present_hook),safetyhook::InlineHook::StartDisabled);
        s.dx12_supported=s.present && bool(s.present.enable());
    }
    SPDLOG_INFO("[Dispatch][Movie] Validated AdHocMedia callbacks; DX12 Present contract={}. No decoder, FOV or 2D-mode writes",s.dx12_supported);
    return true;
}
}
void update(Scope scope, sdk::UObject* world) {
    auto& s=state();
    std::scoped_lock control{s.control_mutex};
    if (s.stopping.load()) { return; }
    const bool enabled=scope.enabled() && world;
    const auto world_address=reinterpret_cast<uintptr_t>(world);
    if (!enabled || s.game_world != world_address || !s.previously_enabled) {
        s.enabled.store(false,std::memory_order_release);
        std::scoped_lock lock{s.mutex};
        if (s.previously_enabled || s.game_world != world_address) {
            s.owner={}; s.frame={}; s.observed_subsystem=0; s.refusal.store(nullptr); s.epoch.fetch_add(1); s.bootstrap_cursor=0; s.bootstrap_limit=0; s.bootstrap_done=false;
        }
        s.game_world=world_address;
    }
    s.previously_enabled=enabled;
    if (!enabled) { s.message.store("Off / outside Dispatch Native OpenXR (manual 2D is unchanged)"); return; }
    if (!s.attempted) {
        s.attempted=true; s.supported=install();
        if (!s.supported) { SPDLOG_WARN("[Dispatch][Movie] Unsupported native binary/class contract; no movie presentation override enabled"); }
        else { s.message.store("Waiting for an active AdHocMedia story movie"); }
    }
    if (!s.supported || (scope.dx12 && !s.dx12_supported)) { s.message.store("Unsupported executable/native callback contract"); return; }
    s.enabled.store(true,std::memory_order_release);
    uintptr_t known{};
    { std::scoped_lock lock{s.mutex}; known=s.observed_subsystem; }
    // Also catch native clear paths which do not call the publisher. This is
    // one cached subsystem read, not a class/widget/object-array search.
    if (known) { select_owner(known); }
    // One bounded bootstrap per enable/world, amortized in small slices. Do
    // not activate the global UObject hooks or periodically rescan widgets.
    if (!s.bootstrap_done) {
        { std::scoped_lock lock{s.mutex}; if (s.owner.valid()) { s.bootstrap_done=true; } }
        if (s.bootstrap_done) { return; }
        const auto array=sdk::FUObjectArray::get();
        if (!array) { return; }
        if (!s.bootstrap_limit) { s.bootstrap_limit=(std::min)(array->get_object_count(),2*1024*1024); }
        const auto end=(std::min)(s.bootstrap_cursor+1024,s.bootstrap_limit);
        for (;s.bootstrap_cursor<end;++s.bootstrap_cursor) {
            const auto p=object_at(array,s.bootstrap_cursor);
            if (p && load<uintptr_t>(p+0x10) == s.klass && load<uintptr_t>(p+0x20) == world_address) {
                select_owner(p); s.bootstrap_done=true; break;
            }
        }
        if (s.bootstrap_cursor >= s.bootstrap_limit) { s.bootstrap_done=true; }
    }
}
std::optional<Frame> read() {
    auto& s=state();
    if (!s.enabled.load(std::memory_order_acquire) || s.stopping.load()) { return {}; }
    std::unique_lock lock{s.mutex,std::try_to_lock};
    if (!lock || (!s.frame.texture && !s.frame.texture12) || !current(s.frame.epoch,s.epoch.load(),s.frame.sampled_ms,clock_ms()) ||
        !owner_current(s.owner)) { return {}; }
    return s.frame;
}
bool current(const Frame& frame) {
    auto& s=state();
    if (!s.enabled.load(std::memory_order_acquire) || s.stopping.load() || !current(frame.epoch,s.epoch.load(),frame.sampled_ms,clock_ms())) { return false; }
    std::unique_lock lock{s.mutex,std::try_to_lock};
    return lock && frame.epoch == s.epoch.load() && owner_current(s.owner);
}
std::optional<D3D12_RESOURCE_STATES> source_state(const Frame& frame, ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapchain) {
    auto& s=state();
    const auto reject=[&](const char* reason)->std::optional<D3D12_RESOURCE_STATES> {
        if (s.refusal.exchange(reason) != reason) { SPDLOG_WARNING_EVERY_N_SEC(2,"[Dispatch][Movie][D3D12] {}",reason); }
        return {};
    };
    if (!present_viewport) { return reject("Waiting for Dispatch's post-flush native Present scope"); }
    if (!frame.texture12 || !device || !queue || !swapchain || !current(frame)) { return reject("Waiting for current movie/XR graphics ownership"); }
    std::unique_lock lock{s.mutex,std::try_to_lock};
    if (!lock) { return {}; }
    if (frame.epoch != s.epoch.load() || !live(s.owner.texture,s.owner.weak_texture) ||
        load<uintptr_t>(s.owner.extension) != s.base+extension_vtable_rva ||
        load<uintptr_t>(s.owner.extension+0x48) != frame.rhi ||
        load<uint64_t>(s.owner.extension+0x30) != s.owner.weak_texture ||
        load<uintptr_t>(frame.rhi) != s.base+dx12_texture_vtable_rva ||
        load<uintptr_t>(frame.rhi+0xc0) != frame.resource12 ||
        load<uintptr_t>(frame.resource12) != s.base+dx12_resource_vtable_rva || load<uintptr_t>(frame.resource12+0x28) ||
        load<uintptr_t>(frame.resource12+0x20) != reinterpret_cast<uintptr_t>(frame.texture12.Get())) { return reject("Movie RHI lease changed or is no longer a committed current source"); }
    // BN + matching source: Present flushes the default context and waits for
    // its execute task before PresentInternal. TLS excludes other callers.
    const auto adapter=load<uintptr_t>(present_viewport+0x18);
    const auto owner_device=load<uintptr_t>(frame.resource12+0x10);
    const auto manager=load<uintptr_t>(owner_device+0x38);
    if (!owner_device || load<uintptr_t>(adapter+0x988) != owner_device ||
        load<uint32_t>(frame.resource12+0x18) != 1 || load<uint32_t>(frame.resource12+0x1c) != 1 ||
        load<uintptr_t>(manager+0x28) != reinterpret_cast<uintptr_t>(queue) ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || device->GetNodeCount() != 1) { return reject("Movie viewport/device/default graphics queue proof did not match"); }
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    if (FAILED(frame.texture12->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get() != device) { return reject("Movie and OpenXR use different D3D12 devices"); }
    auto* native=reinterpret_cast<IDXGISwapChain*>(load<uintptr_t>(present_viewport+0x60));
    Microsoft::WRL::ComPtr<IUnknown> a,b;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&a))) || FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&b))) || a != b) { return reject("Present scope belongs to a different DXGI swapchain"); }
    const auto desc=frame.texture12->GetDesc();
    if (!movie_desc(desc) || desc.Width != frame.width || desc.Height != frame.height) { return reject("Movie descriptor changed before the XR copy"); }
    const auto value=shader_state(frame.resource12,desc.MipLevels,bytes);
    if (!value) { return reject("Movie mip-zero state is unreadable, inconsistent, or not shader-read"); }
    s.refusal.store(nullptr);
    return static_cast<D3D12_RESOURCE_STATES>(*value);
}
const char* status() {
    auto& s=state();
    if (const auto reason=s.refusal.load(); reason && s.enabled.load()) { return reason; }
    return s.message.load();
}
void note_submission(bool requested, bool presented) {
    auto& s=state();
    const bool active=requested && presented && s.enabled.load(std::memory_order_acquire) && !s.stopping.load();
    if (s.presenting.exchange(active) != active) {
        SPDLOG_INFO_EVERY_N_SEC(2,"[Dispatch][Movie] World-locked layer active={}; scene/UI render targets and manual 2D remain unchanged",active);
    }
    if (active) { s.message.store("Active: opaque world-locked movie for both eyes, dedicated menus unchanged"); }
}
void shutdown() {
    auto& s=state();
    s.stopping.store(true,std::memory_order_release);
    s.enabled.store(false,std::memory_order_release);
    std::scoped_lock control{s.control_mutex};
    s.presenting.store(false); s.message.store("Off");
    { std::scoped_lock lock{s.mutex}; s.owner={}; s.frame={}; s.observed_subsystem=0; s.epoch.fetch_add(1); }
    if (s.setter) { (void)s.setter.disable(); }
    if (s.draw) { (void)s.draw.disable(); }
    if (s.present) { (void)s.present.disable(); }
}
}
