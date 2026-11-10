#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace uevr::ue59_slate {
struct Rect { int32_t left{}, top{}, right{}, bottom{}; };
struct Screenshot {
    Rect rect;
    uintptr_t viewport{}, colors{}, hdr_colors{};
};
struct DrawInputs {
    // Read-only prefix, never the complete engine object or an array stride.
    uintptr_t renderer{}, elements{}, window{}, viewport{};
    Screenshot screenshot;
    std::array<int32_t,2> cursor{};
    Rect scene;
    float scale{};
};
static_assert(sizeof(Screenshot) == 0x28);
static_assert(offsetof(DrawInputs, screenshot) == 0x20 && offsetof(DrawInputs, cursor) == 0x48 &&
    offsetof(DrawInputs, scene) == 0x50 && offsetof(DrawInputs, scale) == 0x60 && sizeof(DrawInputs) == 0x68);

inline std::optional<DrawInputs> decode(std::span<const uint8_t> bytes, uintptr_t renderer) {
    if (!renderer || bytes.size() < sizeof(DrawInputs)) { return {}; }
    DrawInputs input{};
    std::memcpy(&input, bytes.data(), sizeof(input));
    if (input.renderer != renderer || !input.window || !input.viewport) { return {}; }
    // Screenshot pointers are an engine-owned snapshot, not a source texture,
    // rectangle, or permission to dereference any of its arrays.
    return input;
}
} // namespace uevr::ue59_slate
