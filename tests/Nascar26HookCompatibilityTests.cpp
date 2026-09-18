#define NOMINMAX
#include <Windows.h>
#include <safetyhook.hpp>
#include <safetyhook/os.hpp>
#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <thread>
#include "utility/Nascar26HookCompatibility.hpp"

namespace {
using namespace uevr::nascar26;
int failures{};
uintptr_t image{};
size_t image_size{};
ObjectVTable* dispatched_hook{};

void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
__declspec(noinline) int original(int n) { return n + 7; }
__declspec(noinline) int unrelated(int n) { return n + 19; }
__declspec(noinline) int replacement(int n) { return dispatched_hook->call<int>(n) + 10; }
using Fn = int (*)(int);
// Real image-backed, non-executable data: exercise the same validation as production.
const Fn table[]{original, unrelated, original};
const Fn single[]{original};
struct ImageTable { uintptr_t prefix; Fn functions[3]; };
const ImageTable source_table{0x12345678, {original, unrelated, original}};
struct Object { uintptr_t vtable; uintptr_t canary; };
const Object image_object{reinterpret_cast<uintptr_t>(source_table.functions), 0xabcdef};

uintptr_t read_slot(uintptr_t address) {
    uintptr_t value{};
    expect(read_memory(address, &value, sizeof(value)), "slot remains readable");
    return value;
}
DWORD protection(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ? info.Protect : 0;
}
bool protect(uint8_t* address, size_t bytes, uint32_t flags, uint32_t* old) {
    return protect_external_memory(address, bytes, flags, old, image, image_size);
}
}

int main() {
    using namespace uevr::nascar26;
    initialize();
    expect(!is_target() && !is_validated_build() && !safetyhook::has_protection_override(),
        "other executables do not opt in or install a protection override");
    expect(matches_name(L"D:\\Games\\NASCAR26_Steam-Win64-Shipping.exe"), "exact basename opts in");
    expect(matches_name(L"d:/Games/nascar26_steam-win64-shipping.EXE"), "case and directory separators are supported");
    for (auto name : {L"NASCAR26_Steam-Win64-Shipping.exe.bak", L"NASCAR26_Steam-Win64-Shipping",
        L"Maverick.exe", L"Hi-Fi-RUSH.exe", L"Sifu-Win64-Shipping.exe", L"DaysGone.exe", L"SHCO.exe",
        L"TheMedium-Win64-Shipping.exe", L"Stalker2-Win64-Shipping.exe", L"SWZeroCompany.exe", L""}) {
        expect(!matches_name(name), "other titles and suffixes remain excluded");
    }
    expect(matches_build_metadata(0x50007, 0x40000, 0xb910822a, 0xc14b000), "exact captured build metadata accepted");
    expect(!matches_build_metadata(0x50007, 0x30000, 0xb910822a, 0xc14b000), "other patch version rejected");
    expect(!matches_build_metadata(0x50008, 0x40000, 0xb910822a, 0xc14b000), "other engine rejected");
    expect(!matches_build_metadata(0x50007, 0x40000, 0xb910822b, 0xc14b000), "other build timestamp rejected");
    expect(!matches_build_metadata(0x50007, 0x40000, 0xb910822a, 0xc14c000), "other image layout rejected");
    expect(!validate_synced_redraw(), "Synced callable validation cannot opt in an unrelated process");
    expect(!validate_ghost_view_setup(), "Ghost view-setup validation cannot opt in an unrelated process");
    expect(!validate_native_renderer(), "Native linked-renderer validation cannot opt in an unrelated process");
    expect(!validate_native_rooting(), "Native GC rooting callable cannot opt in an unrelated process");
    struct OwnedHeader { uintptr_t vtable; uint32_t flags; int32_t index; } owned_header{0x1234, 0, 17};
    const NativeOwnedObject owned{reinterpret_cast<uintptr_t>(&owned_header), owned_header.vtable, owned_header.index};
    struct OwnedItem { uint64_t flags_and_refcount; uintptr_t object; } owned_item{0, owned.object};
    const auto owned_item_address = reinterpret_cast<uintptr_t>(&owned_item);
    expect(native_owned_object_valid(owned, owned_item_address, false), "fresh live capture may be passed to native root registration");
    expect(!native_owned_object_valid(owned, owned_item_address), "unrooted capture cannot be published or reused");
    owned_item.flags_and_refcount = native_owner_root_flag;
    expect(!native_owned_object_valid(owned, owned_item_address), "UE5.7 refcount bits are not root flags");
    owned_item.flags_and_refcount = (uint64_t{native_owner_root_flag} << 32) | 7;
    expect(native_owned_object_valid(owned, owned_item_address), "rooted owner identity accepts the source-proven UE5.7 item layout");
    const auto rooted_item = owned_item;
    for (auto flag : {1u << 21, 1u << 28, 1u << 31}) {
        owned_item.flags_and_refcount = rooted_item.flags_and_refcount | (uint64_t{flag} << 32);
        expect(!native_owned_object_valid(owned, owned_item_address), "garbage, unreachable and unconstructed owners fail closed even if rooted");
    }
    owned_item = rooted_item;
    owned_item.object += 8;
    expect(!native_owned_object_valid(owned, owned_item_address), "level-load reuse of the cached object-array index rejects the old capture");
    owned_item = rooted_item;
    ++owned_header.index;
    expect(!native_owned_object_valid(owned, owned_item_address), "reused object address cannot change its cached index");
    owned_header.index = owned.index;
    ++owned_header.vtable;
    expect(!native_owned_object_valid(owned, owned_item_address), "reused owner with a different class cannot be adopted");
    owned_header.vtable = owned.vtable;
    expect(native_owned_object_valid(owned, owned_item_address) &&
        owned_item.flags_and_refcount == rooted_item.flags_and_refcount && owned_header.flags == 0,
        "owner checks preserve refcounts and public object flags");
    expect(!native_owned_object_valid({}, owned_item_address) && !native_owned_object_valid(owned, 0) &&
        !native_owned_object_valid({owned.object, owned.vtable, -1}, owned_item_address), "missing owner/item and invalid cached index are refused");
    for (unsigned mask = 0; mask < 8; ++mask) {
        expect(!needs_generic_renderer_hook(true, mask & 1, mask & 2, mask & 4),
            "code-preserving NASCAR never retries generic renderer image hooks");
        expect(needs_generic_renderer_hook(false, mask & 1, mask & 2, mask & 4) == (mask != 0),
            "other games retain the existing Native Fix, DIBR, and splitscreen hook policy");
    }
    alignas(16) std::array<uint8_t, 0x48> family_bytes{};
    const uintptr_t fixture_target = 0x3000, fixture_scene = 0x4000, fixture_depth = 0x5000;
    std::memcpy(family_bytes.data() + 0x30, &fixture_target, sizeof(fixture_target));
    std::memcpy(family_bytes.data() + 0x40, &fixture_scene, sizeof(fixture_scene));
    const auto family_address = reinterpret_cast<uintptr_t>(family_bytes.data());
    auto family_targets = read_native_family_targets(family_address);
    expect(family_targets && *family_targets == NativeFamilyTargets{fixture_target, 0, fixture_scene} &&
        native_main_family_targets_valid(*family_targets, fixture_target, fixture_scene),
        "null RenderTargetDepth at +0x38 does not hide the valid Scene at +0x40");
    std::memcpy(family_bytes.data() + 0x38, &fixture_depth, sizeof(fixture_depth));
    family_targets = read_native_family_targets(family_address);
    expect(family_targets && family_targets->scene == fixture_scene && family_targets->depth == fixture_depth &&
        !native_main_family_targets_valid(*family_targets, fixture_target, fixture_scene),
        "depth is not mistaken for Scene or redirected without a validated separate-depth contract");
    expect(!native_main_family_targets_valid({fixture_target, 0, fixture_scene}, fixture_target, 0) &&
        !native_main_family_targets_valid({fixture_target, 0, fixture_scene}, fixture_target, fixture_scene + 1) &&
        !native_main_family_targets_valid({fixture_target, 0, fixture_scene}, fixture_target + 1, fixture_scene),
        "missing scene, another world's scene, and auxiliary targets fail closed");
    expect(!read_native_family_targets(0) && !read_native_family_targets(UINTPTR_MAX - 0x30),
        "null and overflowing family addresses fail closed");
    for (int method = -1; method <= 5; ++method) {
        expect(supports_rendering_method(method, false) == (method == 0 || method == 1),
            "only Native and strict Synced are supported");
        expect(!supports_rendering_method(method, true), "Extreme does not implicitly enable another AFR path");
    }
    SyncedRedraw redraw;
    const RedrawIdentity owner{0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 1, 1920, 1080};
    GhostOwner ghost_owner{owner, 0x6000, 0x7000, 0x8000, 0x9000, 12};
    std::array<double, 16> projection{};
    projection[0] = projection[5] = projection[11] = 1;
    projection[14] = 0.1;
    expect(native_projection_valid(projection), "finite reversed-Z perspective projection accepted");
    for (size_t i = 0; i < projection.size(); ++i) {
        auto bad = projection; bad[i] = std::numeric_limits<double>::quiet_NaN();
        expect(!native_projection_valid(bad), "non-finite projection is never accepted");
    }
    auto bad_projection = projection; bad_projection[11] = 0;
    expect(!native_projection_valid(bad_projection), "orthographic projection rejected");
    const NativeRect eye_rect{0, 0, 2472, 2416};
    expect(native_rect_valid(eye_rect, eye_rect, 2472, 2416), "origin-zero HMD eye rect accepted");
    expect(native_rect_valid(eye_rect, NativeRect{20, 30, 2400, 2300}, 2472, 2416), "valid constrained camera rect accepted");
    expect(!native_rect_valid(NativeRect{2472, 0, 4944, 2416}, eye_rect, 2472, 2416) &&
        !native_rect_valid(eye_rect, NativeRect{0, 0, 2473, 2416}, 2472, 2416) &&
        !native_rect_valid(eye_rect, NativeRect{1, 1, 0, 0}, 2472, 2416), "packed/out-of-bounds/inverted rects rejected");
    std::array<NativeView, 2> native_views{};
    for (int eye = 0; eye < 2; ++eye) {
        native_views[eye] = NativeView{0x10000u + eye * 0x2000u, 0x20000, 0x9000u + eye * 0x1000u,
            200, 100u + eye, eye, eye + 1, 0, 0, eye_rect, eye_rect, true};
    }
    const auto valid_pair = [&](const auto& pair) { return native_pair_valid(pair, 0x20000, 0x9000, 0xa000, 200, 2472, 2416); };
    expect(valid_pair(native_views), "complete two-eye constructor pair accepted");
    for (int eye = 0; eye < 2; ++eye) {
        for (int change = 0; change < 13; ++change) {
            auto bad = native_views;
            auto& v = bad[eye];
            switch (change) {
            case 0: v.valid = false; break;
            case 1: v.family++; break;
            case 2: v.frame++; break;
            case 3: v.state = 0; break;
            case 4: v.state = native_views[1 - eye].state; break;
            case 5: v.index = 1 - eye; break;
            case 6: v.pass = 0; break;
            case 7: v.primary_index = -1; break;
            case 8: v.player_index = 1; break;
            case 9: v.projection_hash = 0; break;
            case 10: v.rect[0]++; break;
            case 11: v.constrained[0] = -1; break;
            case 12: v.view = native_views[1 - eye].view; break;
            }
            expect(!valid_pair(bad), "partial/mixed/stale Native pair rejected");
        }
    }
    NativePairKey pair_key{ghost_owner, 0x30000, 0xa000, 0x40000, 1, 2472, 2416};
    NativePairGate pair_gate;
    expect(!pair_gate.observe(pair_key, 200, true) && !pair_gate.observe(pair_key, 200, true) &&
        !pair_gate.observe(pair_key, 201, true) && pair_gate.observe(pair_key, 202, true), "three distinct consecutive Native frames required");
    expect(!pair_gate.observe(pair_key, 204, true), "missed Native frame restarts pair validation");
    for (int change = 0; change < 8; ++change) {
        pair_gate.reset();
        pair_gate.observe(pair_key, 200, true); pair_gate.observe(pair_key, 201, true);
        auto changed = pair_key;
        switch (change) {
        case 0: changed.owner.draw.world++; break;
        case 1: changed.owner.draw.lifecycle++; break;
        case 2: changed.owner.controller++; break;
        case 3: changed.scene++; break;
        case 4: changed.right_state++; break;
        case 5: changed.render_target++; break;
        case 6: changed.generation++; break;
        case 7: changed.width++; break;
        }
        expect(!pair_gate.observe(changed, 202, true), "Native owner/resource/extent transition cannot reuse previous pair proof");
    }
    expect(!pair_gate.observe(pair_key, UINT64_MAX, true) && !pair_gate.observe(pair_key, 0, false), "overflow/invalid frame resets gate");
    NativeCall native_call{};
    NativeCall* native_slot{};
    {
        NativeCallScope active{native_slot, &native_call};
        { NativeCallScope suspended{native_slot, nullptr}; expect(!native_slot, "recursive Native setup is observation-free"); }
        expect(native_slot == &native_call, "Native context restored on nested return");
    }
    expect(!native_slot, "Native call context does not escape construction");
    std::array<uint8_t, 0xe00> secondary_view{};
    secondary_view.fill(0x5a);
    uintptr_t original_family = 0x20000, right_family = 0x21000;
    int32_t secondary_pass = 2, primary_index = 0;
    std::memcpy(secondary_view.data() + 8, &original_family, 8);
    std::memcpy(secondary_view.data() + 0xdd0, &secondary_pass, 4);
    std::memcpy(secondary_view.data() + 0xdd8, &primary_index, 4);
    const auto secondary_before = secondary_view;
    {
        NativeSingletonScope singleton;
        expect(!singleton.apply(secondary_view.data(), original_family + 1, right_family), "wrong owner does not mutate a view");
        expect(secondary_view == secondary_before, "failed singleton validation is byte-for-byte unchanged");
        expect(singleton.apply(secondary_view.data(), original_family, right_family), "validated secondary singleton is scoped");
        expect(!singleton.apply(secondary_view.data(), original_family, right_family), "scope cannot adopt a second view");
        for (size_t i = 0; i < secondary_view.size(); ++i) {
            if ((i < 8 || i >= 16) && (i < 0xdd0 || i >= 0xdd4)) {
                expect(secondary_view[i] == secondary_before[i], "singleton preserves state, matrices, eye index and all unrelated bytes");
            }
        }
    }
    expect(secondary_view == secondary_before, "singleton restores original family/pass on scope exit");
    std::array<uint8_t, 0x198> copied_family{};
    copied_family.fill(0x5a);
    const std::array<uintptr_t, 4> borrowed{1, 2, 3, 4};
    const std::array<uintptr_t, 4> installed{1, 20, 0, 4};
    std::memcpy(copied_family.data() + 0x160, installed.data(), sizeof(installed));
    const auto copied_before = copied_family;
    release_borrowed_native_interfaces(copied_family.data(), borrowed);
    std::array<uintptr_t, 4> interfaces_after{};
    std::memcpy(interfaces_after.data(), copied_family.data() + 0x160, sizeof(interfaces_after));
    expect(interfaces_after == std::array<uintptr_t, 4>{0, 20, 0, 0}, "only borrowed interfaces relinquished, including fourth interface; newly installed interface kept");
    for (size_t i = 0; i < copied_family.size(); ++i) {
        if (i < 0x160 || i >= 0x180) { expect(copied_family[i] == copied_before[i], "interface release leaves arrays/shared references untouched"); }
    }
    GhostOwnerGate ghost_gate;
    for (uint64_t frame = 100; frame < 111; ++frame) {
        expect(!ghost_gate.observe(ghost_owner, frame, 1000 + frame), "Ghost requires twelve distinct stable frames");
        expect(!ghost_gate.begin_bootstrap(frame, true, true, true), "early bootstrap is refused");
    }
    expect(ghost_gate.observe(ghost_owner, 111, 1111), "stable owner reaches bounded bootstrap gate");
    expect(ghost_gate.observe(ghost_owner, 111, 1112) && ghost_gate.stable_frames == 12,
        "repeated callbacks do not inflate frame stability");
    expect(!ghost_gate.begin_bootstrap(111, false, true, true), "remap-only never allocates a view state");
    expect(!ghost_gate.begin_bootstrap(111, true, false, true), "left eye never bootstraps");
    expect(!ghost_gate.begin_bootstrap(111, true, true, false), "existing pair never bootstraps");
    expect(ghost_gate.begin_bootstrap(111, true, true, true), "one bounded lazy allocation is permitted");
    expect(!ghost_gate.begin_bootstrap(112, true, true, true), "allocation retry requires cooldown");
    expect(ghost_gate.begin_bootstrap(141, true, true, true) && ghost_gate.begin_bootstrap(171, true, true, true),
        "only three bounded attempts per owner");
    expect(!ghost_gate.begin_bootstrap(201, true, true, true), "allocation attempts do not repeat indefinitely");
    ghost_gate.reset();
    for (uint64_t frame = 300; frame < 312; ++frame) { ghost_gate.observe(ghost_owner, frame, 1300 + frame); }
    expect(!ghost_gate.begin_bootstrap(313, true, true, true), "toggle spam cannot reset the allocation budget");
    for (int change = 0; change < 7; ++change) {
        auto moved = ghost_owner;
        switch (change) {
        case 0: ++moved.draw.world; break;
        case 1: ++moved.draw.target; break;
        case 2: ++moved.draw.lifecycle; break;
        case 3: ++moved.player; break;
        case 4: ++moved.controller; break;
        case 5: ++moved.object_index; break;
        default: ++moved.left_state; break;
        }
        expect(!ghost_gate.observe(moved, 400 + change, 1700 + change), "changed owner cannot use previous history immediately");
    }
    expect(!ghost_gate.observe(ghost_owner, 500, 3000), "stale owner observation must warm again");
    expect(!ghost_gate.observe({}, 501, 3001), "invalid owner clears stability without dereferencing it");
    expect(!ghost_pair_valid(0, 1) && !ghost_pair_valid(1, 0) && !ghost_pair_valid(1, 1) && ghost_pair_valid(1, 2),
        "per-eye histories must be non-null and different");
    GhostCall ghost_call{};
    GhostCall* scoped_call{};
    {
        GhostCallScope root_scope{scoped_call, &ghost_call};
        expect(scoped_call == &ghost_call, "root view owns its bounded call context");
        {
            GhostCallScope recursive_scope{scoped_call, nullptr};
            expect(scoped_call == nullptr, "recursive view cannot borrow the outer eye identity");
        }
        expect(scoped_call == &ghost_call, "outer call identity restored after recursive pass-through");
    }
    expect(scoped_call == nullptr, "call identity never survives view construction");
    ghost_call.owner = ghost_owner;
    ghost_call.options = 0xa000;
    expect(!ghost_call.normalize_projection(0x7000, 0x3000, 0xa000, 1), "ordinary projection is untouched");
    ghost_call.bootstrap = true;
    expect(ghost_call.normalize_projection(0x7000, 0x3000, 0xa000, 1), "bootstrap projection keeps original zero index");
    expect(!ghost_call.normalize_projection(0x7001, 0x3000, 0xa000, 1) &&
        !ghost_call.normalize_projection(0x7000, 0x3001, 0xa000, 1) &&
        !ghost_call.normalize_projection(0x7000, 0x3000, 0xa001, 1) &&
        !ghost_call.normalize_projection(0x7000, 0x3000, 0xa000, 0), "unrelated and recursive projection arguments are untouched");
    std::array<uint8_t, 0x300> options{};
    options.fill(0x5a);
    auto options_before = options;
    restore_ghost_bootstrap_labels(options.data());
    int32_t ghost_pass{}, ghost_index{};
    std::memcpy(&ghost_pass, options.data() + 0x1c0, 4);
    std::memcpy(&ghost_index, options.data() + 0x1c4, 4);
    expect(ghost_pass == 1 && ghost_index == 0, "bootstrap labels return to PRIMARY singleton index zero");
    for (size_t i = 0; i < options.size(); ++i) {
        if (i < 0x1c0 || i >= 0x1c8) {
            expect(options[i] == options_before[i], "normalization preserves projection, rect, state, camera and all other options");
        }
    }
    expect(ghost_consumer_matches(1, 1, 2, 2, 1, 0), "constructed consumer must confirm exact family and state");
    expect(!ghost_consumer_matches(1, 2, 2, 2, 1, 0) && !ghost_consumer_matches(1, 1, 2, 3, 1, 0) &&
        !ghost_consumer_matches(1, 1, 2, 2, 2, 0) && !ghost_consumer_matches(1, 1, 2, 2, 1, 1),
        "wrong family, state, pass or index cannot report Ghost Fix active");
    expect(!redraw.pending() && !redraw.take(owner, 101, 1000, true), "no invented second-eye work");
    expect(redraw.schedule(owner, 101, 1000, true), "one completed even eye schedules its next odd eye");
    expect(!redraw.schedule(owner, 101, 1000, true), "duplicate work is bounded to one ticket");
    expect(redraw.take(owner, 101, 1500, true).has_value(), "fresh same-owner same-frame ticket accepted");
    expect(!redraw.pending() && !redraw.take(owner, 101, 1500, true), "ticket consumed before a reentrant draw");
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        expect(redraw.schedule(owner, 101, 1000, true), "schedule lifecycle test");
        auto current = owner;
        uint64_t frame = 101, now = 1001;
        bool eligible = true;
        switch (mutation) {
        case 0: current.engine += 8; break;
        case 1: current.client += 8; break;
        case 2: current.viewport += 8; break;
        case 3: current.world += 8; break;
        case 4: current.target += 8; break;
        case 5: ++current.lifecycle; break;
        case 6: ++current.width; break;
        case 7: ++current.height; break;
        case 8: ++frame; break;
        case 9: --frame; break;
        case 10: now = 1501; break;
        case 11: now = 999; break;
        case 12: eligible = false; break;
        }
        expect(!redraw.take(current, frame, now, eligible), "changed owner, mode, time or frame fails closed");
        expect(!redraw.pending(), "rejected ticket cannot delay a future world tick");
    }
    expect(!redraw.schedule({}, 101, 1000, true), "missing current chain rejected");
    expect(!redraw.schedule(owner, 101, 0, true), "missing observation time rejected");
    expect(!redraw.schedule(owner, 100, 1000, true), "wrong next-eye parity rejected");
    expect(!redraw.schedule(owner, UINT64_MAX, 1000, true), "frame overflow boundary rejected");
    auto invalid_owner = owner; invalid_owner.width = 16385;
    expect(!redraw.schedule(invalid_owner, 101, 1000, true), "invalid viewport dimensions rejected");
    invalid_owner = owner; invalid_owner.height = 0;
    expect(!redraw.schedule(invalid_owner, 101, 1000, true), "minimized viewport rejected");
    expect(redraw.schedule(owner, 101, 1000, true), "reschedule after failed observation");
    expect(!redraw.schedule(owner, 101, 1001, false) && !redraw.pending(), "leaving Synced cancels pending work");
    expect(completed_synced_redraw(owner, owner, 101, 101, 7, 8, true), "exact completed second draw can skip one tick");
    expect(!completed_synced_redraw(owner, owner, 101, 101, 7, 7, true), "no Draw callback means no skipped tick");
    expect(!completed_synced_redraw(owner, owner, 101, 101, 7, 9, true), "multiple unexpected Draw callbacks rejected");
    expect(!completed_synced_redraw(owner, owner, 101, 102, 7, 8, true), "wrong completed frame cannot skip a tick");
    expect(!completed_synced_redraw(owner, owner, 101, 101, 7, 8, false), "mode changed during Draw cannot skip a tick");
    expect(!completed_synced_redraw(owner, {}, 101, 101, 7, 8, true), "owner lost during Draw cannot skip a tick");
    expect(!completed_synced_redraw(owner, owner, 101, 101, UINT64_MAX, 0, true), "serial wrap fails closed");
    for (uint64_t frame = 101; frame < 501; frame += 2) {
        expect(redraw.schedule(owner, frame, 2000, true), "successive world frames can schedule a pair");
        expect(!redraw.take(owner, frame % 4, 2001, true), "pose ring index must not replace a full frame token");
        expect(redraw.schedule(owner, frame, 2001, true), "rejected frame does not poison later valid work");
        expect(redraw.take(owner, frame, 2002, true).has_value(), "full next-eye token survives ring wraps");
    }

    D3D12_RESOURCE_DESC scene{};
    scene.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    scene.Width = 4944;
    scene.Height = 2416;
    scene.DepthOrArraySize = 1;
    scene.MipLevels = 1;
    scene.SampleDesc.Count = 1;
    scene.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    scene.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    expect(valid_texture_desc(scene, 4944, 2416, false), "live packed scene descriptor accepted");
    expect(!valid_texture_desc(scene, 2472, 2416, false), "single-eye scene extent rejected");
    expect(!valid_texture_desc(scene, 1920, 1080, true), "scene is not desktop UI");
    for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM}) {
        auto d = scene; d.Format = format;
        expect(valid_texture_desc(d, 4944, 2416, false), "HDR color descriptor is structurally valid");
        expect(!valid_texture_desc(d, 4944, 2416, true), "HDR scene format not accepted as RGBA8 UI");
        expect(!copy_compatible_format(format), "HDR cannot reach the BGRA8 copy path without conversion");
    }
    for (auto format : {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        auto d = scene; d.Width = 2560; d.Height = 1440; d.Format = format;
        expect(valid_texture_desc(d, 2560, 1440, true), "UI resolution follows the window, not a fixed 1080p assumption");
    }
    for (auto format : {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        expect(copy_compatible_format(format), "BGRA family copy is compatible");
    }
    expect(!copy_compatible_format(DXGI_FORMAT_R8G8B8A8_UNORM), "different channel order requires conversion");
    expect(!copy_compatible_format(DXGI_FORMAT_UNKNOWN), "unknown copy format rejected");
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        auto d = scene;
        switch (mutation) {
        case 0: d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; break;
        case 1: d.Width = 0; break;
        case 2: d.Width = UINT64_MAX; break;
        case 3: d.Height = 0; break;
        case 4: d.DepthOrArraySize = 2; break;
        case 5: d.MipLevels = 2; break;
        case 6: d.SampleDesc.Count = 2; break;
        case 7: d.SampleDesc.Quality = 1; break;
        case 8: d.Format = DXGI_FORMAT_UNKNOWN; break;
        case 9: d.Format = DXGI_FORMAT_D32_FLOAT; break;
        case 10: d.Flags = D3D12_RESOURCE_FLAG_NONE; break;
        case 11: d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; break;
        case 12: d.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE; break;
        }
        expect(!valid_texture_desc(d, 4944, 2416, false), "invalid descriptor cannot publish a scene");
    }
    expect(!valid_texture_desc(scene, 0, 0, false), "uninitialized dimensions rejected");
    expect(!valid_texture_desc(scene, 16385, 2416, false), "unbounded dimensions rejected");
    expect(!scene_observation_fresh(1000, 0), "unobserved source rejected");
    expect(scene_observation_fresh(1000, 1000), "new source is fresh");
    expect(scene_observation_fresh(1500, 1000), "freshness boundary inclusive");
    expect(!scene_observation_fresh(1501, 1000), "stalled source expires");
    expect(!scene_observation_fresh(999, 1000), "regressed time fails closed");
    SceneStability stability;
    const SceneIdentity identity{0x1000, 0x2000, 0x3000, 4944, 2416, DXGI_FORMAT_B8G8R8A8_TYPELESS};
    expect(!stability.observe(identity), "first observation warms up");
    expect(stability.observe(identity) && stability.observe(identity), "unchanged validated source becomes stable");
    auto changed = identity; changed.native += 8;
    expect(!stability.observe(changed) && stability.observe(changed), "new native resource must warm up");
    changed.texture += 8;
    expect(!stability.observe(changed) && stability.observe(changed), "new RHI owner must warm up");
    changed.viewport += 8;
    expect(!stability.observe(changed) && stability.observe(changed), "new viewport must warm up");
    changed.width += 2;
    expect(!stability.observe(changed) && stability.observe(changed), "resize must warm up");
    changed.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    expect(!stability.observe(changed) && stability.observe(changed), "format change must warm up");
    expect(!stability.observe({}), "missing source invalidates previous observation");
    expect(!stability.observe(changed) && stability.observe(changed), "recovered source warms up again");
    stability.reset();
    expect(!stability.observe(changed), "explicit retirement resets stability");

    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    expect(intersects_image(0x1100, 8, 0x1000, 0x1000), "inside image denied");
    expect(intersects_image(0xfff, 2, 0x1000, 0x1000), "crossing start denied");
    expect(intersects_image(0x1fff, 2, 0x1000, 0x1000), "crossing end denied");
    expect(!intersects_image(0xff8, 8, 0x1000, 0x1000), "adjacent before image allowed");
    expect(!intersects_image(0x2000, 8, 0x1000, 0x1000), "adjacent after image allowed");
    expect(intersects_image(max - 2, 8, 0x1000, 0x1000), "address wrap rejected");
    expect(intersects_image(0x1000, 8, max - 2, 8), "image wrap rejected");
    expect(intersects_image(0x3000, 0, 0x1000, 0x1000), "empty write rejected");
    uintptr_t unreadable{};
    expect(!read_memory(1, &unreadable, sizeof(unreadable)), "unreadable pointer fails without dereference");

    image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_memory(image, &dos, sizeof(dos)) || !read_memory(image + dos.e_lfanew, &nt, sizeof(nt))) { return 2; }
    image_size = nt.OptionalHeader.SizeOfImage;
    const auto first = reinterpret_cast<uintptr_t>(&original);
    const auto second = reinterpret_cast<uintptr_t>(&unrelated);
    const auto dest = reinterpret_cast<uintptr_t>(&replacement);
    const auto slot = reinterpret_cast<uintptr_t>(&single[0]);
    const auto many = reinterpret_cast<uintptr_t>(&table[0]);
    expect(find_virtual_slot_in_image(many, first, 3, image) == 0, "ambiguous duplicate function rejected");
    expect(find_virtual_slot_in_image(many, second, 3, image) == many + sizeof(void*), "unique slot resolved");
    expect(find_virtual_slot_in_image(slot, first, 1, image) == slot, "bounded single slot resolved");
    expect(!find_virtual_slot_in_image(slot + 1, first, 1, image), "misaligned slot rejected");
    expect(!find_virtual_slot_in_image(slot, first, 193, image), "unbounded table rejected");
    expect(!find_virtual_slot_in_image(slot, first, 0, image), "empty table rejected");
    expect(!find_virtual_slot(slot, first, 1), "production resolver still requires exact build validation");

    std::array<uint8_t, 32> before{}, after{};
    expect(read_memory(first, before.data(), before.size()), "save original instructions");
    ImageTable table_before{}, table_after{};
    const auto source = reinterpret_cast<uintptr_t>(source_table.functions);
    expect(read_memory(reinterpret_cast<uintptr_t>(&source_table), &table_before, sizeof(table_before)), "save complete image table");
    const auto original_protection = protection(source);
    ObjectVTable hook;
    dispatched_hook = &hook;
    const std::array<SlotPatch, 1> patch{{{0, first, dest}}};
    const std::array<SlotPatch, 1> wrong{{{0, second, dest}}};
    const std::array<SlotPatch, 1> bad_dest{{{0, first, source}}};
    const std::array<SlotPatch, 1> bad_index{{{3, first, dest}}};
    const std::array<SlotPatch, 2> duplicate{{{0, first, dest}, {0, first, second}}};
    expect(!hook.prepare(first, 3, patch, image), "executable source table rejected");
    expect(!hook.prepare(source, 3, wrong, image), "wrong original rejected");
    expect(!hook.prepare(source, 3, bad_dest, image), "non-executable replacement rejected");
    expect(!hook.prepare(source, 3, bad_index, image), "out-of-bounds patch rejected");
    expect(!hook.prepare(source, 3, duplicate, image), "duplicate patches rejected");
    expect(!hook.prepare(source, 3, patch, image + 0x1000), "wrong image rejected");
    expect(!hook.prepare(source + 1, 3, patch, image), "misaligned source rejected");
    expect(!hook.prepare(source, 257, patch, image), "oversized copy rejected");
    expect(!hook.prepare(source, 0, patch, image), "empty copy rejected");
    expect(!hook.prepared() && !hook.active(), "failed preparation never publishes an adapter");
    expect(hook.prepare(source, 3, patch, image), "bounded private table prepared");
    expect(hook.prepare(source, 3, patch, image), "identical preparation is idempotent");
    expect(!hook.prepare(source, 3, wrong, image), "prepared table cannot be changed");
    expect(!hook.install(reinterpret_cast<uintptr_t>(&image_object)), "image-backed object writes forbidden");
    expect(!hook.install(source), "image vtable writes forbidden");
    expect(!hook.install(1) && !hook.install(0), "invalid objects rejected");
    auto* const objects = static_cast<Object*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!objects) { return 2; }
    objects[0] = {source, 0xabcdef};
    objects[1] = {source, 0xabcdef};
    const auto object = reinterpret_cast<uintptr_t>(objects);
    expect(!hook.install(object + 1), "unaligned object rejected");
    DWORD old{}, ignored{};
    VirtualProtect(objects, 4096, PAGE_READONLY, &old);
    expect(!hook.install(object), "read-only object is not made writable");
    VirtualProtect(objects, 4096, old, &ignored);
    std::atomic_bool stop{}, bad_result{};
    std::atomic_uint64_t calls{};
    std::thread reader{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            uintptr_t current{}, fn{};
            if (!read_memory(object, &current, sizeof(current)) || !read_memory(current, &fn, sizeof(fn))) {
                bad_result.store(true); break;
            }
            const auto value = reinterpret_cast<Fn>(fn)(1);
            if (value != 8 && value != 18) { bad_result.store(true); }
            ++calls;
        }
    }};
    uintptr_t retained{};
    for (int i = 0; i < 32; ++i) {
        expect(hook.install(object), "validated private object installs");
        expect(hook.active() && hook.owns(object) && hook.original_address() == first, "original published before dispatch");
        retained = read_slot(object);
        expect(reinterpret_cast<Fn>(read_slot(retained))(1) == 18, "callback calls original without trampoline");
        expect(read_slot(retained + 8) == second && read_slot(retained + 16) == first, "other virtual entries preserved");
        expect(read_slot(retained - 8) == source_table.prefix, "metadata preceding vtable preserved");
        expect(protection(retained) == PAGE_READONLY, "published table is immutable");
        expect(objects[1].vtable == source && objects[0].canary == 0xabcdef, "only selected object's vptr changed");
        expect(protection(source) == original_protection, "source image protection unchanged");
        expect(hook.image_unchanged(), "live source-table validation remains satisfied");
        expect(read_memory(reinterpret_cast<uintptr_t>(&source_table), &table_after, sizeof(table_after)) &&
            memcmp(&table_before, &table_after, sizeof(table_before)) == 0, "entire original image vtable unchanged while active");
        expect(hook.reset(), "object reset succeeds");
        expect(read_slot(object) == source && hook.call<int>(1) == 8, "original stays callable after removal");
    }
    while (calls.load() == 0) { std::this_thread::yield(); }
    stop.store(true, std::memory_order_release);
    reader.join();
    expect(!bad_result.load(), "concurrent dispatch sees complete immutable table");
    expect(read_memory(first, after.data(), after.size()) && before == after, "instructions unchanged");
    expect(reinterpret_cast<Fn>(read_slot(retained))(1) == 18, "in-flight table remains valid after reset");
    expect(hook.install(object), "install for ownership test");
    InterlockedExchangePointer(reinterpret_cast<void**>(object), reinterpret_cast<void*>(many));
    expect(!hook.install(object), "reinstall detects competing owner");
    expect(hook.reset() && read_slot(object) == many, "reset does not overwrite competing owner");
    expect(!hook.install(object), "wrong object's original vtable rejected");
    objects[0].vtable = source;
    expect(hook.install(object) && hook.reset(), "same immutable table can be reinstalled");
    ObjectVTable multiple;
    const std::array<SlotPatch, 2> two{{{0, first, second}, {2, first, second}}};
    expect(multiple.prepare(source, 3, two, image) && multiple.install(object), "multiple slots publish together");
    const auto two_table = read_slot(object);
    expect(read_slot(two_table) == second && read_slot(two_table + 8) == second &&
        read_slot(two_table + 16) == second, "complete multi-slot replacement observed");
    expect(multiple.image_unchanged() && multiple.original_address(0) == first &&
        multiple.original_address(1) == second && multiple.original_address(2) == first,
        "multi-slot adapter retains the exact original table");
    expect(multiple.reset() && read_slot(object) == source, "multi-slot removal restores only the object pointer");
    VirtualFree(objects, 0, MEM_RELEASE);

    uint32_t previous = 0x1234;
    expect(!protect_external_memory(reinterpret_cast<uint8_t*>(first), 8, PAGE_EXECUTE_READWRITE,
        &previous, image, image_size) && previous == 0x1234, "game-image protection request fails without side effects");
    expect(!protect_external_memory(reinterpret_cast<uint8_t*>(source), 8, PAGE_READWRITE,
        &previous, image, image_size), "even non-executable image table writes are refused");
    safetyhook::set_protection_override(protect);
    auto inline_hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(first), reinterpret_cast<void*>(second),
        safetyhook::InlineHook::StartDisabled);
    if (inline_hook) {
        expect(!inline_hook->enable(), "SafetyHook refuses a game-image inline write");
        inline_hook->reset();
    }
    expect(read_memory(first, after.data(), after.size()) && before == after, "denied inline install leaves code unchanged");
    safetyhook::set_protection_override(nullptr);
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) { return 2; }
    expect(protect_external_memory(page, 0x1000, PAGE_READONLY, &previous, image, image_size),
        "external allocations retain normal protection behavior");
    expect(protection(reinterpret_cast<uintptr_t>(page)) == PAGE_READONLY, "external protection applied");
    VirtualFree(page, 0, MEM_RELEASE);
    std::cout << "NASCAR compatibility failures: " << failures << '\n';
    return failures ? 1 : 0;
}
