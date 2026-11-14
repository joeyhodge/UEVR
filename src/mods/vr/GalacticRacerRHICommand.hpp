#pragma once

#include <sdk/GalacticRacerRuntime.hpp>

namespace uevr::swgr_rhi {

inline constexpr size_t root_offset = 0x28;
inline constexpr size_t link_offset = 0x30;
inline constexpr size_t command_next_offset = sizeof(uintptr_t);
inline constexpr size_t constructor_size = 0x41a;
inline constexpr const char* constructor_signature =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 0F 29 74 24 30 48 89 D7 48 89 CE 0F 57 F6";

// Independently checked in the original and updated SWGR Clang constructors.
// The allocator occupies [0,0x28); Root is zeroed and CommandLink becomes &Root.
inline constexpr std::array<uint8_t, 30> constructor_entry{
    0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xec,
    0x48,0x0f,0x29,0x74,0x24,0x30,0x48,0x89,0xd7,0x48,0x89,0xce,0x0f,0x57,0xf6};
inline constexpr std::array<uint8_t, 11> constructor_root_zero{
    0x0f,0x11,0x76,0x28,0xc7,0x46,0x65,0x01,0x00,0x00,0x00};
inline constexpr std::array<uint8_t, 4> constructor_root_address{0x48,0x8d,0x5e,0x28};
inline constexpr std::array<uint8_t, 4> constructor_link_store{0x48,0x89,0x5e,0x30};

template<size_t N>
bool matches(const sdk::discovery::Memory& memory, uintptr_t address, const std::array<uint8_t, N>& code) {
    std::array<uint8_t, N> actual{};
    return memory.executable && memory.executable(memory.context, address, N) &&
        memory.load(address, actual) && actual == code;
}

inline bool command_list_layout(const sdk::discovery::Memory& memory, uintptr_t constructor,
    size_t unwind_size, uintptr_t image, size_t image_size) {
    if (!image || constructor < image || constructor - image > image_size ||
        unwind_size != constructor_size || unwind_size > image_size - (constructor - image) ||
        constructor > UINTPTR_MAX - unwind_size) { return false; }
    return matches(memory, constructor, constructor_entry) &&
        matches(memory, constructor + 0x47, constructor_root_zero) &&
        matches(memory, constructor + 0x168, constructor_root_address) &&
        matches(memory, constructor + 0x21a, constructor_link_store);
}

struct Head {
    uintptr_t root{}, link{};
    bool operator==(const Head&) const = default;
};
static_assert(sizeof(Head) == 2 * sizeof(uintptr_t));

// Empty/temporarily invalid lists never establish a cached offset. Read only
// the proven fields and the first/last commands, not allocator cursors or a walk.
template<class ValidCommand>
std::optional<uintptr_t> command_root(const sdk::discovery::Memory& memory, uintptr_t list,
    bool layout_validated, ValidCommand&& valid_command) {
    if (!layout_validated || !sdk::galactic_racer::pointer(list) ||
        list > UINTPTR_MAX - root_offset - sizeof(Head)) { return {}; }
    const auto field = list + root_offset;
    Head head{}, after{};
    if (!memory.load(field, head)) { return {}; }
    if (!head.root) { return head.link == field ? std::optional<uintptr_t>{0} : std::nullopt; }
    if (!sdk::galactic_racer::pointer(head.root) || !sdk::galactic_racer::pointer(head.link) ||
        head.link < 0x10000 + command_next_offset) { return {}; }
    const auto last = head.link - command_next_offset;
    const auto in_header = [&](uintptr_t p) { return p >= list && p - list < root_offset + sizeof(Head); };
    uintptr_t next{}, tail_next{};
    if (in_header(head.root) || in_header(last) || !valid_command(head.root) ||
        (last != head.root && !valid_command(last)) ||
        !memory.load(head.root + command_next_offset, next) ||
        !memory.load(head.link, tail_next) || tail_next != 0 ||
        (next == 0) != (last == head.root) ||
        (next && (!sdk::galactic_racer::pointer(next) || next == head.root || in_header(next))) ||
        !memory.load(field, after) || after != head) { return {}; }
    return head.root;
}

}
