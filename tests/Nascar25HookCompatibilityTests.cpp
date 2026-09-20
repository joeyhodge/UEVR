#define NOMINMAX
#include <Windows.h>
#include <safetyhook/os.hpp>
#include <iostream>
#include <vector>
#include "utility/NascarHookCompatibility.hpp"

namespace {
int failures{};
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAILED NASCAR25: " << message << '\n'; }
}
template<class T> void put(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template<class T> T get(const std::vector<uint8_t>& bytes, size_t offset) {
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
}

int run_nascar25_tests() {
    namespace n = uevr::nascar;
    namespace old = uevr::nascar26;
    n::initialize();
    expect(!n::is_target() && !n::is_validated_build() && !n::is_title25() && !safetyhook::has_protection_override(),
        "unrelated executable cannot enable either adapter or protection override");
    expect(n::title25::matches_name(L"D:\\Games\\NASCAR25_Steam-Win64-Shipping.exe") &&
        n::title25::matches_name(L"c:/anywhere/nascar25_steam-win64-shipping.EXE"), "exact basename independent of install path");
    for (auto name : {L"NASCAR26_Steam-Win64-Shipping.exe", L"NASCAR25_Steam-Win64-Shipping.exe.bak",
        L"NASCAR25.runtime-analysis.exe", L"NASCAR25.exe", L"Maverick.exe", L"Other-Win64-Shipping.exe", L""}) {
        expect(!n::title25::matches_name(name), "synthetic images and other games excluded");
    }
    expect(!old::matches_name(L"NASCAR25_Steam-Win64-Shipping.exe"), "25 never inherits 26 identity");
    for (bool title25 : {false, true}) {
        for (bool validated : {false, true}) {
            for (bool dx12 : {false, true}) {
                expect(n::uses_legacy_dedicated_ui(title25, validated, dx12) == (title25 && validated && dx12),
                    "legacy UI allocation and window extent require exact NASCAR25 validation and DX12");
            }
        }
    }
    expect(!n::uses_legacy_dedicated_ui(true), "other games cannot enter the NASCAR25 UI route at runtime");
    {
        using Evidence = n::title25::DedicatedUIReadiness;
        constexpr std::array guards{
            &Evidence::exact_title, &Evidence::validated_build, &Evidence::dx12,
            &Evidence::code_preserving_mode, &Evidence::game_data_initialized, &Evidence::engine_valid,
            &Evidence::slate_hook_valid, &Evidence::stable_slate_draw, &Evidence::render_callback_seen,
            &Evidence::packed_scene_target_valid,
        };
        constexpr auto combinations = 1u << guards.size();
        for (unsigned mask = 0; mask < combinations; ++mask) {
            Evidence evidence{};
            for (size_t i = 0; i < guards.size(); ++i) { evidence.*guards[i] = (mask & (1u << i)) != 0; }
            expect(n::title25::can_initialize_dedicated_ui(evidence) == (mask == combinations - 1),
                "UI startup requires all exact-build, mode, Slate, callback and packed-target evidence");
        }

        Evidence startup{true, true, true, true, true, true, true, true, false, true};
        expect(!n::title25::can_initialize_dedicated_ui(startup),
            "packed scene alone cannot allocate UI before a validated render callback");
        startup.render_callback_seen = true;
        expect(n::title25::can_initialize_dedicated_ui(startup),
            "Native Fix startup UI can initialize without generic PreRender discovery");
        startup.packed_scene_target_valid = false;
        expect(!n::title25::can_initialize_dedicated_ui(startup),
            "old callback evidence cannot authorize UI creation with a missing or resized scene target");
        startup.packed_scene_target_valid = true;
        expect(n::title25::can_initialize_dedicated_ui(startup),
            "revalidated packed target permits UI retry without toggling Native Fix");
    }
    for (bool target : {false, true}) {
        for (bool build : {false, true}) {
            for (bool dx12 : {false, true}) {
                for (bool contract : {false, true}) {
                    expect(n::title25::uses_validated_rhi_root(target, build, dx12, contract) ==
                        (target && build && dx12 && contract), "exact RHI root requires all NASCAR25 guards");
                }
            }
        }
    }
    expect(!n::title25::validate_rhi_command_layout(), "unrelated process cannot validate NASCAR25 command ABI");
    {
        std::vector<uint8_t> command_list(0x38);
        put(command_list, 0, uintptr_t{0x1000}); // Plausible allocator cursor, never the root.
        put(command_list, 0x28, uintptr_t{0x2000});
        const auto before = command_list;
        constexpr uintptr_t base = 0x10000;
        size_t reads{};
        auto read = [&](uintptr_t address, void* output, size_t bytes) {
            ++reads;
            if (address != base + 0x28 || bytes != sizeof(uintptr_t)) { return false; }
            std::memcpy(output, command_list.data() + 0x28, bytes);
            return true;
        };
        expect(n::title25::read_rhi_command_root(base, true, read) == uintptr_t{0x2000} && reads == 1,
            "read only the queued root, not a command-like allocator cursor");
        expect(command_list == before, "command-root observation never mutates command-list storage");
        reads = 0;
        expect(!n::title25::read_rhi_command_root(base, false, read) && reads == 0,
            "unvalidated ABI never reads memory");
        for (uintptr_t invalid : {uintptr_t{}, uintptr_t{base + 1}, UINTPTR_MAX - uintptr_t{7}}) {
            expect(!n::title25::read_rhi_command_root(invalid, true, read) && reads == 0,
                "null, misaligned, or overflowing command list rejected before reading");
        }
        put(command_list, 0x28, uintptr_t{});
        expect(n::title25::read_rhi_command_root(base, true, read) == uintptr_t{},
            "empty root retained; plausible allocator cursor is not a fallback");
        put(command_list, 0x28, uintptr_t{0x2001});
        expect(!n::title25::read_rhi_command_root(base, true, read), "unaligned command root rejected");
        expect(!n::title25::read_rhi_command_root(base, true,
            [](uintptr_t, void*, size_t) { return false; }), "unreadable root fails closed without a scan");
    }
    expect(n::title25::matches_build_metadata(0x50005, 0x40000, 0x9fdd0f8a, 0xbb5e000), "captured 5.5.4 metadata accepted");
    for (uint32_t version : {0x4001b, 0x50004, 0x50006, 0x50007, 0x50008}) {
        expect(!n::title25::matches_build_metadata(version, 0x40000, 0x9fdd0f8a, 0xbb5e000), "other engine layouts rejected");
    }
    expect(!n::title25::matches_build_metadata(0x50005, 0x30000, 0x9fdd0f8a, 0xbb5e000) &&
        !n::title25::matches_build_metadata(0x50005, 0x40000, 0x9fdd0f8b, 0xbb5e000) &&
        !n::title25::matches_build_metadata(0x50005, 0x40000, 0x9fdd0f8a, 0xbb5f000), "unknown update fails closed");
    expect(!n::title25::validate_synced_redraw() && !n::title25::validate_ghost_view_setup() &&
        !n::title25::validate_native_renderer() && !n::title25::validate_native_rooting() && !n::title25::read_native_display_gamma(),
        "no callable validation or gamma reads without verified executable");

    const auto& a = n::layout25;
    const auto& b = n::layout26;
    expect(a.tick_slot == 97 && b.tick_slot == 94 && a.engine_viewport_offset == 0xbd0 && b.engine_viewport_offset == 0xc88,
        "engine differences remain explicit");
    expect(a.init_options_slot == 94 && a.calc_scene_view_slot == 95 && a.projection_data_slot == 111 &&
        b.init_options_slot == 91 && b.calc_scene_view_slot == 92 && b.projection_data_slot == 109,
        "LocalPlayer dispatch uses each exact virtual layout");
    expect(a.stereo_slots == 20 && a.stereo_manager_slot == 15 && b.stereo_slots == 21 && b.stereo_manager_slot == 16 &&
        sizeof(n::WindowSize25) == 16 && sizeof(n::WindowSize) == 8, "legacy immediate and RDG callbacks have different ABIs");
    expect(a.texture_getter_slot == 4 && a.texture_resource_offset == 0xb8 && a.texture_owner_resource_offset == 0x118 &&
        b.texture_getter_slot == 5 && b.texture_resource_offset == 0xd0 && b.texture_owner_resource_offset == 0x138,
        "UI and capture chains do not borrow 26 offsets");
    expect(!a.packed_object_item && a.object_item_object_offset == 0 && a.object_item_flags_offset == 8 &&
        b.packed_object_item && b.object_item_object_offset == 8 && b.object_item_flags_offset == 0,
        "standard and packed UObject items stay distinct");
    expect(b.engine_global_rva == old::engine_global_rva && b.engine_vtable_rva == old::engine_vtable_rva &&
        b.tick_rva == old::tick_rva && b.draw_rva == old::draw_rva && b.viewport_vtable_rva == old::viewport_vtable_rva &&
        b.slate_getter_return_rva == old::slate_getter_return_rva && b.renderer_plural_rva == old::renderer_plural_rva &&
        b.family_copy_rva == old::family_copy_rva && b.family_delete_rva == old::family_delete_rva &&
        b.native_owner_set_flags_rva == old::native_owner_set_flags_rva, "working 26 entrypoints unchanged");

    for (const auto* abi : {&a, &b}) {
        std::vector<uint8_t> options(0x300, 0x5a);
        put(options, abi->options_pass_offset, int32_t{2});
        put(options, abi->options_index_offset, int32_t{1});
        auto expected = options;
        put(expected, abi->options_pass_offset, int32_t{1});
        put(expected, abi->options_index_offset, int32_t{0});
        n::restore_ghost_bootstrap_labels(options.data(), *abi);
        expect(options == expected, "bootstrap restores only pass/index; projection, state and other bytes preserved");

        std::vector<uint8_t> view(0x1700, 0x5a);
        put(view, 8, uintptr_t{0x1000}); put(view, abi->view_pass_offset, int32_t{2});
        put(view, abi->view_index_offset, int32_t{1}); put(view, abi->view_primary_offset, int32_t{0});
        const auto original = view;
        {
            n::NativeSingletonScope scope;
            expect(scope.apply(view.data(), 0x1000, 0x2000, *abi), "validated secondary singleton accepted");
            auto expected_view = original;
            put(expected_view, 8, uintptr_t{0x2000}); put(expected_view, abi->view_pass_offset, int32_t{1});
            expect(view == expected_view, "only family and pass change; eye index/history and projection preserved");
            expect(!scope.apply(view.data(), 0x2000, 0x3000, *abi), "scope cannot be applied twice");
            scope.restore(); scope.restore();
        }
        expect(view == original, "full view restored on normal scope exit and repeated restore");
        for (int reason = 0; reason < 3; ++reason) {
            auto invalid = original;
            if (reason == 0) { put(invalid, 8, uintptr_t{0x9999}); }
            if (reason == 1) { put(invalid, abi->view_pass_offset, int32_t{1}); }
            if (reason == 2) { put(invalid, abi->view_primary_offset, int32_t{-1}); }
            const auto before = invalid;
            n::NativeSingletonScope scope;
            expect(!scope.apply(invalid.data(), 0x1000, 0x2000, *abi) && invalid == before,
                "wrong family/pass/primary index causes no writes");
        }

        std::vector<uint8_t> family(0x200, 0x5a);
        const std::array<uintptr_t, 4> borrowed{0x100, 0x200, 0x300, 0x400};
        std::memcpy(family.data() + abi->family_borrowed_offset, borrowed.data(), sizeof(borrowed));
        put(family, abi->family_borrowed_offset + 16, uintptr_t{0x999});
        auto expected_family = family;
        for (size_t i : {0, 1, 3}) { put(expected_family, abi->family_borrowed_offset + i * 8, uintptr_t{}); }
        n::release_borrowed_native_interfaces(family.data(), borrowed, *abi);
        expect(family == expected_family, "clear only shallow-borrowed interfaces, retain new owned interface and family fields");
        expect(abi->family_borrowed_offset + sizeof(borrowed) <= abi->family_size && abi->family_size <= 0x1b0,
            "family storage covers both exact copy/destructor extents");
    }

    struct Object { uintptr_t vtable; uint32_t flags; int32_t index; } object{0x1000, 0, 7};
    struct Item { uintptr_t object; uint32_t flags; int32_t cluster, serial, refcount; } item{
        reinterpret_cast<uintptr_t>(&object), n::native_owner_root_flag, 0, 23, 0};
    static_assert(sizeof(Item) == 24 && offsetof(Item, flags) == 8 && offsetof(Item, serial) == 16);
    n::NativeOwnedObject owner{reinterpret_cast<uintptr_t>(&object), object.vtable, object.index};
    const auto address = reinterpret_cast<uintptr_t>(&item);
    expect(n::title25::native_owned_object_valid(owner, address, true), "rooted UE5.5 object matches standard item");
    for (uint32_t flag : {1u << 21, 1u << 28, 1u << 31}) {
        item.flags = n::native_owner_root_flag | flag;
        expect(!n::title25::native_owned_object_valid(owner, address, false), "garbage/unreachable item rejected");
    }
    item.flags = 0;
    expect(n::title25::native_owned_object_valid(owner, address, false) &&
        !n::title25::native_owned_object_valid(owner, address, true), "unrooted creation does not count as rooted publication");
    item.flags = n::native_owner_root_flag;
    object.index = 8;
    expect(!n::title25::native_owned_object_valid(owner, address, true), "recycled object index rejected");
    object.index = 7; object.vtable = 0x2000;
    expect(!n::title25::native_owned_object_valid(owner, address, true), "wrong class rejected");
    object.vtable = 0x1000; item.object = 1;
    expect(!n::title25::native_owned_object_valid(owner, address, true), "wrong item rejected before object adoption");
    expect(!n::title25::native_owned_object_valid(owner, 1, true), "unreadable item fails without dereferencing object");

    for (auto extent : {std::pair{1280u, 720u}, std::pair{1920u, 1080u}, std::pair{2560u, 1440u}, std::pair{3840u, 2160u}}) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = extent.first; desc.Height = extent.second;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        expect(n::valid_texture_desc(desc, extent.first, extent.second, true), "dedicated UI extent not hardcoded to one monitor size");
        desc.DepthOrArraySize = 2;
        expect(!n::valid_texture_desc(desc, extent.first, extent.second, true), "array target not treated as packed UI");
    }
    for (int method = 0; method < 5; ++method) {
        expect(n::supports_rendering_method(method, false) == (method == 0 || method == 1), "Native/Synced only; no unvalidated mode");
        expect(!n::supports_rendering_method(method, true), "Extreme Compatibility cannot bypass protected-image safeguards");
    }
    std::cout << "NASCAR25 compatibility failures: " << failures << '\n';
    return failures;
}
