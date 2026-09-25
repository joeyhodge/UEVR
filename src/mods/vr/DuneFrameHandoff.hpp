#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace uevr::dune_frame {

// September 24 shipping image. A new image must prove these contracts again.
inline constexpr uint32_t image_timestamp = 0x6AB3D64E;
inline constexpr uint32_t image_size = 0x0D7C1000;
inline constexpr uint32_t family_slot = 7;
inline constexpr uint32_t view_slot = 9;
inline constexpr uint32_t graph_list_offset = 0x50;
inline constexpr uint32_t family_frame_offset = 0x84;

inline constexpr uint32_t renderer_calls_rva = 0x060904D7;
inline constexpr std::array<uint8_t, 63> renderer_calls{
    0x4C,0x8D,0x43,0x20,0x49,0x8B,0xD6,0x48,0x8B,0x0C,0x06,0x48,0x8B,0x01,0xFF,0x50,0x38,
    0x33,0xFF,0x39,0x7B,0x30,0x7E,0x2E,0x90,0x48,0x8B,0x83,0xD0,0x00,0x00,0x00,
    0x49,0x8B,0xD6,0x48,0x8B,0x0C,0x06,0x48,0x63,0xC7,0x4C,0x69,0xC0,0x10,0xCB,0x00,0x00,
    0x4C,0x8B,0x09,0x4C,0x03,0x83,0x10,0x05,0x00,0x00,0x41,0xFF,0x51,0x48};
inline constexpr uint32_t graph_list_store_rva = 0x069D58B6;
inline constexpr std::array<uint8_t, 40> graph_list_store{
    0x48,0x8B,0xDA,0x48,0x8B,0xF1,0xE8,0xEF,0xD9,0x01,0x00,0x48,0x89,0x06,0x45,0x33,0xFF,
    0x4C,0x89,0x7E,0x10,0x4C,0x89,0x7E,0x20,0x44,0x89,0x7E,0x28,0x48,0x8B,0x06,
    0x48,0x89,0x5E,0x50,0x48,0x89,0x46,0x58};
inline constexpr uint32_t command_link_rva = 0x0608DF19;
inline constexpr std::array<uint8_t, 21> command_link{
    0x48,0x8B,0x47,0x08,0x48,0x89,0x08,0x48,0x8D,0x41,0x08,0x48,0x89,0x47,0x08,
    0x4C,0x89,0x28,0x4C,0x89,0x21};

template <typename Read>
bool validate_binary(uintptr_t base, uint32_t timestamp, uint32_t size, Read&& read) {
    if (!base || timestamp != image_timestamp || size != image_size) { return false; }
    const auto matches = [&](uint32_t rva, const auto& expected) {
        auto bytes = expected;
        return read(base + rva, bytes.data(), bytes.size()) && bytes == expected;
    };
    return matches(renderer_calls_rva, renderer_calls) &&
        matches(graph_list_store_rva, graph_list_store) && matches(command_link_rva, command_link);
}

constexpr bool enabled(bool validated, bool dx12, bool openxr, bool native, bool native_fix) {
    return validated && dx12 && openxr && native && native_fix;
}

constexpr bool should_publish(uint64_t current_generation, uint64_t command_generation, uint32_t frame,
    uint64_t last_generation, uint32_t last_frame) {
    const auto delta = frame - last_frame;
    return current_generation != 0 && current_generation == command_generation &&
        (last_generation != command_generation || (delta != 0 && delta < 0x80000000u));
}

constexpr bool aligned_pointer(uintptr_t address) {
    return address >= 0x10000 && address <= std::numeric_limits<uintptr_t>::max() - 0x2000 &&
        (address & (alignof(uintptr_t) - 1)) == 0;
}

struct FamilyFrame {
    uint32_t frame{};
    uintptr_t scene{};
    uintptr_t target{};
    bool operator==(const FamilyFrame&) const = default;
};

template <typename Read, typename ValidObject>
std::optional<FamilyFrame> read_family(uintptr_t family, uintptr_t main_target,
    Read&& read, ValidObject&& valid_object) {
    if (!aligned_pointer(family) || !aligned_pointer(main_target) || !valid_object(family)) { return {}; }
    uintptr_t data{};
    int32_t count{}, capacity{};
    FamilyFrame result{};
    if (!read(family + 8, &data, sizeof(data)) || !read(family + 0x10, &count, sizeof(count)) ||
        !read(family + 0x14, &capacity, sizeof(capacity)) ||
        !read(family + 0x20, &result.target, sizeof(result.target)) ||
        !read(family + 0x28, &result.scene, sizeof(result.scene)) ||
        !read(family + family_frame_offset, &result.frame, sizeof(result.frame)) ||
        result.target != main_target || !valid_object(result.target) || !valid_object(result.scene) ||
        !aligned_pointer(data) || count < 1 || count > 16 || capacity < count || capacity > 1024 ||
        result.frame < 10 || result.frame == std::numeric_limits<uint32_t>::max()) { return {}; }

    std::array<uintptr_t, 16> views{};
    if (!read(data, views.data(), static_cast<size_t>(count) * sizeof(uintptr_t))) { return {}; }
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t owner{};
        int32_t pass{};
        if (!aligned_pointer(views[i]) || !valid_object(views[i]) ||
            !read(views[i] + 0x10, &owner, sizeof(owner)) || owner != family ||
            !read(views[i] + 0x13A0, &pass, sizeof(pass)) || pass < 0 || pass > 2) { return {}; }
    }
    return result;
}

// CommandLink points at the final command's Next member, never an allocator cursor.
// The image contract and both ends of the list must agree before touching a vtable.
template <typename Read, typename ValidCommand>
std::optional<uintptr_t> read_tail(uintptr_t graph, Read&& read, ValidCommand&& valid_command) {
    uintptr_t list{}, root{}, link{}, next{}, root_next{};
    if (!aligned_pointer(graph) || !read(graph + graph_list_offset, &list, sizeof(list)) ||
        !aligned_pointer(list) || !read(list, &root, sizeof(root)) ||
        !read(list + 8, &link, sizeof(link)) || !aligned_pointer(root) ||
        !aligned_pointer(link) || link < 8 || link == list) { return {}; }
    const auto tail = link - 8;
    if (!aligned_pointer(tail) || !valid_command(root) || !valid_command(tail) ||
        !read(link, &next, sizeof(next)) || next != 0 ||
        !read(root + 8, &root_next, sizeof(root_next)) ||
        (root == tail ? root_next != 0 : !aligned_pointer(root_next))) { return {}; }
    return tail;
}

struct PendingFrame {
    uintptr_t command{};
    uintptr_t vtable{};
    uint32_t frame{};
    uint64_t generation{};
};

// Caller serializes access. Never evict a pending command or relabel its frame.
template <size_t Capacity = 8>
struct PendingFrames {
    std::array<PendingFrame, Capacity> entries{};

    bool contains(uintptr_t command) const {
        for (const auto& entry : entries) { if (entry.command == command && command != 0) { return true; } }
        return false;
    }

    bool reserve(PendingFrame frame) {
        if (!frame.command || !frame.vtable || contains(frame.command)) { return false; }
        for (auto& entry : entries) { if (!entry.command) { entry = frame; return true; } }
        return false;
    }

    std::optional<PendingFrame> take(uintptr_t command) {
        for (auto& entry : entries) {
            if (entry.command == command && command != 0) {
                const auto result = entry;
                entry = {};
                return result;
            }
        }
        return {};
    }
};

} // namespace uevr::dune_frame
