#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include <sdk/BoundedDiscovery.hpp>

namespace uevr::gears_aim {
struct DebugCanvasQuery {
    size_t slot{};
    uintptr_t return_address{};
};

// Draw's checked XRSystem -> GetHMDDevice -> FIntPoint sret sequence. Only
// relocations, selected slots, engine member and stack displacements vary.
inline constexpr std::array<uint8_t, 88> debug_canvas_code{
    0x48,0x8b,0x0d,0,0,0,0, 0x48,0x8b,0x89,0,0,0,0,
    0x48,0x85,0xc9, 0x74,0, 0x48,0x8b,0x01, 0xff,0x90,0,0,0,0,
    0x48,0x85,0xc0, 0x74,0, 0x48,0x8b,0x05,0,0,0,0,
    0x48,0x8b,0x88,0,0,0,0, 0x48,0x8b,0x01, 0xff,0x90,0,0,0,0,
    0x48,0x8d,0x94,0x24,0,0,0,0, 0x48,0x8b,0xc8, 0x4c,0x8b,0x00,
    0x41,0xff,0x90,0,0,0,0, 0x48,0x8b,0x08, 0x48,0x89,0x8c,0x24,0,0,0,0
};

inline constexpr auto debug_canvas_mask = [] {
    std::array<uint8_t, debug_canvas_code.size()> mask{};
    mask.fill(0xff);
    for (const auto offset : {3,10,24,36,43,52,60,73,84}) {
        std::fill_n(mask.begin() + offset, 4, 0);
    }
    mask[18] = mask[32] = 0;
    return mask;
}();

inline std::optional<DebugCanvasQuery> debug_canvas_query(const sdk::discovery::Memory& memory,
    uintptr_t draw, size_t draw_size, uintptr_t engine, size_t xr_offset, size_t get_hmd_slot)
{
    constexpr size_t body_start = 0x25; // Do not interpret an installed Draw detour.
    constexpr size_t limit = 0x1000;
    const auto size = (std::min)(draw_size, limit);
    if (draw < 0x10000 || engine < 0x10000 || (engine & 7) != 0 || xr_offset == 0 ||
        xr_offset > 0x10000 || (xr_offset & 7) != 0 || get_hmd_slot >= 200 ||
        size < body_start + debug_canvas_code.size() ||
        draw > (std::numeric_limits<uintptr_t>::max)() - size || !memory.read || !memory.executable ||
        !memory.executable(memory.context, draw, size)) { return {}; }

    std::array<uint8_t, limit> bytes{};
    if (!memory.read(memory.context, draw, bytes.data(), size)) { return {}; }
    std::optional<DebugCanvasQuery> result;
    for (size_t offset = body_start; offset + debug_canvas_code.size() <= size; ++offset) {
        const auto* code = bytes.data() + offset;
        bool matches = true;
        for (size_t i = 0; i < debug_canvas_code.size(); ++i) {
            if ((code[i] & debug_canvas_mask[i]) != debug_canvas_code[i]) { matches = false; break; }
        }
        if (!matches) { continue; }
        const auto u32 = [&](size_t at) {
            uint32_t value{};
            std::memcpy(&value, code + at, sizeof(value));
            return value;
        };
        const auto i32 = [&](size_t at) {
            int32_t value{};
            std::memcpy(&value, code + at, sizeof(value));
            return value;
        };
        const auto at = draw + offset;
        const auto first_global = sdk::discovery::relative_address(at + 7, i32(3));
        const auto second_global = sdk::discovery::relative_address(at + 40, i32(36));
        const auto first_skip = sdk::discovery::relative_address(at + 19, int8_t(code[18]));
        const auto second_skip = sdk::discovery::relative_address(at + 33, int8_t(code[32]));
        const auto stack_output = u32(60);
        const auto stack_copy = u32(84);
        const auto stack_slot = [](uint32_t value) { return value >= 0x20 && value <= 0xff8 && (value & 7) == 0; };
        uintptr_t observed_engine{};
        if (!first_global || first_global != second_global || (*first_global & 7) != 0 ||
            !memory.load(*first_global, observed_engine) || observed_engine != engine ||
            first_skip != at + debug_canvas_code.size() || second_skip != first_skip ||
            u32(10) != xr_offset || u32(43) != xr_offset ||
            u32(24) != get_hmd_slot * 8 || u32(52) != get_hmd_slot * 8 ||
            !stack_slot(stack_output) || !stack_slot(stack_copy) || stack_output == stack_copy ||
            (u32(73) != 27 * 8 && u32(73) != 28 * 8)) { continue; }
        if (result) { return {}; }
        result = DebugCanvasQuery{u32(73) / 8, at + 77};
    }
    return result;
}

inline int32_t* write_debug_canvas_size(int32_t* out) {
    if (!out) { return nullptr; }
    out[0] = 1024;
    out[1] = 1024;
    return out;
}
}
