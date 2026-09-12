#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uevr::borderlands4_slate {

struct ArrayView {
    uintptr_t data{};
    int32_t count{};
    uint32_t padding{};
};

struct Point {
    int32_t x{};
    int32_t y{};
};

struct Rect {
    Point min{};
    Point max{};
};

// UE5.5.4 FSlateDrawWindowPassInputs prefix, not the full array element stride.
struct Inputs {
    uintptr_t renderer{};
    uintptr_t window_element_list{};
    uintptr_t window{};
    uintptr_t viewport_info{};
    ArrayView post_process_requests{};
    Point cursor_position{};
    Rect scene_view_rect{};
    float viewport_scale_ui{};
};

static_assert(sizeof(ArrayView) == 0x10 && sizeof(Inputs) == 0x50);
static_assert(offsetof(Inputs, scene_view_rect) == 0x38);
static_assert(offsetof(Inputs, viewport_scale_ui) == 0x48);

enum class Abi { RendererFirst, OutputsFirst, SingleWindowArray };

struct Match {
    Abi abi{};
    uintptr_t outputs{};
    Inputs inputs{};
    uint32_t width{};
    uint32_t height{};
};

// Read and IsObject must be bounded, fault-contained observations, never calls
// into candidate objects. Nothing is cached, so an incomplete first draw retries.
template <typename Read, typename IsObject>
std::optional<Match> probe(uintptr_t rcx, uintptr_t rdx, uintptr_t r8, uintptr_t r9,
    Read&& read, IsObject&& is_object) {
    std::optional<Match> result{};
    size_t matches{};

    const auto consider = [&](uintptr_t address, uintptr_t renderer, uintptr_t outputs, Abi abi) {
        Inputs inputs{};
        if (address == 0 || renderer == 0 || !read(address, &inputs, sizeof(inputs)) ||
            inputs.renderer != renderer || inputs.window_element_list == 0 ||
            inputs.window == 0 || inputs.viewport_info == 0 ||
            !std::isfinite(inputs.viewport_scale_ui)) {
            return;
        }

        // Widen before subtracting: malformed rectangles must not overflow.
        const auto width = int64_t{inputs.scene_view_rect.max.x} - inputs.scene_view_rect.min.x;
        const auto height = int64_t{inputs.scene_view_rect.max.y} - inputs.scene_view_rect.min.y;
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
            return;
        }

        if (abi != Abi::SingleWindowArray) {
            std::array<uintptr_t, 3> output_storage{};
            if (outputs == 0 || outputs == renderer || outputs == address ||
                !read(outputs, output_storage.data(), sizeof(output_storage))) {
                return;
            }
        }

        uintptr_t element_list_head{};
        if (!read(inputs.window_element_list, &element_list_head, sizeof(element_list_head)) ||
            !is_object(renderer) || !is_object(inputs.window) || !is_object(inputs.viewport_info)) {
            return;
        }

        ++matches;
        result = Match{abi, outputs, inputs, static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    };

    consider(r9, rcx, rdx, Abi::RendererFirst);
    consider(r9, rdx, rcx, Abi::OutputsFirst);

    ArrayView windows{};
    if (r8 != 0 && read(r8, &windows, sizeof(windows)) && windows.data != 0 && windows.count == 1) {
        // Do not guess the full stride or choose a window from a multi-window draw.
        consider(windows.data, rcx, 0, Abi::SingleWindowArray);
    }

    return matches == 1 ? result : std::nullopt;
}

} // namespace uevr::borderlands4_slate
