#pragma once

#include "ProSpiNativeCameraSource.hpp"

namespace uevr::prospi::trace::native {

struct CodeFingerprint { uint32_t rva; std::string_view bytes; };
struct VerifiedBuild {
    uint32_t timestamp;
    Layout layout;
    std::array<CodeFingerprint, 11> code;
    uint32_t counter_instruction;
};

// The native publisher and UE bridge retain the same fields in these two builds.
// Code, globals and vtables move independently; never apply a single RVA delta.
inline constexpr std::array<VerifiedBuild, 2> verified_builds{{
    {1786532279, {0, 783106048, 0x12f98f20, 0x12f994e8, 0x12fa8740,
        0x083be008, 0x07a4e7c0, 0x05818b20, 0x05817000}, {{
        {0x05819570, "488bc44889580848897010574881ecc00000000f2970e80f2978d8440f2940c8"},
        {0x058195e0, "4885db480f44d84038b0140400007405488bd8eb08488bc8e8c3010000488bcf"},
        {0x05819616, "440f285370440f295424300f2883800000000f29442420f3410f5cc20f28d045"},
        {0x05811410, "dfe85a810000e8f5890100b801000000f00fc1051873790d8b0d1273790d908b"},
        {0x058170e0, "40534883ec20488bd9488d0d6aa2d514e84bedb101488b430833c9488b400848"},
        {0x05818b20, "40534883ec20488bd9488d0ddd88d514e80bd3b101488bc34883c4205bc3cccc"},
        {0x05817000, "4883ec28488d0d4fa3d514e830eeb10133c04883c428c3cccccccccccccccccc"},
        {0x05811e80, "4883ec28488d0dd2f3d514e8b03fb201488b058970780d488b40104883c428c3"},
        {0x030ddb50, "4883ec48f20f101a8b42080f28e3f30f100d0e6578040f28c389442438f30f10"},
        {0x030dd3b0, "40534883ec50f20f1012488bd98b4208488d4c24300f28ca89442448f30f1044"},
        {0x030dd850, "48895c2408574883ec30488b5910488bf90f297424204885db7411488b832802"},
    }}, 0x05811420},
    {1790730967, {0, 771923968, 0x132bf3e0, 0x132bf9a8, 0x132cec10,
        0x0838d708, 0x07a28668, 0x057e5400, 0x057e38e0}, {{
        {0x057e5e50, "488bc44889580848897010574881ecc00000000f2970e80f2978d8440f2940c8"},
        {0x057e5ec0, "4885db480f44d84038b0140400007405488bd8eb08488bc8e8c3010000488bcf"},
        {0x057e5ef6, "440f285370440f295424300f2883800000000f29442420f3410f5cc20f28d045"},
        {0x057ddcfb, "b801000000f00fc105080faf0d8b0d020faf0d908b0dfb0eaf0d9048ffc983e1"},
        {0x057e39c0, "40534883ec20488bd9488d0d8ae90a15e87bccb201488b430833c9488b400848"},
        {0x057e5400, "40534883ec20488bd9488d0dfdcf0a15e83bb2b201488bc34883c4205bc3cccc"},
        {0x057e38e0, "4883ec28488d0d6fea0a15e860cdb20133c04883c428c3cccccccccccccccccc"},
        {0x057de760, "4883ec28488d0df23a0b15e8e01eb301488b05690cae0d488b40104883c428c3"},
        {0x030f1190, "4883ec48f20f101a8b42080f28e3f30f100dced174040f28c389442438f30f10"},
        {0x030f09f0, "40534883ec50f20f1012488bd98b4208488d4c24300f28ca89442448f30f1044"},
        {0x030f0e90, "48895c2408574883ec30488b5910488bf90f297424204885db7411488b832802"},
    }}, 0x057ddd00},
}};

inline const VerifiedBuild* verified_build(uint32_t timestamp, size_t image_size) noexcept {
    for (const auto& build : verified_builds) {
        if (build.timestamp == timestamp && build.layout.image_size == image_size) { return &build; }
    }
    return nullptr;
}

template <typename Read>
bool resolve_layout(Read read, uintptr_t base, uint32_t timestamp, size_t image_size, Layout& out) noexcept {
    out = {};
    const auto build = verified_build(timestamp, image_size);
    if (!build || !pointer(base) || image_size > 0x0000800000000000ull - base) { return false; }
    auto candidate = build->layout;
    candidate.base = base;
    const auto bytes = [&](uint32_t rva, void* destination, size_t count) {
        return module(candidate, base + rva, count) && read(base + rva, destination, count);
    };
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (const auto& f : build->code) {
        std::array<uint8_t, 32> actual{};
        if (f.bytes.size() != actual.size() * 2 || !bytes(f.rva, actual.data(), actual.size())) { return false; }
        for (size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != ((digit(f.bytes[i * 2]) << 4) | digit(f.bytes[i * 2 + 1]))) { return false; }
        }
    }
    const auto rip_matches = [&](uint32_t rva, std::string_view opcode, uint32_t target, size_t target_size) {
        std::array<uint8_t, 9> instruction{};
        const auto length = opcode.size() + sizeof(int32_t);
        if (length > instruction.size() || !bytes(rva, instruction.data(), length) ||
            std::memcmp(instruction.data(), opcode.data(), opcode.size()) != 0) { return false; }
        int32_t displacement{};
        std::memcpy(&displacement, instruction.data() + opcode.size(), sizeof(displacement));
        const auto resolved = (int64_t)rva + (int64_t)length + displacement;
        return resolved == target && module(candidate, base + target, target_size);
    };
    // Root each data address in its actual producer, including the DWORD frame counter.
    if (!rip_matches(build->code[7].rva + 0x10, "\x48\x8b\x05", candidate.context_rva, 8) ||
        !rip_matches(build->code[4].rva + 0x1f, "\x48\x3b\x05", candidate.sentinel_rva, 8) ||
        !rip_matches(build->counter_instruction, "\xf0\x0f\xc1\x05", candidate.frame_rva, 4)) { return false; }
    const auto slot_matches = [&](uint32_t vtable, uint32_t slot, uint32_t function) {
        uintptr_t actual{};
        return module(candidate, base + function, 1) && bytes(vtable + slot, &actual, sizeof(actual)) &&
            actual == base + function;
    };
    if (!slot_matches(candidate.sentinel_vtable_rva, 0x38, candidate.empty_getter_rva) ||
        !slot_matches(candidate.bridge_vtable_rva, 8, build->code[8].rva) ||
        !slot_matches(candidate.bridge_vtable_rva, 0x10, build->code[9].rva) ||
        !slot_matches(candidate.bridge_vtable_rva, 0x18, build->code[10].rva)) { return false; }
    out = candidate;
    return true;
}

} // namespace uevr::prospi::trace::native
