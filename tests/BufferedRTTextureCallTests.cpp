#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include "utility/BufferedRTTextureCall.hpp"
#include "mods/vr/StupidNeverDiesUI.hpp"

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

// Synthetic old-UE5 call contract. No game binary or private symbols are needed.
constexpr std::array<uint8_t, 39> stem{
    0x4c,0x8d,0x4d,0xc0,                    // descriptor = rbp-0x40
    0x48,0x8b,0x0d,0x10,0,0,0,             // receiver = [rip+0x10]
    0x4c,0x8d,0x05,0x20,0,0,0,             // command list = rip+0x20
    0x48,0x8d,0x54,0x24,0x70,               // hidden result = rsp+0x70
    0x48,0x8b,0x01,                         // vtable = [receiver]
    0xff,0x90,0x98,0x01,0,0,                // call [vtable+0x198]
    0x48,0x8b,0x44,0x24,0x70,               // consume the same hidden result
    0xc3,0x90
};

void test_discovery() {
    using namespace uevr::buffered_rt;
    expect(is_name_constructor(0x1000, 0x1000), "a known name constructor is never a texture creator");
    expect(!is_name_constructor(0x2000, 0x1000) && !is_name_constructor(0, 0), "unknown identities do not create false constructor matches");
    const auto found = find_after_name(stem, 0x100000);
    expect(found && found->callsite == 0x10001a && found->length == 6 &&
        found->vtable_offset == 0x198 && found->receiver_cell == 0x10001b &&
        found->command_list == 0x100032 && found->output == StackCell{NDR_RSP, 0x70},
        "complete descriptor/RHI/command-list/output tuple resolves without fixed EXE addresses");
    // The bug's constructor and a later state helper both precede the real call.
    std::vector<uint8_t> prefixed{0xe8,0,0,0,0, 0x83,0xf8,0, 0x90, 0xe8,0,0,0,0};
    prefixed.insert(prefixed.end(), stem.begin(), stem.end());
    const auto after_helpers = find_after_name(prefixed, 0x200000);
    expect(after_helpers && after_helpers->callsite == 0x200000 + 14 + 26,
        "constructor/default-state E8 helpers cannot become the recovered allocator");
    for (size_t i = 0; i < 37; ++i) {
        expect(!find_after_name(std::span{stem}.first(i), 0x100000), "truncated argument/call/output contracts fail closed");
    }
    for (size_t i : std::array<size_t, 7>{0,4,11,18,23,26,32}) {
        auto changed = stem; changed[i] = 0x90;
        expect(!find_after_name(changed, 0x100000), "mutated descriptor, receiver, command, output, vtable, call or consumption rejects");
    }
    auto wrong_output = stem; wrong_output[36] = 0x78;
    expect(!find_after_name(wrong_output, 0x100000), "a different returned reference does not prove the hidden-result ABI");
    auto bad_slot = stem; bad_slot[28] = 1;
    expect(!find_after_name(bad_slot, 0x100000), "unaligned virtual slot rejects");
    auto twice = std::vector<uint8_t>(stem.begin(), stem.end() - 2);
    twice.insert(twice.end(), stem.begin(), stem.end());
    expect(!find_after_name(twice, 0x100000), "ambiguous allocator sequences reject");
    expect(!find_after_name(stem, (std::numeric_limits<uintptr_t>::max)() - 10), "address-range overflow rejects");
}

void test_descriptor_and_binding() {
    using namespace uevr::buffered_rt;
    std::array<uint8_t, 0x100> original{};
    const uint32_t scene_width = 2468, scene_height = 2416;
    std::memcpy(original.data() + 0x24, &scene_width, 4);
    std::memcpy(original.data() + 0x28, &scene_height, 4);
    original[0x33] = 18;
    auto copy = original;
    expect(patch_descriptor(copy, scene_width, scene_height, 1920, 1080), "validated old descriptor accepts UI extent/format in a copy");
    uint32_t width{}, height{};
    std::memcpy(&width, copy.data() + 0x24, 4); std::memcpy(&height, copy.data() + 0x28, 4);
    expect(width == 1920 && height == 1080 && copy[0x33] == 2 && original[0x33] == 18,
        "the game keeps its original HDR format and scene dimensions");
    copy = original;
    expect(!patch_descriptor(copy, 1, 2, 1920, 1080) && copy == original, "wrong scene identity cannot patch the descriptor");
    expect(!patch_descriptor(copy, scene_width, scene_height, 0, 1080) && copy == original, "invalid UI extent leaves descriptor unchanged");
    std::memcpy(copy.data() + 0x40, &scene_width, 4); std::memcpy(copy.data() + 0x44, &scene_height, 4);
    expect(!patch_descriptor(copy, scene_width, scene_height, 1920, 1080), "ambiguous descriptor layout rejects before modification");
    TextureCall proof{}; proof.command_list = 0x90000; proof.output = {NDR_RSP, 0x70};
    const Arguments args{0x80000,0x30070,0x90000,0x30400,0xa0000,0x30000,0x30200};
    expect(matches_arguments(proof, args, 0x80000, 0x80000), "exact current RHI, immediate list and stack result are required");
    expect(!matches_arguments(proof, args, 0x80008, 0x80000), "a different RHI device cannot use the proof");
    auto bad = args; bad.command_list += 8;
    expect(!matches_arguments(proof, bad, 0x80000, 0x80000), "a different command-list owner rejects");
    bad = args; bad.output += 8;
    expect(!matches_arguments(proof, bad, 0x80000, 0x80000), "a different output reference rejects");
    bad = args; bad.descriptor = bad.output;
    expect(!matches_arguments(proof, bad, 0x80000, 0x80000), "descriptor/result overlap rejects");
    bad = args; bad.descriptor = 0;
    expect(!matches_arguments(proof, bad, 0x80000, 0x80000), "null descriptor rejects");
    proof.output.base = NDR_RAX;
    expect(!matches_arguments(proof, args, 0x80000, 0x80000), "non-stack output proof rejects");
}

void test_memory() {
    using uevr::buffered_rt::readable;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    expect(memory != nullptr, "memory fixture allocates");
    if (!memory) { return; }
    DWORD previous{};
    VirtualProtect(memory + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous);
    expect(readable(reinterpret_cast<uintptr_t>(memory), 0x100), "readable descriptor memory accepts");
    expect(!readable(reinterpret_cast<uintptr_t>(memory + info.dwPageSize - 8), 0x100), "crossing an inaccessible page rejects without dereference");
    expect(!uevr::buffered_rt::readable_texture(0x441d32) && !readable(0, 8), "name IDs and null are never read as texture pointers");
    expect(!readable((std::numeric_limits<uintptr_t>::max)() - 3, 8), "pointer overflow rejects");
    VirtualFree(memory, 0, MEM_RELEASE);
}

void test_retirement() {
    using namespace uevr::buffered_rt;
    constexpr uintptr_t base = 0x100000, destroy = 0x180000;
    constexpr std::array<uint8_t, 18> release{
        0x8b,0xc6,0xf0,0x0f,0xc1,0x41,0x08,0x25,0xff,0xff,0xff,0x3f,
        0x83,0xf8,0x01,0x75,0x05,0xe8};
    std::vector<uint8_t> code;
    const auto append = [&](uintptr_t target) {
        code.insert(code.end(), release.begin(), release.end());
        const auto displacement = static_cast<int32_t>(target - (base + code.size() + 4));
        const auto* bytes = reinterpret_cast<const uint8_t*>(&displacement);
        code.insert(code.end(), bytes, bytes + 4);
    };
    append(destroy);
    expect(!find_destroy(code, base), "one uncorroborated release is insufficient");
    append(destroy);
    expect(find_destroy(code, base) == destroy, "two engine releases must agree on the deferred destroy target");
    auto changed = code; changed[11] = 0x7f;
    expect(!find_destroy(changed, base), "a different refcount mask invalidates retirement proof");
    changed = code; changed[15] = 0x74;
    expect(!find_destroy(changed, base), "destroy must be conditional on exactly the last reference");
    append(destroy + 16);
    expect(!find_destroy(code, base), "disagreeing release targets reject");
    expect(!find_destroy(code, (std::numeric_limits<uintptr_t>::max)() - 8), "retirement window overflow rejects");

    struct Texture { uintptr_t vtable{}; LONG flags{}; LONG destroys{}; } texture;
    static_assert(offsetof(Texture, flags) == 8);
    const auto destroy_fake = +[](void* pointer) {
        auto* value = static_cast<Texture*>(pointer);
        InterlockedIncrement(&value->destroys);
        InterlockedOr(&value->flags, 0x40000000);
    };
    const ReleaseTexture deleter{reinterpret_cast<uintptr_t>(destroy_fake)};
    texture.flags = 2;
    {
        std::unique_ptr<Texture, ReleaseTexture> owner{&texture, deleter};
        expect(texture.flags == 2, "adopting the creator's ref does not add a redundant reference");
    }
    expect(texture.flags == 1 && texture.destroys == 0, "a queued engine reference keeps the UI resource alive");
    deleter(&texture);
    expect(texture.flags == 0x40000000 && texture.destroys == 1,
        "the last owned reference enters the engine's deferred deletion exactly once");
    deleter(nullptr);
    texture.flags = 16; texture.destroys = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) { threads.emplace_back([&] { deleter(&texture); }); }
    for (auto& thread : threads) { thread.join(); }
    expect(texture.flags == 0x40000000 && texture.destroys == 1,
        "concurrent engine/backend retirement cannot duplicate deferred deletion");
}

void test_dedicated_ui() {
    namespace ui = uevr::stupid_never_dies_ui;
    expect(ui::target(L"D:\\Game\\Binaries\\Win64\\StupidNeverDies.exe", true) &&
        ui::target(L"STUPIDNEVERDIES.EXE", true), "UI adapter selects only the exact case-insensitive title");
    expect(!ui::target(L"StupidNeverDies.exe", false) && !ui::target(L"OtherStupidNeverDies.exe", true) &&
        !ui::target(L"StupidNeverDies.exe.backup", true), "other games and DX11 retain their existing UI paths");
    constexpr uintptr_t base = 0x100000, name = 0x180000;
    constexpr std::u16string_view text = u"SlateOutputTexture";
    const auto read_name = [&](uintptr_t address, void* out, size_t size) {
        if (address != name || size != (text.size() + 1) * 2) { return false; }
        std::memcpy(out, text.data(), size); return true;
    };
    const auto fixture = [&](uintptr_t address) {
        std::vector<uint8_t> code{
            0x48,0x8b,0xd3, 0x48,0x89,0x5d,0x18, 0xe8,0,0,0,0,
            0x48,0x89,0x44,0x24,0x58, 0x48,0x85,0xc0, 0x75,0x40,
            0x4c,0x8d,0x05,0,0,0,0, 0x48,0x8b,0xd3,
            0x48,0x8d,0x8d,0xd8,0x01,0,0, 0xe8,0,0,0,0,
            0x48,0x8b,0xd0, 0x45,0x33,0xc0, 0x49,0x8b,0xcd, 0xe8,0,0,0,0};
        const auto relative = [&](size_t next, uintptr_t target) {
            const auto displacement = static_cast<int32_t>(target - (address + next));
            std::memcpy(code.data() + next - 4, &displacement, 4);
        };
        relative(12, base + 0x2000); relative(29, name);
        relative(44, base + 0x3000); relative(58, base + 0x4000);
        code.resize(90, 0x90); code.push_back(0xc3);
        return code;
    };
    const auto code = fixture(base);
    const auto found = ui::find(code, base, read_name);
    expect(found && found->callsite == base + 39 && found->lookup == base + 7 &&
        found->name == name && found->create == base + 0x3000 && found->registration == base + 0x4000 &&
        found->lookup_function == base + 0x2000 && found->output == uevr::buffered_rt::StackCell{NDR_RBP, 0x1d8},
        "UI discovery proves lookup, pooled creation and registration without guessing the next call ABI");
    for (auto offset : {0,3,7,12,17,20,22,29,32,39,44,47,50,53}) {
        auto bad = code; bad[offset] = 0x90;
        expect(!ui::find(bad, base, read_name), "every stage of the pooled registration/fast-path contract is required");
    }
    for (size_t size = 0; size < 58; ++size) {
        expect(!ui::find(std::span{code}.first(size), base, read_name), "truncated UI discovery cannot authorize a redirect");
    }
    expect(!ui::find(code, base, [](uintptr_t, void*, size_t) { return false; }), "unreadable anchor rejects");
    auto twice = code; twice.pop_back();
    const auto second = fixture(base + twice.size());
    twice.insert(twice.end(), second.begin(), second.end());
    expect(!ui::find(twice, base, read_name), "ambiguous UI registration transactions reject");
    if (found) {
        expect(ui::output_matches(*found, 0x303d8, 0x30000, 0x30200), "pooled hidden return is a proven local stack cell, not a graph builder");
        expect(!ui::output_matches(*found, 0x303d0, 0x30000, 0x30200) &&
            !ui::output_matches(*found, 0, UINTPTR_MAX - 16, 0x30200), "wrong hidden return and stack overflow reject");
    }
    ui::Inputs in{}; in.renderer = 0x10000; in.elements = 0x40000; in.window = 0x20000;
    in.viewport_info = 0x30000; in.max_x = 1920; in.max_y = 1080; in.scale = 1;
    const auto address = reinterpret_cast<uintptr_t>(&in);
    const auto read = [&](uintptr_t at, void* out, size_t bytes) {
        if (at == address && bytes == sizeof(in)) { std::memcpy(out, &in, bytes); return true; }
        if (at == in.elements && bytes == 8) { const uintptr_t head = 1; std::memcpy(out, &head, 8); return true; }
        return false;
    };
    const auto object = [](uintptr_t value) { return value == 0x10000 || value == 0x20000 || value == 0x30000; };
    expect(ui::inputs(in.renderer, address, read, object).has_value(), "renderer/graph/inputs packet validates independently of legacy viewport probing");
    in.scale = -1;
    expect(ui::inputs(in.renderer, address, read, object).has_value() && in.scale == -1,
        "SWindow's normal application-UI-scale sentinel is preserved, not rejected or rewritten");
    in.scale = 0;
    expect(ui::inputs(in.renderer, address, read, object).has_value(), "an unused zero scale is not divided by or rewritten");
    expect(!ui::inputs(0x50000, address, read, object), "wrong renderer cannot activate this UI route");
    in.scale = std::numeric_limits<float>::quiet_NaN();
    expect(!ui::inputs(in.renderer, address, read, object), "invalid UI scale rejects without changing input coordinates");
    in.scale = 1; in.min_x = INT32_MIN; in.max_x = INT32_MAX;
    expect(!ui::inputs(in.renderer, address, read, object), "rectangle validation widens before subtraction");
    in.min_x = 0; in.max_x = 1920; in.request_count = 1;
    expect(!ui::inputs(in.renderer, address, read, object), "nonempty post-process arrays require an owner");
}

void test_ui_resource_format() {
    namespace ui = uevr::stupid_never_dies_ui;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1920; desc.Height = 1080;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    expect(ui::color_target(desc) && ui::ui_target(desc, 1920, 1080),
        "observed 1920x1080 BGRA typeless UI must reach its existing typed views and copy consumer");
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        auto value = desc; value.Format = format;
        expect(ui::ui_target(value, 1920, 1080), "compatible typed BGRA resource formats remain accepted");
    }
    for (const auto format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_TYPELESS}) {
        auto value = desc; value.Format = format;
        expect(!ui::ui_target(value, 1920, 1080), "unconverted RGBA, HDR and depth families cannot use the BGRA UI consumer");
    }
    expect(!ui::ui_target(desc, 0, 1080) && !ui::ui_target(desc, 1920, 0) &&
        !ui::ui_target(desc, 4944, 2416), "UI format compatibility never bypasses extent identity");
    auto value = desc; value.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    expect(!ui::ui_target(value, 1920, 1080), "UI must still support the spectator shader view");
    value = desc; value.Flags = D3D12_RESOURCE_FLAG_NONE;
    expect(!ui::ui_target(value, 1920, 1080), "non-render-target resources reject");
    value = desc; value.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    expect(!ui::ui_target(value, 1920, 1080), "depth-stencil resources reject");
    value = desc; value.DepthOrArraySize = 2;
    expect(!ui::ui_target(value, 1920, 1080), "texture arrays cannot be silently treated as a UI plane");
    value = desc; value.MipLevels = 2;
    expect(!ui::ui_target(value, 1920, 1080), "mip chains cannot use a whole-resource UI copy");
    value = desc; value.SampleDesc.Count = 2;
    expect(!ui::ui_target(value, 1920, 1080), "MSAA requires a resolve, not this UI copy path");
    value = desc; value.SampleDesc.Quality = 1;
    expect(!ui::ui_target(value, 1920, 1080), "nonstandard sample quality rejects");
    value = desc; value.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    expect(!ui::ui_target(value, 1920, 1080), "only 2D color render targets are supported");
}
}

int test_buffered_rt_texture_call() {
    failures = 0;
    test_discovery(); test_descriptor_and_binding(); test_memory(); test_retirement(); test_dedicated_ui(); test_ui_resource_format();
    return failures;
}
