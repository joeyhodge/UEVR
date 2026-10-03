#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sdk/BoundedDiscovery.hpp>

namespace uevr::halloween_rt {

template<size_t N>
bool code_matches(const sdk::discovery::Memory& memory, uintptr_t address, const std::array<uint8_t, N>& expected) {
    std::array<uint8_t, N> actual{};
    return memory.executable && memory.executable(memory.context, address, N) &&
        memory.load(address, actual) && actual == expected;
}

// Both examined UE5.7.4 builds pass these two stack refs to the virtual
// allocator, then join the engine fallback only AFTER it assigns both refs.
inline constexpr std::array<uint8_t, 48> allocate_arguments{
    0x48,0x8d,0x44,0x24,0x70,0x48,0x89,0x44,0x24,0x48,
    0x48,0x8d,0x44,0x24,0x78,0x48,0x89,0x44,0x24,0x40,
    0x0f,0xb6,0x87,0x94,0x02,0x00,0x00,0x48,0x89,0x5c,0x24,0x38,
    0x4c,0x89,0x6c,0x24,0x30,0x89,0x5c,0x24,0x28,0x88,0x44,0x24,0x20,0x41,0xff,0xd2};
inline constexpr std::array<uint8_t, 39> publish_rt{
    0x48,0x8b,0x97,0xa8,0x02,0x00,0x00,0x48,0x8b,0x44,0x24,0x78,
    0x4a,0x8b,0x0c,0xf2,0x48,0x3b,0xc8,0x74,0x17,0x4a,0x89,0x04,0xf2,
    0x48,0x85,0xc0,0x74,0x04,0xf0,0xff,0x40,0x08,0x48,0x85,0xc9,0x74,0x05};
inline constexpr std::array<uint8_t, 37> publish_srv{
    0x48,0x8b,0x97,0xb8,0x02,0x00,0x00,0x48,0x8b,0x4c,0x24,0x70,
    0x4a,0x8b,0x04,0xf2,0x48,0x3b,0xc1,0x74,0x1a,0x4a,0x89,0x0c,0xf2,
    0x48,0x85,0xc9,0x74,0x04,0xf0,0xff,0x41,0x08,0x48,0x85,0xc0};

inline std::optional<uintptr_t> allocation_join(const sdk::discovery::Memory& memory, uintptr_t return_address) {
    std::array<uint8_t, 8> branch{};
    int32_t displacement{};
    if (return_address < allocate_arguments.size() ||
        !code_matches(memory, return_address - allocate_arguments.size(), allocate_arguments) ||
        !memory.executable(memory.context, return_address, branch.size()) || !memory.load(return_address, branch) ||
        branch[0] != 0x84 || branch[1] != 0xc0 || branch[2] != 0x0f || branch[3] != 0x85) { return {}; }
    std::memcpy(&displacement, branch.data() + 4, sizeof(displacement));
    if (displacement < 0x100 || displacement > 0x400) { return {}; }
    const auto join = sdk::discovery::relative_address(return_address + branch.size(), displacement);
    if (!join || !code_matches(memory, *join, publish_rt) ||
        !code_matches(memory, *join + 0x2c, publish_srv)) { return {}; }
    return join;
}

// The two publication stores release their previous refs through this same
// engine helper. Its packed refcount/deferred deletion must not be replaced
// with the SDK's decrement-only FRHIResource::release implementation.
inline constexpr std::array<uint8_t, 32> release_entry{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,
    0xbf,0xff,0xff,0xff,0xff,0xf0,0x0f,0xc1,0x79,0x08,0x81,0xe7,0xff,0xff,0xff,0x3f,0x83,0xef,0x01};
inline std::optional<uintptr_t> allocation_release(const sdk::discovery::Memory& memory, uintptr_t join) {
    std::optional<uintptr_t> result;
    for (const auto offset : {0x27u, 0x56u}) {
        std::array<uint8_t, 5> call{};
        int32_t displacement{};
        if (!memory.executable || !memory.executable(memory.context, join + offset, call.size()) ||
            !memory.load(join + offset, call) || call[0] != 0xe8) { return {}; }
        std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
        const auto target = sdk::discovery::relative_address(join + offset + call.size(), displacement);
        if (!target || !code_matches(memory, *target, release_entry) || (result && result != target)) { return {}; }
        result = target;
    }
    return result;
}

// Read only the direct FD3D12Texture -> FD3D12Resource -> ID3D12Resource path.
// Never call the getter's fallback virtual function on a speculative object.
inline constexpr std::array<uint8_t, 29> native_getter{
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0xd0,0x00,0x00,0x00,
    0x33,0xdb,0x48,0x85,0xc0,0x74,0x09,0x48,0x8b,0x58,0x20,0x48,0x85,0xdb,0x75,0x21};
inline bool pointer(uintptr_t value) { return value >= 0x10000 && value < 0x0000800000000000 && (value & 7) == 0; }

inline std::optional<uintptr_t> native_resource(const sdk::discovery::Memory& memory, uintptr_t texture) {
    uintptr_t table{}, wrapper{}, resource{};
    std::array<uintptr_t, 16> entries{};
    if (!pointer(texture) || !memory.load(texture, table) || !pointer(table) || !memory.load(table, entries)) { return {}; }
    size_t matches{};
    for (const auto entry : entries) { matches += code_matches(memory, entry, native_getter) ? 1 : 0; }
    if (matches != 1 || !memory.load(texture + 0xd0, wrapper) || !pointer(wrapper) ||
        !memory.load(wrapper + 0x20, resource) || !pointer(resource)) { return {}; }
    return resource;
}

struct NativeDescription {
    uint64_t width{};
    uint32_t height{}, dimension{}, format{}, flags{}, samples{}, quality{};
    uint16_t array_size{}, mips{};
};
inline bool valid_scene(const NativeDescription& d, uint32_t width, uint32_t height) {
    const bool color = d.format == 23 || d.format == 24 || d.format == 27 || d.format == 28 ||
        d.format == 87 || d.format == 90 || d.format == 9 || d.format == 10;
    return width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
        d.dimension == 3 && d.width == width && d.height == height && color &&
        d.array_size == 1 && d.mips == 1 && d.samples == 1 && d.quality == 0 &&
        (d.flags & 1) != 0 && (d.flags & (2 | 8)) == 0;
}

// UE5.7.4 SlateRHIRenderer.cpp: PostProcessRequests is no longer in Inputs.
// RCX=this, RDX=hidden outputs, R8=FRDGBuilder, R9=Inputs. No legacy viewport
// emulation, and no treating hidden output storage as an RHI command list.
struct SlateInputs {
    uintptr_t renderer{}, elements{}, window{}, viewport_info{};
    int32_t cursor_x{}, cursor_y{}, min_x{}, min_y{}, max_x{}, max_y{};
    float scale{};
    uint32_t used_post_buffers{};
};
static_assert(sizeof(SlateInputs) == 0x40 && offsetof(SlateInputs, min_x) == 0x28 && offsetof(SlateInputs, scale) == 0x38);
inline constexpr std::array<uint8_t, 34> slate_entry_contract{
    0x4d,0x8b,0x69,0x18,0x4c,0x8b,0xe1,0x49,0x8b,0x41,0x08,0x49,0x8b,0xf9,
    0x48,0x89,0x55,0x08,0x4d,0x8b,0xf8,0x48,0x89,0x4d,0xa0,0x33,0xd2,
    0x49,0x8b,0x88,0xc0,0x00,0x00,0x00};
inline constexpr std::array<uint8_t, 17> slate_register_arguments{
    0x48,0x0f,0x45,0xcb,0x45,0x33,0xc9,0x48,0x89,0x4d,0x10,0x48,0x8b,0xd1,0x49,0x8b,0xcf};

struct SlateMatch { uint32_t width{}, height{}; uintptr_t command_list{}; };
template<class IsObject>
std::optional<SlateMatch> slate_inputs(const sdk::discovery::Memory& memory, uintptr_t renderer,
    uintptr_t outputs, uintptr_t graph, uintptr_t inputs_address, IsObject&& is_object) {
    SlateInputs in{};
    std::array<uintptr_t, 3> output_storage{};
    uintptr_t command_list{};
    if (!pointer(renderer) || !pointer(outputs) || !pointer(graph) || !pointer(inputs_address) ||
        outputs == renderer || outputs == graph || outputs == inputs_address ||
        !memory.load(inputs_address, in) || !memory.load(outputs, output_storage) ||
        in.renderer != renderer || !pointer(in.elements) || !is_object(renderer) ||
        !is_object(in.window) || !is_object(in.viewport_info) ||
        !std::isfinite(in.scale) || in.scale <= 0.0f || in.scale > 8.0f ||
        !memory.load(graph + 0xc0, command_list) || !pointer(command_list) ||
        command_list == graph || command_list == outputs || command_list == inputs_address) { return {}; }
    const int64_t width = int64_t{in.max_x} - in.min_x;
    const int64_t height = int64_t{in.max_y} - in.min_y;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) { return {}; }
    return SlateMatch{static_cast<uint32_t>(width), static_cast<uint32_t>(height), command_list};
}
}
