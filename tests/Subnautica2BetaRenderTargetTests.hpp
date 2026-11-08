#pragma once

#include <array>
#include <vector>
#include "mods/vr/Subnautica2BetaRenderTarget.hpp"

template<class Expect> void test_subnautica2_beta_render_target(const Expect& expect) {
    namespace b = uevr::subnautica2_beta;
    expect(b::target(L"D:\\Games\\Subnautica2\\Subnautica2-Win64-Test.exe", 0x50007) &&
        b::target(L"subnautica2-WIN64-TEST.EXE", 0x50007), "Subnautica beta is an exact, case-insensitive UE5.7 Test target");
    for (auto name : {L"Subnautica2-Win64-Shipping.exe", L"Subnautica2-WinGDK-Shipping.exe", L"Subnautica2.exe",
                      L"OtherGame-Win64-Test.exe", L"Subnautica2-Win64-Test.exe.bak", L"Subnautica2-Win64-TestEditor.exe"}) {
        expect(!b::target(name, 0x50007), "Shipping, launchers and unrelated binaries stay unchanged");
    }
    expect(!b::target(L"Subnautica2-Win64-Test.exe", 0x50006) && !b::target(L"Subnautica2-Win64-Test.exe", 0x50008),
        "another engine family must establish its own contract");
    constexpr std::array<uint8_t, 9> captured{0x48,0x8d,0x54,0x24,0x70,0x48,0x8d,0x4d,0xc0};
    const auto found = b::finalize_output(captured);
    expect(found && found->output == b::StackCell{NDR_RSP, 0x70} &&
        found->initializer == b::StackCell{NDR_RBP, -0x40}, "captured Finalize output is not the initializer or RAX");
    expect(found && found->address(0x100000, 0x100200) == 0x100070, "the decoded output follows the current stack, not fixed EXE addresses");
    expect(found && found->address(0x900000, 0x900200) == 0x900070, "relocation and a different stack preserve the same ownership rule");
    for (size_t n = 0; n < captured.size(); ++n) {
        expect(!b::finalize_output(std::span{captured}.first(n)), "partial argument setup rejects");
    }
    auto reversed = std::vector<uint8_t>(captured.begin() + 5, captured.end());
    reversed.insert(reversed.end(), captured.begin(), captured.begin() + 5);
    expect(b::finalize_output(reversed).has_value(), "both compiler LEA orders resolve");
    constexpr std::array<uint8_t, 12> other_stack{0x48,0x8d,0x55,0x68,0x48,0x8d,0x8c,0x24,0x90,0,0,0};
    const auto changed_stack = b::finalize_output(other_stack);
    expect(changed_stack && changed_stack->address(0x100000,0x100200) == 0x100268,
        "a different compiler displacement and RBP output resolve without patching addresses");
    auto padded = std::vector<uint8_t>(captured.begin(),captured.end()); padded.insert(padded.begin()+5,{0x66,0x90});
    expect(b::finalize_output(padded).has_value(), "alignment NOPs do not change the ownership proof");
    for (auto extra : {std::vector<uint8_t>{0x31,0xd2}, std::vector<uint8_t>{0x48,0x83,0xec,0x20},
                      std::vector<uint8_t>{0x48,0x83,0xc5,0x08}, std::vector<uint8_t>{0x50},
                      std::vector<uint8_t>{0xe8,0,0,0,0}, std::vector<uint8_t>{0x75,0},
                      std::vector<uint8_t>{0xc3}, std::vector<uint8_t>{0x48,0x89,0x02}}) {
        auto code = std::vector<uint8_t>(captured.begin(), captured.end()); code.insert(code.end(), extra.begin(), extra.end());
        expect(!b::finalize_output(code), "clobbers, calls, branches, stack changes and writes cannot authorize a post-call reference");
    }
    auto duplicate = std::vector<uint8_t>(captured.begin(), captured.end()); duplicate.insert(duplicate.end(), captured.begin(), captured.end());
    expect(!b::finalize_output(duplicate), "ambiguous repeated argument definitions reject");
    auto different_register = captured; different_register[2] = 0x44;
    expect(!b::finalize_output(different_register), "RAX is not a substitute for the hidden output argument");
    auto narrow_address = std::vector<uint8_t>(captured.begin(), captured.end()); narrow_address.insert(narrow_address.begin(), 0x67);
    expect(!b::finalize_output(narrow_address), "32-bit stack addressing cannot be resolved as the 64-bit stack");
    auto oversized = std::vector<uint8_t>(captured.begin(), captured.end()); oversized.resize(0x31, 0x90);
    expect(!b::finalize_output(oversized), "oversized setup cannot expand the bounded decoder window");
    expect(found && !found->address(0, 0x100200) && !found->address(UINTPTR_MAX - 16, 0x100200) &&
        !found->address(0x100001, 0x100200), "invalid, wrapped or unaligned stack locations reject");
    b::FinalizeOutput overlap{{NDR_RSP, 0x40}, {NDR_RSP, 0x70}};
    expect(!overlap.address(0x100000, 0x100200), "an initializer member is never the returned texture reference");
    expect(b::scene_extent(4944,2416,2472,2416) && b::scene_extent(2472,2416,2472,2416), "native and single-eye viewport extents validate");
    expect(!b::scene_extent(1920,1080,2472,2416) && !b::scene_extent(4945,2416,2472,2416) &&
        !b::scene_extent(4944,2415,2472,2416) && !b::scene_extent(0,0,0,0), "desktop/UI, scaled and empty candidates reject");
    std::array<uint8_t, 0x78> rhi{};
    rhi[0x20] = 1; rhi[0x50] = rhi[0x51] = 1;
    const uint32_t width = 4944, height = 2416;
    std::memcpy(rhi.data() + 0x44, &width, sizeof(width)); std::memcpy(rhi.data() + 0x48, &height, sizeof(height));
    expect(b::rhi_shape(rhi,2472,2416), "PDB-matched main RHI shape validates before a virtual call");
    for (size_t n = 0; n < 0x54; ++n) { expect(!b::rhi_shape(std::span{rhi}.first(n),2472,2416), "truncated RHI descriptor rejects"); }
    for (size_t offset : {size_t{0x20},size_t{0x44},size_t{0x48},size_t{0x50},size_t{0x51}}) {
        auto changed = rhi; changed[offset] ^= 1;
        expect(!b::rhi_shape(changed,2472,2416), "changed flags, extent, mips or samples cannot authorize a virtual call");
    }
    rhi[0x20] = 3; expect(b::rhi_shape(rhi,2472,2416), "color resolve-target support is not mistaken for depth");
    rhi[0x20] = 5; expect(!b::rhi_shape(rhi,2472,2416), "a depth-stencil allocation is not scene color");
    rhi[0x20] = 1; rhi[0x23] = 8;
    expect(!b::rhi_shape(rhi,2472,2416), "a depth-stencil resolve allocation is not scene color");
    for (const uint32_t w : {640u,960u,1920u,2472u,4096u,8192u}) {
        for (const uint32_t h : {720u,1080u,2160u,2416u,4096u}) {
            expect(b::scene_extent(uint64_t(w)*2,h,w,h) && !b::scene_extent(uint64_t(w)*2+1,h,w,h),
                "framing validation follows current eye resolution, not a 1920x1080 assumption");
        }
    }
}
