#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include <sdk/EngineVersion.hpp>
#include <sdk/FSceneViewLayoutPolicy.hpp>
#include <sdk/UE6LayoutValidation.hpp>
#include <sdk/UE6TextureLayout.hpp>
#include <sdk/ObjectLivenessPolicy.hpp>
#include "mods/vr/UECompatibility.hpp"
#include "mods/vr/UE60Slate.hpp"

namespace {
int failures{};
void expect(bool value, const char* description) {
    if (!value) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
}
struct Code {
    std::vector<uint8_t> bytes;
    sdk::discovery::Memory memory() {
        return {this, [](void* context, uintptr_t address, void* output, size_t size) {
            const auto& data = static_cast<Code*>(context)->bytes;
            if (address < 0x1000 || address - 0x1000 > data.size() || size > data.size() - (address - 0x1000)) { return false; }
            std::memcpy(output, data.data() + address - 0x1000, size);
            return true;
        }, [](void* context, uintptr_t address, size_t size) {
            const auto& data = static_cast<Code*>(context)->bytes;
            return address >= 0x1000 && address - 0x1000 <= data.size() && size <= data.size() - (address - 0x1000);
        }};
    }
};
void test_helpers() {
    Code size{{0x48,0x89,0xd0, 0x4c,0x8b,0x41,0x10, 0x4c,0x89,0x02, 0xc3}};
    expect(sdk::ue6::viewport_helper(size.memory(), 0x1000, true), "size getter copies eight bytes and returns sret");
    expect(!sdk::ue6::viewport_helper(size.memory(), 0x1000, false), "size cannot become canvas getter");
    Code canvas{{0x48,0x8b,0x81,0x80,0x02,0,0, 0xc3}};
    expect(sdk::ue6::viewport_helper(canvas.memory(), 0x1000, false), "read-only canvas accessor");
    expect(!sdk::ue6::viewport_helper(canvas.memory(), 0x1000, true), "canvas cannot become size getter");
    for (Code bad : {
        Code{{0x48,0x89,0xd0,0x48,0x89,0xca,0x4c,0x8b,0x41,0x10,0x4c,0x89,0x02,0xc3}},
        Code{{0x48,0x89,0xd0,0x4c,0x8b,0x41,0x10,0x4c,0x89,0x42,0x08,0xc3}},
        Code{{0x48,0x89,0xd0,0x4c,0x8b,0x41,0x10,0x44,0x89,0x02,0xc3}},
        Code{{0x48,0x89,0xd0,0x4c,0x8b,0x41,0x10,0x4c,0x89,0x02,0x33,0xc0,0xc3}},
        Code{{0xe8,0,0,0,0,0xc3}}, Code{{0xeb,0xfe}}}) {
        expect(!sdk::ue6::viewport_helper(bad.memory(), 0x1000, true), "reject redirected, partial, wrong-return, calling or looping helper");
    }
    Code write{{0x48,0x8b,0x41,0x10,0x48,0x89,0x01,0xc3}};
    expect(!sdk::ue6::viewport_helper(write.memory(), 0x1000, false), "canvas cannot mutate this");
    Code partial_zero{{0x30,0xc0,0xc3}};
    Code partial_pointer{{0x4c,0x8b,0x41,0x10,0x66,0x44,0x89,0xc0,0xc3}};
    expect(!sdk::ue6::viewport_helper(partial_zero.memory(), 0x1000, false) &&
        !sdk::ue6::viewport_helper(partial_pointer.memory(), 0x1000, false), "partial registers cannot prove a returned pointer");
    const std::array<uint8_t,5> accessor{0x48,0x8d,0x41,0x30,0xc3};
    expect(sdk::ue6::texture_desc_offset(accessor) == 0x30, "descriptor LEA proof");
    const std::array<std::array<uint8_t,5>,5> invalid{{
        {0x48,0x8d,0x41,0xf0,0xc3}, {0x48,0x8d,0x41,0x08,0xc3},
        {0x48,0x8d,0x42,0x30,0xc3}, {0x48,0x8d,0x41,0x30,0xcc}, {0x48,0x8b,0x41,0x30,0xc3}}};
    for (const auto& bad : invalid) { expect(!sdk::ue6::texture_desc_offset(bad), "invalid descriptor accessor rejected"); }
    uintptr_t function{};
    expect(!sdk::ue6::vtable_function(size.memory(), UINTPTR_MAX - 1, 4, function), "vtable addition cannot wrap");
}
void test_profiles() {
    expect(sdk::ue6::viewport_debug_canvas_index == 53 && sdk::ue6::viewport_size_xy_index == 5,
        "Draw discovery and optional viewport calls share the reviewed indices");
    const auto liveness = sdk::object_liveness::classify(6,0,0);
    expect(sdk::object_liveness::rejected(liveness, 1u << 21) && sdk::object_liveness::rejected(liveness, 1u << 28) &&
        !sdk::object_liveness::rejected(liveness, 1u << 29), "UE6 garbage/unreachable masks must not reject RefCounted");
    for (const auto text : {L"6.0.0-0+++UE6+Main", L"++UE6+Main-6.0-CL-0", L"++ue6+Main-6.0.3-CL-0"}) {
        const auto version = sdk::parse_engine_version(text);
        expect(version && version->is_validated_ue6(), "reviewed embedded version formats");
    }
    for (const auto text : {L"++UE7+Main-7.0-CL-0", L"++UEfoo+Main-6.0", L"++UE6+Main-5.8", L"6.0.x", L"6.0.0.1", L"random"}) {
        expect(!sdk::parse_engine_version(text), "malformed or mismatched version rejected");
    }
    for (uint16_t major : {4,5}) {
        expect(uevr::compat::make_profile({major,8,0,sdk::EngineVersionSource::Embedded}).source_validated, "historical profile preserved");
    }
    const auto known = uevr::compat::make_profile({6,0,0,sdk::EngineVersionSource::Embedded});
    expect(known.command_list_root_offset == 0x28 && known.slate_input_layout == uevr::compat::SlateInputLayout::UE60 &&
        known.scene_view_layout == uevr::compat::SceneViewLayout::UE60, "reviewed capabilities");
    const auto future = uevr::compat::make_profile({6,1,0,sdk::EngineVersionSource::Embedded});
    expect(!future.source_validated && !future.supports_live_openxr_resize && !future.supports_modern_slate_scan, "future minor fails closed");
    expect(uevr::compat::has_reviewed_slate_ui_source(known, false), "UE6 Slate is not blocked by the UE5.8 metadata gate");
    expect(!uevr::compat::has_reviewed_slate_ui_source(future, true), "future UE6 Slate cannot inherit a UE5.8 metadata match");
    const auto ue58 = uevr::compat::make_profile({5,8,0,sdk::EngineVersionSource::Embedded});
    expect(uevr::compat::has_reviewed_slate_ui_source(ue58, true) &&
        !uevr::compat::has_reviewed_slate_ui_source(ue58, false), "UE5.8 Slate preserves its existing metadata gate");
    auto incomplete = known;
    incomplete.source_validated = false;
    expect(!uevr::compat::has_reviewed_slate_ui_source(incomplete, true), "unreviewed UE6 Slate profile fails closed");
    incomplete = known; incomplete.slate_input_layout = uevr::compat::SlateInputLayout::UE58;
    expect(!uevr::compat::has_reviewed_slate_ui_source(incomplete, true), "UE6 cannot normalize a legacy Slate prefix");
}
void test_slate() {
    uevr::ue60::SlateInputPrefix input{};
    input.renderer = input.window_element_list = input.window = input.viewport_info = reinterpret_cast<void*>(0x10000);
    input.hdr.maximum_nits = input.hdr.full_frame_nits = 100;
    input.hdr.paper_white_nits = 203;
    input.scene_rect.max = {2560,1440}; input.ui_scale = 1;
    expect(uevr::ue60::valid_sdr_input(input, input.renderer), "SDR accepts stock paper-white metadata");
    input.hdr.hdr_supported = 1;
    expect(!uevr::ue60::valid_sdr_input(input, input.renderer), "HDR preserves original path");
    input.hdr.hdr_supported = 0; input.hdr.output_format = 3;
    expect(!uevr::ue60::valid_sdr_input(input, input.renderer), "HDR output rejected");
    input.hdr.output_format = 0; input.ui_scale = std::numeric_limits<float>::quiet_NaN();
    expect(!uevr::ue60::valid_sdr_input(input, input.renderer), "nonfinite scale rejected");
    input.ui_scale = 1; input.scene_rect.min.x = INT32_MIN; input.scene_rect.max.x = INT32_MAX;
    expect(!uevr::ue60::valid_sdr_input(input, input.renderer), "rect overflow rejected");
    input.scene_rect.min.x = 0; input.scene_rect.max.x = 2560; input.viewport_info = nullptr;
    expect(!uevr::ue60::valid_sdr_input(input, input.renderer), "incomplete input rejected");
}
void test_texture() {
    sdk::ue6::TextureDescPrefix desc{};
    desc.width = 2048; desc.height = 1024; desc.depth = desc.array_size = desc.mips = desc.samples = 1;
    desc.format = 37; desc.fast_vram_percent = 255;
    expect(sdk::ue6::valid_texture_desc(desc), "stock 2D descriptor including encoded fast-VRAM percentage");
    expect(sdk::ue6::matches_native_format(desc.format, DXGI_FORMAT_R8G8B8A8_UNORM), "native color mapping");
    expect(!sdk::ue6::matches_native_format(desc.format, DXGI_FORMAT_B8G8R8A8_UNORM), "different color rejected");
    desc.format = 95;
    expect(!sdk::ue6::valid_texture_desc(desc), "PF_MAX rejected");
    desc.format = 10; desc.dimension = 4;
    expect(!sdk::ue6::valid_texture_desc(desc), "volume/cube rejected");
    desc.dimension = 0; desc.array_size = 2;
    expect(!sdk::ue6::valid_texture_desc(desc), "2D cannot claim array");
    desc.dimension = 1;
    expect(sdk::ue6::valid_texture_desc(desc), "explicit 2D array accepted");
}
}
int main() { test_profiles(); test_helpers(); test_slate(); test_texture(); return failures ? 1 : 0; }
