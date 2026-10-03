#pragma once

#include <array>
#include <span>
#include <sdk/KtjLObjectLayout.hpp>

namespace uevr::ktjl::hooks {
inline constexpr uintptr_t add_object_rva = 0x636808;
inline constexpr std::array<uint8_t, 44> add_object_entry{
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x68,0x18,0x48,0x89,0x70,0x20,0x48,
    0x89,0x48,0x08,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0x8D,0x0D,0xE5,
    0x0E,0x92,0x08,0x45,0x8B,0xF9,0x41,0x8B,0xF8,0x4C,0x8B,0xF2};
inline constexpr std::array<uint8_t, 21> add_object_store{
    0x4C,0x89,0x36,0xC7,0x46,0x08,0x00,0x00,0x00,0x80,0x89,0x5E,0x0C,0x44,0x89,0x7E,
    0x10,0x41,0x89,0x7E,0x0C};

inline bool validates_add_object(const sdk::discovery::Memory& memory, uintptr_t base, uintptr_t entry) {
    // FUObjectArray::AllocateUObjectIndex saves RDX in R14, writes R14 to
    // FUObjectItem::Object, then writes InternalIndex through [R14+0xc].
    return entry == base + add_object_rva && sdk::ktjl::validated_image(memory, base) &&
        sdk::ktjl::matches_code(memory, entry, add_object_entry) &&
        sdk::ktjl::matches_code(memory, base + 0x6368D9, add_object_store);
}

inline constexpr uintptr_t texture_create_rva = 0x3BA8174;
inline constexpr uintptr_t texture_call_rva = 0x2EA04D6;
inline constexpr uintptr_t texture_return_rva = 0x2EA04DB;
inline constexpr uintptr_t allocate_return_rva = 0x2EA049B;
inline constexpr std::array<uint8_t, 99> texture_entry{
    0x48,0x83,0xEC,0x68,0x48,0x8B,0x84,0x24,0xB8,0x00,0x00,0x00,0xC7,0x44,0x24,0x58,
    0x01,0x00,0x00,0x00,0x48,0x89,0x44,0x24,0x50,0x48,0x8B,0x84,0x24,0xB0,0x00,0x00,
    0x00,0x48,0x89,0x44,0x24,0x48,0x48,0x8B,0x84,0x24,0xA8,0x00,0x00,0x00,0x48,0x89,
    0x44,0x24,0x40,0x8A,0x84,0x24,0xA0,0x00,0x00,0x00,0xC6,0x44,0x24,0x38,0x00,0x88,
    0x44,0x24,0x30,0x8B,0x84,0x24,0x98,0x00,0x00,0x00,0x89,0x44,0x24,0x28,0x8B,0x84,
    0x24,0x90,0x00,0x00,0x00,0x89,0x44,0x24,0x20,0xE8,0x9A,0xE3,0xAF,0xFC,0x48,0x83,
    0xC4,0x68,0xC3};
inline constexpr std::array<uint8_t, 64> texture_callsite{
    0x84,0xC0,0x75,0x3C,0x8B,0x54,0x24,0x64,0x48,0x8D,0x45,0x80,0x8B,0x4C,0x24,0x60,
    0x44,0x8B,0xCB,0x48,0x89,0x44,0x24,0x48,0x45,0x8A,0xC5,0x48,0x8D,0x45,0x88,0x48,
    0x89,0x44,0x24,0x40,0x48,0x8D,0x45,0xD0,0x48,0x89,0x44,0x24,0x38,0xC6,0x44,0x24,
    0x30,0x00,0x89,0x5C,0x24,0x28,0x83,0x64,0x24,0x20,0x00,0xE8,0x99,0x7C,0xD0,0x00};

inline constexpr uintptr_t texture_vtable_rva = 0x6985110;
inline constexpr uintptr_t native_resource_rva = 0x55A0BC4;
inline constexpr std::array<uint8_t, 19> native_resource_code{
    0x48,0x8B,0x91,0xC0,0x00,0x00,0x00,0x33,0xC0,0x48,0x85,0xD2,0x74,0x04,0x48,0x8B,0x42,0x20,0xC3};
// D3D12RHI initializes PF_B8G8R8A8.PlatformFormat to TYPELESS (90),
// not the typed RTV/SRV format (87). This agrees with UE4.25-Plus source.
inline constexpr uintptr_t bgra_format_rva = 0x55526E1;
inline constexpr std::array<uint8_t, 10> bgra_format_code{0xC7,0x05,0x69,0xFC,0x87,0x03,0x5A,0x00,0x00,0x00};

inline bool validates_native_texture(const sdk::discovery::Memory& memory, uintptr_t base) {
    uintptr_t accessor{};
    return sdk::ktjl::matches_code(memory, base + native_resource_rva, native_resource_code) &&
        sdk::ktjl::matches_code(memory, base + bgra_format_rva, bgra_format_code) &&
        memory.load(base + texture_vtable_rva + 8 * sizeof(uintptr_t), accessor) && accessor == base + native_resource_rva;
}

inline std::optional<uintptr_t> read_native_texture(const sdk::discovery::Memory& memory, uintptr_t base, uintptr_t texture) {
    uintptr_t table{}, wrapper{}, resource{};
    if (!sdk::ktjl::pointer(texture) || !memory.load(texture, table) || table != base + texture_vtable_rva ||
        !memory.load(texture + 0xC0, wrapper) || !sdk::ktjl::pointer(wrapper) ||
        !memory.load(wrapper + 0x20, resource) || !sdk::ktjl::pointer(resource)) { return std::nullopt; }
    return resource;
}

struct TextureDescription {
    uint32_t dimension{}, format{}, flags{}, height{};
    uint64_t width{};
    uint16_t array_size{}, mips{};
    uint32_t samples{}, quality{};
};

inline bool valid_texture_description(const TextureDescription& d, uint32_t width, uint32_t height) {
    return width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
        d.dimension == 3 && d.width == width && d.height == height &&
        d.array_size == 1 && d.mips == 1 && d.samples == 1 && d.quality == 0 &&
        (d.format == 87 || d.format == 90) && (d.flags & 1) != 0 && (d.flags & (2 | 8)) == 0;
}

inline bool validates_texture(const sdk::discovery::Memory& memory, uintptr_t base) {
    return sdk::ktjl::validated_image(memory, base) &&
        sdk::ktjl::matches_code(memory, base + texture_create_rva, texture_entry) &&
        sdk::ktjl::matches_code(memory, base + allocate_return_rva, texture_callsite);
}

inline bool owns_texture_call(uintptr_t base, uintptr_t caller, bool pending,
                             uint32_t width, uint32_t height, uint32_t mips,
                             uint32_t flags, uint32_t target_flags, bool separate) {
    return pending && caller == base + texture_return_rva && width != 0 && height != 0 &&
        width <= 16384 && height <= 16384 && mips == 1 && flags == 0 && target_flags == 1 && !separate;
}

struct ClassChain {
    std::array<uintptr_t, 64> classes{};
    size_t count{};
};

template<class ReadNext>
std::optional<ClassChain> collect_class_chain(uintptr_t first, ReadNext&& read_next) {
    if (!sdk::ktjl::pointer(first)) { return std::nullopt; }
    ClassChain result{};
    for (auto current = first; current != 0;) {
        if (!sdk::ktjl::pointer(current) || result.count == result.classes.size()) { return std::nullopt; }
        for (size_t i = 0; i < result.count; ++i) {
            if (result.classes[i] == current) { return std::nullopt; }
        }
        uintptr_t next{};
        if (!read_next(current, next)) { return std::nullopt; }
        result.classes[result.count++] = current;
        current = next;
    }
    return result;
}
}
